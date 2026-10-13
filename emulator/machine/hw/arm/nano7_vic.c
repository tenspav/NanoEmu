/*
 * nano7_vic.c -- dual PL192-style vectored interrupt controller for the
 * iPod nano 7G machine (0x38e00000).
 *
 * Layout, as used by RetailOS (init 0x8006e94, dispatch 0x800d324):
 *   0x38e00000  bank 0, IRQs 0..31   (PL192 register layout)
 *   0x38e01000  bank 1, IRQs 32..63
 *   0x38e02000  glue: +0x08/+0x0c per-bank edge-clear (1<<n written after
 *               every handler; +0x08/+0x0c = 0xffffffff at init)
 *
 * Per-bank registers modelled:
 *   +0x000 IRQSTATUS  +0x004 FIQSTATUS  +0x008 RAWINTR  +0x00c INTSELECT
 *   +0x010 INTENABLE (W1S)  +0x014 INTENCLEAR (W1C)
 *   +0x018 SOFTINT (W1S)    +0x01c SOFTINTCLEAR (W1C)
 *   +0x020 PROTECTION  +0x024 SWPRIORITYMASK  +0x028 PRIORITYDAISY
 *   +0x100..0x17c VECTADDR[32]   (OS writes the IRQ number: 0..63)
 *   +0x200..0x27c VECTPRIORITY[32]
 *   +0xf00 VICADDRESS: read = VECTADDR of the highest-priority pending
 *          IRQ (lowest priority value, then lowest number); write = EOI
 *          (OS writes 0xdeadbeef at init, 0x01010101 after each IRQ)
 *   +0xfe0.. PL192 peripheral/PrimeCell IDs
 *
 * The OS dispatcher reads bank-0 IRQSTATUS, then bank-1 IRQSTATUS, reads
 * that bank's VICADDRESS for the vector (= IRQ number), calls the handler,
 * writes 1<<(n&31) to the glue edge-clear reg and EOIs VICADDRESS.
 * Inputs are level-sensitive; no hardware priority masking (the OS runs
 * the handler with IRQs masked, so nesting never happens).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "nano7_vic.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"

#define VIC_SIZE      0x3000
#define VIC_LOG_MAX   200

typedef struct Nano7VicBank {
    uint32_t level;
    uint32_t soft;
    uint32_t select;
    uint32_t enable;
    uint32_t protection;
    uint32_t swprio_mask;
    uint32_t daisy;
    uint32_t vect[32];
    uint32_t prio[32];
} Nano7VicBank;

typedef struct Nano7VicState {
    MemoryRegion mr;
    hwaddr base;
    Nano7VicBank bank[2];
    uint32_t glue[0x1000 / 4];
    qemu_irq cpu_irq, cpu_fiq;
    unsigned log_lines;
    uint64_t fired[NANO7_VIC_NUM_IRQS];
} Nano7VicState;

static const uint8_t vic_id[8] = {
    0x92, 0x11, 0x04, 0x00, 0x0d, 0xf0, 0x05, 0xb1
};

static uint32_t vic_guest_pc(void)
{
    return current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0;
}

static void G_GNUC_PRINTF(2, 3) vic_log(Nano7VicState *s, const char *fmt, ...)
{
    va_list ap;

    if (s->log_lines >= VIC_LOG_MAX) {
        return;
    }
    s->log_lines++;
    fprintf(stderr, "[nano7-vic] pc=0x%08x ", vic_guest_pc());
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static uint32_t vic_raw(Nano7VicBank *b)
{
    return b->level | b->soft;
}

static uint32_t vic_irqstatus(Nano7VicBank *b)
{
    return vic_raw(b) & b->enable & ~b->select;
}

static uint32_t vic_fiqstatus(Nano7VicBank *b)
{
    return vic_raw(b) & b->enable & b->select;
}

static void vic_update(Nano7VicState *s)
{
    qemu_set_irq(s->cpu_irq, vic_irqstatus(&s->bank[0]) ||
                             vic_irqstatus(&s->bank[1]));
    qemu_set_irq(s->cpu_fiq, vic_fiqstatus(&s->bank[0]) ||
                             vic_fiqstatus(&s->bank[1]));
}

static void vic_set_irq(void *opaque, int n, int level)
{
    Nano7VicState *s = opaque;
    Nano7VicBank *b = &s->bank[n / 32];
    uint32_t bit = 1u << (n % 32);

    if (level) {
        if (!(b->level & bit) && s->fired[n]++ == 0) {
            vic_log(s, "first assertion of IRQ %d (enabled=%d)", n,
                    !!(b->enable & bit));
        }
        b->level |= bit;
    } else {
        b->level &= ~bit;
    }
    vic_update(s);
}

static uint32_t vic_current_vector(Nano7VicState *s, int bi)
{
    Nano7VicBank *b = &s->bank[bi];
    uint32_t pend = vic_irqstatus(b);
    int best = -1;
    int i;

    for (i = 0; i < 32; i++) {
        if ((pend & (1u << i)) && (b->swprio_mask & (1u << b->prio[i])) &&
            (best < 0 || b->prio[i] < b->prio[best])) {
            best = i;
        }
    }
    return best < 0 ? 0 : b->vect[best];
}

static uint64_t vic_read(void *opaque, hwaddr offset, unsigned size)
{
    Nano7VicState *s = opaque;
    int bi = offset >> 12;
    hwaddr r = offset & 0xfff;
    Nano7VicBank *b;

    if (bi == 2) {
        return s->glue[r >> 2];
    }
    b = &s->bank[bi];
    switch (r) {
    case 0x000: return vic_irqstatus(b);
    case 0x004: return vic_fiqstatus(b);
    case 0x008: return vic_raw(b);
    case 0x00c: return b->select;
    case 0x010: return b->enable;
    case 0x018: return b->soft;
    case 0x020: return b->protection;
    case 0x024: return b->swprio_mask;
    case 0x028: return b->daisy;
    case 0xf00: return vic_current_vector(s, bi);
    }
    if (r >= 0x100 && r < 0x180) {
        return b->vect[(r - 0x100) >> 2];
    }
    if (r >= 0x200 && r < 0x280) {
        return b->prio[(r - 0x200) >> 2];
    }
    if (r >= 0xfe0) {
        return vic_id[(r - 0xfe0) >> 2];
    }
    vic_log(s, "R%u unhandled 0x%08" PRIx64, size * 8, s->base + offset);
    return 0;
}

static void vic_write(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    Nano7VicState *s = opaque;
    int bi = offset >> 12;
    hwaddr r = offset & 0xfff;
    uint32_t v = value;
    Nano7VicBank *b;

    if (bi == 2) {
        /* edge-clear glue: level inputs only, so just store */
        s->glue[r >> 2] = v;
        return;
    }
    b = &s->bank[bi];
    switch (r) {
    case 0x00c: b->select = v; break;
    case 0x010:
        b->enable |= v;
        vic_log(s, "bank%d enable |= 0x%08x -> 0x%08x", bi, v, b->enable);
        break;
    case 0x014: b->enable &= ~v; break;
    case 0x018: b->soft |= v; break;
    case 0x01c: b->soft &= ~v; break;
    case 0x020: b->protection = v & 1; break;
    case 0x024: b->swprio_mask = v & 0xffff; break;
    case 0x028: b->daisy = v & 0xf; break;
    case 0xf00: break;                      /* EOI */
    default:
        if (r >= 0x100 && r < 0x180) {
            b->vect[(r - 0x100) >> 2] = v;
        } else if (r >= 0x200 && r < 0x280) {
            b->prio[(r - 0x200) >> 2] = v & 0xf;
        } else {
            vic_log(s, "W%u unhandled 0x%08" PRIx64 " = 0x%08x", size * 8,
                    s->base + offset, v);
        }
        return;
    }
    vic_update(s);
}

static const MemoryRegionOps vic_ops = {
    .read = vic_read,
    .write = vic_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

qemu_irq *nano7_vic_attach(MemoryRegion *parent, hwaddr base,
                           qemu_irq cpu_irq, qemu_irq cpu_fiq)
{
    Nano7VicState *s = g_new0(Nano7VicState, 1);
    int bi, i;

    s->base = base;
    s->cpu_irq = cpu_irq;
    s->cpu_fiq = cpu_fiq;
    for (bi = 0; bi < 2; bi++) {
        s->bank[bi].swprio_mask = 0xffff;
        s->bank[bi].daisy = 0xf;
        for (i = 0; i < 32; i++) {
            s->bank[bi].prio[i] = 0xf;
        }
    }
    memory_region_init_io(&s->mr, NULL, &vic_ops, s, "nano7.vic", VIC_SIZE);
    memory_region_add_subregion_overlap(parent, base, &s->mr, 0);
    return qemu_allocate_irqs(vic_set_irq, s, NANO7_VIC_NUM_IRQS);
}
