/*
 * nano7_ui.h -- emulator front-end controls for the iPod nano 7G machine:
 * hot keys (save/load state, reset, ...) and the button keys. See
 * nano7_ui.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NANO7_UI_H
#define NANO7_UI_H

#include "exec/memory.h"

/* state_dir: where save-state slots live; screens_dir: F12 screenshots */
void nano7_ui_init(const char *state_dir, const char *screens_dir);
/* a RAM region to include in save states (name = file name) */
void nano7_ui_add_ram(MemoryRegion *mr, const char *name);
/* the panel frame changed (240x432 XRGB, stays valid) */
void nano7_ui_frame(const uint32_t *frame);
/* the panel's power (PMU reg 0x10: 0 while the iPod sleeps): off = the
 * screen is drawn black */
void nano7_ui_set_display(bool on);
/* re-render the console (the usage HUD changed) */
void nano7_ui_redraw(void);
/* reload the boot state (F1); false if it was not saved yet */
bool nano7_ui_reset(void);
/* absolute pointer coordinates (0..range-1 over the displayed console) ->
 * panel pixel, undoing rotation and zoom */
void nano7_ui_map_touch(int64_t u, int64_t v, int64_t range, int *px,
                        int *py);
/* the left mouse button changed at the last mapped position: true if the
 * front end takes it (a button drawn on the device frame, or a press that
 * began off the screen), false = a touch */
bool nano7_ui_pointer_button(bool down);

/* front-end commands (the macOS menu): queued from any thread, run in the
 * main loop. ZOOM arg = pixel zoom, SPEED arg = 1/2/4, PROFILE arg =
 * NANO7_PROFILE_* or -1 = next, BUTTON arg = NANO7_BTN_* (a 100 ms tap) */
enum {
    NANO7_UI_SAVE, NANO7_UI_LOAD, NANO7_UI_RESET, NANO7_UI_PAUSE,
    NANO7_UI_ROTATE, NANO7_UI_PADKEYS, NANO7_UI_HUD, NANO7_UI_SHOT,
    NANO7_UI_ZOOM, NANO7_UI_SPEED, NANO7_UI_PROFILE, NANO7_UI_FRAME,
    NANO7_UI_BUTTON, NANO7_UI_MUTE,
    NANO7_UI_IMPORT,    /* files were added to the import folder: put them
                           on the disk and reload the boot state */
};
void nano7_ui_command(int cmd, int arg);
/* current value for a menu check mark (paused, HUD on, zoom, speed,
 * profile (-1 = the guest's own clocks), frame, gamepad keys, rotation) */
int nano7_ui_state(int cmd);

#endif
