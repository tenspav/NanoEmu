/*
 * nano7_i2s.c -- I2S controller 0 (0x3ca00000, audio out) of the iPod nano
 * 7G (S5L8747) with a PCM sink on the host.
 *
 * RE (Session 27, snap6; docs/research/audio-path.md):
 *   +0x00 ctrl     enable writes 1; shutdown 0x080bdf38 writes 0 and spins
 *                  until bit 1 is SET -> bit 1 always reads 1
 *   +0x04 format   (old & 0x780) | 0x0b000019 (+0x40 for 24-bit words);
 *                  start ORs 0x80          +0x30 = 0x1000 (stored)
 *   +0x08 TX ctrl  start |= 6, stop &= ~4
 *   +0x10 TX FIFO  fed by DMA engine 1 (request line 10): s16 halfwords,
 *                  L/R interleaved
 *   +0x3c status   W1C; bit15 TX underrun, bit16 RX overrun -> reads 0
 *   +0x40 divisor  272 (0x110) = 44.1 kHz, 250 (0xfa) = 48 kHz, ... ~12 MHz /
 *                  divisor; start writes 0x96 first, the rate routine then
 *                  writes the real divisor
 * No IRQ; the DMA engine paces playback, so the divisor sets the DMA rate
 * (nano7_dma_set_fifo_rate, 4 bytes per frame).
 *
 * Sink (audio-out=FILE / run.sh --audio-out): every byte written to the TX
 * FIFO is appended: raw s16le stereo, a WAV header when the name ends in
 * ".wav" (sizes patched at exit), non-blocking for a FIFO (dropped when no
 * reader keeps up). The exit summary gives seconds and peak level, to tell
 * real music from silence.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "nano7_i2s.h"
#include "nano7_dma.h"
#include "migration/vmstate.h"

#define I2S_SIZE        0x1000
#define I2S_TX_FIFO     0x10
#define I2S_BUF         65536

typedef struct Nano7I2sState {
    MemoryRegion mr;
    hwaddr base;
    uint32_t regs[I2S_SIZE / 4];
    uint32_t rate;              /* frames / s */
    int fd;
    bool wav, fifo;
    uint8_t buf[I2S_BUF];
    unsigned buf_len;
    uint64_t bytes, dropped;
    int peak;
} Nano7I2sState;

static Nano7I2sState *i2s_singleton;

static uint32_t i2s_rate_for(uint32_t div)
{
    static const uint32_t rates[] = { 8000, 11025, 12000, 16000, 22050, 24000,
                                      32000, 44100, 48000 };
    uint32_t r = div ? 12000000 / div : 44100, best = 44100, i;

    for (i = 0; i < ARRAY_SIZE(rates); i++) {   /* snap to a standard rate */
        if (abs((int)rates[i] - (int)r) < abs((int)best - (int)r)) {
            best = rates[i];
        }
    }
    return best;
}

static void i2s_wav_header(Nano7I2sState *s)
{
    uint8_t h[44];
    uint32_t data = s->bytes > 0xffffffd3u ? 0xffffffd3u : s->bytes;

    memcpy(h, "RIFF", 4);
    stl_le_p(h + 4, data + 36);
    memcpy(h + 8, "WAVEfmt ", 8);
    stl_le_p(h + 16, 16);
    stw_le_p(h + 20, 1);                /* PCM */
    stw_le_p(h + 22, 2);                /* stereo */
    stl_le_p(h + 24, s->rate);
    stl_le_p(h + 28, s->rate * 4);
    stw_le_p(h + 32, 4);
    stw_le_p(h + 34, 16);
    memcpy(h + 36, "data", 4);
    stl_le_p(h + 40, data);
    if (pwrite(s->fd, h, sizeof(h), 0) != sizeof(h)) {
        fprintf(stderr, "[nano7-i2s] WAV header write failed\n");
    }
}

static void i2s_flush(Nano7I2sState *s)
{
    ssize_t n;

    if (s->fd < 0 || !s->buf_len) {
        s->buf_len = 0;
        return;
    }
    n = write(s->fd, s->buf, s->buf_len);
    if (n < (ssize_t)s->buf_len) {
        s->dropped += s->buf_len - MAX(n, 0);
    }
    s->buf_len = 0;
}

static void i2s_summary(void)
{
    Nano7I2sState *s = i2s_singleton;

    if (!s) {
        return;
    }
    i2s_flush(s);
    if (s->fd >= 0 && s->wav) {
        i2s_wav_header(s);
    }
    fprintf(stderr, "[nano7-i2s] summary: %" PRIu64 " bytes = %.1f s of "
            "%u Hz s16 stereo, peak %d / 32767%s, %" PRIu64 " dropped\n",
            s->bytes, s->rate ? s->bytes / 4.0 / s->rate : 0, s->rate,
            s->peak, s->peak ? "" : " (silence)", s->dropped);
}

static void i2s_set_rate(Nano7I2sState *s)
{
    uint32_t r = i2s_rate_for(s->regs[0x40 / 4]);

    if (r != s->rate) {
        s->rate = r;
        fprintf(stderr, "[nano7-i2s] sample rate %u Hz (divisor %u)\n", r,
                s->regs[0x40 / 4]);
        if (s->fd >= 0 && s->wav) {
            i2s_wav_header(s);
        }
    }
    nano7_dma_set_fifo_rate(s->base + I2S_TX_FIFO, s->rate * 4);
}

static uint64_t i2s_read(void *opaque, hwaddr off, unsigned size)
{
    Nano7I2sState *s = opaque;

    switch (off & ~3) {
    case 0x00:
        return s->regs[0] | 2;          /* idle / reset done */
    case 0x3c:
        return 0;                       /* no underrun / overrun */
    case I2S_TX_FIFO:
        return 0;
    default:
        return s->regs[off / 4];
    }
}

static void i2s_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    Nano7I2sState *s = opaque;
    unsigned i;

    if ((off & ~3) == I2S_TX_FIFO) {
        for (i = 0; i < size; i++) {
            s->buf[s->buf_len++] = v >> (8 * i);
            if (s->buf_len == I2S_BUF) {
                i2s_flush(s);
            }
        }
        if (size == 2) {                /* track the level of real audio */
            int smp = abs((int16_t)v);

            s->peak = MAX(s->peak, smp);
        }
        s->bytes += size;
        return;
    }
    if (off == 0x3c) {
        return;                         /* W1C of always-clear bits */
    }
    s->regs[off / 4] = v;
    if (off == 0x40) {
        i2s_set_rate(s);
    }
}

static const MemoryRegionOps i2s_ops = {
    .read = i2s_read,
    .write = i2s_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static int i2s_post_load(void *opaque, int version_id)
{
    i2s_set_rate(opaque);
    return 0;
}

static const VMStateDescription vmstate_i2s = {
    .name = "nano7-i2s",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = i2s_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Nano7I2sState, I2S_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

void nano7_i2s_attach(MemoryRegion *parent, hwaddr base,
                      const char *audio_out)
{
    Nano7I2sState *s = g_new0(Nano7I2sState, 1);
    struct stat st;

    s->base = base;
    s->fd = -1;
    s->rate = 44100;
    if (audio_out) {
        s->fifo = stat(audio_out, &st) == 0 && S_ISFIFO(st.st_mode);
        /* O_RDWR on a FIFO never blocks waiting for a reader */
        s->fd = open(audio_out, s->fifo ? O_RDWR | O_NONBLOCK :
                     O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (s->fd < 0) {
            fprintf(stderr, "[nano7-i2s] audio-out %s: %s\n", audio_out,
                    strerror(errno));
            exit(1);
        }
        s->wav = !s->fifo && g_str_has_suffix(audio_out, ".wav");
        if (s->wav) {
            i2s_wav_header(s);
            lseek(s->fd, 44, SEEK_SET);
        }
        fprintf(stderr, "[nano7-i2s] audio-out %s (%s)\n", audio_out,
                s->fifo ? "FIFO, raw s16le stereo" : s->wav ? "WAV" :
                "raw s16le stereo");
    }
    i2s_singleton = s;
    atexit(i2s_summary);
    nano7_dma_set_fifo_rate(base + I2S_TX_FIFO, s->rate * 4);
    vmstate_register_any(NULL, &vmstate_i2s, s);
    memory_region_init_io(&s->mr, NULL, &i2s_ops, s, "nano7.i2s0", I2S_SIZE);
    memory_region_add_subregion_overlap(parent, base, &s->mr, 0);
}
