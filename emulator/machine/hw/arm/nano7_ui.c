/*
 * nano7_ui.c -- emulator front-end controls for the iPod nano 7G machine.
 *
 * One QEMU keyboard handler (VNC, a display window, or the monitor's
 * sendkey) for everything:
 *   h / p / space            Home / Power / Play-Pause   (nano7_pmu.c)
 *   Up = + / Down -          volume up / down
 *   F1  reset                F2  save state      F4  load state
 * Actions run from a bottom half in the main loop, not inside the input
 * dispatch.
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

#define UI_MAX_RAM      8

typedef enum UiAction {
    UI_NONE, UI_SAVE, UI_LOAD, UI_RESET, UI_BOOT_SAVE,
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
    ui->vmse = qemu_add_vm_change_state_handler(ui_vm_state, NULL);
    hs = qemu_input_handler_register(NULL, &ui_key_handler);
    qemu_input_handler_activate(hs);
}
