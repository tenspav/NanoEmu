/*
 * nano7_touch.h -- multitouch controller on serial port 2 (0x3d200000,
 * IRQ 11) fed by the host mouse, for the iPod nano 7G machine. See
 * nano7_touch.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NANO7_TOUCH_H
#define NANO7_TOUCH_H

#include "exec/memory.h"
#include "hw/irq.h"

#define NANO7_IRQ_TOUCH_SPI 11

/* needs nano7_gpio_attach() first (report-ready line = GPIO 0x26) */
void nano7_touch_attach(MemoryRegion *parent, hwaddr base, qemu_irq irq);

#endif
