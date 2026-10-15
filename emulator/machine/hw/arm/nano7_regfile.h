/*
 * nano7_regfile.h -- generic "register file" peripheral stub for the
 * iPod nano 7G machine. See nano7_regfile.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NANO7_REGFILE_H
#define NANO7_REGFILE_H

#include "exec/memory.h"

typedef struct Nano7RegfileOpts {
    uint8_t fill;        /* value returned for never-written bytes */
    uint64_t counter_mask; /* bitmask of 32-bit word indices (offset/4)
                              that read as a live free-running counter */
    uint64_t ones_mask;    /* bitmask of word indices that always read
                              all-ones (e.g. status "ready" registers) */
    uint64_t done_mask;    /* word idx -> always read 0x80000000
                              ("busy bit cleared, done bit set") */
    int64_t preset_off;    /* word idx (or -1) preset at reset... */
    uint32_t preset_val;   /* ...to this value (e.g. PMGR warm-boot bit) */
    /* extra presets (word idx / value); idx -1 ends the list */
    int64_t preset2_off;
    uint32_t preset2_val;
    int64_t preset3_off;
    uint32_t preset3_val;
    /* power-gate controller (0x39700000): +0x10 = gate domains off,
     * +0x0c = wake; status +0x14 (bit n off, bit n+10 settled) and the
     * request shadow +0x08 follow at once */
    bool pgate;
} Nano7RegfileOpts;

void nano7_regfile_attach2(MemoryRegion *parent, const char *name,
                           hwaddr base, uint64_t size,
                           const Nano7RegfileOpts *opts);
void nano7_regfile_attach(MemoryRegion *parent, const char *name,
                          hwaddr base, uint64_t size);

/* emulator-side access to a named register file (e.g. "clkcon"): peek =
 * the 32-bit word at off; returns which of its bytes were written (bit n =
 * byte n; by the guest or a poke; the others hold the fill), 0 also for an
 * unknown name; poke = write the bytes selected by bytemask as if the guest
 * had */
unsigned nano7_regfile_peek(const char *name, hwaddr off, uint32_t *val);
void nano7_regfile_poke(const char *name, hwaddr off, uint32_t val,
                        unsigned bytemask);

#endif
