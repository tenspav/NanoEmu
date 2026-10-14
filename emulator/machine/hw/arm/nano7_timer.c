/*
 * nano7_timer.c -- S5L8702-style 8-channel timer block for the iPod nano
 * 7G machine (0x3c700000), all channels sharing one VIC line (IRQ 8).
 *
 * Channel bases (RetailOS 0x8000a60 / 0x8000c58): A..D = +0x00/+0x20/+0x40/
 * +0x60, E..H = +0xa0/+0xc0/+0xe0/+0x100. Per channel:
 *   +0x00 CON   bits 12..14 interrupt enables, bits 16..18 pending (W1C;
 *               E..H report pending in TSTAT instead)
 *   +0x04 CMD   bit0 start/run, bit1 clear counter (self-clearing)
 *   +0x08 DATA0 period in ticks   +0x0c DATA1
 *   +0x10 PRE   prescaler (divide by PRE+1)   +0x14 CNT (live)
 * +0x118 TSTAT pending bits of E..H: channel n at shift (7-n)*8,
 *               bit0/1/2 <-> CON enable bit 12/13/14 (W1C)
 *
 * Tick source is fixed 3 MHz (the OS computes DATA0 = 3000000 /
 * ((PRE+1) * Hz) in 0x8000a60; the scheduler tick is channel B,
 * DATA0=6000 -> 500 Hz). Every channel runs in periodic mode: on each
 * DATA0 expiry the enabled CON bits 12..14 are latched as pending.
 *
 * +0x84 and +0xb4 (= TECNT) keep the old free-running ~16 MHz counter
 * behaviour used for busy-wait delays; everything else outside the
 * channel registers is a write-store / read-back regfile (never written
 * = 0xff, like the generic regfile it replaces; +0x88 = PLL config).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "nano7_timer.h"
#include "hw/core/cpu.h"
#include "qemu/timer.h"
#include "target/arm/cpu.h"
#include "migration/vmstate.h"

#define TMR_SIZE        0x1000
#define TMR_NCH         8
#define TMR_TSTAT       0x118
#define TMR_HZ          3000000ULL
#define TMR_MIN_NS      10000       /* clamp: at most 100 kHz of IRQs */
#define TMR_LOG_MAX     200

typedef struct Nano7TimerState Nano7TimerState;

typedef struct Nano7TimerChan {
    Nano7TimerState *s;
    int n;
    uint32_t con, cmd, data0, data1, pre;
    int64_t start_ns;       /* start of the current period */
    QEMUTimer *timer;
    uint64_t expiries;
} Nano7TimerChan;

struct Nano7TimerState {
    MemoryRegion mr;
    hwaddr base;
    Nano7TimerChan ch[TMR_NCH];
    uint32_t tstat;
    uint32_t regs[TMR_SIZE / 4];
    bool written[TMR_SIZE / 4];
    qemu_irq irq;
    uint64_t usec_base;
    unsigned log_lines;
};

static uint32_t tmr_guest_pc(void)
{
    return current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0;
}

static void G_GNUC_PRINTF(2, 3) tmr_log(Nano7TimerState *s, const char *fmt, ...)
{
    va_list ap;

    if (s->log_lines >= TMR_LOG_MAX) {
        return;
    }
    s->log_lines++;
    fprintf(stderr, "[nano7-tmr] pc=0x%08x ", tmr_guest_pc());
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* offset -> channel index (or -1), *reg = offset within the channel */
static int tmr_chan_of(hwaddr offset, hwaddr *reg)
{
    if (offset < 0x80) {
        *reg = offset & 0x1f;
        return offset >> 5;
    }
    if (offset >= 0xa0 && offset < 0x120) {
        *reg = (offset - 0xa0) & 0x1f;
        return 4 + ((offset - 0xa0) >> 5);
    }
    return -1;
}

static int tmr_tstat_shift(int n)
{
    return (7 - n) * 8;
}

static int64_t tmr_period_ns(Nano7TimerChan *c)
{
    uint64_t ticks = (uint64_t)MAX(c->data0, 1) * ((uint64_t)c->pre + 1);
    int64_t ns = muldiv64(ticks, NANOSECONDS_PER_SECOND, TMR_HZ);

    return MAX(ns, TMR_MIN_NS);
}

static void tmr_update(Nano7TimerState *s)
{
    bool level = s->tstat != 0;
    int n;

    for (n = 0; n < 4; n++) {
        level |= (s->ch[n].con & 0x70000) != 0;
    }
    qemu_set_irq(s->irq, level);
}

static void tmr_expire(void *opaque)
{
    Nano7TimerChan *c = opaque;
    Nano7TimerState *s = c->s;
    uint32_t en = (c->con >> 12) & 7;

    if (c->expiries++ == 0) {
        tmr_log(s, "ch%d first expiry (con=0x%08x data0=%u pre=%u, "
                "%" PRId64 " ns)", c->n, c->con, c->data0, c->pre,
                tmr_period_ns(c));
    }
    if (c->n < 4) {
        c->con |= en << 16;
    } else {
        s->tstat |= en << tmr_tstat_shift(c->n);
    }
    c->start_ns += tmr_period_ns(c);
    /* don't try to catch up after a long host stall */
    if (c->start_ns + tmr_period_ns(c) <
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)) {
        c->start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    timer_mod_ns(c->timer, c->start_ns + tmr_period_ns(c));
    tmr_update(s);
}

/*
 * 64-bit free-running 1 MHz (microsecond) counter: +0x84 = low word,
 * +0x80 = high word. RetailOS's system clock 0x83edbc8 reads hi/lo/hi
 * until stable and splits the value with /1000/1000 into {s, ms}.
 * usec_base lets a snapshot resume continue near the dumped uptime.
 */
static uint64_t tmr_usec(Nano7TimerState *s)
{
    return s->usec_base + qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000;
}

static uint64_t tmr_read(void *opaque, hwaddr offset, unsigned size)
{
    Nano7TimerState *s = opaque;
    hwaddr reg;
    int n;

    if (offset == 0x84 || offset == 0xb4) {
        return (uint32_t)tmr_usec(s);
    }
    if (offset == 0x80) {
        return (uint32_t)(tmr_usec(s) >> 32);
    }
    if (offset == TMR_TSTAT) {
        return s->tstat;
    }
    n = tmr_chan_of(offset, &reg);
    if (n >= 0) {
        Nano7TimerChan *c = &s->ch[n];

        switch (reg) {
        case 0x00: return c->con;
        case 0x04: return c->cmd;
        case 0x08: return c->data0;
        case 0x0c: return c->data1;
        case 0x10: return c->pre;
        case 0x14:
            if (c->cmd & 1) {
                int64_t el = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                             c->start_ns;
                return muldiv64(MAX(el, 0), TMR_HZ,
                                NANOSECONDS_PER_SECOND * (c->pre + 1));
            }
            return 0;
        }
        return 0;
    }
    return s->written[offset >> 2] ? s->regs[offset >> 2] : 0xffffffff;
}

static void tmr_write(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    Nano7TimerState *s = opaque;
    uint32_t v = value;
    hwaddr reg;
    int n;

    if (offset == TMR_TSTAT) {
        s->tstat &= ~v;
        tmr_update(s);
        return;
    }
    n = tmr_chan_of(offset, &reg);
    if (n < 0) {
        s->regs[offset >> 2] = v;
        s->written[offset >> 2] = true;
        return;
    }

    Nano7TimerChan *c = &s->ch[n];
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    switch (reg) {
    case 0x00:
        if ((v ^ c->con) & ~0x70000u) {
            tmr_log(s, "ch%d CON = 0x%08x", n, v);   /* not plain acks */
        }
        if (n < 4) {
            c->con = (v & ~0x70000u) | (c->con & 0x70000u & ~v);
        } else {
            c->con = v;
        }
        break;
    case 0x04:
        tmr_log(s, "ch%d CMD = 0x%08x (data0=%u pre=%u)", n, v,
                c->data0, c->pre);
        if (v & 2) {
            c->start_ns = now;
        }
        if ((v & 1) && !(c->cmd & 1)) {
            c->start_ns = now;
        }
        c->cmd = v & ~2u;
        if (c->cmd & 1) {
            timer_mod_ns(c->timer, c->start_ns + tmr_period_ns(c));
        } else {
            timer_del(c->timer);
        }
        break;
    case 0x08: c->data0 = v; break;
    case 0x0c: c->data1 = v; break;
    case 0x10: c->pre = v; break;
    default: break;
    }
    tmr_update(s);
}

static const MemoryRegionOps tmr_ops = {
    .read = tmr_read,
    .write = tmr_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static const VMStateDescription vmstate_tmr_chan = {
    .name = "nano7-timer-chan",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(con, Nano7TimerChan),
        VMSTATE_UINT32(cmd, Nano7TimerChan),
        VMSTATE_UINT32(data0, Nano7TimerChan),
        VMSTATE_UINT32(data1, Nano7TimerChan),
        VMSTATE_UINT32(pre, Nano7TimerChan),
        VMSTATE_INT64(start_ns, Nano7TimerChan),
        VMSTATE_TIMER_PTR(timer, Nano7TimerChan),
        VMSTATE_UINT64(expiries, Nano7TimerChan),
        VMSTATE_END_OF_LIST()
    }
};

static int tmr_post_load(void *opaque, int version_id)
{
    tmr_update(opaque);
    return 0;
}

static const VMStateDescription vmstate_tmr = {
    .name = "nano7-timer",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = tmr_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(ch, Nano7TimerState, TMR_NCH, 1,
                             vmstate_tmr_chan, Nano7TimerChan),
        VMSTATE_UINT32(tstat, Nano7TimerState),
        VMSTATE_UINT32_ARRAY(regs, Nano7TimerState, TMR_SIZE / 4),
        VMSTATE_BOOL_ARRAY(written, Nano7TimerState, TMR_SIZE / 4),
        VMSTATE_UINT64(usec_base, Nano7TimerState),
        VMSTATE_END_OF_LIST()
    }
};

void nano7_timer_attach(MemoryRegion *parent, hwaddr base, qemu_irq irq,
                        uint64_t usec_base)
{
    Nano7TimerState *s = g_new0(Nano7TimerState, 1);
    int n;

    s->base = base;
    s->irq = irq;
    s->usec_base = usec_base;
    for (n = 0; n < TMR_NCH; n++) {
        s->ch[n].s = s;
        s->ch[n].n = n;
        s->ch[n].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tmr_expire,
                                      &s->ch[n]);
    }
    vmstate_register_any(NULL, &vmstate_tmr, s);
    memory_region_init_io(&s->mr, NULL, &tmr_ops, s, "nano7.tmr", TMR_SIZE);
    memory_region_add_subregion_overlap(parent, base, &s->mr, 0);
}
