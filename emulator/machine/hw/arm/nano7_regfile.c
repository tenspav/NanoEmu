/*
 * nano7_regfile.c -- generic register-file stub for the iPod nano 7G.
 *
 * Strategy: a flat register file. Writes store, reads return the stored
 * value. Registers never written return all-ones: on this SoC most CLKCON
 * registers are "gate" registers where 1-bits mean "clock enabled / block
 * ready", so all-ones satisfies the common "wait until enabled" poll idiom
 * while write-store/read-back satisfies "wait for the bit I just set".
 *
 * Accesses are logged (deduped per (PC, offset)) so we can see exactly
 * which registers the firmware cares about.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "nano7_regfile.h"
#include "hw/core/cpu.h"
#include "qemu/timer.h"
#include "target/arm/cpu.h"
#include "migration/vmstate.h"

typedef struct Nano7RegfileState {
    MemoryRegion mr;
    hwaddr base;
    char *name;
    uint64_t counter_mask;  /* word idx -> live free-running counter */
    uint64_t ones_mask;     /* word idx -> always read all-ones */
    uint64_t done_mask;     /* word idx -> always read 0x80000000 */
    int64_t preset_off;     /* word idx preset with preset_val, -1 = none */
    uint32_t preset_val;
    uint8_t fill;           /* value returned for never-written bytes */
    bool pgate;             /* power-gate request/status semantics */
    uint8_t *regs;          /* flat store, accessed LE */
    GHashTable *seen;       /* dedup of (pc << 32) | offset */
    uint8_t *written;       /* per byte: ever written (read-back vs fill) */
    uint32_t size;
} Nano7RegfileState;

static uint32_t nano7_guest_pc(void)
{
    CPUState *cs = current_cpu;

    if (!cs) {
        return 0;
    }
    return ARM_CPU(cs)->env.regs[15];
}

static void nano7_regfile_log(Nano7RegfileState *s, hwaddr offset,
                             bool is_write, uint64_t value, unsigned size)
{
    uint32_t pc = nano7_guest_pc();
    uint64_t key = ((uint64_t)pc << 32) | (uint32_t)offset;

    if (g_hash_table_contains(s->seen, GUINT_TO_POINTER(key))) {
        return;
    }
    g_hash_table_add(s->seen, GUINT_TO_POINTER(key));
    if (is_write) {
        fprintf(stderr, "[nano7-%s] pc=0x%08x W%u addr=0x%08" PRIx64
                " value=0x%0*" PRIx64 "\n", s->name, pc, size * 8,
                s->base + offset, size * 2, value);
    } else {
        fprintf(stderr, "[nano7-%s] pc=0x%08x R%u addr=0x%08" PRIx64 "\n",
                s->name, pc, size * 8, s->base + offset);
    }
    fflush(stderr);
}

static uint64_t nano7_regfile_read(void *opaque, hwaddr offset, unsigned size)
{
    Nano7RegfileState *s = opaque;
    uint64_t ret = 0;
    unsigned i;

    nano7_regfile_log(s, offset, false, 0, size);
    if (size == 4 && (offset & 3) == 0) {
        uint32_t idx = offset >> 2;

        if (s->counter_mask & (1ULL << idx)) {
            /* free-running counter (~16 MHz from virtual clock) */
            return (uint32_t)(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) >> 6);
        }
        if (s->ones_mask & (1ULL << idx)) {
            return 0xffffffff;
        }
        if (s->done_mask & (1ULL << idx)) {
            return 0x80000000;
        }
    }
    for (i = 0; i < size; i++) {
        uint8_t b;
        if (s->written[offset + i]) {
            b = s->regs[offset + i];
        } else {
            b = s->fill;/* never written: "enabled/ready" or 0 */
        }
        ret |= (uint64_t)b << (8 * i);
    }
    return ret;
}

static void nano7_regfile_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    Nano7RegfileState *s = opaque;
    unsigned i;

    nano7_regfile_log(s, offset, true, value, size);
    if (s->pgate && size == 4 && (offset == 0x0c || offset == 0x10)) {
        uint32_t m = value, st = ldl_le_p(s->regs + 0x14);
        uint32_t sh = ldl_le_p(s->regs + 0x08);

        if (offset == 0x10) {           /* gate off */
            st |= m | (m << 10);
            sh |= m;
        } else {                        /* wake */
            st &= ~(m | (m << 10));
            sh &= ~m;
        }
        stl_le_p(s->regs + 0x14, st);
        stl_le_p(s->regs + 0x08, sh);
        for (i = 0; i < 4; i++) {
            s->written[0x14 + i] = 1;
            s->written[0x08 + i] = 1;
        }
        return;
    }
    for (i = 0; i < size; i++) {
        s->regs[offset + i] = (value >> (8 * i)) & 0xff;
        s->written[offset + i] = 1;
    }
}

static const MemoryRegionOps nano7_regfile_ops = {
    .read = nano7_regfile_read,
    .write = nano7_regfile_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static const VMStateDescription vmstate_regfile = {
    .name = "nano7-regfile",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(size, Nano7RegfileState),
        VMSTATE_VBUFFER_UINT32(regs, Nano7RegfileState, 1, NULL, size),
        VMSTATE_VBUFFER_UINT32(written, Nano7RegfileState, 1, NULL, size),
        VMSTATE_END_OF_LIST()
    }
};

static GSList *regfiles;        /* every attached register file */

static Nano7RegfileState *regfile_find(const char *name)
{
    GSList *l;

    for (l = regfiles; l; l = l->next) {
        Nano7RegfileState *s = l->data;

        if (!strcmp(s->name, name)) {
            return s;
        }
    }
    return NULL;
}

unsigned nano7_regfile_peek(const char *name, hwaddr off, uint32_t *val)
{
    Nano7RegfileState *s = regfile_find(name);
    unsigned mask = 0;
    int i;

    if (!s || off + 4 > s->size) {
        return 0;
    }
    for (i = 0; i < 4; i++) {
        mask |= s->written[off + i] ? 1u << i : 0;
    }
    *val = ldl_le_p(s->regs + off);
    return mask;
}

void nano7_regfile_poke(const char *name, hwaddr off, uint32_t val,
                        unsigned bytemask)
{
    Nano7RegfileState *s = regfile_find(name);
    int i;

    if (!s || off + 4 > s->size) {
        return;
    }
    for (i = 0; i < 4; i++) {
        if (bytemask & (1u << i)) {
            s->regs[off + i] = val >> (8 * i);
            s->written[off + i] = 1;
        }
    }
}

void nano7_regfile_attach2(MemoryRegion *parent, const char *name,
                           hwaddr base, uint64_t size,
                           const Nano7RegfileOpts *opts)
{
    Nano7RegfileState *s = g_new0(Nano7RegfileState, 1);
    char *mrname = g_strdup_printf("nano7.%s", name);

    regfiles = g_slist_prepend(regfiles, s);

    s->base = base;
    s->name = g_strdup(name);
    s->counter_mask = opts->counter_mask;
    s->ones_mask = opts->ones_mask;
    s->done_mask = opts->done_mask;
    s->preset_off = opts->preset_off;
    s->preset_val = opts->preset_val;
    s->fill = opts->fill;
    s->pgate = opts->pgate;
    s->regs = g_malloc0(size);
    s->seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    s->written = g_malloc0(size);
    s->size = size;
    vmstate_register_any(NULL, &vmstate_regfile, s);
    if (opts->preset_off >= 0) {
        hwaddr off = opts->preset_off * 4;
        if (off + 4 <= size) {
            int i;
            for (i = 0; i < 4; i++) {
                s->regs[off + i] = (opts->preset_val >> (8 * i)) & 0xff;
                s->written[off + i] = 1;
            }
        }
    }
    const int64_t extra_offs[2] = { opts->preset2_off, opts->preset3_off };
    const uint32_t extra_vals[2] = { opts->preset2_val, opts->preset3_val };
    for (int p = 0; p < 2; p++) {
        if (extra_offs[p] >= 0) {
            hwaddr off = extra_offs[p] * 4;
            if (off + 4 <= size) {
                for (int i = 0; i < 4; i++) {
                    s->regs[off + i] = (extra_vals[p] >> (8 * i)) & 0xff;
                    s->written[off + i] = 1;
                }
            }
        }
    }
    memory_region_init_io(&s->mr, NULL, &nano7_regfile_ops, s, mrname, size);
    g_free(mrname);
    memory_region_add_subregion_overlap(parent, base, &s->mr, 0);
}

void nano7_regfile_attach(MemoryRegion *parent, const char *name,
                          hwaddr base, uint64_t size)
{
    Nano7RegfileOpts opts = { .fill = 0xff };

    nano7_regfile_attach2(parent, name, base, size, &opts);
}
