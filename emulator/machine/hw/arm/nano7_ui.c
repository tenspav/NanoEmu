/*
 * nano7_ui.c -- emulator front-end controls for the iPod nano 7G machine.
 *
 * One QEMU keyboard handler (VNC, a display window, or the monitor's
 * sendkey) for everything:
 *   h / Esc / Home  Home     p  Power (sleep / wake)    space  Play-Pause
 *   Up = + / Down -          volume up / down
 *   Left / Right (held)      tilt the device like a steering wheel
 *                            (accelerometer, nano7_accel.c)            (nano7_pmu.c)
 *   F1  reset                F2  save state      F4  load state
 *   F7  rotate the display 90 degrees clockwise (0 / 90 / 180 / 270; touch
 *       coordinates follow)
 *   F9  integer zoom 1x / 2x / 3x (nearest neighbour, no blur); NANO7_ZOOM
 *       (run.sh --zoom) sets the start zoom, and when a display window is
 *       resized (SDL) the zoom follows: the largest integer factor that fits
 *   F10 usage HUD on / off: a strip under the display with CPU / GPU load
 *       graphs (nano7_hud.c; run.sh --hud starts with it shown)
 *   F8  BT gamepad keys on / off: the keyboard drives the fake Bluetooth
 *       HID gamepad (nano7_btuart.c: WASD left stick, arrows right stick,
 *       J K U I = A B X Y, Q / E = L / R, Enter start, Backspace select,
 *       Esc home); unmapped keys (h, p, space, = -) still reach the iPod
 *   F11 clock profile STOCK -> SAFE -> MAX (nano7_power.c; with the 65 C
 *       governor; run.sh --clock-profile starts with one)
 *   F12 screenshot: the panel frame as PNG into screens-dir
 *   F5 / Pause  pause / resume the guest (a "soft" pause: vCPUs paused and
 *       guest time frozen, nano7_clock.c; the run state stays "running"
 *       and QEMU's virtual clock keeps going, because QEMU drops input
 *       while the VM is stopped and VNC delivers keys through a
 *       virtual-clock queue - either would make the pause unrecoverable)
 *   F6  fast-forward 1x / 2x / 4x (cycle)   Tab (held)  4x while held
 *       (guest time base, nano7_clock.c; the RTC runs fast too)
 * Cmd (Meta / Super) shortcuts, for keyboards whose F keys are media keys
 * (Macs); while Cmd is held no key reaches the iPod or the gamepad:
 *   Cmd-S save   Cmd-L / Cmd-O load   Cmd-R reset   Cmd-P pause
 *   Cmd-1..4 zoom   Cmd-T rotate   Cmd-U HUD   Cmd-K clock profile
 *   Cmd-F fast-forward   Cmd-G gamepad keys   Cmd-E screenshot
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
 * resume set-up, or at the bootrom), so it works for both start modes;
 * QMP / HMP system_reset takes the same path (nano7.c machine reset).
 * Over QMP: system_reset, and send-key f2 / f4 for save / load.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "nano7_ui.h"
#include "nano7_i2s.h"
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
#include "sysemu/cpus.h"
#include "qemu/seqlock.h"
#include "qemu/thread.h"
#include "sysemu/cpu-timers-internal.h"
#include "qemu/timer.h"
#include "ui/input.h"
#include "ui/console.h"
#include "nano7_lcd.h"
#include "nano7_clock.h"
#include "nano7_hud.h"
#include "nano7_power.h"
#include "nano7_btuart.h"
#include "nano7_skin.h"
#include <math.h>
#ifdef CONFIG_DARWIN
#include "nano7_macmenu.h"
#endif
#include "nano7_accel.h"
#include <zlib.h>

#define UI_MAX_RAM      8
#define UI_ZOOM_MAX     6       /* window-fit limit (F9 cycles 1..3) */

typedef enum UiAction {
    UI_NONE, UI_SAVE, UI_LOAD, UI_RESET, UI_BOOT_SAVE, UI_SHOT, UI_PAUSE,
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
    const uint32_t *lcd;        /* the LCD frame even while dark */
    bool dark;                  /* panel powered off (iPod asleep) */
    uint32_t *out;              /* rendered console surface */
    int rot;                    /* clockwise degrees: 0, 90, 180, 270 */
    int zoom;                   /* 1..UI_ZOOM_MAX */
    int out_w, out_h;
    int ff_speed;               /* F6 setting; Tab overrides while held */
    bool paused;                /* soft pause (F5 / Pause) */
    bool meta_l, meta_r;        /* Cmd held: shortcuts, nothing to the iPod */
    /* device frame (NANO7_FRAME=1, nano7_skin.c): the console shows the
     * body with clickable buttons around the panel */
    bool skin;
    uint32_t *canvas;           /* unrotated skin canvas */
    bool skin_valid;            /* out holds a full skin render of ... */
    unsigned skin_pressed;      /* ... these pressed buttons */
    int skin_panel_pressed;
    bool skin_dark;
    Nano7PanelInfo skin_pi;     /* ... and this panel content */
    double ptr_x, ptr_y;        /* pointer on the device (units) */
    bool ptr_panel;             /* the pointer is over the side panel */
    double pan_x, pan_y;        /* ... at this panel point (units) */
    int capture;                /* press began: 0 screen/none, 1 button, 2 off,
                                 * 3 side-panel command button */
    int held_btn;               /* capture 1: NANO7_BTN_*, 3: panel id */
    /* keys whose press was a Cmd shortcut: their release is dropped too
     * (it may arrive after Cmd is up, and p / h / space are iPod buttons) */
    bool swallowed[Q_KEY_CODE__MAX];
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

/* ----------------------------------------------------------- soft pause --- */
static void ui_set_paused(bool on)
{
    if (on) {
        pause_all_vcpus();
        /* pause_all_vcpus() also disables QEMU_CLOCK_VIRTUAL timers, which
         * would freeze VNC's key queue; guest time is frozen instead */
        qemu_clock_enable(QEMU_CLOCK_VIRTUAL, true);
        nano7_clock_set_speed(0);   /* no guest time passes while paused */
    } else {
        nano7_clock_set_speed(ui->ff_speed);
        resume_all_vcpus();
    }
    ui->paused = on;
}

/* ----------------------------------------------------------- save state --- */
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
        if (ui->paused) {
            ui_set_paused(true);    /* vm_start lifted the soft pause */
        }
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
    int64_t v_before;
    bool ok = true;
    int64_t t0 = g_get_monotonic_time();
    unsigned i;

    if (!g_file_test(dev, G_FILE_TEST_EXISTS)) {
        fprintf(stderr, "[nano7-ui] no saved state '%s' (%s)\n", slot, dir);
        return false;
    /* a loaded state starts with the panel lit (the PMU's display-power
     * write that put it to sleep is not replayed) */
    nano7_ui_set_display(true);
    }
    vm_stop(RUN_STATE_RESTORE_VM);
    v_before = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
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
        /* QEMU restored the saved virtual time; move it forward again so
         * it stays monotonic (QEMU's own timers, e.g. VNC's key queue,
         * would otherwise sit in the future) and shift the device timers */
        int64_t delta = v_before - qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

        seqlock_write_lock(&timers_state.vm_clock_seqlock,
                           &timers_state.vm_clock_lock);
        timers_state.cpu_clock_offset += delta;   /* ticks are disabled */
        seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                             &timers_state.vm_clock_lock);
        nano7_clock_after_load(delta);
        nano7_lcd_refresh();
    }
    if (ok) {
        vm_start();
        if (ui->paused) {
            ui_set_paused(true);
        }
    }
    fprintf(stderr, "[nano7-ui] load state '%s' <- %s: %s (%.0f ms)%s\n",
            slot, dir, ok ? "ok" : "FAILED",
            (g_get_monotonic_time() - t0) / 1e3,
            ok ? "" : " - VM left stopped");
    return ok;
}

/* -------------------------------------------------------------- display --- */
static void ui_render_panel(void);
/* skin canvas size in units, then rotated output size in pixels */
/*
 * Skin layout (units): the device (nano7_skin.c, rotated with the screen)
 * on the left, vertically centred, and the side panel (live numbers and
 * labelled buttons) on its right while the HUD is on (Cmd-U). The panel
 * never rotates.
 */
static bool ui_panel_on(void)
{
    return nano7_hud_on();
}

/* the device as displayed: size (rotated) and top offset, in units */
static void ui_dev_rect(int *rw, int *rh, int *oy)
{
    int dw, dh;

    nano7_skin_size(&dw, &dh);
    *rw = (ui->rot % 180) ? dh : dw;
    *rh = (ui->rot % 180) ? dw : dh;
    *oy = (MAX(dw, dh) - *rh) / 2;
}

/* the whole console in units */
static void ui_skin_dims(int *cw, int *ch)
{
    int dw, dh, rw, rh, oy;

    nano7_skin_size(&dw, &dh);
    ui_dev_rect(&rw, &rh, &oy);
    *cw = rw + (ui_panel_on() ? NANO7_PANEL_W : 0);
    *ch = MAX(dw, dh);
}

static void ui_panel_info(Nano7PanelInfo *pi)
{
    uint32_t used, total;

    memset(pi, 0, sizeof(*pi));
    nano7_hud_stats(&pi->fps, &pi->cpu, &pi->gpu, &used, &total);
    pi->ram_used = roundf(used / 1048576.0f * 10) / 10;
    pi->ram_total = roundf(total / 1048576.0f);
    pi->cpu_mhz = roundf(nano7_power_cpu_mhz());
    pi->gpu_mhz = roundf(nano7_power_gpu_mhz());
    pi->temp = roundf(nano7_power_temp());
    pi->fps = roundf(pi->fps);
    pi->cpu = roundf(pi->cpu * 100) / 100;
    pi->gpu = roundf(pi->gpu * 100) / 100;
    pi->profile = nano7_power_profile();
    pi->rot = ui->rot;
    pi->paused = ui->paused;
    pi->muted = nano7_i2s_muted();
}

static void ui_render_skin(void)
{
    int dw, dh, rw, rh, oy, cw, ch, z = ui->zoom, x, y;
    int pw, ph, w, h, dy;
    unsigned pressed = ui->capture == 1 && ui->held_btn < 32 ?
                       1u << ui->held_btn : 0;
    uint32_t bg = nano7_skin_bg();
    int panel_pressed = ui->capture == 1 || ui->capture == 3 ?
                        ui->held_btn : -1;
    Nano7PanelInfo pi;

    nano7_skin_size(&dw, &dh);
    ui_dev_rect(&rw, &rh, &oy);
    ui_skin_dims(&cw, &ch);
    memset(&pi, 0, sizeof(pi));
    if (ui_panel_on()) {
        ui_panel_info(&pi);
    }
    /* fast path (every guest frame): only the screen changed - scale the
     * panel frame into its rectangle and update just that */
    if (ui->skin_valid && !ui->rot && ui->out && ui->frame &&
        ui->out_w == cw * z && ui->out_h == ch * z &&
        ui->skin_pressed == pressed && ui->skin_panel_pressed ==
        panel_pressed && !ui->skin_dark && !memcmp(&ui->skin_pi, &pi,
                                                    sizeof(pi))) {
        int sx, sy;

        nano7_skin_screen_origin(&sx, &sy);
        sx *= z;
        sy = (sy + oy) * z;
        for (y = 0; y < NANO7_LCD_H * z; y++) {
            const uint32_t *src = ui->frame + (y / z) * NANO7_LCD_W;
            uint32_t *dst = ui->out + (sy + y) * ui->out_w + sx;

            for (x = 0; x < NANO7_LCD_W * z; x++) {
                dst[x] = src[x / z];
            }
        }
        dpy_gfx_update(ui->con, sx, sy, NANO7_LCD_W * z, NANO7_LCD_H * z);
        return;
    }
    pw = dw * z;                        /* the unrotated device canvas */
    ph = dh * z;
    w = cw * z;
    h = ch * z;
    dy = oy * z;
    if (w != ui->out_w || h != ui->out_h || !ui->out) {
        uint32_t *old = ui->out;

        ui->out = g_new0(uint32_t, w * h);
        ui->out_w = w;
        ui->out_h = h;
        dpy_gfx_replace_surface(ui->con, qemu_create_displaysurface_from(
                                    w, h, PIXMAN_x8r8g8b8, w * 4,
                                    (uint8_t *)ui->out));
        g_free(old);
        g_free(ui->canvas);
        ui->canvas = NULL;
    }
    if (!ui->canvas) {
        ui->canvas = g_new0(uint32_t, pw * ph);
    }
    nano7_skin_draw(ui->canvas, z, ui->frame, pressed);
    for (y = 0; y < h; y++) {           /* device, rotated, at (0, dy) */
        uint32_t *row = ui->out + y * w;
        int ry = y - dy;

        if (ry < 0 || ry >= rh * z) {
            for (x = 0; x < rw * z; x++) {
                row[x] = bg;
            }
            continue;
        }
        if (!ui->rot) {
            memcpy(row, ui->canvas + ry * pw, pw * sizeof(uint32_t));
            continue;
        }
        for (x = 0; x < rw * z; x++) {  /* rotated pixel -> canvas pixel */
            int sx, sy;

            switch (ui->rot) {
            case 90:  sx = ry;          sy = ph - 1 - x;  break;
            case 180: sx = pw - 1 - x;  sy = ph - 1 - ry; break;
            default:  sx = pw - 1 - ry; sy = x;           break;
            }
            row[x] = ui->canvas[sy * pw + sx];
        }
    }
    if (ui_panel_on()) {
        nano7_skin_panel_draw(ui->out + rw * z, w, z, ch, &pi, panel_pressed);
    }
    ui->skin_valid = !ui->rot;          /* the fast path is unrotated */
    ui->skin_pressed = pressed;
    ui->skin_panel_pressed = panel_pressed;
    ui->skin_dark = !ui->frame;
    ui->skin_pi = pi;
    dpy_gfx_update_full(ui->con);
}

static void ui_render(void)
{
    if (ui->skin) {
        ui_render_skin();
        return;
    }
    ui_render_panel();
}

static void ui_render_panel(void)
{
    int lw = (ui->rot % 180) ? NANO7_LCD_H : NANO7_LCD_W;   /* rotated */
    int lh = (ui->rot % 180) ? NANO7_LCD_W : NANO7_LCD_H;
    int hh = nano7_hud_on() ? NANO7_HUD_ROWS : 0;           /* HUD strip */
    int w = lw * ui->zoom, h = (lh + hh) * ui->zoom, x, y;

    ui->skin_valid = false;

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
    if (hh) {
        g_autofree uint32_t *strip = g_new(uint32_t, lw * hh);

        nano7_hud_draw(strip, lw);
        for (y = lh * ui->zoom; y < h; y++) {
            const uint32_t *src = strip + (y / ui->zoom - lh) * lw;

            for (x = 0; x < w; x++) {
                ui->out[y * w + x] = src[x / ui->zoom];
            }
        }
    }
    for (y = 0; y < lh * ui->zoom; y++) {
        uint32_t *row = ui->out + y * w;
        int ly = y / ui->zoom;

        for (x = 0; x < w; x++) {
            int lx = x / ui->zoom, sx, sy;

            switch (ui->rot) {      /* rotated pixel -> panel pixel */
            case 90:
                sx = ly;
                sy = NANO7_LCD_H - 1 - lx;
                break;
            case 180:
                sx = NANO7_LCD_W - 1 - lx;
                sy = NANO7_LCD_H - 1 - ly;
                break;
            case 270:
                sx = NANO7_LCD_W - 1 - ly;
                sy = lx;
                break;
            default:
                sx = lx;
                sy = ly;
                break;
            }
            row[x] = ui->frame ? ui->frame[sy * NANO7_LCD_W + sx] : 0;
        }
    }
    dpy_gfx_update_full(ui->con);
}

void nano7_ui_frame(const uint32_t *frame)
{
    if (ui) {
        ui->lcd = frame;
        ui->frame = ui->dark ? NULL : frame;
        nano7_hud_lcd_frame();
        ui_render();
    }
}

void nano7_ui_set_display(bool on)
{
    if (ui && ui->dark == on) {
        ui->dark = !on;
        ui->frame = on ? ui->lcd : NULL;
        fprintf(stderr, "[nano7-ui] display %s\n", on ? "on" : "off");
        ui_render();
    }
}

void nano7_ui_redraw(void)
{
    if (ui) {
        ui_render();
    }
}

/* absolute pointer (0..range-1 over the console) -> panel pixel */
static void nano7_ui_map_panel(int64_t u, int64_t v, int64_t range, int *px,
                               int *py);

/* skin: pointer -> canvas units (undoing the rotation), then the screen
 * pixel under it (clamped: a drag that leaves the screen stays on its
 * edge) */
static void ui_map_skin(int64_t u, int64_t v, int64_t range, int *px, int *py)
{
    int dw, dh, rw, rh, oy, lw, lh;
    double fx, fy;

    nano7_skin_size(&dw, &dh);
    ui_dev_rect(&rw, &rh, &oy);
    ui_skin_dims(&lw, &lh);
    fx = (double)u * lw / range;
    fy = (double)v * lh / range;
    ui->ptr_panel = fx >= rw;
    if (ui->ptr_panel) {
        ui->pan_x = fx - rw;
        ui->pan_y = fy;
        fx = fy = -1000;                /* off the device */
    } else {
        fy -= oy;
    }
    switch (ui->rot) {
    case 90:  ui->ptr_x = fy;      ui->ptr_y = dh - fx; break;
    case 180: ui->ptr_x = dw - fx; ui->ptr_y = dh - fy; break;
    case 270: ui->ptr_x = dw - fy; ui->ptr_y = fx;      break;
    default:  ui->ptr_x = fx;      ui->ptr_y = fy;      break;
    }
    nano7_skin_screen_xy(ui->ptr_x, ui->ptr_y, px, py);
}

/* a mouse button changed: true if the skin takes it (a drawn button, or a
 * press that began off the screen), false = it is a touch */
bool nano7_ui_pointer_button(bool down)
{
    int px, py, hit;

    if (!ui || !ui->skin) {
        return false;
    }
    if (!down) {
        int c = ui->capture;

        ui->capture = 0;
        if (c == 1) {
            nano7_pmu_button(ui->held_btn, false);
        }
        if (c == 1 || c == 3) {
            ui_render();
        }
        return c != 0;
    }
    if (ui->ptr_panel) {
        hit = nano7_skin_panel_hit(ui->pan_x, ui->pan_y);
        fprintf(stderr, "[nano7-ui] panel press at (%.0f, %.0f) -> %d\n",
                ui->pan_x, ui->pan_y, hit);
        if (hit >= NANO7_SKIN_P_STOCK) {    /* an emulator command */
            static const struct { int id, cmd, arg; } map[] = {
                { NANO7_SKIN_P_STOCK, NANO7_UI_PROFILE, NANO7_PROFILE_STOCK },
                { NANO7_SKIN_P_SAFE, NANO7_UI_PROFILE, NANO7_PROFILE_SAFE },
                { NANO7_SKIN_P_MAX, NANO7_UI_PROFILE, NANO7_PROFILE_MAX },
                { NANO7_SKIN_P_PAUSE, NANO7_UI_PAUSE, 0 },
                { NANO7_SKIN_P_RESET, NANO7_UI_RESET, 0 },
                { NANO7_SKIN_P_MUTE, NANO7_UI_MUTE, 0 },
                { NANO7_SKIN_P_ROTATE, NANO7_UI_ROTATE, 0 },
                { NANO7_SKIN_P_SHOT, NANO7_UI_SHOT, 0 },
            };
            unsigned i;

            for (i = 0; i < ARRAY_SIZE(map); i++) {
                if (map[i].id == hit) {
                    nano7_ui_command(map[i].cmd, map[i].arg);
                }
            }
            ui->capture = 3;
            ui->held_btn = hit;
            ui_render();
            return true;
        }
    } else {
        hit = nano7_skin_hit(ui->ptr_x, ui->ptr_y, &px, &py);
    }
    if (hit == NANO7_SKIN_SCREEN) {
        ui->capture = 0;
        return false;
    }
    if (hit == NANO7_SKIN_NONE) {
        ui->capture = 2;
        return true;
    }
    ui->capture = 1;
    ui->held_btn = hit;
    nano7_pmu_button(hit, true);
    ui_render();
    return true;
}

void nano7_ui_map_touch(int64_t u, int64_t v, int64_t range, int *px, int *py)
{
    if (ui && ui->skin) {
        ui_map_skin(u, v, range, px, py);
        return;
    }
    nano7_ui_map_panel(u, v, range, px, py);
}

static void nano7_ui_map_panel(int64_t u, int64_t v, int64_t range, int *px,
                               int *py)
{
    int lw = ui && (ui->rot % 180) ? NANO7_LCD_H : NANO7_LCD_W;
    int lh = ui && (ui->rot % 180) ? NANO7_LCD_W : NANO7_LCD_H;
    int hh = nano7_hud_on() ? NANO7_HUD_ROWS : 0;
    /* the console is the panel plus the HUD strip; a press on the strip
     * lands on the panel's bottom row */
    int lx = u * lw / range, ly = MIN(v * (lh + hh) / range, lh - 1);

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

/* the display window was resized (SDL, debounced by the console layer):
 * take the largest integer zoom that fits, so pixels stay square and
 * sharp; the window then snaps to that size (the renderer letterboxes at
 * an integer scale if it cannot, e.g. in full screen) */
static void ui_gfx_ui_info(void *opaque, uint32_t head, QemuUIInfo *info)
{
    int lw = (ui->rot % 180) ? NANO7_LCD_H : NANO7_LCD_W;
    int lh = (ui->rot % 180) ? NANO7_LCD_W : NANO7_LCD_H;
    int hh = nano7_hud_on() ? NANO7_HUD_ROWS : 0;
    int z;

    if (!info->width || !info->height) {
        return;
    }
    if (ui->skin) {                     /* the whole device must fit */
        int cw, ch;

        ui_skin_dims(&cw, &ch);
        lw = cw;
        lh = ch;
        hh = 0;
    }
    z = MIN(info->width / lw, info->height / (lh + hh));
    z = MAX(1, MIN(z, UI_ZOOM_MAX));
    if (z != ui->zoom) {
        ui->zoom = z;
        fprintf(stderr, "[nano7-ui] zoom %dx (window %ux%u)\n", z,
                info->width, info->height);
        ui_render();
    }
}

static const GraphicHwOps ui_gfx_ops = {
    .invalidate = ui_gfx_invalidate,
    .gfx_update = ui_gfx_update,
    .ui_info    = ui_gfx_ui_info,
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

/* -------------------------------------------------------------- actions --- */
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
        nano7_fmc_disk_rebuild();       /* picks up newly imported files */
        ui_load_state("boot");
        break;
    case UI_BOOT_SAVE:
        ui->boot_saved = ui_save_state("boot");
        break;
    case UI_SHOT:
        ui_screenshot();
        break;
    case UI_PAUSE:
        if (runstate_is_running()) {
            ui_set_paused(!ui->paused);
            fprintf(stderr, "[nano7-ui] %s\n", ui->paused ? "paused" :
                    "resumed");
        }
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

bool nano7_ui_reset(void)
{
    if (!ui || !ui->boot_saved) {
        return false;
    }
    ui_request(UI_RESET);
    return true;
}

/* first time the VM runs: snapshot the start state for F1 reset */
static void ui_vm_state(void *opaque, bool running, RunState state)
{
    if (running && !ui->boot_saved && ui->pending == UI_NONE) {
        ui->boot_saved = true;
        ui_request(UI_BOOT_SAVE);
    }
}

static void ui_set_zoom(int z)
{
    ui->zoom = z;
    fprintf(stderr, "[nano7-ui] zoom %dx\n", ui->zoom);
    ui_render();
}

/* the F-key actions (also Cmd shortcuts and menu commands) */
static void ui_fkey(int qcode)
{
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
        ui_set_zoom(ui->zoom % 3 + 1);
        break;
    case Q_KEY_CODE_F12:
        ui_request(UI_SHOT);
        break;
    case Q_KEY_CODE_F8:
        nano7_btuart_pad_mode(!nano7_btuart_pad_state());
        fprintf(stderr, "[nano7-ui] BT gamepad keys %s\n",
                nano7_btuart_pad_state() ? "on (WASD / arrows / J K U I Q E "
                "/ Enter / Backspace / Esc)" : "off");
        nano7_ui_redraw();
        break;
    case Q_KEY_CODE_F11:
        nano7_power_cycle_profile();
        nano7_ui_redraw();
        break;
    case Q_KEY_CODE_F10:
        nano7_hud_set(!nano7_hud_on());
        fprintf(stderr, "[nano7-ui] usage HUD %s\n",
                nano7_hud_on() ? "on" : "off");
        ui_render();
        break;
    case Q_KEY_CODE_F5:
    case Q_KEY_CODE_PAUSE:
        /* not here: keys from QMP send-key / VNC can arrive from a
         * QEMU_CLOCK_VIRTUAL timer, and pausing waits for that clock's
         * timers to finish (a deadlock from inside one) */
        ui_request(UI_PAUSE);
        break;
    case Q_KEY_CODE_F6:
        ui->ff_speed = ui->ff_speed >= 4 ? 1 : ui->ff_speed * 2;
        if (!ui->paused) {
            nano7_clock_set_speed(ui->ff_speed);
        }
        fprintf(stderr, "[nano7-ui] speed %dx\n", ui->ff_speed);
        break;
    default:
        break;
    }
}

/* -------------------------------------------------- front-end commands --- */
/* the macOS menu (nano7_macmenu.m) and other front ends: queued here from
 * any thread, run in the main loop */
typedef struct UiCmd {
    int cmd, arg;
} UiCmd;

static QemuMutex ui_cmd_lock;
static UiCmd ui_cmdq[32];
static unsigned ui_cmd_n;
static QEMUBH *ui_cmd_bh;
static QEMUTimer *ui_tap_timer;
static int ui_tapped = -1;              /* button pressed by a menu tap */

static void ui_tap_release(void *opaque)
{
    if (ui_tapped >= 0) {
        nano7_pmu_button(ui_tapped, false);
        ui_tapped = -1;
    }
}

static void ui_cmd_run(int cmd, int arg)
{
    switch (cmd) {
    case NANO7_UI_SAVE:    ui_fkey(Q_KEY_CODE_F2);  break;
    case NANO7_UI_LOAD:    ui_fkey(Q_KEY_CODE_F4);  break;
    case NANO7_UI_RESET:   ui_fkey(Q_KEY_CODE_F1);  break;
    case NANO7_UI_PAUSE:   ui_fkey(Q_KEY_CODE_F5);  break;
    case NANO7_UI_ROTATE:  /* arg 0: next 90 degrees, 1..4: 0 / 90 / 180 / 270 */
        if (arg >= 1 && arg <= 4) {
            ui->rot = (arg - 1) * 90;
            fprintf(stderr, "[nano7-ui] rotation %d\n", ui->rot);
            ui_render();
        } else {
            ui_fkey(Q_KEY_CODE_F7);
        }
        break;
    case NANO7_UI_PADKEYS: ui_fkey(Q_KEY_CODE_F8);  break;
    case NANO7_UI_HUD:     ui_fkey(Q_KEY_CODE_F10); break;
    case NANO7_UI_SHOT:    ui_fkey(Q_KEY_CODE_F12); break;
    case NANO7_UI_ZOOM:
        ui_set_zoom(MAX(1, MIN(arg, UI_ZOOM_MAX)));
        break;
    case NANO7_UI_SPEED:
        ui->ff_speed = arg == 2 || arg == 4 ? arg : 1;
        if (!ui->paused) {
            nano7_clock_set_speed(ui->ff_speed);
        }
        fprintf(stderr, "[nano7-ui] speed %dx\n", ui->ff_speed);
        break;
    case NANO7_UI_PROFILE:
        if (arg < 0) {
            nano7_power_cycle_profile();
        } else if (arg < NANO7_PROFILE_COUNT) {
            nano7_power_set_profile(arg);
        }
        nano7_ui_redraw();
        break;
    case NANO7_UI_FRAME:
        ui->skin = !ui->skin;
        ui->capture = 0;
        fprintf(stderr, "[nano7-ui] device frame %s\n", ui->skin ? "on" :
                "off");
        ui_render();
        break;
    case NANO7_UI_MUTE:
        nano7_i2s_set_mute(!nano7_i2s_muted());
        break;
    case NANO7_UI_IMPORT:
        /* a reset puts the new files on the disk overlay; the boot state
         * then sees them in /Apps/Data/Importar (a running OS would not:
         * its FAT and directory caches predate them) */
        fprintf(stderr, "[nano7-ui] import: %d file(s), reloading the boot "
                "state\n", arg);
        ui_fkey(Q_KEY_CODE_F1);
        break;
    case NANO7_UI_BUTTON:               /* a 100 ms tap */
        if (arg < 0 || arg >= NANO7_BTN_COUNT) {
            break;
        }
        ui_tap_release(NULL);
        ui_tapped = arg;
        nano7_pmu_button(arg, true);
        timer_mod(ui_tap_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 100);
        break;
    default:
        break;
    }
}

static void ui_cmd_bh_fn(void *opaque)
{
    UiCmd q[ARRAY_SIZE(ui_cmdq)];
    unsigned n, i;

    qemu_mutex_lock(&ui_cmd_lock);
    n = ui_cmd_n;
    memcpy(q, ui_cmdq, n * sizeof(q[0]));
    ui_cmd_n = 0;
    qemu_mutex_unlock(&ui_cmd_lock);
    for (i = 0; i < n; i++) {
        ui_cmd_run(q[i].cmd, q[i].arg);
    }
}

void nano7_ui_command(int cmd, int arg)
{
    if (!ui) {
        return;
    }
    qemu_mutex_lock(&ui_cmd_lock);
    if (ui_cmd_n < ARRAY_SIZE(ui_cmdq)) {
        ui_cmdq[ui_cmd_n++] = (UiCmd){ cmd, arg };
    }
    qemu_mutex_unlock(&ui_cmd_lock);
    qemu_bh_schedule(ui_cmd_bh);
}

int nano7_ui_state(int cmd)
{
    if (!ui) {
        return 0;
    }
    switch (cmd) {
    case NANO7_UI_PAUSE:   return ui->paused;
    case NANO7_UI_HUD:     return nano7_hud_on();
    case NANO7_UI_ZOOM:    return ui->zoom;
    case NANO7_UI_SPEED:   return ui->ff_speed;
    case NANO7_UI_PROFILE: return nano7_power_profile();
    case NANO7_UI_FRAME:   return ui->skin;
    case NANO7_UI_PADKEYS: return nano7_btuart_pad_state();
    case NANO7_UI_ROTATE:  return ui->rot;
    case NANO7_UI_MUTE:    return nano7_i2s_muted();
    default:               return 0;
    }
}


/* Cmd + key -> the F key with the same action (0: not a shortcut) */
static int ui_meta_shortcut(int qcode)
{
    switch (qcode) {
    case Q_KEY_CODE_S: return Q_KEY_CODE_F2;
    case Q_KEY_CODE_L:
    case Q_KEY_CODE_O: return Q_KEY_CODE_F4;
    case Q_KEY_CODE_R: return Q_KEY_CODE_F1;
    case Q_KEY_CODE_P: return Q_KEY_CODE_F5;
    case Q_KEY_CODE_T: return Q_KEY_CODE_F7;
    case Q_KEY_CODE_U: return Q_KEY_CODE_F10;
    case Q_KEY_CODE_K: return Q_KEY_CODE_F11;
    case Q_KEY_CODE_E: return Q_KEY_CODE_F12;
    case Q_KEY_CODE_F: return Q_KEY_CODE_F6;
    case Q_KEY_CODE_G: return Q_KEY_CODE_F8;
    default:           return 0;
    }
}

static void ui_key_event(DeviceState *dev, QemuConsole *src, InputEvent *evt)
{
    InputKeyEvent *key = evt->u.key.data;
    int qcode = qemu_input_key_value_to_qcode(key->key);

    if (qcode == Q_KEY_CODE_META_L || qcode == Q_KEY_CODE_META_R) {
        *(qcode == Q_KEY_CODE_META_L ? &ui->meta_l : &ui->meta_r) = key->down;
        return;
    }
    if (qcode < 0 || qcode >= Q_KEY_CODE__MAX) {
        return;
    }
    if (key->down) {
        /* macOS sends no release for a key let go while Cmd is down, so
         * the flag must not outlive the next real press */
        ui->swallowed[qcode] = false;
    } else if (ui->swallowed[qcode]) {
        ui->swallowed[qcode] = false;
        return;
    }
    /* Cmd shortcuts (a release still reaches a button pressed before Cmd) */
    if ((ui->meta_l || ui->meta_r) && key->down) {
        ui->swallowed[qcode] = true;
        if (qcode >= Q_KEY_CODE_1 && qcode <= Q_KEY_CODE_4) {
            ui_set_zoom(qcode - Q_KEY_CODE_1 + 1);
            return;
        }
        qcode = ui_meta_shortcut(qcode);
        if (!qcode) {
            return;
        }
    } else if (nano7_btuart_pad_key(qcode, key->down)) {    /* F8 pad mode */
        return;
    } else if (nano7_accel_key(qcode, key->down)) {     /* tilt */
        return;
    } else if (nano7_pmu_key(qcode, key->down)) {
        return;
    }
    if (qcode == Q_KEY_CODE_TAB) {      /* hold to fast-forward */
        if (ui->paused) {
            return;
        }
        nano7_clock_set_speed(key->down ? 4 : ui->ff_speed);
        fprintf(stderr, "[nano7-ui] speed %dx%s\n", nano7_clock_speed(),
                key->down ? " (Tab held)" : "");
        return;
    }
    if (!key->down) {
        return;
    }
    ui_fkey(qcode);
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
    ui->zoom = getenv("NANO7_ZOOM") ? atoi(getenv("NANO7_ZOOM")) : 1;
    ui->zoom = MAX(1, MIN(ui->zoom, UI_ZOOM_MAX));
    ui->ff_speed = 1;
    ui->skin = getenv("NANO7_FRAME") && atoi(getenv("NANO7_FRAME"));
    qemu_mutex_init(&ui_cmd_lock);
    ui_cmd_bh = qemu_bh_new(ui_cmd_bh_fn, NULL);
    ui_tap_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, ui_tap_release, NULL);
#ifdef CONFIG_DARWIN
    nano7_macmenu_init();       /* native menu bar once SDL is up */
#endif
    nano7_clock_set_speed(1);   /* registers the guest clock's vmstate */
    ui->con = graphic_console_init(NULL, 0, &ui_gfx_ops, ui);
    nano7_hud_init();           /* NANO7_HUD=1 (run.sh --hud) */
    ui_render();                /* black 240x432 until the first frame */
    ui->vmse = qemu_add_vm_change_state_handler(ui_vm_state, NULL);
    hs = qemu_input_handler_register(NULL, &ui_key_handler);
    qemu_input_handler_activate(hs);
}
