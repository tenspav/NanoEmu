/*
 * nano7_unimpl.h -- catch-all unimplemented MMIO logger for the iPod nano 7G
 * (S5L8747-class) machine. See nano7_unimpl.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NANO7_UNIMPL_H
#define NANO7_UNIMPL_H

#include "exec/memory.h"

/*
 * Map a low-priority I/O region that accepts every access and logs each
 * unique (guest PC, address) pair once to stderr. Higher-priority regions
 * (RAM/ROM/real devices) registered on top of it take precedence.
 */
void nano7_unimpl_attach(MemoryRegion *parent, hwaddr base, uint64_t size,
                         int priority);

#endif
