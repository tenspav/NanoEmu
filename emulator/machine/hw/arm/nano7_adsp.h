/*
 * nano7_adsp.h -- the media decode co-processor (0x38500000, IRQ 18) of
 * the iPod nano 7G machine. See nano7_adsp.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NANO7_ADSP_H
#define NANO7_ADSP_H

#include "exec/memory.h"
#include "hw/irq.h"

#define NANO7_IRQ_ADSP  18

void nano7_adsp_attach(MemoryRegion *parent, hwaddr base, qemu_irq irq);

#endif
