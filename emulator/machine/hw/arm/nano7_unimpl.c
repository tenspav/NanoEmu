/*
 * nano7_unimpl.c -- catch-all unimplemented MMIO logger for nano7.
 *
 * Every read/write to the covered address range is logged to stderr with
 * guest PC, address, size and value. Logging is deduplicated: each unique
 * (PC, address) pair is reported only once, so polling loops do not flood
 * the log. This is the reverse-engineering workhorse: boot firmware against
 * it and let the firmware tell us which peripheral to implement next.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "nano7_unimpl.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"

#define NANO7_UNIMPL_MAX_UNIQUE 100000

typedef struct Nano7UnimplRegion {
    MemoryRegion mr;
    hwaddr base;
} Nano7UnimplRegion;

/* Shared across all regions so dedup works across ranges. */
static GHashTable *seen;        /* set of (pc << 32) | addr */
static bool capped_notice_done;

static uint32_t nano7_guest_pc(void)
{
    CPUState *cs = current_cpu;

    if (!cs) {
        return 0;
    }
    return ARM_CPU(cs)->env.regs[15];
}

static void nano7_unimpl_log(Nano7UnimplRegion *r, hwaddr offset,
                             bool is_write, uint64_t value, unsigned size)
{
    uint64_t addr = r->base + offset;
    uint32_t pc = nano7_guest_pc();
    uint64_t key = ((uint64_t)pc << 32) | (uint32_t)addr;

    if (!seen) {
        seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    }
    if (g_hash_table_contains(seen, GUINT_TO_POINTER(key))) {
        return;
    }
    if (g_hash_table_size(seen) >= NANO7_UNIMPL_MAX_UNIQUE) {
        if (!capped_notice_done) {
            capped_notice_done = true;
            fprintf(stderr, "[nano7-unimpl] %d unique accesses logged, "
                            "suppressing further output\n",
                    NANO7_UNIMPL_MAX_UNIQUE);
        }
        return;
    }
    g_hash_table_add(seen, GUINT_TO_POINTER(key));

    if (is_write) {
        fprintf(stderr, "[nano7-unimpl] pc=0x%08x W%u addr=0x%08" PRIx64
                " value=0x%0*" PRIx64 "\n",
                pc, size * 8, addr, size * 2, value);
    } else {
        fprintf(stderr, "[nano7-unimpl] pc=0x%08x R%u addr=0x%08" PRIx64
                " -> 0x0\n", pc, size * 8, addr);
    }
    fflush(stderr);
}

static uint64_t nano7_unimpl_read(void *opaque, hwaddr offset, unsigned size)
{
    nano7_unimpl_log(opaque, offset, false, 0, size);
    return 0;
}

static void nano7_unimpl_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    nano7_unimpl_log(opaque, offset, true, value, size);
}

static const MemoryRegionOps nano7_unimpl_ops = {
    .read = nano7_unimpl_read,
    .write = nano7_unimpl_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

void nano7_unimpl_attach(MemoryRegion *parent, hwaddr base, uint64_t size,
                         int priority)
{
    Nano7UnimplRegion *r = g_new0(Nano7UnimplRegion, 1);
    char *name = g_strdup_printf("nano7.unimpl@%08" PRIx64, (uint64_t)base);

    r->base = base;
    memory_region_init_io(&r->mr, NULL, &nano7_unimpl_ops, r, name, size);
    g_free(name);
    memory_region_add_subregion_overlap(parent, base, &r->mr, priority);
}
