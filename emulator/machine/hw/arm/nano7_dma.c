/*
 * nano7_dma.c -- the two 8-channel DMA engines of the iPod nano 7G
 * (S5L8747): engine 1 at 0x38200000 (VIC IRQ 16), engine 2 at 0x38700000
 * (IRQ 17).
 *
 * RE (Session 27, snap6; docs/research/audio-path.md): start routine
 * 0x080b1cb4 writes, per channel at base + 0x100 + 0x20 * n:
 *   +0x00 SRC   +0x04 DST   +0x08 NEXT (descriptor | 0x80000000 alias)
 *   +0x0c CFG   bit26 source increment, bit27 destination increment,
 *               bits 23:21 element size exponent (audio 0x84249000 = 16 bit)
 *   +0x14 COUNT elements (& 0x1fffffff), not bytes
 *   +0x10 CTRL  written last: 0x8001 | srcreq << 1 | dstreq << 6 | type
 *               (0x800 mem->periph, 0x1000 periph->mem, 0 mem->mem);
 *               bit0 = run; the stop routine 0x080ba2a4 clears it
 *   engine +0x04 completion status (bit n), +0x08 ack (write 1 clears)
 * In-memory descriptors are {SRC, DST, NEXT, CFG, COUNT}; NEXT is reloaded
 * when a pass ends (0 = stop). Audio: engine 1 channel 2, a one-descriptor
 * ring (NEXT = itself) of 4096-byte passes of s16 stereo to the I2S0 TX FIFO
 * 0x3ca00010; MeCCAOutputTask rewrites the descriptor during a pass and
 * waits for the per-channel event that the ISR 0x220010ee posts.
 *
 * Model: a pass moves its data through the address space (so peripheral
 * FIFOs see the writes) when it completes. Passes to a paced FIFO
 * (nano7_dma_set_fifo_rate: the I2S0 TX FIFO at the sample rate) take
 * bytes / rate of guest time (nano7_clock: pause / fast-forward apply);
 * others complete after 20 us + 10 ns per byte. Then: status bit, IRQ
 * while status != 0, reload from NEXT or stop.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "nano7_dma.h"
#include "nano7_clock.h"
#include "qemu/timer.h"
#include "exec/address-spaces.h"
#include "migration/vmstate.h"

#define DMA_SIZE        0x1000
#define DMA_NCH         8
#define DMA_MAX_FIFOS   4

typedef struct Nano7DmaState Nano7DmaState;

typedef struct DmaChan {
    Nano7DmaState *s;
    int n;
    uint32_t src, dst, next, cfg, ctrl, count;
    bool active;
    QEMUTimer *timer;
    uint64_t passes;
} DmaChan;

struct Nano7DmaState {
    MemoryRegion mr;
    int engine;
    uint32_t status;
    uint32_t regs[DMA_SIZE / 4];
    DmaChan ch[DMA_NCH];
    qemu_irq irq;
    unsigned log_lines;
};

static struct {
    uint32_t addr, rate;
} dma_fifos[DMA_MAX_FIFOS];

void nano7_dma_set_fifo_rate(uint32_t fifo_addr, uint32_t bytes_per_sec)
{
    int i, free = -1;

    for (i = 0; i < DMA_MAX_FIFOS; i++) {
        if (dma_fifos[i].addr == fifo_addr) {
            dma_fifos[i].rate = bytes_per_sec;
            return;
        }
        if (!dma_fifos[i].addr && free < 0) {
            free = i;
        }
    }
    if (free >= 0) {
        dma_fifos[free].addr = fifo_addr;
        dma_fifos[free].rate = bytes_per_sec;
    }
}

static uint32_t dma_fifo_rate(uint32_t addr)
{
    int i;

    for (i = 0; i < DMA_MAX_FIFOS; i++) {
        if (dma_fifos[i].addr && dma_fifos[i].addr == addr) {
            return dma_fifos[i].rate;
        }
    }
    return 0;
}

static void dma_update(Nano7DmaState *s)
{
    qemu_set_irq(s->irq, s->status != 0);
}

static unsigned dma_esz(DmaChan *c)
{
    return 1u << ((c->cfg >> 21) & 7);
}

static uint64_t dma_bytes(DmaChan *c)
{
    return (uint64_t)(c->count & 0x1fffffff) * dma_esz(c);
}

/* schedule the end of the current pass */
static void dma_arm(DmaChan *c)
{
    uint64_t bytes = dma_bytes(c);
    uint32_t rate = dma_fifo_rate(c->dst & 0x7fffffff);

    if (rate) {
        nano7_timer_mod(c->timer, nano7_now_ns() +
                        muldiv64(bytes, NANOSECONDS_PER_SECOND, rate));
    } else {
        nano7_timer_mod(c->timer, nano7_now_ns() + 20000 + bytes * 10);
    }
}

/* move the pass's data: element by element through the address space */
static void dma_move(DmaChan *c)
{
    unsigned esz = dma_esz(c), i;
    uint32_t n = c->count & 0x1fffffff;
    uint32_t src = c->src & 0x7fffffff, dst = c->dst & 0x7fffffff;
    uint8_t buf[8];

    if (esz > sizeof(buf)) {
        return;
    }
    for (i = 0; i < n; i++) {
        address_space_read(&address_space_memory, src, MEMTXATTRS_UNSPECIFIED,
                           buf, esz);
        address_space_write(&address_space_memory, dst,
                            MEMTXATTRS_UNSPECIFIED, buf, esz);
        if (c->cfg & (1u << 26)) {
            src += esz;
        }
        if (c->cfg & (1u << 27)) {
            dst += esz;
        }
    }
}

static void dma_pass_done(void *opaque)
{
    DmaChan *c = opaque;
    Nano7DmaState *s = c->s;
    uint32_t next;

    if (!c->active) {
        return;
    }
    dma_move(c);
    c->passes++;
    if (s->log_lines < 16) {
        s->log_lines++;
        fprintf(stderr, "[nano7-dma] t=%.3f s engine %d ch %d pass done: "
                "0x%08x -> 0x%08x, %" PRIu64 " bytes, next 0x%08x\n",
                nano7_now_ns() / 1e9, s->engine, c->n, c->src, c->dst,
                dma_bytes(c), c->next);
    }
    s->status |= 1u << c->n;
    next = c->next & 0x7fffffff;
    if (next) {                 /* reload the next descriptor */
        uint32_t d[5];

        address_space_read(&address_space_memory, next,
                           MEMTXATTRS_UNSPECIFIED, d, sizeof(d));
        c->src = le32_to_cpu(d[0]);
        c->dst = le32_to_cpu(d[1]);
        c->next = le32_to_cpu(d[2]);
        c->cfg = le32_to_cpu(d[3]);
        c->count = le32_to_cpu(d[4]);
        dma_arm(c);
    } else {
        c->active = false;
        c->ctrl &= ~1u;
    }
    dma_update(s);
}

static uint64_t dma_read(void *opaque, hwaddr off, unsigned size)
{
    Nano7DmaState *s = opaque;

    if (off == 0x04) {
        return s->status;
    }
    if (off >= 0x100 && off < 0x100 + 0x20 * DMA_NCH) {
        DmaChan *c = &s->ch[(off - 0x100) / 0x20];

        switch ((off - 0x100) % 0x20) {
        case 0x00: return c->src;
        case 0x04: return c->dst;
        case 0x08: return c->next;
        case 0x0c: return c->cfg;
        case 0x10: return c->ctrl;
        case 0x14: return c->count;
        }
    }
    return s->regs[off / 4];
}

static void dma_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    Nano7DmaState *s = opaque;

    if (off == 0x08) {
        s->status &= ~(uint32_t)v;
        dma_update(s);
        return;
    }
    if (off >= 0x100 && off < 0x100 + 0x20 * DMA_NCH) {
        DmaChan *c = &s->ch[(off - 0x100) / 0x20];

        switch ((off - 0x100) % 0x20) {
        case 0x00: c->src = v; break;
        case 0x04: c->dst = v; break;
        case 0x08: c->next = v; break;
        case 0x0c: c->cfg = v; break;
        case 0x14: c->count = v; break;
        case 0x10:
            c->ctrl = v;
            if ((v & 1) && !c->active) {
                c->active = true;
                if (s->log_lines < 16) {
                    s->log_lines++;
                    fprintf(stderr, "[nano7-dma] t=%.3f s engine %d ch %d "
                            "start ctrl 0x%04x cfg 0x%08x 0x%08x -> 0x%08x "
                            "count %u next 0x%08x\n", nano7_now_ns() / 1e9,
                            s->engine, c->n, (uint32_t)v, c->cfg, c->src,
                            c->dst, c->count, c->next);
                }
                dma_arm(c);
            } else if (!(v & 1) && c->active) {
                c->active = false;      /* abort */
                timer_del(c->timer);
            }
            break;
        }
        return;
    }
    s->regs[off / 4] = v;
}

static const MemoryRegionOps dma_ops = {
    .read = dma_read,
    .write = dma_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static const VMStateDescription vmstate_dma_chan = {
    .name = "nano7-dma-chan",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(src, DmaChan),
        VMSTATE_UINT32(dst, DmaChan),
        VMSTATE_UINT32(next, DmaChan),
        VMSTATE_UINT32(cfg, DmaChan),
        VMSTATE_UINT32(ctrl, DmaChan),
        VMSTATE_UINT32(count, DmaChan),
        VMSTATE_BOOL(active, DmaChan),
        VMSTATE_TIMER_PTR(timer, DmaChan),
        VMSTATE_UINT64(passes, DmaChan),
        VMSTATE_END_OF_LIST()
    }
};

static int dma_post_load(void *opaque, int version_id)
{
    dma_update(opaque);
    return 0;
}

static const VMStateDescription vmstate_dma = {
    .name = "nano7-dma",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = dma_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(status, Nano7DmaState),
        VMSTATE_UINT32_ARRAY(regs, Nano7DmaState, DMA_SIZE / 4),
        VMSTATE_STRUCT_ARRAY(ch, Nano7DmaState, DMA_NCH, 1, vmstate_dma_chan,
                             DmaChan),
        VMSTATE_END_OF_LIST()
    }
};

void nano7_dma_attach(MemoryRegion *parent, hwaddr base, int engine,
                      qemu_irq irq)
{
    Nano7DmaState *s = g_new0(Nano7DmaState, 1);
    g_autofree char *name = g_strdup_printf("nano7.dma%d", engine);
    int n;

    s->engine = engine;
    s->irq = irq;
    for (n = 0; n < DMA_NCH; n++) {
        s->ch[n].s = s;
        s->ch[n].n = n;
        s->ch[n].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dma_pass_done,
                                      &s->ch[n]);
        nano7_clock_track(s->ch[n].timer, true);
    }
    vmstate_register_any(NULL, &vmstate_dma, s);
    memory_region_init_io(&s->mr, NULL, &dma_ops, s, name, DMA_SIZE);
    memory_region_add_subregion_overlap(parent, base, &s->mr, 0);
}
