/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NANO7_H264_H
#define NANO7_H264_H

#include "exec/memory.h"
#include "hw/irq.h"

/* Historical task name: deep research identifies this block as MPEG-4/H.263.
 * Actual H.264 is at 0x38f00000, IRQ35, and is not modeled here. */
#define TYPE_NANO7_H264 "nano7-h264"
#define NANO7_IRQ_H264 45

void nano7_h264_attach(MemoryRegion *parent, hwaddr base, qemu_irq irq);

#endif
