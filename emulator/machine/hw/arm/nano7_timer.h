/*
 * nano7_timer.h -- S5L8702-style 8-channel timer block for the iPod
 * nano 7G machine. See nano7_timer.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NANO7_TIMER_H
#define NANO7_TIMER_H

#include "exec/memory.h"
#include "hw/irq.h"

void nano7_timer_attach(MemoryRegion *parent, hwaddr base, qemu_irq irq,
                        uint64_t usec_base);

#endif
