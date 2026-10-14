/*
 * nano7_disp.c -- 0x39000000 display pipe (2D blitter) for the iPod
 * nano 7G (S5L8747). Frame done on VIC IRQ 37.
 *
 * It is a memory-to-memory 2D blitter, not a scan-out (Session 18 RE, ring
 * interpreter 0x081baae0, record builders 0x081bb534/3dc/8e8/83c/a3c):
 *   +0x00  status        bit0 idle/ready (init + power-off spin for it),
 *                        bit1 running (submit 0x083bab28 won't kick)
 *   +0x04  mode + kick   bits 25:24 L0 mode, 29:28 L1 mode: 0 copy,
 *                        2 blend (+0x280 words), 1 solid fill (+0x290,
 *                        written as is, alpha included);
 *                        ring command 10 ORs in bit0 = kick (one-shot)
 *   +0x08  irq enable    bit0      +0x0c irq status bit0 frame done, W1C
 *   L0 (source)  +0x10 format (6 = ARGB8888, straight alpha), +0x14 addr,
 *                +0x1c stride (pixels, low half), +0x20 origin (x<<16|y,
 *                in quarter pixels), +0x24 size (w<<16|h)
 *   L1 (destination, read-modify-write)  +0x30 format, +0x34 base,
 *                +0x3c stride (pixels), +0x40 size, +0x44 position (x<<16|y)
 *   +0x50/+0x54  x/y step (src<<16)/dst, 0x10000 = 1:1 (nearest here)
 *   +0x280..+0x28c blend: w0/w1 source colour/alpha factor, w2/w3 dest;
 *                each: bits 30:29 selector (0 = the word's own constant:
 *                bytes 0-2 colour, byte 0 alpha), 27:24 op (0 one, 1 source
 *                alpha, 7 constant alpha = low byte of w1 / w3), bit 28 =
 *                1 - factor. out = S * fs + D * fd per channel.
 *   +0x290 fill colour (ARGB)   +0x260 version, driver wants >= 0x60005
 * Other offsets (+0x60.. scaler taps, +0x220.. colour matrix) are stored.
 *
 * Model: the blit runs at the kick; +0x00 = 2 (running), 20 us + 5 ns per
 * pixel later +0x00 = 1, +0x0c bit0, IRQ 37 while (+0x0c & +0x08 & 1).
 * Only format 6 is blitted (others logged once). What is on screen is the
 * LCD controller's business (nano7_lcd.c). NANO7_DISP_TRACE=1 logs every
 * kick.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "nano7_disp.h"
#include "hw/core/cpu.h"
#include "qemu/timer.h"
#include "target/arm/cpu.h"
#include "exec/address-spaces.h"

#define DISP_SIZE       0x1000
#define DISP_BLIT_NS    20000       /* per blit, plus 5 ns per pixel */
#define DISP_LOG_MAX    64      /* then every 256th kick */
#define DISP_MAX_W      1024
#define DISP_MAX_H      1024

typedef struct Nano7DispState {
    MemoryRegion mr;
    uint32_t regs[DISP_SIZE / 4];
    uint32_t status, irq_status;
    QEMUTimer *timer;
    uint64_t last_px;
    qemu_irq irq;
    uint64_t kicks, frames;
    uint64_t blits, blit_px;
    bool trace;                 /* NANO7_DISP_TRACE: log every kick/dump */
    bool warned_fmt;
    uint32_t layout_seen[256];  /* layouts (layer regs minus FB addresses) */
    unsigned layout_logs;
    unsigned log_lines;
} Nano7DispState;

static Nano7DispState *disp_singleton;

static uint32_t disp_guest_pc(void)
{
    return current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0;
}

static void disp_summary(void)
{
    if (disp_singleton) {
        fprintf(stderr, "[nano7-disp] summary: %" PRIu64 " kicks, %" PRIu64
                " frames done, %" PRIu64 " blits (%" PRIu64 " px)\n",
                disp_singleton->kicks, disp_singleton->frames,
                disp_singleton->blits, disp_singleton->blit_px);
    }
}

static void disp_update(Nano7DispState *s)
{
    qemu_set_irq(s->irq, (s->irq_status & s->regs[0x08 / 4] & 1) != 0);
}

static inline uint32_t disp_ld(uint32_t a)
{
    return address_space_ldl_le(&address_space_memory, a & 0x7fffffff,
                                MEMTXATTRS_UNSPECIFIED, NULL);
}

/* blend factor (0..255) from one +0x280 word: selector 0 = the word's own
 * constant for this channel (bytes 0-2 colour, byte 0 alpha), else op 0
 * one, 1 source alpha, 7 constant alpha (low byte of w1 / w3); bit 28
 * inverts. Checked against every preset the driver uses (Session 18). */
static unsigned disp_factor(uint32_t w, unsigned sa, unsigned konst,
                            unsigned chan_const)
{
    unsigned f;

    if (!((w >> 29) & 3)) {
        f = chan_const;
    } else {
        switch ((w >> 24) & 0xf) {
        case 0:  f = 255; break;
        case 1:  f = sa; break;
        case 7:  f = konst; break;
        default: f = 255; break;
        }
    }
    return (w & (1u << 28)) ? 255 - f : f;
}

static uint32_t disp_blend(Nano7DispState *s, uint32_t S, uint32_t D)
{
    uint32_t w0 = s->regs[0x280 / 4], w1 = s->regs[0x284 / 4];
    uint32_t w2 = s->regs[0x288 / 4], w3 = s->regs[0x28c / 4];
    unsigned sa = S >> 24;
    uint32_t out = 0;
    int sh;

    for (sh = 0; sh < 32; sh += 8) {
        bool alpha = sh == 24;
        unsigned fs = alpha ? disp_factor(w1, sa, w1 & 0xff, w1 & 0xff) :
                      disp_factor(w0, sa, w1 & 0xff, (w0 >> sh) & 0xff);
        unsigned fd = alpha ? disp_factor(w3, sa, w3 & 0xff, w3 & 0xff) :
                      disp_factor(w2, sa, w3 & 0xff, (w2 >> sh) & 0xff);
        unsigned v = (((S >> sh) & 0xff) * fs + ((D >> sh) & 0xff) * fd +
                      127) / 255;

        out |= MIN(v, 255u) << sh;
    }
    return out;
}

/* Run the blit described by the registers (called at the kick). */
static void disp_blit(Nano7DispState *s, uint32_t ctl)
{
    unsigned mode = (ctl >> 24) & 3;
    uint32_t src = s->regs[0x14 / 4], sstride = s->regs[0x1c / 4] & 0xffff;
    uint32_t sorg = s->regs[0x20 / 4], ssz = s->regs[0x24 / 4];
    uint32_t dst = s->regs[0x34 / 4], dstride = s->regs[0x3c / 4] & 0xffff;
    uint32_t dsz = s->regs[0x40 / 4], dpos = s->regs[0x44 / 4];
    uint32_t sx0 = (sorg >> 16) / 4, sy0 = (sorg & 0xffff) / 4;
    uint32_t dw = dsz >> 16, dh = dsz & 0xffff;
    uint32_t dx0 = dpos >> 16, dy0 = dpos & 0xffff;
    uint32_t xstep = s->regs[0x50 / 4], ystep = s->regs[0x54 / 4];
    uint32_t sw = ssz >> 16, sh = ssz & 0xffff;
    uint32_t fill = s->regs[0x290 / 4], x, y;
    g_autofree uint32_t *row = NULL;

    if (s->regs[0x30 / 4] != 6 || (mode != 1 && s->regs[0x10 / 4] != 6)) {
        if (!s->warned_fmt) {
            s->warned_fmt = true;
            fprintf(stderr, "[nano7-disp] blit with format L0 %u / L1 %u not "
                    "modeled (only 6 = ARGB8888)\n", s->regs[0x10 / 4],
                    s->regs[0x30 / 4]);
        }
        return;
    }
    if (!dst || !dw || !dh || dw > DISP_MAX_W || dh > DISP_MAX_H) {
        return;
    }
    if (!xstep) {
        xstep = 0x10000;
    }
    if (!ystep) {
        ystep = 0x10000;
    }
    row = g_new(uint32_t, dw);
    for (y = 0; y < dh; y++) {
        uint32_t drow = dst + ((dy0 + y) * dstride + dx0) * 4;
        uint32_t sy = sy0 + MIN((uint32_t)(((uint64_t)y * ystep) >> 16),
                                sh ? sh - 1 : 0);

        address_space_read(&address_space_memory, drow & 0x7fffffff,
                           MEMTXATTRS_UNSPECIFIED, row, dw * 4);
        for (x = 0; x < dw; x++) {
            uint32_t sx = sx0 + MIN((uint32_t)(((uint64_t)x * xstep) >> 16),
                                    sw ? sw - 1 : 0);
            uint32_t S = mode == 1 ? fill :
                         disp_ld(src + (sy * sstride + sx) * 4);
            uint32_t D = le32_to_cpu(row[x]);

            /* copy and fill write the source; only mode 2 blends (a fill
             * clears to +0x290 incl. alpha: keeping the old alpha left
             * opaque black boxes where the old clock digits were) */
            row[x] = cpu_to_le32(mode == 2 ? disp_blend(s, S, D) : S);
        }
        address_space_write(&address_space_memory, drow & 0x7fffffff,
                            MEMTXATTRS_UNSPECIFIED, row, dw * 4);
    }
    s->blits++;
    s->blit_px += (uint64_t)dw * dh;
    s->last_px = (uint64_t)dw * dh;
}

/* Log all layer / blend registers whenever the layout (anything but the
 * FB addresses) changes: the layer register semantics are still partly
 * unknown, this is the raw material. */
static void disp_log_layout(Nano7DispState *s)
{
    static const uint16_t ranges[][2] = {
        { 0x10, 0x5c }, { 0x220, 0x244 }, { 0x280, 0x290 },
    };
    uint32_t h = 2166136261u;
    unsigned r, o;

    for (r = 0; r < ARRAY_SIZE(ranges); r++) {
        for (o = ranges[r][0]; o < ranges[r][1]; o += 4) {
            if (o != 0x14 && o != 0x34) {
                h = (h ^ s->regs[o / 4]) * 16777619u;
            }
        }
    }
    for (o = 0; o < s->layout_logs; o++) {
        if (s->layout_seen[o] == h) {
            return;
        }
    }
    if (s->layout_logs >= ARRAY_SIZE(s->layout_seen)) {
        return;
    }
    s->layout_seen[s->layout_logs++] = h;
    fprintf(stderr, "[nano7-disp] t=%.3f s kick #%" PRIu64 " layout:",
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9, s->kicks);
    for (r = 0; r < ARRAY_SIZE(ranges); r++) {
        fprintf(stderr, " |%03x:", ranges[r][0]);
        for (o = ranges[r][0]; o < ranges[r][1]; o += 4) {
            fprintf(stderr, " %x", s->regs[o / 4]);
        }
    }
    fputc('\n', stderr);
}

static void disp_frame_done(void *opaque)
{
    Nano7DispState *s = opaque;

    s->frames++;
    s->status = 1;
    s->irq_status |= 1;
    disp_update(s);
}

static uint64_t disp_read(void *opaque, hwaddr offset, unsigned size)
{
    Nano7DispState *s = opaque;

    switch (offset) {
    case 0x00:
        return s->status;
    case 0x04:
        return s->regs[0x04 / 4];   /* mode bits; the kick bit reads 0 */
    case 0x0c:
        return s->irq_status;
    default:
        return s->regs[offset / 4];
    }
}

static void disp_write(void *opaque, hwaddr offset, uint64_t value,
                       unsigned size)
{
    Nano7DispState *s = opaque;
    uint32_t v = value;

    switch (offset) {
    case 0x04:
        s->regs[0x04 / 4] = v & ~1u;
        if (v & 1) {
            s->kicks++;
            s->last_px = 0;
            disp_log_layout(s);
            disp_blit(s, v);
            if (s->trace || s->log_lines++ < DISP_LOG_MAX ||
                !(s->kicks & 0xff)) {
                fprintf(stderr, "[nano7-disp] t=%.3f s pc=0x%08x kick #%" PRIu64
                        " mode 0x%08x L0 0x%08x %08x -> L1 0x%08x size "
                        "%08x at %08x, irq en %u\n",
                        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9,
                        disp_guest_pc(), s->kicks, v & ~1u,
                        s->regs[0x14 / 4], s->regs[0x24 / 4],
                        s->regs[0x34 / 4], s->regs[0x40 / 4],
                        s->regs[0x44 / 4], s->regs[0x08 / 4] & 1);
                fflush(stderr);
            }
            s->status = 2;
            timer_mod_ns(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                         DISP_BLIT_NS + s->last_px * 5);
        }
        break;
    case 0x0c:
        s->irq_status &= ~v;
        break;
    default:
        s->regs[offset / 4] = v;
        break;
    }
    disp_update(s);
}

static const MemoryRegionOps disp_ops = {
    .read = disp_read,
    .write = disp_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

void nano7_disp_attach(MemoryRegion *parent, hwaddr base, qemu_irq irq)
{
    Nano7DispState *s = g_new0(Nano7DispState, 1);

    s->irq = irq;
    s->status = 1;
    s->trace = getenv("NANO7_DISP_TRACE") != NULL;
    s->regs[0x260 / 4] = 0x60005;
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, disp_frame_done, s);
    disp_singleton = s;
    atexit(disp_summary);
    memory_region_init_io(&s->mr, NULL, &disp_ops, s, "nano7.disp",
                          DISP_SIZE);
    memory_region_add_subregion_overlap(parent, base, &s->mr, 0);
}
