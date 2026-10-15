/*
 * nano7_hostpad.c -- a real gamepad on the host drives the fake Bluetooth
 * HID gamepad (nano7_btuart.c), so homebrew games can be played in the
 * emulator with the pad they will meet on the device.
 *
 * NANO7_HOST_PAD (run.sh --host-pad) = "auto" (the first /dev/input/event*
 * with gamepad buttons and two axes; checked again every 2 s until one
 * shows up, and after it is unplugged) or a path: an evdev node, or a FIFO /
 * file carrying the same struct input_event records (tests, bridges).
 * Linux: evdev (QEMU here is built without SDL). The node must be readable
 * (group "input" on most distributions). macOS: IOKit HID, see the
 * __APPLE__ part at the end. Elsewhere it says so and does nothing.
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

/* shared by the backends: buttons + sticks + hat (D-pad, added to the left
 * stick at full deflection) -> one NanoPad report, sent when it changed */
static struct {
    bool valid;
    uint16_t buttons;
    int8_t axis[4];
} hp_sent;

static void hp_emit(uint16_t buttons, const int8_t stick[4],
                    const int8_t hat[2])
{
    int8_t axis[4];
    int i;

    for (i = 0; i < 4; i++) {
        int v = stick[i] + (i < 2 ? hat[i] * 127 : 0);

        axis[i] = MAX(-127, MIN(127, v));
    }
    if (hp_sent.valid && buttons == hp_sent.buttons &&
        !memcmp(axis, hp_sent.axis, 4)) {
        return;
    }
    hp_sent.valid = true;
    hp_sent.buttons = buttons;
    memcpy(hp_sent.axis, axis, 4);
    nano7_btuart_pad_host(buttons, axis);
}

/* the pad went away: everything released */
static void hp_release(void)
{
    static const int8_t zero[4];

    hp_sent.valid = false;
    nano7_btuart_pad_host(0, zero);
}

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
    hp_emit(hp.buttons | hp.trig, hp.stick, hp.hat);
}

static void hp_close(const char *why)
{
    qemu_set_fd_handler(hp.fd, NULL, NULL, NULL);
    close(hp.fd);
    hp.fd = -1;
    info_report("nano7-hostpad: %s - released, retrying", why);
    hp_release();
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

#elif defined(__APPLE__)
/*
 * macOS: IOKit HID Manager (plain C, no SDL / GameController dependency; it
 * works with any QEMU display). The manager lives on its own thread with
 * its own CFRunLoop, because QEMU's main loop is not a run loop (and with
 * the Cocoa display the main thread belongs to AppKit). Input callbacks
 * update a snapshot under a mutex and kick a bottom half; the bottom half
 * sends the report from the main loop. Hot-plug comes with the manager.
 * Gamepads do not need the Input Monitoring permission (keyboards do).
 *
 * NANO7_HOST_PAD = "auto" (the first Generic Desktop joystick / gamepad /
 * multi-axis controller) or a product-name substring to pick one.
 * HID button numbers differ between makers; built-in tables (from the
 * controllers' public HID descriptors as SDL documents them, NOT tested on
 * hardware here) map them by position onto NanoPad's buttons as on Linux:
 *   Sony (DualShock 4 / DualSense, 0x054c), Microsoft (Xbox Wireless over
 *   Bluetooth, 0x045e), Nintendo (Switch Pro, 0x057e); anything else:
 *   HID button n = NanoPad button n.
 * NANO7_HOST_PAD_BUTTONS="HID:PAD,..." (1-based, e.g. "2:1,3:2") overrides
 * entries; NANO7_HOST_PAD_TRACE=1 logs every element change (usage page,
 * usage, value) to find the right numbers for a new pad.
 * Axes: X / Y left stick; the right stick is Z / Rz when the device has Rz,
 * else Rx / Ry. Sony's Rx / Ry and Xbox's Brake / Accelerator (Simulation
 * page) are the analog triggers: past half way they press TL2 / TR2. Hat
 * switch = D-pad (8 directions, out of range = centred); Generic Desktop
 * D-pad usages 0x90-0x93 too. Dead zone 10 % of the range.
 */
#include "qemu/thread.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDUsageTables.h>

#define HP_DEAD         0.10
#define HP_BUTTONS      32              /* HID buttons 1..32 */

typedef struct MacPad {
    QemuMutex lock;
    QEMUBH *bh;
    char *want;                 /* NULL = any gamepad */
    IOHIDDeviceRef dev;         /* the one in use */
    int8_t map[HP_BUTTONS + 1]; /* HID button -> NanoPad bit, -1 none */
    bool right_rz;              /* right stick on Z / Rz */
    bool sony;
    bool trace;
    /* snapshot, under lock */
    uint16_t buttons, trig;
    int8_t stick[4], hat[2];
    bool gone;
} MacPad;

static MacPad mp;

/* NanoPad bits: 0 A, 1 B, 2 X, 3 Y, 4 L, 5 R, 6 select, 7 start, 8 home,
 * 9 L3, 10 R3, 11 L2, 12 R2; index = HID button number */
static const int8_t map_sony[15] = {
    -1, 2, 0, 1, 3, 4, 5, 11, 12, 6, 7, 9, 10, 8, -1,
};      /* square cross circle triangle L1 R1 L2 R2 share options L3 R3 PS
         * touchpad */
static const int8_t map_xbox[16] = {
    -1, 0, 1, -1, 2, 3, -1, 4, 5, -1, -1, 6, 7, 8, 9, 10,
};      /* A B - X Y - LB RB - - view menu xbox LS RS */
static const int8_t map_switch[15] = {
    -1, 0, 1, 2, 3, 4, 5, 11, 12, 6, 7, 9, 10, 8, -1,
};      /* B A Y X L R ZL ZR minus plus LS RS home capture (by position) */

static int mp_int_prop(IOHIDDeviceRef d, CFStringRef key)
{
    CFTypeRef v = IOHIDDeviceGetProperty(d, key);
    int n = 0;

    if (v && CFGetTypeID(v) == CFNumberGetTypeID()) {
        CFNumberGetValue(v, kCFNumberIntType, &n);
    }
    return n;
}

static void mp_name(IOHIDDeviceRef d, char *buf, size_t len)
{
    CFTypeRef v = IOHIDDeviceGetProperty(d, CFSTR(kIOHIDProductKey));

    snprintf(buf, len, "?");
    if (v && CFGetTypeID(v) == CFStringGetTypeID()) {
        CFStringGetCString(v, buf, len, kCFStringEncodingUTF8);
    }
}

static bool mp_has_rz(IOHIDDeviceRef d)
{
    CFArrayRef els = IOHIDDeviceCopyMatchingElements(d, NULL,
                                                     kIOHIDOptionsTypeNone);
    bool rz = false;
    CFIndex i;

    for (i = 0; els && i < CFArrayGetCount(els); i++) {
        IOHIDElementRef e = (IOHIDElementRef)CFArrayGetValueAtIndex(els, i);

        rz |= IOHIDElementGetUsagePage(e) == kHIDPage_GenericDesktop &&
              IOHIDElementGetUsage(e) == kHIDUsage_GD_Rz;
    }
    if (els) {
        CFRelease(els);
    }
    return rz;
}

/* button table for this device (+ the user's overrides) */
static void mp_setup(IOHIDDeviceRef d)
{
    int vid = mp_int_prop(d, CFSTR(kIOHIDVendorIDKey));
    const int8_t *t = NULL;
    size_t n = 0, i;
    const char *o = getenv("NANO7_HOST_PAD_BUTTONS");
    char name[128];

    switch (vid) {
    case 0x054c: t = map_sony; n = ARRAY_SIZE(map_sony); break;
    case 0x045e: t = map_xbox; n = ARRAY_SIZE(map_xbox); break;
    case 0x057e: t = map_switch; n = ARRAY_SIZE(map_switch); break;
    }
    for (i = 0; i <= HP_BUTTONS; i++) {
        mp.map[i] = t ? (i < n ? t[i] : -1) : (i >= 1 && i <= 16 ? i - 1 : -1);
    }
    while (o && *o) {           /* "HID:PAD,..." */
        int h = atoi(o), p;
        const char *c = strchr(o, ':');

        if (!c) {
            break;
        }
        p = atoi(c + 1);
        if (h >= 1 && h <= HP_BUTTONS) {
            mp.map[h] = p >= 1 && p <= 16 ? p - 1 : -1;
        }
        o = strchr(c, ',');
        o = o ? o + 1 : NULL;
    }
    mp.sony = vid == 0x054c;
    mp.right_rz = mp_has_rz(d);
    mp_name(d, name, sizeof(name));
    info_report("nano7-hostpad: gamepad \"%s\" (vendor 0x%04x, %s buttons, "
                "right stick on %s)", name, vid,
                t ? "known" : "HID-order", mp.right_rz ? "Z / Rz" : "Rx / Ry");
}

static int8_t mp_axis(IOHIDElementRef e, CFIndex v)
{
    CFIndex lo = IOHIDElementGetLogicalMin(e), hi = IOHIDElementGetLogicalMax(e);
    double c = (lo + hi) / 2.0, h = (hi - lo) / 2.0, f;

    if (h <= 0) {
        return 0;
    }
    f = (v - c) / h;
    if (fabs(f) < HP_DEAD) {
        return 0;
    }
    return (int8_t)MAX(-127, MIN(127, lrint(f * 127)));
}

static bool mp_half(IOHIDElementRef e, CFIndex v)
{
    CFIndex lo = IOHIDElementGetLogicalMin(e), hi = IOHIDElementGetLogicalMax(e);

    return hi > lo && (v - lo) * 2 > hi - lo;
}

static void mp_trigger(int bit, bool on)
{
    mp.trig = on ? mp.trig | 1u << bit : mp.trig & ~(1u << bit);
}

/* IOKit thread */
static void mp_value(void *ctx, IOReturn res, void *sender, IOHIDValueRef val)
{
    IOHIDElementRef e = IOHIDValueGetElement(val);
    IOHIDDeviceRef d = IOHIDElementGetDevice(e);
    uint32_t page = IOHIDElementGetUsagePage(e), use = IOHIDElementGetUsage(e);
    CFIndex v = IOHIDValueGetIntegerValue(val);

    if (d != mp.dev) {
        return;
    }
    if (mp.trace) {
        fprintf(stderr, "[nano7-hostpad] page 0x%02x usage 0x%02x = %ld\n",
                page, use, (long)v);
    }
    qemu_mutex_lock(&mp.lock);
    if (page == kHIDPage_Button && use >= 1 && use <= HP_BUTTONS) {
        int b = mp.map[use];

        if (b >= 0) {
            mp.buttons = v ? mp.buttons | 1u << b : mp.buttons & ~(1u << b);
        }
    } else if (page == kHIDPage_GenericDesktop) {
        switch (use) {
        case kHIDUsage_GD_X:  mp.stick[0] = mp_axis(e, v); break;
        case kHIDUsage_GD_Y:  mp.stick[1] = mp_axis(e, v); break;
        case kHIDUsage_GD_Z:
        case kHIDUsage_GD_Rz:
            if (mp.right_rz) {
                mp.stick[use == kHIDUsage_GD_Z ? 2 : 3] = mp_axis(e, v);
            }
            break;
        case kHIDUsage_GD_Rx:
        case kHIDUsage_GD_Ry:
            if (!mp.right_rz) {
                mp.stick[use == kHIDUsage_GD_Rx ? 2 : 3] = mp_axis(e, v);
            } else if (mp.sony) {       /* analog L2 / R2 */
                mp_trigger(use == kHIDUsage_GD_Rx ? 11 : 12, mp_half(e, v));
            }
            break;
        case kHIDUsage_GD_Hatswitch: {
            static const int8_t hx[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
            static const int8_t hy[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
            CFIndex dir = v - IOHIDElementGetLogicalMin(e);

            mp.hat[0] = dir >= 0 && dir < 8 ? hx[dir] : 0;
            mp.hat[1] = dir >= 0 && dir < 8 ? hy[dir] : 0;
            break;
        }
        case kHIDUsage_GD_DPadUp:    mp.hat[1] = v ? -1 : 0; break;
        case kHIDUsage_GD_DPadDown:  mp.hat[1] = v ? 1 : 0;  break;
        case kHIDUsage_GD_DPadRight: mp.hat[0] = v ? 1 : 0;  break;
        case kHIDUsage_GD_DPadLeft:  mp.hat[0] = v ? -1 : 0; break;
        }
    } else if (page == kHIDPage_Simulation) {   /* Xbox triggers */
        if (use == kHIDUsage_Sim_Brake || use == kHIDUsage_Sim_Accelerator) {
            mp_trigger(use == kHIDUsage_Sim_Brake ? 11 : 12, mp_half(e, v));
        }
    }
    qemu_mutex_unlock(&mp.lock);
    qemu_bh_schedule(mp.bh);
}

static void mp_matched(void *ctx, IOReturn res, void *sender,
                       IOHIDDeviceRef d)
{
    char name[128];

    if (mp.dev) {
        return;                 /* one pad at a time */
    }
    mp_name(d, name, sizeof(name));
    if (mp.want && !strstr(name, mp.want)) {
        return;
    }
    qemu_mutex_lock(&mp.lock);
    mp.dev = d;
    mp.buttons = mp.trig = 0;
    memset(mp.stick, 0, sizeof(mp.stick));
    memset(mp.hat, 0, sizeof(mp.hat));
    mp.gone = false;
    mp_setup(d);
    qemu_mutex_unlock(&mp.lock);
}

static void mp_removed(void *ctx, IOReturn res, void *sender,
                       IOHIDDeviceRef d)
{
    if (d != mp.dev) {
        return;
    }
    qemu_mutex_lock(&mp.lock);
    mp.dev = NULL;
    mp.gone = true;
    qemu_mutex_unlock(&mp.lock);
    info_report("nano7-hostpad: gamepad disconnected - released, waiting "
                "for one");
    qemu_bh_schedule(mp.bh);
}

static CFDictionaryRef mp_match(uint32_t usage)
{
    int page = kHIDPage_GenericDesktop, u = usage;
    CFNumberRef np = CFNumberCreate(NULL, kCFNumberIntType, &page);
    CFNumberRef nu = CFNumberCreate(NULL, kCFNumberIntType, &u);
    const void *k[2] = { CFSTR(kIOHIDDeviceUsagePageKey),
                         CFSTR(kIOHIDDeviceUsageKey) };
    const void *v[2] = { np, nu };
    CFDictionaryRef d = CFDictionaryCreate(NULL, k, v, 2,
                                           &kCFTypeDictionaryKeyCallBacks,
                                           &kCFTypeDictionaryValueCallBacks);

    CFRelease(np);
    CFRelease(nu);
    return d;
}

static void *mp_thread(void *opaque)
{
    IOHIDManagerRef m = IOHIDManagerCreate(kCFAllocatorDefault,
                                           kIOHIDOptionsTypeNone);
    const void *dicts[3] = { mp_match(kHIDUsage_GD_Joystick),
                             mp_match(kHIDUsage_GD_GamePad),
                             mp_match(kHIDUsage_GD_MultiAxisController) };
    CFArrayRef match = CFArrayCreate(NULL, dicts, 3, &kCFTypeArrayCallBacks);
    IOReturn r;
    int i;

    IOHIDManagerSetDeviceMatchingMultiple(m, match);
    IOHIDManagerRegisterDeviceMatchingCallback(m, mp_matched, NULL);
    IOHIDManagerRegisterDeviceRemovalCallback(m, mp_removed, NULL);
    IOHIDManagerRegisterInputValueCallback(m, mp_value, NULL);
    IOHIDManagerScheduleWithRunLoop(m, CFRunLoopGetCurrent(),
                                    kCFRunLoopDefaultMode);
    r = IOHIDManagerOpen(m, kIOHIDOptionsTypeNone);
    if (r != kIOReturnSuccess) {
        warn_report("nano7-hostpad: IOHIDManagerOpen failed (0x%x)", r);
    }
    for (i = 0; i < 3; i++) {
        CFRelease(dicts[i]);
    }
    CFRelease(match);
    CFRunLoopRun();             /* for the life of the emulator */
    return NULL;
}

/* main loop: snapshot -> report */
static void mp_bh(void *opaque)
{
    uint16_t buttons;
    int8_t stick[4], hat[2];
    bool gone;

    qemu_mutex_lock(&mp.lock);
    buttons = mp.buttons | mp.trig;
    memcpy(stick, mp.stick, 4);
    memcpy(hat, mp.hat, 2);
    gone = mp.gone;
    mp.gone = false;
    qemu_mutex_unlock(&mp.lock);
    if (gone) {
        hp_release();
    } else {
        hp_emit(buttons, stick, hat);
    }
}

void nano7_hostpad_init(void)
{
    const char *e = getenv("NANO7_HOST_PAD");
    QemuThread t;

    if (!e || !*e || !strcmp(e, "none")) {
        return;
    }
    mp.want = strcmp(e, "auto") ? g_strdup(e) : NULL;
    mp.trace = getenv("NANO7_HOST_PAD_TRACE") &&
               strcmp(getenv("NANO7_HOST_PAD_TRACE"), "0");
    qemu_mutex_init(&mp.lock);
    mp.bh = qemu_bh_new(mp_bh, NULL);
    qemu_thread_create(&t, "nano7-hostpad", mp_thread, NULL,
                       QEMU_THREAD_DETACHED);
    info_report("nano7-hostpad: IOKit HID, waiting for %s%s%s",
                mp.want ? "a gamepad named \"" : "any gamepad",
                mp.want ? mp.want : "", mp.want ? "\"" : "");
}

#else /* neither Linux nor macOS */

void nano7_hostpad_init(void)
{
    const char *e = getenv("NANO7_HOST_PAD");

    if (e && *e && strcmp(e, "none")) {
        warn_report("nano7-hostpad: host gamepads are read through Linux "
                    "evdev or macOS IOKit; not available on this host");
    }
}

#endif
