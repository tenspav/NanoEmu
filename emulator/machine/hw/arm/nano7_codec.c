/*
 * nano7_codec.c -- serial port 0 (0x3c300000, VIC IRQ 9) with the audio
 * codec behind it, for the iPod nano 7G (S5L8747).
 *
 * SPI0 is the same synchronous serial port as port 2 (touch, see
 * nano7_touch.c for the register semantics): same transfer routine
 * 0x083f4ee8, handler 0x08000fec, flush +0x00 |= 0xc, lengths +0x34 / +0x4c,
 * START +0x04 bit8, status +0x08 (bit0/22 done, bit1 TX request, bit5 RX
 * data, W1C; RX level bits 15:11), CS +0x0c bit1. Init 0x08010bec also
 * writes +0x30/+0x38/+0x3c/+0x40/+0x44 (clock set-up, stored).
 *
 * Codec (driver table key 0x4281; Session 27 RE, spec in
 * docs/research/audio-path.md): 16-bit register address, 8-bit data.
 *   write   [0x6c, msb, lsb, 0x00, data]
 *   stereo  [0x6c, msb, lsb | 0x80, n - 1, d0, d1...] -> reg, reg + 1, ...
 *   read    [0x6d, msb, lsb, 0x00, x]  -> the data byte is rx[4]
 * The driver does read-modify-write through 0x08417e64, so every register
 * reads back; there is no chip-ID read. Status 0x002f: the stop sequence
 * 0x080d073e spins (no timeout) until bit 7 is SET -> it always reads 0x80.
 * 0x9901 (0xa5, 0x00) is an unlock; stored like the rest.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "nano7_codec.h"
#include "nano7_clock.h"
#include "qemu/timer.h"
#include "migration/vmstate.h"

#define SSP_SIZE        0x1000
#define SSP_MAX_XFER    64
#define SSP_XFER_NS     20000

#define ST_DONE         (1u << 0)
#define ST_TXREQ        (1u << 1)
#define ST_RXDATA       (1u << 5)
#define ST_DONE2        (1u << 22)

#define CODEC_STATUS    0x002f

typedef struct Nano7CodecState {
    MemoryRegion mr;
    uint32_t regs[SSP_SIZE / 4];
    uint32_t flags;
    qemu_irq irq;
    QEMUTimer *xfer_timer;
    uint8_t tx[SSP_MAX_XFER], rx[SSP_MAX_XFER];
    uint32_t tx_len, tx_want, rx_len, rx_pos;
    bool busy;
    uint8_t creg[0x10000];      /* codec register file */
    uint64_t writes, reads;
    unsigned log_lines;
} Nano7CodecState;

static void ssp_update(Nano7CodecState *s)
{
    qemu_set_irq(s->irq, (s->flags & (ST_DONE | ST_TXREQ | ST_RXDATA |
                                      ST_DONE2)) != 0);
}

static uint8_t codec_read(Nano7CodecState *s, uint16_t reg)
{
    return reg == CODEC_STATUS ? s->creg[reg] | 0x80 : s->creg[reg];
}

static void codec_log(Nano7CodecState *s, const char *what, uint16_t reg,
                      uint8_t v)
{
    if (s->log_lines++ < 64) {
        fprintf(stderr, "[nano7-codec] t=%.3f s %s 0x%04x %s 0x%02x\n",
                nano7_now_ns() / 1e9, what, reg,
                what[0] == 'R' ? "->" : "=", v);
    }
}

/* all TX bytes are in: act on the codec command, build the full-duplex RX */
static void codec_xfer(Nano7CodecState *s)
{
    uint8_t *t = s->tx;
    unsigned n = s->tx_want, i;

    memset(s->rx, 0, n);
    if (n >= 5 && (t[0] == 0x6c || t[0] == 0x6d)) {
        bool multi = t[0] == 0x6c && (t[2] & 0x80) && t[3];
        uint16_t reg = t[1] << 8 | (multi ? t[2] & 0x7f : t[2]);

        if (t[0] == 0x6d) {
            s->rx[4] = codec_read(s, reg);
            s->reads++;
            codec_log(s, "R", reg, s->rx[4]);
        } else if (multi) {
            for (i = 0; i <= t[3] && 4 + i < n; i++) {
                s->creg[(uint16_t)(reg + i)] = t[4 + i];
                codec_log(s, "W", reg + i, t[4 + i]);
            }
            s->writes++;
        } else {
            s->creg[reg] = t[4];
            s->writes++;
            codec_log(s, "W", reg, t[4]);
        }
    }
    s->rx_len = n;
    s->rx_pos = 0;
}

static void ssp_xfer_done(void *opaque)
{
    Nano7CodecState *s = opaque;

    codec_xfer(s);
    s->busy = false;
    s->flags |= ST_DONE | ST_DONE2 | ST_RXDATA;
    ssp_update(s);
}

static uint64_t ssp_read(void *opaque, hwaddr off, unsigned size)
{
    Nano7CodecState *s = opaque;

    switch (off) {
    case 0x08:
        return s->flags | MIN(s->rx_len - s->rx_pos, 31u) << 11;
    case 0x20:
        return s->rx_pos < s->rx_len ? s->rx[s->rx_pos++] : 0;
    default:
        return s->regs[off / 4];
    }
}

static void ssp_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    Nano7CodecState *s = opaque;
    uint32_t v = value, old = s->regs[off / 4];

    switch (off) {
    case 0x00:
        if (v & 0xc) {                  /* flush FIFOs */
            s->rx_len = s->rx_pos = 0;
        }
        break;
    case 0x04:
        if ((v & 0x100) && !(old & 0x100) && !s->busy) {    /* START */
            s->tx_want = MIN(s->regs[0x4c / 4] ? s->regs[0x4c / 4] :
                             s->regs[0x34 / 4], SSP_MAX_XFER);
            s->tx_len = 0;
            s->busy = true;
            s->flags |= ST_TXREQ;
        }
        break;
    case 0x08:
        s->flags &= ~v;
        break;
    case 0x10:
        if (s->busy && s->tx_len < s->tx_want) {
            s->tx[s->tx_len++] = v;
            if (s->tx_len == s->tx_want) {
                s->flags &= ~ST_TXREQ;
                timer_mod_ns(s->xfer_timer,
                             qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                             SSP_XFER_NS);
            }
        }
        break;
    }
    s->regs[off / 4] = v;
    ssp_update(s);
}

static const MemoryRegionOps ssp_ops = {
    .read = ssp_read,
    .write = ssp_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static int codec_post_load(void *opaque, int version_id)
{
    ssp_update(opaque);
    return 0;
}

static const VMStateDescription vmstate_codec = {
    .name = "nano7-codec",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = codec_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Nano7CodecState, SSP_SIZE / 4),
        VMSTATE_UINT32(flags, Nano7CodecState),
        VMSTATE_TIMER_PTR(xfer_timer, Nano7CodecState),
        VMSTATE_UINT8_ARRAY(tx, Nano7CodecState, SSP_MAX_XFER),
        VMSTATE_UINT8_ARRAY(rx, Nano7CodecState, SSP_MAX_XFER),
        VMSTATE_UINT32(tx_len, Nano7CodecState),
        VMSTATE_UINT32(tx_want, Nano7CodecState),
        VMSTATE_UINT32(rx_len, Nano7CodecState),
        VMSTATE_UINT32(rx_pos, Nano7CodecState),
        VMSTATE_BOOL(busy, Nano7CodecState),
        VMSTATE_BUFFER(creg, Nano7CodecState),
        VMSTATE_END_OF_LIST()
    }
};

void nano7_codec_attach(MemoryRegion *parent, hwaddr base, qemu_irq irq)
{
    Nano7CodecState *s = g_new0(Nano7CodecState, 1);

    s->irq = irq;
    s->xfer_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ssp_xfer_done, s);
    nano7_clock_track(s->xfer_timer, false);
    vmstate_register_any(NULL, &vmstate_codec, s);
    memory_region_init_io(&s->mr, NULL, &ssp_ops, s, "nano7.spi0-codec",
                          SSP_SIZE);
    memory_region_add_subregion_overlap(parent, base, &s->mr, 0);
}
