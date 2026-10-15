/*
 * nano7_codec.h -- serial port 0 + audio codec for the iPod nano 7G
 * machine. See nano7_codec.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NANO7_CODEC_H
#define NANO7_CODEC_H

#include "exec/memory.h"
#include "hw/irq.h"

#define NANO7_IRQ_SPI0  9

void nano7_codec_attach(MemoryRegion *parent, hwaddr base, qemu_irq irq);

#endif
