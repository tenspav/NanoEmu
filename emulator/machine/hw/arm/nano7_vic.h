/*
 * nano7_vic.h -- dual PL192-style vectored interrupt controller for the
 * iPod nano 7G machine. See nano7_vic.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NANO7_VIC_H
#define NANO7_VIC_H

#include "exec/memory.h"
#include "hw/irq.h"

#define NANO7_VIC_NUM_IRQS 64

/* Returns NANO7_VIC_NUM_IRQS input lines (0..31 bank 0, 32..63 bank 1). */
qemu_irq *nano7_vic_attach(MemoryRegion *parent, hwaddr base,
                           qemu_irq cpu_irq, qemu_irq cpu_fiq);

#endif
