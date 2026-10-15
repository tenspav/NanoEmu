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
/* re-render the console (the usage HUD changed) */
void nano7_ui_redraw(void);
/* reload the boot state (F1); false if it was not saved yet */
bool nano7_ui_reset(void);
/* absolute pointer coordinates (0..range-1 over the displayed console) ->
 * panel pixel, undoing rotation and zoom */
void nano7_ui_map_touch(int64_t u, int64_t v, int64_t range, int *px,
                        int *py);

#endif
