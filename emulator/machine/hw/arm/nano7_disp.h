/*
 * nano7_disp.h -- 0x39000000 display pipe (frame done on IRQ 37) for the
 * iPod nano 7G machine. See nano7_disp.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NANO7_DISP_H
#define NANO7_DISP_H

#include "exec/memory.h"
#include "hw/irq.h"

#define NANO7_IRQ_DISP 37

/* dump_dir: write changed layer framebuffers as PPMs there (NULL = off) */
void nano7_disp_attach(MemoryRegion *parent, hwaddr base, qemu_irq irq,
                       const char *dump_dir);

#endif
