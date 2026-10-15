/*
 * nano7_spicmd.h -- 0x3de00000 serial register-command engine (IRQ 52)
 * for the iPod nano 7G machine. See nano7_spicmd.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NANO7_SPICMD_H
#define NANO7_SPICMD_H

#include "exec/memory.h"
#include "hw/irq.h"

#define NANO7_IRQ_SPICMD 52

void nano7_spicmd_attach(MemoryRegion *parent, hwaddr base, qemu_irq irq);

#endif
