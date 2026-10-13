/*
 * nano7_torn.c -- torn-block read watcher for DumpRAM v2 snapshots.
 *
 * torn-map.bin (LE u32): 'TORN', 2, block_bytes, n_blocks, then n_blocks x
 * {h0, hb, ha}; a block is torn if h0 != hb (changed between the ctx capture
 * and its write) or hb != ha (changed while being written).
 *
 * Each torn block is overlaid (priority 1, above the DRAM region) with an I/O
 * region that forwards to the same DRAM backing store and keeps a per-word
 * "written since resume" bitmap. A read of a word the guest has not written
 * yet returns snapshot data that may not match the ctx: a STALE read. The
 * first few stale reads per block are logged with the guest pc, and a summary
 * line is printed every 10 s of host time.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "qemu/bitmap.h"
#include "nano7_torn.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"


typedef struct TornBlock {
    MemoryRegion mr;
    hwaddr base;
    uint8_t *host;
    uint32_t bytes;
    unsigned long *fresh;      /* one bit per 32-bit word */
    uint64_t stale_reads, reads, writes;
    uint32_t stale_words;      /* distinct stale words read */
    unsigned long *seen;       /* stale words already counted */
    unsigned logged;
    char tag[3];
} TornBlock;

static TornBlock *torn_blocks;
static unsigned torn_n;
static QEMUTimer *torn_timer;
static uint32_t torn_ign_lo, torn_ign_hi;   /* reads from this pc range ignored */
static unsigned torn_log_max = 4;          /* env NANO7_TORN_LOG overrides */
#define TORN_EXCL_MAX 1024
static hwaddr torn_excl_base[TORN_EXCL_MAX];
static uint32_t torn_excl_bytes[TORN_EXCL_MAX];
static unsigned torn_excl_n;

void nano7_torn_exclude(hwaddr base, uint32_t bytes)
{
    if (torn_excl_n < TORN_EXCL_MAX) {
        torn_excl_base[torn_excl_n] = base;
        torn_excl_bytes[torn_excl_n++] = bytes;
    }
}

static bool torn_excluded(hwaddr base, uint32_t bytes)
{
    for (unsigned i = 0; i < torn_excl_n; i++) {
        if (base >= torn_excl_base[i] &&
            base + bytes <= torn_excl_base[i] + torn_excl_bytes[i]) {
            return true;
        }
    }
    return false;
}

static uint32_t torn_pc(void)
{
    return current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0;
}

static TornBlock *torn_find(hwaddr addr)
{
    for (unsigned i = 0; i < torn_n; i++) {
        if (addr >= torn_blocks[i].base &&
            addr < torn_blocks[i].base + torn_blocks[i].bytes) {
            return &torn_blocks[i];
        }
    }
    return NULL;
}

static uint64_t torn_read(void *opaque, hwaddr off, unsigned size)
{
    TornBlock *b = opaque;
    uint64_t v = ldn_le_p(b->host + off, size);
    unsigned w = off >> 2;

    uint32_t pc = torn_pc();

    if (pc >= torn_ign_lo && pc < torn_ign_hi) {
        return v;
    }
    b->reads++;
    if (!test_bit(w, b->fresh)) {
        b->stale_reads++;
        if (!test_and_set_bit(w, b->seen)) {
            b->stale_words++;
            if (b->logged < torn_log_max) {
                b->logged++;
                fprintf(stderr, "[nano7-torn] STALE read pc=0x%08x addr=0x%08x"
                        " size=%u val=0x%08" PRIx64 " (block 0x%08x %s)\n",
                        pc, (uint32_t)(b->base + off), size, v,
                        (uint32_t)b->base, b->tag);
            }
        }
    }
    return v;
}

static void torn_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    TornBlock *b = opaque;

    b->writes++;
    stn_le_p(b->host + off, size, v);
    /* only a full aligned word makes the word fully fresh */
    if (size == 4 && !(off & 3)) {
        set_bit(off >> 2, b->fresh);
    } else if (size == 8 && !(off & 3)) {
        set_bit(off >> 2, b->fresh);
        set_bit((off >> 2) + 1, b->fresh);
    }
}

static const MemoryRegionOps torn_ops = {
    .read = torn_read,
    .write = torn_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static void torn_summary(void *opaque)
{
    uint64_t sr = 0, r = 0;
    unsigned hit = 0;

    for (unsigned i = 0; i < torn_n; i++) {
        sr += torn_blocks[i].stale_reads;
        r += torn_blocks[i].reads;
        hit += torn_blocks[i].stale_words != 0;
    }
    fprintf(stderr, "[nano7-torn] summary: %u/%u torn blocks hit by stale reads,"
            " %" PRIu64 " stale / %" PRIu64 " reads\n", hit, torn_n, sr, r);
    for (unsigned i = 0; i < torn_n; i++) {
        TornBlock *b = &torn_blocks[i];
        if (b->stale_words) {
            fprintf(stderr, "[nano7-torn]   0x%08x %-2s stale_words=%u "
                    "stale_reads=%" PRIu64 " reads=%" PRIu64 " writes=%" PRIu64
                    "\n", (uint32_t)b->base, b->tag, b->stale_words,
                    b->stale_reads, b->reads, b->writes);
        }
    }
    timer_mod(torn_timer, qemu_clock_get_ms(QEMU_CLOCK_HOST) + 10000);
}

void nano7_torn_mark(hwaddr addr)
{
    TornBlock *b = torn_find(addr);

    if (b) {
        set_bit((addr - b->base) >> 2, b->fresh);
    }
}

void nano7_torn_set_ignore_pc(uint32_t lo, uint32_t hi)
{
    torn_ign_lo = lo;
    torn_ign_hi = hi;
}

void nano7_torn_attach(MemoryRegion *sysmem, uint8_t *dram, hwaddr base,
                       const char *path)
{
    if (getenv("NANO7_TORN_LOG")) {
        torn_log_max = atoi(getenv("NANO7_TORN_LOG"));
    }
    gchar *data = NULL;
    gsize len = 0;
    uint32_t bs, n, excluded = 0;
    const uint8_t *p;

    if (!g_file_get_contents(path, &data, &len, NULL) || len < 16 ||
        ldl_le_p(data) != 0x4e524f54u || ldl_le_p(data + 4) != 2) {
        error_report("nano7: bad torn-map '%s'", path);
        exit(1);
    }
    bs = ldl_le_p(data + 8);
    n = ldl_le_p(data + 12);
    if (len < 16 + 12ull * n || bs == 0 || (uint64_t)bs * n > 64 * MiB) {
        error_report("nano7: torn-map '%s' truncated", path);
        exit(1);
    }
    torn_blocks = g_new0(TornBlock, n);
    p = (const uint8_t *)data + 16;
    for (uint32_t i = 0; i < n; i++, p += 12) {
        uint32_t h0 = ldl_le_p(p), hb = ldl_le_p(p + 4), ha = ldl_le_p(p + 8);
        TornBlock *b;

        if (h0 == hb && hb == ha) {
            continue;
        }
        if (torn_excluded(base + (hwaddr)i * bs, bs)) {
            excluded++;     /* replaced by the T0 overlay: not stale */
            continue;
        }
        b = &torn_blocks[torn_n++];
        b->base = base + (hwaddr)i * bs;
        b->host = dram + (size_t)i * bs;
        b->bytes = bs;
        b->fresh = bitmap_new(bs / 4);
        b->seen = bitmap_new(bs / 4);
        snprintf(b->tag, sizeof(b->tag), "%s%s", h0 != hb ? "P" : "",
                 hb != ha ? "D" : "");
        memory_region_init_io(&b->mr, NULL, &torn_ops, b, "nano7.torn", bs);
        memory_region_add_subregion_overlap(sysmem, b->base, &b->mr, 1);
    }
    g_free(data);
    info_report("nano7: torn-map '%s': %u/%u blocks of %u KB watched"
                " (%u torn blocks covered by the overlay)",
                path, torn_n, n, bs / 1024, excluded);
    torn_timer = timer_new_ms(QEMU_CLOCK_HOST, torn_summary, NULL);
    timer_mod(torn_timer, qemu_clock_get_ms(QEMU_CLOCK_HOST) + 10000);
}
