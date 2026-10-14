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

#endif
