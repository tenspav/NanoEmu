/*
 * nano7_dma.h -- the two 8-channel DMA engines of the iPod nano 7G
 * (S5L8747). See nano7_dma.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NANO7_DMA_H
#define NANO7_DMA_H

#include "exec/memory.h"
#include "hw/irq.h"

#define NANO7_IRQ_DMA1  16      /* engine 1, 0x38200000 */
#define NANO7_IRQ_DMA2  17      /* engine 2, 0x38700000 */

void nano7_dma_attach(MemoryRegion *parent, hwaddr base, int engine,
                      qemu_irq irq);
/* byte rate of a paced peripheral FIFO (0 = not paced) */
void nano7_dma_set_fifo_rate(uint32_t fifo_addr, uint32_t bytes_per_sec);

#endif
