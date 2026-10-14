/*
 * nano7_adsp.c -- the media decode co-processor at 0x38500000 (VIC IRQ 18)
 * of the iPod nano 7G (S5L8747). The MeCCADecode thread hands it compressed
 * frames through an IRAM mailbox and reads PCM back.
 *
 * RE (Session 27, snap6):
 *   +0xc80  ID, must read 0x004e7309 or the decoder panics (-> power-off)
 *   +0x110  port state command: 4 = stop -> +0x114 & 7 reads 7 (polled by
 *           0x083f404a); 0x10 / 0x20 / 0x40 / 0x60 = run -> reads 0. The
 *           frame feed 0x0814959c / 0x084d47a4 only queues input while it
 *           reads 0 (a stuck 7 = no input at all, only doorbells)
 *   +0x100  input queue head: the descriptor address | 1 (system memory),
 *           24-byte descriptor {next, len << 16 | 0xf80, src, -, out, out}
 *           built by 0x084d4680 at [codec obj + 0x40e8]; src = the frame in
 *           DRAM (0x80000000 alias), one MP3 frame per feed
 *   +0x400  status (bits 1:0), +0x420 16-word command window
 *   +0x938..+0x97c  18 queue descriptors (stored)
 *   +0x984 / +0x988 / +0x99c job parameters, +0x98c = 1 start doorbell
 *   interrupt group 0: +0xa8c set-enable, +0xa90 clear-enable, +0xa94
 *           mask readback, +0xa98 pending (27 bits), +0xc48 ack (W1C);
 *           group 1 at +0xb0c..+0xb18, same layout
 *   pending bit 2 = command / frame done (callers 0x082ad07c, 0x082ad8d8
 *           poll it with the IRQ masked; the ISR 0x0815971d acks it and
 *           posts the decoder event, which has a 500 ms timeout);
 *   bit 12 = data request (ISR re-arms with +0x804 / +0x858) - never raised
 * Mailbox in IRAM: commands 0x2203ff00, status halfwords 0x2203ff28, input
 * 0x22038000, PCM output 0x22038100 (pointer struct 0x08811308).
 *
 * Result protocol (parser 0x082ad500, after the 500 ms semaphore wait in
 * DecodeFrame 0x082acca0): status halfwords at 0x2203ff28: h0 bit0 = 1 and
 * h1 = 0 = frame decoded (h1 = 2: MP3 needs more input); 7 empty results
 * in a row end playback. PCM descriptor at 0x22038000: h[1] mode (1 =
 * single buffer), h[2] capacity in halfwords, entry 0 ready flag at +0xa,
 * length (halfwords) at +0xc; samples at 0x22038100. Nothing on the ARM
 * side writes the descriptor. Config jobs (+0x984 = 0) only wait for bit 2;
 * each frame is the input descriptor at +0x100 plus the +0x98c = 1
 * doorbell. +0x1000.. is the DRAM controller (periodic writes, stored).
 *
 * Model (host-side decode, HLE): every job write completes after 20 us of
 * guest time and raises pending bit 2. At a +0x98c doorbell the queued
 * frame is read from guest RAM and decoded with minimp3 (minimp3.h, CC0,
 * github.com/lieff/minimp3 @ ea99364f); the PCM goes to 0x22038100 with a
 * success status, or h1 = 2 ("need more input") while the decoder syncs /
 * fills its bit reservoir. Doorbells without input (or MP3 decoding off:
 * NANO7_ADSP_SILENT=1) return one MP3 frame of silence. The decoder is
 * reset on a config job and on a port stop.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "nano7_adsp.h"
#include "nano7_clock.h"
#include "qemu/timer.h"
#include "exec/address-spaces.h"
#include "migration/vmstate.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "minimp3.h"

#define ADSP_SIZE       0x2000
#define ADSP_ID         0x004e7309
#define ADSP_JOB_NS     20000
#define PEND_DONE       (1u << 2)
#define PEND_MASK       0x07ffffffu

#define MBOX_STATUS     0x2203ff28      /* result halfwords */
#define MBOX_PCM_DESC   0x22038000
#define MBOX_PCM        0x22038100
#define FRAME_HALFWORDS (1152 * 2)      /* one MP3 frame, s16 stereo */
#define IN_BUF          16384           /* compressed bytes awaiting decode */
#define DESC_CTRL_TAG   0xf80

typedef struct Nano7AdspState {
    MemoryRegion mr;
    uint32_t regs[ADSP_SIZE / 4];
    uint32_t mask[2], pending[2];
    qemu_irq irq;
    QEMUTimer *job_timer;
    uint64_t jobs, frames;
    unsigned log_lines;
    uint32_t port_state;        /* +0x114 & 7 */
    /* host-side MP3 decoder (not migrated: reset on load) */
    mp3dec_t dec;
    uint8_t in[IN_BUF];
    unsigned in_len;
    bool silent, trace;        /* NANO7_ADSP_SILENT / NANO7_ADSP_TRACE */
    uint64_t decoded, starved, in_bytes;
    unsigned rate, channels;
} Nano7AdspState;

static Nano7AdspState *adsp_singleton;

static void adsp_update(Nano7AdspState *s)
{
    qemu_set_irq(s->irq, ((s->pending[0] & s->mask[0]) |
                          (s->pending[1] & s->mask[1])) != 0);
}

static void adsp_job_done(void *opaque)
{
    Nano7AdspState *s = opaque;

    s->pending[0] |= PEND_DONE;
    adsp_update(s);
}

static void adsp_iram_w16(hwaddr a, uint16_t v)
{
    uint16_t le = cpu_to_le16(v);

    address_space_write(&address_space_memory, a, MEMTXATTRS_UNSPECIFIED,
                        &le, 2);
}

static void adsp_dec_reset(Nano7AdspState *s)
{
    mp3dec_init(&s->dec);
    s->in_len = 0;
}

/* result: n halfwords of PCM at MBOX_PCM (0 with err 2 = need more input) */
static void adsp_result(Nano7AdspState *s, const int16_t *pcm, unsigned n,
                        uint16_t err)
{
    if (n) {
        uint16_t le[MINIMP3_MAX_SAMPLES_PER_FRAME];
        unsigned i;

        for (i = 0; i < n; i++) {
            le[i] = cpu_to_le16(pcm[i]);
        }
        address_space_write(&address_space_memory, MBOX_PCM,
                            MEMTXATTRS_UNSPECIFIED, le, n * 2);
    }
    adsp_iram_w16(MBOX_PCM_DESC + 2, 1);                /* single buffer */
    adsp_iram_w16(MBOX_PCM_DESC + 4, FRAME_HALFWORDS);  /* capacity */
    adsp_iram_w16(MBOX_PCM_DESC + 0xa, n != 0);         /* entry 0 ready */
    adsp_iram_w16(MBOX_PCM_DESC + 0xc, n);
    adsp_iram_w16(MBOX_STATUS, 1);                      /* valid */
    adsp_iram_w16(MBOX_STATUS + 2, err);
}

/* append the queued input descriptor's bytes to the decoder buffer */
static bool adsp_take_input(Nano7AdspState *s)
{
    uint32_t head = s->regs[0x100 / 4], w[3], src, len;

    if (!(head & 1)) {
        return false;
    }
    s->regs[0x100 / 4] = 0;                     /* consumed */
    address_space_read(&address_space_memory, head & 0x7ffffffc,
                       MEMTXATTRS_UNSPECIFIED, w, sizeof(w));
    if ((le32_to_cpu(w[1]) & 0xffff) != DESC_CTRL_TAG) {
        return false;
    }
    len = le32_to_cpu(w[1]) >> 16;
    src = le32_to_cpu(w[2]) & 0x7fffffff;
    if (len > IN_BUF - s->in_len) {             /* resync on overflow */
        s->in_len = 0;
        len = MIN(len, IN_BUF);
    }
    address_space_read(&address_space_memory, src, MEMTXATTRS_UNSPECIFIED,
                       s->in + s->in_len, len);
    s->in_len += len;
    s->in_bytes += len;
    return true;
}

/* frame doorbell: decode the queued frame (or return silence) */
static void adsp_frame(Nano7AdspState *s)
{
    static const int16_t zeros[FRAME_HALFWORDS];
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    mp3dec_frame_info_t info;
    int samples;

    s->frames++;
    if (!adsp_take_input(s) || s->silent) {
        adsp_result(s, zeros, FRAME_HALFWORDS, 0);
        return;
    }
    samples = mp3dec_decode_frame(&s->dec, s->in, s->in_len, pcm, &info);
    if (info.frame_bytes > 0) {
        s->in_len -= MIN((unsigned)info.frame_bytes, s->in_len);
        memmove(s->in, s->in + info.frame_bytes, s->in_len);
    }
    if (samples <= 0) {                 /* syncing / bit reservoir / ID3 */
        s->starved++;
        adsp_result(s, NULL, 0, 2);
        return;
    }
    if (info.hz != (int)s->rate || info.channels != (int)s->channels) {
        s->rate = info.hz;
        s->channels = info.channels;
        fprintf(stderr, "[nano7-adsp] MP3 %d Hz, %d ch, %d kbps\n",
                info.hz, info.channels, info.bitrate_kbps);
    }
    s->decoded++;
    adsp_result(s, pcm, samples * info.channels, 0);
}

static void adsp_job(Nano7AdspState *s, hwaddr off, uint32_t v)
{
    s->jobs++;
    if (s->log_lines < 16) {
        s->log_lines++;
        fprintf(stderr, "[nano7-adsp] t=%.3f s job #%" PRIu64 " (+0x%03x = "
                "0x%x)\n", nano7_now_ns() / 1e9, s->jobs, (unsigned)off, v);
    }
    nano7_timer_mod(s->job_timer, nano7_now_ns() + ADSP_JOB_NS);
}

static uint64_t adsp_read(void *opaque, hwaddr off, unsigned size)
{
    Nano7AdspState *s = opaque;

    switch (off) {
    case 0xc80:
        return ADSP_ID;
    case 0x114:
        return s->port_state;
    case 0xa94:
        return s->mask[0];
    case 0xa98:
        return s->pending[0];
    case 0xb14:
        return s->mask[1];
    case 0xb18:
        return s->pending[1];
    default:
        return s->regs[off / 4];
    }
}

static void adsp_write(void *opaque, hwaddr off, uint64_t value,
                       unsigned size)
{
    Nano7AdspState *s = opaque;
    uint32_t v = value;

    if (s->trace && s->frames < 4) {    /* first frames' register writes */
        fprintf(stderr, "[nano7-adsp] t=%.6f W +0x%03x = 0x%08x\n",
                nano7_now_ns() / 1e9, (unsigned)off, v);
    }
    switch (off) {
    case 0xa8c:
        s->mask[0] |= v & PEND_MASK;
        break;
    case 0xa90:
        s->mask[0] &= ~v;
        break;
    case 0xb0c:
        s->mask[1] |= v & PEND_MASK;
        break;
    case 0xb10:
        s->mask[1] &= ~v;
        break;
    case 0xc48:
        s->pending[0] &= ~v;
        break;
    case 0x110:                         /* port command: 4 = stop */
        s->port_state = v == 4 ? 7 : 0;
        if (v == 4) {
            adsp_dec_reset(s);
        }
        break;
    case 0x984:                         /* config job: new stream */
        adsp_dec_reset(s);
        adsp_job(s, off, v);
        break;
    case 0x98c:
        if (v & 1) {
            adsp_frame(s);
            adsp_job(s, off, v);
        }
        break;
    }
    s->regs[off / 4] = v;
    adsp_update(s);
}

static const MemoryRegionOps adsp_ops = {
    .read = adsp_read,
    .write = adsp_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void adsp_summary(void)
{
    if (adsp_singleton && adsp_singleton->jobs) {
        Nano7AdspState *s = adsp_singleton;

        fprintf(stderr, "[nano7-adsp] summary: %" PRIu64 " jobs, %" PRIu64
                " frame doorbells: %" PRIu64 " MP3 frames decoded from %"
                PRIu64 " input bytes, %" PRIu64 " need-more-input%s\n",
                s->jobs, s->frames, s->decoded, s->in_bytes, s->starved,
                s->silent ? " (NANO7_ADSP_SILENT)" : "");
    }
}

static int adsp_post_load(void *opaque, int version_id)
{
    adsp_dec_reset(opaque);
    adsp_update(opaque);
    return 0;
}

static const VMStateDescription vmstate_adsp = {
    .name = "nano7-adsp",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = adsp_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Nano7AdspState, ADSP_SIZE / 4),
        VMSTATE_UINT32_ARRAY(mask, Nano7AdspState, 2),
        VMSTATE_UINT32_ARRAY(pending, Nano7AdspState, 2),
        VMSTATE_TIMER_PTR(job_timer, Nano7AdspState),
        VMSTATE_UINT64(jobs, Nano7AdspState),
        VMSTATE_UINT64(frames, Nano7AdspState),
        VMSTATE_UINT32(port_state, Nano7AdspState),
        VMSTATE_END_OF_LIST()
    }
};

void nano7_adsp_attach(MemoryRegion *parent, hwaddr base, qemu_irq irq)
{
    Nano7AdspState *s = g_new0(Nano7AdspState, 1);

    s->irq = irq;
    s->silent = getenv("NANO7_ADSP_SILENT") != NULL;
    s->trace = getenv("NANO7_ADSP_TRACE") != NULL;
    adsp_dec_reset(s);
    s->job_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, adsp_job_done, s);
    nano7_clock_track(s->job_timer, true);
    adsp_singleton = s;
    atexit(adsp_summary);
    vmstate_register_any(NULL, &vmstate_adsp, s);
    memory_region_init_io(&s->mr, NULL, &adsp_ops, s, "nano7.adsp",
                          ADSP_SIZE);
    memory_region_add_subregion_overlap(parent, base, &s->mr, 0);
}
