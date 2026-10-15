/*
 * nano7_hostpad.c -- a real gamepad on the host drives the fake Bluetooth
 * HID gamepad (nano7_btuart.c), so homebrew games can be played in the
 * emulator with the pad they will meet on the device.
 *
 * NANO7_HOST_PAD (run.sh --host-pad) = "auto" (the first /dev/input/event*
 * with gamepad buttons and two axes; checked again every 2 s until one
 * shows up, and after it is unplugged) or a path: an evdev node, or a FIFO /
 * file carrying the same struct input_event records (tests, bridges).
 * Linux evdev only - QEMU here is built without SDL; on other hosts it says
 * so and does nothing. The node must be readable (group "input" on most
 * distributions).
 *
 * Mapping (Linux gamepad API, Documentation/input/gamepad.rst; positions,
 * Xbox naming) onto NanoPad's report (buttons 1-16, X Y Z Rz):
 *   BTN_SOUTH = A (1)  EAST = B (2)  WEST = X (3)  NORTH = Y (4)
 *   TL = L (5)  TR = R (6)  SELECT (7)  START (8)  MODE = home (9)
 *   THUMBL (10)  THUMBR (11)  TL2 (12)  TR2 (13)
 *   ABS_X / ABS_Y = left stick X / Y; right stick = ABS_RX / ABS_RY when the
 *   pad has them (Xbox-style: ABS_Z / ABS_RZ are then the analog triggers,
 *   which also press TL2 / TR2 past half way), else ABS_Z / ABS_RZ
 *   (generic HID pads); D-pad (ABS_HAT0X / HAT0Y or BTN_DPAD_*) = left stick
 *   at full deflection, added to it.
 * Axes are scaled from the device's range to -127..127 with its flat zone
 * (EVIOCGABS; a FIFO gets -32768..32767, flat 4096). One report per
 * SYN_REPORT that changed the state.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "nano7_hostpad.h"
#include "nano7_btuart.h"

#ifdef __linux__
#include <linux/input.h>

#define HP_RETRY_MS     2000

typedef struct HostPad {
    char *spec;                 /* "auto" or a path */
    int fd;
    QEMUTimer *retry;
    bool rx_right;              /* right stick on ABS_RX / RY */
    struct { int min, max, flat; } abs[ABS_CNT];
    uint16_t buttons;
    int8_t stick[4];            /* X Y Z Rz from the sticks */
    int8_t hat[2];
    uint16_t trig;              /* TL2 / TR2 from analog triggers */
    uint16_t sent_buttons;
    int8_t sent_axis[4];
    bool sent_valid;
} HostPad;

static HostPad hp = { .fd = -1 };

static bool hp_bit(const unsigned long *bits, int n)
{
    return bits[n / (8 * sizeof(long))] >> (n % (8 * sizeof(long))) & 1;
}

/* EV_KEY with gamepad buttons and EV_ABS with X / Y: a gamepad */
static bool hp_is_pad(int fd)
{
    unsigned long key[KEY_CNT / (8 * sizeof(long)) + 1] = { 0 };
    unsigned long abs[ABS_CNT / (8 * sizeof(long)) + 1] = { 0 };

    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key)), key) < 0 ||
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs) < 0) {
        return false;
    }
    return (hp_bit(key, BTN_SOUTH) || hp_bit(key, BTN_JOYSTICK)) &&
           hp_bit(abs, ABS_X) && hp_bit(abs, ABS_Y);
}

static void hp_setup(int fd)
{
    unsigned long abs[ABS_CNT / (8 * sizeof(long)) + 1] = { 0 };
    char name[128] = "?";
    bool evdev = ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs) >= 0;
    int i;

    for (i = 0; i < ABS_CNT; i++) {
        struct input_absinfo ai;

        hp.abs[i].min = -32768;
        hp.abs[i].max = 32767;
        hp.abs[i].flat = 4096;
        if (evdev && hp_bit(abs, i) &&
            ioctl(fd, EVIOCGABS(i), &ai) >= 0 && ai.maximum > ai.minimum) {
            hp.abs[i].min = ai.minimum;
            hp.abs[i].max = ai.maximum;
            hp.abs[i].flat = ai.flat;
        }
    }
    hp.rx_right = !evdev || hp_bit(abs, ABS_RX);
    if (evdev) {
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
    }
    hp.buttons = hp.trig = 0;
    memset(hp.stick, 0, sizeof(hp.stick));
    memset(hp.hat, 0, sizeof(hp.hat));
    info_report("nano7-hostpad: %s \"%s\" (right stick on %s)",
                evdev ? "gamepad" : "event stream", name,
                hp.rx_right ? "RX / RY" : "Z / RZ");
}

/* -127..127 around the range's centre, 0 inside the flat zone */
static int8_t hp_scale(int code, int v)
{
    double c = (hp.abs[code].min + hp.abs[code].max) / 2.0;
    double h = (hp.abs[code].max - hp.abs[code].min) / 2.0;

    if (fabs(v - c) <= hp.abs[code].flat) {
        return 0;
    }
    return (int8_t)MAX(-127, MIN(127, lrint((v - c) / h * 127)));
}

/* 0..1 for a trigger */
static double hp_unit(int code, int v)
{
    return (double)(v - hp.abs[code].min) /
           (hp.abs[code].max - hp.abs[code].min);
}

static void hp_key(int code, int value)
{
    static const struct { int code, bit; } map[] = {
        { BTN_SOUTH, 0 }, { BTN_EAST, 1 }, { BTN_WEST, 2 },
        { BTN_NORTH, 3 }, { BTN_TL, 4 }, { BTN_TR, 5 },
        { BTN_SELECT, 6 }, { BTN_START, 7 }, { BTN_MODE, 8 },
        { BTN_THUMBL, 9 }, { BTN_THUMBR, 10 }, { BTN_TL2, 11 },
        { BTN_TR2, 12 },
    };
    unsigned i;

    switch (code) {             /* D-pad buttons -> the hat */
    case BTN_DPAD_LEFT:  hp.hat[0] = value ? -1 : 0; return;
    case BTN_DPAD_RIGHT: hp.hat[0] = value ? 1 : 0;  return;
    case BTN_DPAD_UP:    hp.hat[1] = value ? -1 : 0; return;
    case BTN_DPAD_DOWN:  hp.hat[1] = value ? 1 : 0;  return;
    }
    for (i = 0; i < ARRAY_SIZE(map); i++) {
        if (map[i].code == code) {
            hp.buttons = value ? hp.buttons | 1u << map[i].bit
                               : hp.buttons & ~(1u << map[i].bit);
        }
    }
}

static void hp_abs(int code, int value)
{
    switch (code) {
    case ABS_X:
        hp.stick[0] = hp_scale(code, value);
        break;
    case ABS_Y:
        hp.stick[1] = hp_scale(code, value);
        break;
    case ABS_RX:
    case ABS_RY:
        if (hp.rx_right) {
            hp.stick[code == ABS_RX ? 2 : 3] = hp_scale(code, value);
        }
        break;
    case ABS_Z:
    case ABS_RZ:
        if (!hp.rx_right) {
            hp.stick[code == ABS_Z ? 2 : 3] = hp_scale(code, value);
        } else {                /* analog triggers -> TL2 / TR2 */
            unsigned bit = 1u << (code == ABS_Z ? 11 : 12);

            hp.trig = hp_unit(code, value) > .5 ? hp.trig | bit
                                                 : hp.trig & ~bit;
        }
        break;
    case ABS_HAT0X:
        hp.hat[0] = value < 0 ? -1 : value > 0;
        break;
    case ABS_HAT0Y:
        hp.hat[1] = value < 0 ? -1 : value > 0;
        break;
    }
}

static void hp_sync(void)
{
    uint16_t buttons = hp.buttons | hp.trig;
    int8_t axis[4];
    int i;

    for (i = 0; i < 4; i++) {
        int v = hp.stick[i] + (i < 2 ? hp.hat[i] * 127 : 0);

        axis[i] = MAX(-127, MIN(127, v));
    }
    if (hp.sent_valid && buttons == hp.sent_buttons &&
        !memcmp(axis, hp.sent_axis, 4)) {
        return;
    }
    hp.sent_valid = true;
    hp.sent_buttons = buttons;
    memcpy(hp.sent_axis, axis, 4);
    nano7_btuart_pad_host(buttons, axis);
}

static void hp_close(const char *why)
{
    static const int8_t zero[4];

    qemu_set_fd_handler(hp.fd, NULL, NULL, NULL);
    close(hp.fd);
    hp.fd = -1;
    info_report("nano7-hostpad: %s - released, retrying", why);
    hp.sent_valid = false;
    nano7_btuart_pad_host(0, zero);
    timer_mod(hp.retry, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
              HP_RETRY_MS);
}

static void hp_read(void *opaque)
{
    struct input_event ev[64];
    ssize_t n;
    int i;

    for (;;) {
        n = read(hp.fd, ev, sizeof(ev));
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            return;
        }
        if (n <= 0) {           /* unplugged, or the FIFO's writer left */
            hp_close(n ? "device gone" : "end of stream");
            return;
        }
        for (i = 0; i < n / (ssize_t)sizeof(ev[0]); i++) {
            if (ev[i].type == EV_KEY) {
                hp_key(ev[i].code, ev[i].value);
            } else if (ev[i].type == EV_ABS && ev[i].code < ABS_CNT) {
                hp_abs(ev[i].code, ev[i].value);
            } else if (ev[i].type == EV_SYN && ev[i].code == SYN_REPORT) {
                hp_sync();
            }
        }
    }
}

static int hp_open_auto(void)
{
    g_autoptr(GDir) dir = g_dir_open("/dev/input", 0, NULL);
    const char *e;
    bool denied = false;
    static bool told;

    while (dir && (e = g_dir_read_name(dir))) {
        g_autofree char *path = g_strdup_printf("/dev/input/%s", e);
        int fd;

        if (!g_str_has_prefix(e, "event")) {
            continue;
        }
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            denied |= errno == EACCES;
            continue;
        }
        if (hp_is_pad(fd)) {
            return fd;
        }
        close(fd);
    }
    if (!told) {
        told = true;
        info_report("nano7-hostpad: no gamepad yet%s; checking every %d s",
                    denied ? " (some /dev/input nodes are not readable: add "
                    "yourself to the \"input\" group)" : "",
                    HP_RETRY_MS / 1000);
    }
    return -1;
}

static void hp_try_open(void *opaque)
{
    int fd;

    if (!strcmp(hp.spec, "auto")) {
        fd = hp_open_auto();
    } else {
        struct stat st;
        /* a FIFO opened read-write never sees end-of-stream when its
         * writer leaves (a bridge can reconnect) */
        bool fifo = !stat(hp.spec, &st) && S_ISFIFO(st.st_mode);

        fd = open(hp.spec, (fifo ? O_RDWR : O_RDONLY) | O_NONBLOCK |
                  O_CLOEXEC);
        if (fd < 0 && errno != ENOENT && errno != ENXIO) {
            warn_report("nano7-hostpad: %s: %s", hp.spec, strerror(errno));
        }
    }
    if (fd < 0) {
        timer_mod(hp.retry, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                  HP_RETRY_MS);
        return;
    }
    hp.fd = fd;
    hp_setup(fd);
    qemu_set_fd_handler(fd, hp_read, NULL, NULL);
}

void nano7_hostpad_init(void)
{
    const char *e = getenv("NANO7_HOST_PAD");

    if (!e || !*e || !strcmp(e, "none")) {
        return;
    }
    hp.spec = g_strdup(e);
    hp.retry = timer_new_ms(QEMU_CLOCK_REALTIME, hp_try_open, NULL);
    hp_try_open(NULL);
}

#else /* !__linux__ */

void nano7_hostpad_init(void)
{
    const char *e = getenv("NANO7_HOST_PAD");

    if (e && *e && strcmp(e, "none")) {
        warn_report("nano7-hostpad: host gamepads are read through Linux "
                    "evdev; not available on this host");
    }
}

#endif
