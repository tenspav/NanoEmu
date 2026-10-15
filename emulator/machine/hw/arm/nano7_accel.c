/*
 * nano7_accel.c -- the iPod nano 7G accelerometer: an ST LIS3DH on I2C1,
 * 7-bit address 0x18 (docs/research/accel.md: WHO_AM_I 0x0f = 0x33 probed
 * at 0x08147ff0; the OS's "Accelerometer" task samples it every 10 ms and
 * keeps the last sample, scaled to 1/65536 g, at 0x08a91e50, which apps
 * such as the Mario Kart 64 port read for tilt steering).
 *
 * How the OS reads it (Session 66 RE): the chip's interrupt reaches the CPU
 * through the PMU (event status reg4 bit4 -> mask 0x2000); the PMU event
 * thread then drains the FIFO (0x081838e4): FIFO_SRC (0x2f) bits 4:0 =
 * samples, and per sample 5 bytes from 0xa9 (auto-increment) of which it
 * keeps the high bytes OUT_X_H / Y_H / Z_H as signed 8-bit counts at +-4 g
 * (32 per g), minus calibration offsets, through the axis matrix (Z
 * negated in snap6), times 2048 into the sample at 0x08a91e50. Set-up:
 * CTRL1 0x57 (100 Hz), FIFO_CTRL 0x85 (stream, watermark 5), CTRL5 FIFO
 * enable; CTRL3 is not used.
 * Model: WHO_AM_I, STATUS (new data), OUT_X_L..OUT_Z_H (16-bit
 * left-justified, 1 g = 0x2000 at +-4 g) from the emulated orientation, a
 * FIFO filled at 100 Hz (up to 32 samples, all the current orientation)
 * whose FIFO_SRC counts them; reading OUT_Z_H pops one. Reaching the
 * watermark (5: every 50 ms) raises the PMU event, and the OS drains all
 * waiting samples (one event per sample cost ~16 % idle CPU). Sub-address bit 7 = auto-increment, as on the
 * chip (OUT wraps 0x2d -> 0x28).
 *
 * Orientation: the device held in landscape, screen tilted 30 degrees
 * towards the player (gravity (-0.5, 0, 0.87) g as the OS sees it; the
 * chip's Z is the opposite), rotated
 * like a steering wheel by the tilt angle: Left / Right arrow held (outside
 * the F8 pad mode) ramps it to -/+ 30 degrees in 0.25 s and releasing
 * brings it back; NANO7_TILT=deg pins it (tests). NANO7_ACCEL=0 stops the
 * samples and their PMU events (the chip then answers but never has data,
 * as the old plain register file did). NANO7_ACCEL_TRACE=1 logs
 * the register traffic and the OS's sample once a second.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/timer.h"
#include "migration/vmstate.h"
#include "exec/address-spaces.h"
#include "ui/input.h"
#include "nano7_accel.h"
#include "nano7_clock.h"
#include "nano7_pmu.h"

#define ACC_ADDR        0x18
#define ACC_TICK_NS     10000000        /* 10 ms */
#define ACC_MAX_DEG     35.0            /* MK64: full lock at ~0.45 g */
#define ACC_FIFO        32
#define ACC_RAMP_DEG    (ACC_MAX_DEG / 25)      /* per tick: 0.25 s */
#define ACC_PITCH_DEG   30.0
#define ACC_OS_SAMPLE   0x08a91e50u

typedef struct Nano7AccelState {
    uint8_t regs[256];
    uint8_t ptr;
    bool have_ptr, autoinc;
    QEMUTimer *timer;
    int32_t tilt_mdeg;          /* current, 1/1000 degree */
    int keys;                   /* bit 0 left, bit 1 right */
    bool fixed, trace;
    bool irq_off;               /* NANO7_ACCEL=0: no samples, no events */
    unsigned ticks;
    uint8_t fifo;               /* unread samples */
} Nano7AccelState;

static Nano7AccelState *acc;

static void acc_update_out(Nano7AccelState *s)
{
    double t = s->tilt_mdeg / 1000.0 * M_PI / 180;
    double p = ACC_PITCH_DEG * M_PI / 180;
    double g[3] = { -sin(p), sin(t) * cos(p), -cos(t) * cos(p) };
    int i;

    for (i = 0; i < 3; i++) {
        int16_t v = (int16_t)MAX(-32768, MIN(32767, lrint(g[i] * 8192)));

        s->regs[0x28 + 2 * i] = v & 0xff;
        s->regs[0x29 + 2 * i] = (uint16_t)v >> 8;
    }
}

/* FIFO_SRC: WTM 7, OVRN 6 (full), EMPTY 5, FSS 4:0 */
static void acc_fifo_src(Nano7AccelState *s)
{
    s->regs[0x2f] = MIN(s->fifo, 31) |
                    (s->fifo && s->fifo >= (s->regs[0x2e] & 0x1f) ? 0x80 : 0) |
                    (s->fifo == ACC_FIFO ? 0x40 : 0) | (s->fifo ? 0 : 0x20);
}

static void acc_tick(void *opaque)
{
    Nano7AccelState *s = opaque;
    int target = (s->keys & 1 ? -1 : 0) + (s->keys & 2 ? 1 : 0);
    int32_t want = target * (int32_t)(ACC_MAX_DEG * 1000);
    int32_t step = ACC_RAMP_DEG * 1000;

    if (!s->fixed && s->tilt_mdeg != want) {
        s->tilt_mdeg = s->tilt_mdeg < want ? MIN(want, s->tilt_mdeg + step)
                                           : MAX(want, s->tilt_mdeg - step);
        acc_update_out(s);
    }
    /* powered (CTRL1 ODR), FIFO enabled and not in bypass: a new sample */
    if (!s->irq_off && (s->regs[0x20] & 0xf0) && (s->regs[0x24] & 0x40) &&
        (s->regs[0x2e] & 0xc0)) {
        s->fifo = MIN(s->fifo + 1, ACC_FIFO);
        acc_fifo_src(s);
        if (s->fifo >= MAX(1, s->regs[0x2e] & 0x1f)) {
            nano7_pmu_raise(4, 1u << 4);  /* watermark: INT -> PMU 0x2000 */
        }
    }
    if (s->trace && ++s->ticks % 100 == 0) {
        int32_t v[3];
        int i;

        for (i = 0; i < 3; i++) {
            v[i] = address_space_ldl_le(&address_space_memory,
                                        ACC_OS_SAMPLE + 4 * i,
                                        MEMTXATTRS_UNSPECIFIED, NULL);
        }
        fprintf(stderr, "[nano7-accel] tilt %.1f deg, OS sample %d %d %d "
                "(%.3f %.3f %.3f g)\n", s->tilt_mdeg / 1000.0, v[0], v[1],
                v[2], v[0] / 65536.0, v[1] / 65536.0, v[2] / 65536.0);
    }
    nano7_timer_mod(s->timer, nano7_now_ns() + ACC_TICK_NS);
}

static void acc_start(void *opaque, bool rx)
{
    Nano7AccelState *s = opaque;

    if (!rx) {
        s->have_ptr = false;
    }
}

static bool acc_write(void *opaque, uint8_t v)
{
    Nano7AccelState *s = opaque;

    if (!s->have_ptr) {
        s->ptr = v & 0x7f;
        s->autoinc = v & 0x80;
        s->have_ptr = true;
    } else {
        if (s->trace) {
            fprintf(stderr, "[nano7-accel] write reg 0x%02x = 0x%02x\n",
                    s->ptr, v);
        }
        if (s->ptr != 0x0f && (s->ptr < 0x27 || s->ptr > 0x2d)) {
            s->regs[s->ptr] = v;        /* read-only registers keep */
        }
        if (s->autoinc) {
            s->ptr = (s->ptr + 1) & 0x7f;
        }
    }
    return true;
}

static uint8_t acc_read(void *opaque)
{
    Nano7AccelState *s = opaque;
    uint8_t v = s->regs[s->ptr];

    if (s->trace && (s->ptr < 0x28 || s->ptr > 0x2d)) {
        fprintf(stderr, "[nano7-accel] read reg 0x%02x = 0x%02x\n", s->ptr,
                v);
    }
    if (s->ptr == 0x2d && s->fifo) {    /* one sample read */
        s->fifo--;
        acc_fifo_src(s);
    }
    if (s->autoinc) {
        s->ptr = s->ptr == 0x2d ? 0x28 : (s->ptr + 1) & 0x7f;
    }
    return v;
}

bool nano7_accel_key(int qcode, bool down)
{
    int bit = qcode == Q_KEY_CODE_LEFT ? 1 : qcode == Q_KEY_CODE_RIGHT ? 2 : 0;

    if (!acc || !bit) {
        return false;
    }
    acc->keys = down ? acc->keys | bit : acc->keys & ~bit;
    return true;
}

static int acc_post_load(void *opaque, int version_id)
{
    acc_update_out(opaque);
    return 0;
}

static const VMStateDescription vmstate_accel = {
    .name = "nano7-accel",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = acc_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, Nano7AccelState, 256),
        VMSTATE_UINT8(ptr, Nano7AccelState),
        VMSTATE_BOOL(have_ptr, Nano7AccelState),
        VMSTATE_BOOL(autoinc, Nano7AccelState),
        VMSTATE_INT32(tilt_mdeg, Nano7AccelState),
        VMSTATE_UINT8(fifo, Nano7AccelState),
        VMSTATE_TIMER_PTR(timer, Nano7AccelState),
        VMSTATE_END_OF_LIST()
    }
};

void nano7_accel_attach(Nano7I2CBus *bus)
{
    Nano7AccelState *s = g_new0(Nano7AccelState, 1);
    Nano7I2CSlave sl = {
        .addr = ACC_ADDR, .opaque = s,
        .start = acc_start, .write = acc_write, .read = acc_read,
    };
    const char *t = getenv("NANO7_TILT");

    s->regs[0x0f] = 0x33;       /* WHO_AM_I: LIS3DH */
    s->regs[0x27] = 0x0f;       /* STATUS: ZYXDA + X / Y / Z new data */
    s->regs[0x2f] = 0x20;       /* FIFO_SRC: empty */
    /* as the OS left the chip before the snapshot (a snapshot resume never
     * re-runs its set-up, 0x0815fbdc): 100 Hz, +-4 g, FIFO stream mode,
     * watermark 5 */
    s->regs[0x20] = 0x57;
    s->regs[0x23] = 0x10;
    s->regs[0x24] = 0x40;
    s->regs[0x2e] = 0x85;
    s->irq_off = getenv("NANO7_ACCEL") && !strcmp(getenv("NANO7_ACCEL"), "0");
    s->trace = getenv("NANO7_ACCEL_TRACE") &&
               strcmp(getenv("NANO7_ACCEL_TRACE"), "0");
    if (t && *t) {
        s->fixed = true;
        s->tilt_mdeg = lrint(g_ascii_strtod(t, NULL) * 1000);
    }
    acc_update_out(s);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, acc_tick, s);
    nano7_clock_track(s->timer, true);
    nano7_timer_mod(s->timer, nano7_now_ns() + ACC_TICK_NS);
    vmstate_register_any(NULL, &vmstate_accel, s);
    nano7_i2c_add_slave(bus, &sl);
    acc = s;
}
