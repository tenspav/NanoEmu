/*
 * nano7_ui.c -- emulator front-end controls for the iPod nano 7G machine.
 *
 * One QEMU keyboard handler (VNC, a display window, or the monitor's
 * sendkey) for everything:
 *   h / p / space            Home / Power / Play-Pause   (nano7_pmu.c)
 *   Up = + / Down -          volume up / down
 *   F1  reset                F2  save state      F4  load state
 *   F7  rotate the display 90 degrees clockwise (0 / 90 / 180 / 270; touch
 *       coordinates follow)
 *   F9  integer zoom 1x / 2x / 3x (nearest neighbour, no blur)
 *   F12 screenshot: the panel frame as PNG into screens-dir
 *   F5 / Pause  pause / resume the guest
 *   F6  fast-forward 1x / 2x / 4x (cycle)   Tab (held)  4x while held
 *       (guest time base, nano7_clock.c; the RTC runs fast too)
 * Actions run from a bottom half in the main loop, not inside the input
 * dispatch.
 *
 * Display: the LCD model hands over its 240x432 panel frame whenever it
 * changes (nano7_ui_frame); this module owns the QEMU graphic console and
 * renders the frame rotated and zoomed into the console surface (resized
 * when rotation / zoom change the output size).
 *
 * Save state (slot directory under state-dir, default "state"):
 *   ram-<region>.bin   raw contents of each RAM region (DRAM, IRAM, VROM)
 *   devices.vmstate    every non-RAM vmstate section: CPU, virtual clock,
 *                      and each nano7 device (VIC, timers, FMC, LCD, ...),
 *                      written by QEMU's device-state saver
 *   nand.bin           copy of the NAND page store
 * Loading restores the three in the same order with the VM stopped, then
 * flushes translated code. Reset = load the "boot" slot, saved
 * automatically when the VM first starts running (after the snapshot
 * resume set-up, or at the bootrom), so it works for both start modes.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "nano7_ui.h"
#include "nano7_pmu.h"
#include "nano7_fmc.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-migration.h"
#include "qom/object.h"
#include "io/channel-file.h"
#include "migration/migration.h"
#include "migration/qemu-file.h"
#include "migration/savevm.h"
#include "qemu/main-loop.h"
#include "exec/tb-flush.h"
#include "hw/core/cpu.h"
#include "sysemu/runstate.h"
#include "ui/input.h"
#include "ui/console.h"
#include "nano7_lcd.h"
#include "nano7_clock.h"
#include <zlib.h>

#define UI_MAX_RAM      8

typedef enum UiAction {
    UI_NONE, UI_SAVE, UI_LOAD, UI_RESET, UI_BOOT_SAVE, UI_SHOT,
} UiAction;

typedef struct UiRam {
    MemoryRegion *mr;
    char *name;
} UiRam;

typedef struct Nano7UiState {
    char *state_dir, *screens_dir;
    UiRam ram[UI_MAX_RAM];
    unsigned nram;
    QEMUBH *bh;
    UiAction pending;
    bool boot_saved;
    VMChangeStateEntry *vmse;
    /* display */
    QemuConsole *con;
    const uint32_t *frame;      /* the LCD's panel frame (240x432 XRGB) */
    uint32_t *out;              /* rendered console surface */
    int rot;                    /* clockwise degrees: 0, 90, 180, 270 */
    int zoom;                   /* 1..3 */
    int out_w, out_h;
    int ff_speed;               /* F6 setting; Tab overrides while held */
} Nano7UiState;

static Nano7UiState *ui;

void nano7_ui_add_ram(MemoryRegion *mr, const char *name)
{
    if (ui && ui->nram < UI_MAX_RAM) {
        ui->ram[ui->nram].mr = mr;
        ui->ram[ui->nram].name = g_strdup(name);
        ui->nram++;
    }
}

/* ------------------------------------------------------------ save state --- */
static char *ui_slot_dir(const char *slot)
{
    return g_build_filename(ui->state_dir, slot, NULL);
}

static bool ui_save_state(const char *slot)
{
    g_autofree char *dir = ui_slot_dir(slot);
    g_autofree char *dev = g_build_filename(dir, "devices.vmstate", NULL);
    g_autofree char *nand = g_build_filename(dir, "nand.bin", NULL);
    bool running = runstate_is_running(), ok = true;
    Error *err = NULL;
    int64_t t0 = g_get_monotonic_time();
    unsigned i;

    if (g_mkdir_with_parents(dir, 0755) < 0) {
        fprintf(stderr, "[nano7-ui] cannot create %s\n", dir);
        return false;
    }
    vm_stop(RUN_STATE_SAVE_VM);
    for (i = 0; i < ui->nram && ok; i++) {
        g_autofree char *f = g_strdup_printf("%s/ram-%s.bin", dir,
                                             ui->ram[i].name);

        ok = g_file_set_contents(f, memory_region_get_ram_ptr(ui->ram[i].mr),
                                 memory_region_size(ui->ram[i].mr), NULL);
    }
    if (ok) {
        qmp_xen_save_devices_state(dev, true, false, &err);
        if (err) {
            error_report_err(err);
            ok = false;
        }
    }
    ok = ok && nano7_fmc_store_save(nand);
    if (running) {
        vm_start();
    }
    fprintf(stderr, "[nano7-ui] save state '%s' -> %s: %s (%.0f ms)\n", slot,
            dir, ok ? "ok" : "FAILED", (g_get_monotonic_time() - t0) / 1e3);
    return ok;
}

/* qemu_loadvm_state() on a device-state file (what the Xen helper does,
 * minus its migration teardown, which asserts outside a real migration) */
static bool ui_load_devices(const char *path)
{
    Error *err = NULL;
    QIOChannelFile *ioc = qio_channel_file_new_path(path, O_RDONLY, 0, &err);
    QEMUFile *f;
    int ret;

    if (!ioc) {
        error_report_err(err);
        return false;
    }
    f = qemu_file_new_input(QIO_CHANNEL(ioc));
    object_unref(OBJECT(ioc));
    ret = qemu_loadvm_state(f);
    qemu_loadvm_state_cleanup();    /* frees the RAM receive bitmaps */
    qemu_fclose(f);
    if (ret < 0) {
        fprintf(stderr, "[nano7-ui] device state %s: load failed (%d)\n",
                path, ret);
    }
    return ret >= 0;
}

static bool ui_load_state(const char *slot)
{
    g_autofree char *dir = ui_slot_dir(slot);
    g_autofree char *dev = g_build_filename(dir, "devices.vmstate", NULL);
    g_autofree char *nand = g_build_filename(dir, "nand.bin", NULL);
    bool ok = true;
    int64_t t0 = g_get_monotonic_time();
    unsigned i;

    if (!g_file_test(dev, G_FILE_TEST_EXISTS)) {
        fprintf(stderr, "[nano7-ui] no saved state '%s' (%s)\n", slot, dir);
        return false;
    }
    vm_stop(RUN_STATE_RESTORE_VM);
    for (i = 0; i < ui->nram && ok; i++) {
        g_autofree char *f = g_strdup_printf("%s/ram-%s.bin", dir,
                                             ui->ram[i].name);
        g_autofree char *data = NULL;
        gsize len = 0;

        ok = g_file_get_contents(f, &data, &len, NULL) &&
             len == memory_region_size(ui->ram[i].mr);
        if (ok) {
            memcpy(memory_region_get_ram_ptr(ui->ram[i].mr), data, len);
        }
    }
    ok = ok && nano7_fmc_store_load(nand);
    if (ok) {
        /* the device-state stream has no configuration section */
        object_property_set_bool(OBJECT(migrate_get_current()),
                                 "send-configuration", false, &error_abort);
        ok = ui_load_devices(dev);
    }
    if (first_cpu) {
        tb_flush(first_cpu);    /* RAM changed under translated code */
    }
    if (ok) {
        vm_start();
    }
    fprintf(stderr, "[nano7-ui] load state '%s' <- %s: %s (%.0f ms)%s\n",
            slot, dir, ok ? "ok" : "FAILED",
            (g_get_monotonic_time() - t0) / 1e3,
            ok ? "" : " - VM left stopped");
    return ok;
}

/* ----------------------------------------------------------------- display --- */
static void ui_render(void)
{
    int lw = (ui->rot % 180) ? NANO7_LCD_H : NANO7_LCD_W;   /* rotated */
    int lh = (ui->rot % 180) ? NANO7_LCD_W : NANO7_LCD_H;
    int w = lw * ui->zoom, h = lh * ui->zoom, x, y;

    if (w != ui->out_w || h != ui->out_h || !ui->out) {
        uint32_t *old = ui->out;

        ui->out = g_new0(uint32_t, w * h);
        ui->out_w = w;
        ui->out_h = h;
        dpy_gfx_replace_surface(ui->con, qemu_create_displaysurface_from(
                                    w, h, PIXMAN_x8r8g8b8, w * 4,
                                    (uint8_t *)ui->out));
        g_free(old);
    }
    for (y = 0; y < h; y++) {
        uint32_t *row = ui->out + y * w;
        int ly = y / ui->zoom;

        for (x = 0; x < w; x++) {
            int lx = x / ui->zoom, sx, sy;

            switch (ui->rot) {      /* rotated pixel -> panel pixel */
            case 90:  sx = ly;                 sy = NANO7_LCD_H - 1 - lx; break;
            case 180: sx = NANO7_LCD_W - 1 - lx; sy = NANO7_LCD_H - 1 - ly; break;
            case 270: sx = NANO7_LCD_W - 1 - ly; sy = lx;                 break;
            default:  sx = lx;                 sy = ly;                 break;
            }
            row[x] = ui->frame ? ui->frame[sy * NANO7_LCD_W + sx] : 0;
        }
    }
    dpy_gfx_update_full(ui->con);
}

void nano7_ui_frame(const uint32_t *frame)
{
    if (ui) {
        ui->frame = frame;
        ui_render();
    }
}

/* absolute pointer (0..range-1 over the console) -> panel pixel */
void nano7_ui_map_touch(int64_t u, int64_t v, int64_t range, int *px, int *py)
{
    int lw = ui && (ui->rot % 180) ? NANO7_LCD_H : NANO7_LCD_W;
    int lh = ui && (ui->rot % 180) ? NANO7_LCD_W : NANO7_LCD_H;
    int lx = u * lw / range, ly = v * lh / range;

    switch (ui ? ui->rot : 0) {
    case 90:  *px = ly;                   *py = NANO7_LCD_H - 1 - lx; break;
    case 180: *px = NANO7_LCD_W - 1 - lx; *py = NANO7_LCD_H - 1 - ly; break;
    case 270: *px = NANO7_LCD_W - 1 - ly; *py = lx;                   break;
    default:  *px = lx;                   *py = ly;                   break;
    }
}

static void ui_gfx_invalidate(void *opaque)
{
    ui_render();
}

static void ui_gfx_update(void *opaque)
{
    /* frames are pushed by the LCD model (nano7_ui_frame) */
}

static const GraphicHwOps ui_gfx_ops = {
    .invalidate = ui_gfx_invalidate,
    .gfx_update = ui_gfx_update,
};

/* F12: the panel frame (unrotated, 240x432) as an RGB PNG */
static void ui_png_chunk(FILE *f, const char *type, const uint8_t *d,
                         uint32_t n)
{
    uint8_t hdr[8];
    uint32_t crc = crc32(0, (const Bytef *)type, 4);

    stl_be_p(hdr, n);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    fwrite(d, 1, n, f);
    crc = crc32(crc, d, n);
    stl_be_p(hdr, crc);
    fwrite(hdr, 1, 4, f);
}

static void ui_screenshot(void)
{
    const int w = NANO7_LCD_W, h = NANO7_LCD_H;
    g_autofree uint8_t *raw = g_malloc(h * (w * 3 + 1));
    uLongf zlen = compressBound(h * (w * 3 + 1));
    g_autofree uint8_t *z = g_malloc(zlen);
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    g_autofree char *stamp = g_date_time_format(now, "%Y%m%d-%H%M%S");
    g_autofree char *path = NULL;
    uint8_t ihdr[13] = { 0 };
    FILE *f;
    int x, y;

    if (!ui->frame) {
        fprintf(stderr, "[nano7-ui] screenshot: no frame yet\n");
        return;
    }
    for (y = 0; y < h; y++) {
        uint8_t *o = raw + y * (w * 3 + 1);

        *o++ = 0;                               /* filter: none */
        for (x = 0; x < w; x++) {
            uint32_t p = ui->frame[y * w + x];

            *o++ = p >> 16;
            *o++ = p >> 8;
            *o++ = p;
        }
    }
    compress2(z, &zlen, raw, h * (w * 3 + 1), 6);
    g_mkdir_with_parents(ui->screens_dir, 0755);
    path = g_strdup_printf("%s/nano7-%s.png", ui->screens_dir, stamp);
    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[nano7-ui] screenshot: cannot write %s\n", path);
        return;
    }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    stl_be_p(ihdr, w);
    stl_be_p(ihdr + 4, h);
    ihdr[8] = 8;                                /* 8-bit RGB */
    ihdr[9] = 2;
    ui_png_chunk(f, "IHDR", ihdr, 13);
    ui_png_chunk(f, "IDAT", z, zlen);
    ui_png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    fprintf(stderr, "[nano7-ui] screenshot -> %s\n", path);
}

/* ----------------------------------------------------------------- actions --- */
static void ui_bh(void *opaque)
{
    UiAction a = ui->pending;

    ui->pending = UI_NONE;
    switch (a) {
    case UI_SAVE:
        ui_save_state("slot0");
        break;
    case UI_LOAD:
        ui_load_state("slot0");
        break;
    case UI_RESET:
        fprintf(stderr, "[nano7-ui] reset: reloading the boot state\n");
        ui_load_state("boot");
        break;
    case UI_BOOT_SAVE:
        ui->boot_saved = ui_save_state("boot");
        break;
    case UI_SHOT:
        ui_screenshot();
        break;
    default:
        break;
    }
}

static void ui_request(UiAction a)
{
    ui->pending = a;
    qemu_bh_schedule(ui->bh);
}

/* first time the VM runs: snapshot the start state for F1 reset */
static void ui_vm_state(void *opaque, bool running, RunState state)
{
    if (running && !ui->boot_saved && ui->pending == UI_NONE) {
        ui->boot_saved = true;
        ui_request(UI_BOOT_SAVE);
    }
}

static void ui_key_event(DeviceState *dev, QemuConsole *src, InputEvent *evt)
{
    InputKeyEvent *key = evt->u.key.data;
    int qcode = qemu_input_key_value_to_qcode(key->key);

    if (nano7_pmu_key(qcode, key->down)) {
        return;
    }
    if (qcode == Q_KEY_CODE_TAB) {      /* hold to fast-forward */
        nano7_clock_set_speed(key->down ? 4 : ui->ff_speed);
        fprintf(stderr, "[nano7-ui] speed %dx%s\n", nano7_clock_speed(),
                key->down ? " (Tab held)" : "");
        return;
    }
    if (!key->down) {
        return;
    }
    switch (qcode) {
    case Q_KEY_CODE_F1:
        ui_request(UI_RESET);
        break;
    case Q_KEY_CODE_F2:
        ui_request(UI_SAVE);
        break;
    case Q_KEY_CODE_F4:
        ui_request(UI_LOAD);
        break;
    case Q_KEY_CODE_F7:
        ui->rot = (ui->rot + 90) % 360;
        fprintf(stderr, "[nano7-ui] rotation %d\n", ui->rot);
        ui_render();
        break;
    case Q_KEY_CODE_F9:
        ui->zoom = ui->zoom % 3 + 1;
        fprintf(stderr, "[nano7-ui] zoom %dx\n", ui->zoom);
        ui_render();
        break;
    case Q_KEY_CODE_F12:
        ui_request(UI_SHOT);
        break;
    case Q_KEY_CODE_F5:
    case Q_KEY_CODE_PAUSE:
        if (runstate_is_running()) {
            vm_stop(RUN_STATE_PAUSED);
            fprintf(stderr, "[nano7-ui] paused\n");
        } else if (runstate_check(RUN_STATE_PAUSED)) {
            vm_start();
            fprintf(stderr, "[nano7-ui] resumed\n");
        }
        break;
    case Q_KEY_CODE_F6:
        ui->ff_speed = ui->ff_speed >= 4 ? 1 : ui->ff_speed * 2;
        nano7_clock_set_speed(ui->ff_speed);
        fprintf(stderr, "[nano7-ui] speed %dx\n", ui->ff_speed);
        break;
    default:
        break;
    }
}

static const QemuInputHandler ui_key_handler = {
    .name  = "nano7 keys",
    .mask  = INPUT_EVENT_MASK_KEY,
    .event = ui_key_event,
};

void nano7_ui_init(const char *state_dir, const char *screens_dir)
{
    QemuInputHandlerState *hs;

    ui = g_new0(Nano7UiState, 1);
    ui->state_dir = g_strdup(state_dir ? state_dir : "state");
    ui->screens_dir = g_strdup(screens_dir ? screens_dir : "screens");
    ui->bh = qemu_bh_new(ui_bh, NULL);
    ui->zoom = 1;
    ui->ff_speed = 1;
    nano7_clock_set_speed(1);   /* registers the guest clock's vmstate */
    ui->con = graphic_console_init(NULL, 0, &ui_gfx_ops, ui);
    ui_render();                /* black 240x432 until the first frame */
    ui->vmse = qemu_add_vm_change_state_handler(ui_vm_state, NULL);
    hs = qemu_input_handler_register(NULL, &ui_key_handler);
    qemu_input_handler_activate(hs);
}
