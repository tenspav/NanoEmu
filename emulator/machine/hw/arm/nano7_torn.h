/*
 * nano7_torn.h -- watch guest reads of DRAM blocks a DumpRAM v2 torn-map.bin
 * marks unreliable. See nano7_torn.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NANO7_TORN_H
#define NANO7_TORN_H

#include "exec/memory.h"

/* Overlay every torn block of `dram` (host ptr, guest base) with a watcher. */
void nano7_torn_attach(MemoryRegion *sysmem, uint8_t *dram, hwaddr base,
                       const char *path);
/* Before attach: blocks inside [base, base+bytes) are not watched (e.g. they
 * were replaced by a DumpRAM v4 overlay captured atomically at T0). */
void nano7_torn_exclude(hwaddr base, uint32_t bytes);
/* Mark a word as fresh (e.g. an os-poke store), so reads of it don't count. */
void nano7_torn_mark(hwaddr addr);
/* Ignore reads issued from guest pc in [lo, hi) (e.g. the dump app itself). */
void nano7_torn_set_ignore_pc(uint32_t lo, uint32_t hi);

#endif
