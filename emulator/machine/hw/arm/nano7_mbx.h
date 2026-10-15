/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NANO7_MBX_H
#define NANO7_MBX_H

#include "exec/memory.h"
#include "hw/irq.h"

#define TYPE_NANO7_MBX "nano7-mbx"
#define NANO7_IRQ_MBX 53

/* Maps all seven apertures at their fixed SoC addresses. */
void nano7_mbx_attach(MemoryRegion *parent, qemu_irq irq);

/* the renderer's cumulative workload counters (zeros before attach) */
struct MbxrStats;
void nano7_mbx_stats(struct MbxrStats *st);

#endif
