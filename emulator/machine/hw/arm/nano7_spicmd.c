/*
 * nano7_spicmd.c -- 0x3de00000 serial register-command engine for the
 * iPod nano 7G (S5L8747). Clock gate 0x35, interrupt VIC IRQ 52.
 *
 * Register use, as RetailOS drives it (Session 8 RE):
 *   +0x00  config        init writes 0x1703 (0x800ad94)
 *   +0x04  enable        init writes 1
 *   +0x0c  irq status    IRQ 52 handler 0x8000c9c reads it, writes it back
 *                        (W1C), then signals event 0x3c
 *   +0x10  irq mask      init clears bits 0..1 (= unmask both channels)
 *   +0x14  ch0 control   bit0 = go/busy, bit1 = 16-bit frame (else 8-bit)
 *   +0x18  ch0 tx data   16-bit frame [W|addr7|data8] or (addr7 << 8)
 *   +0x1c  ch1 control   same layout, fire-and-forget path (0x85508b4)
 *   +0x20  ch1 tx data
 *   +0x24  misc/divider  written by 0x80acc50
 * ch0 path 0x855082c: go, then svc 0xa wait(event 0x3c, timeout 0x20);
 * without the IRQ the wait times out and the write returns error 0x1f.
 *
 * Model: a "go" completes after a short delay: go bit clears, status bit
 * (1 << channel) is set and IRQ 52 is raised while (status & ~mask).
 * Accesses are logged (deduped per PC/offset).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "nano7_spicmd.h"
#include "hw/core/cpu.h"
#include "qemu/timer.h"
#include "target/arm/cpu.h"

#define SPC_SIZE        0x1000
#define SPC_XFER_NS     20000       /* 16 bits at ~1 MHz */
#define SPC_LOG_MAX     400

typedef struct Nano7SpicmdState {
    MemoryRegion mr;
    hwaddr base;
    uint32_t regs[SPC_SIZE / 4];
    uint32_t status;
    QEMUTimer *timer[2];
    qemu_irq irq;
    unsigned log_lines;
    uint64_t xfers;
    GHashTable *seen;
} Nano7SpicmdState;

static uint32_t spc_guest_pc(void)
{
    return current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0;
}

static void spc_log(Nano7SpicmdState *s, hwaddr offset, bool is_write,
                    uint32_t value)
{
    uint32_t pc = spc_guest_pc();
    uint64_t key = ((uint64_t)pc << 32) | ((uint32_t)offset << 1) | is_write;

    if (s->log_lines >= SPC_LOG_MAX ||
        g_hash_table_contains(s->seen, (gpointer)(uintptr_t)key)) {
        return;
    }
    g_hash_table_add(s->seen, (gpointer)(uintptr_t)key);
    s->log_lines++;
    if (is_write) {
        fprintf(stderr, "[nano7-spc] pc=0x%08x W32 addr=0x%08" PRIx64
                " value=0x%08x\n", pc, s->base + offset, value);
    } else {
        fprintf(stderr, "[nano7-spc] pc=0x%08x R32 addr=0x%08" PRIx64 "\n",
                pc, s->base + offset);
    }
    fflush(stderr);
}

static void spc_update(Nano7SpicmdState *s)
{
    qemu_set_irq(s->irq, (s->status & ~s->regs[0x10 >> 2] & 3) != 0);
}

static void spc_done(Nano7SpicmdState *s, int ch)
{
    hwaddr ctl = ch ? 0x1c : 0x14;

    s->regs[ctl >> 2] &= ~1u;
    s->status |= 1u << ch;
    if (s->xfers++ < 64) {
        fprintf(stderr, "[nano7-spc] ch%d frame 0x%04x done -> IRQ %d\n",
                ch, s->regs[(ctl + 4) >> 2] & 0xffff, NANO7_IRQ_SPICMD);
        fflush(stderr);
    }
    spc_update(s);
}

static void spc_timer0(void *opaque)
{
    spc_done(opaque, 0);
}

static void spc_timer1(void *opaque)
{
    spc_done(opaque, 1);
}

static uint64_t spc_read(void *opaque, hwaddr offset, unsigned size)
{
    Nano7SpicmdState *s = opaque;

    spc_log(s, offset, false, 0);
    if (offset == 0x0c) {
        return s->status;
    }
    return s->regs[offset >> 2];
}

static void spc_write(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    Nano7SpicmdState *s = opaque;
    uint32_t v = value;

    spc_log(s, offset, true, v);
    switch (offset) {
    case 0x0c:
        s->status &= ~v;
        break;
    case 0x14:
    case 0x1c: {
        int ch = offset == 0x1c;

        s->regs[offset >> 2] = v;
        if (v & 1) {
            timer_mod_ns(s->timer[ch],
                         qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                         SPC_XFER_NS);
        }
        break;
    }
    default:
        s->regs[offset >> 2] = v;
        break;
    }
    spc_update(s);
}

static const MemoryRegionOps spc_ops = {
    .read = spc_read,
    .write = spc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

void nano7_spicmd_attach(MemoryRegion *parent, hwaddr base, qemu_irq irq)
{
    Nano7SpicmdState *s = g_new0(Nano7SpicmdState, 1);

    s->base = base;
    s->irq = irq;
    s->seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    s->timer[0] = timer_new_ns(QEMU_CLOCK_VIRTUAL, spc_timer0, s);
    s->timer[1] = timer_new_ns(QEMU_CLOCK_VIRTUAL, spc_timer1, s);
    memory_region_init_io(&s->mr, NULL, &spc_ops, s, "nano7.spicmd",
                          SPC_SIZE);
    memory_region_add_subregion_overlap(parent, base, &s->mr, 0);
}
