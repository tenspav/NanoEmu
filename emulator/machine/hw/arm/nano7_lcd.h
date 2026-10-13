/*
 * nano7_lcd.h -- 0x38900000 LCD controller (2 scan-out windows) + panel TE
 * pulse for the iPod nano 7G machine. See nano7_lcd.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NANO7_LCD_H
#define NANO7_LCD_H

#include "exec/memory.h"

#define NANO7_LCD_W     240
#define NANO7_LCD_H     432

/* dump_dir: write each changed scanned-out frame as a PPM there (NULL =
 * off). Needs nano7_gpio_attach() first (TE is GPIO 14). */
void nano7_lcd_attach(MemoryRegion *parent, hwaddr base, const char *dump_dir);

#endif
