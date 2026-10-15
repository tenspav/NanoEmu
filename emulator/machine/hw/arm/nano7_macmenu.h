/*
 * nano7_macmenu.h -- native macOS menu bar for the desktop front end. See
 * nano7_macmenu.m (built on macOS only).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NANO7_MACMENU_H
#define NANO7_MACMENU_H

/* install the File / Emulation / View / Device menus once SDL has created
 * the application's menu bar (polled from a QEMU timer) */
void nano7_macmenu_init(void);

#endif
