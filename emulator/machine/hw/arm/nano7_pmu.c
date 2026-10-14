/*
 * nano7_pmu.c -- the PMU on I2C1 (slave 0x73) as far as RetailOS needs it
 * for the buttons, plus button front-ends (keyboard, timed script).
 *
 * RE (Session 17, snap6 build):
 *   regs 1..4  event status, read then written back (W1C) by 0x08143208;
 *              packed LE into one word: reg3 bit0 Home, bit1 Power, bit3
 *              Play/Pause (translation 0x084d8536)
 *   regs 5..8  levels, sampled by 0x083a6f80: reg7 bit4 Home, bit5 Power,
 *              reg8 bit1 Play/Pause; 0 = pressed
 *   regs 0x7c..0x7f wake status (0x08160290), same layout (reg 0x7e bit0
 *              Home, bit1 Power)
 *   GPIO 0x56 = event line, low while any status bit is pending; its
 *   callback 0x080fb268 wakes the button thread (entry 0x08567808), which
 *   loops status read + dispatch 0x080fb270 until the line is high.
 *   Events: 0x08408570(id, 1/0), id 1 home, 9 powerkey, 6 playpause.
 *   A Power hold > ~150 ms may run PressAndHold (shutdown): keep it short.
 * Battery / charger (Session 19 RE):
 *   0x30  ADC control: bits 3:0 channel, bit4 start/busy (the OS aborts if
 *         it is set before a start, polls it clear ~1 ms after); result is
 *         10 bits (0x32 << 2) | 0x31. Channel 4 = battery, mV = 2500 +
 *         raw * 2000 / 1023 (level 0..15 from table 0x0890c490; below
 *         level 2, ~3666 mV, the OS shows "Low Battery"); channel 1 =
 *         charger load %, must stay < 3. Conversions complete instantly.
 *   0x3c..0x3f temperature fields (0x0818a4e8): ~24 C
 *   0x05  bit6 external power, bit3 charger good (both = on USB power)
 *   0x29  bit2 + current-limit code bits 1:0 (2 = 500 mA)   0x0d bit3 = 0
 * Real-time clock (Session 20 RE; read 0x0801ed02, set path 0x08010b1c):
 *   0x40..0x43 u32 LE free-running 1 Hz counter (seconds since PMU power-
 *              up, never written by the OS; it polls it at boot)
 *   0x74..0x77 u32 LE persistent offset: Unix time (UTC) = counter + offset;
 *              the OS writes it when the time is set
 *   0x45..0x49 alarm (counter domain) + enable: plain registers
 * The OS also keeps a time-of-day cache (12 bytes at 0x08a6b7fc: u16 year,
 * u8 month, day, u16 yday, s8 tz (15 min), u8 dst, hour, min, sec, flags)
 * that it advances with uptime and only now and then refreshes from the
 * RTC, so an RTC that disagrees with it makes the status-bar clock jump.
 * Model: the counter runs on the virtual clock, started so that counter +
 * offset = the rtc= time: "snapshot" (default) = the cache's time at T0
 * (read on the first RTC read, taken as UTC), "host" = host time, or Unix
 * seconds. The offset starts at the snapshot's 0x6a090928 (also cached by
 * the OS in RAM). Reading 0x40 latches the 4 counter bytes.
 * Other registers are a plain register file (0 at reset). Every register
 * touched is logged once.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "nano7_pmu.h"
#include "nano7_gpio.h"
#include "qemu/timer.h"
#include "nano7_clock.h"
#include "ui/input.h"
#include "exec/address-spaces.h"
#include "migration/vmstate.h"

#define PMU_ADDR        0x73
#define PMU_HOLD_MS     100
#define PMU_SCRIPT_MAX  32
#define PMU_BATT_MV     4100        /* battery level 14, as in snap6 */
#define PMU_RTC_OFFSET  0x6a090928u /* snap6 offset (2026-05-17 00:17:44) */
#define PMU_TOD_CACHE   0x08a6b7fcu /* OS time-of-day cache (snap6 build) */

typedef struct PmuBtn {
    const char *name;
    uint8_t lvl_reg, lvl_bit, ev_bit;   /* ev_bit: in reg3 and reg 0x7e */
} PmuBtn;

static const PmuBtn pmu_btns[NANO7_BTN_COUNT] = {
    [NANO7_BTN_HOME]  = { "home",  7, 1u << 4, 1u << 0 },
    [NANO7_BTN_POWER] = { "power", 7, 1u << 5, 1u << 1 },
    [NANO7_BTN_PLAY]  = { "play",  8, 1u << 1, 1u << 3 },
    /* volume buttons are plain GPIOs (lvl_reg 0 = not on the PMU) */
    [NANO7_BTN_VOLUP]   = { "volup",   0, 0, 0 },
    [NANO7_BTN_VOLDOWN] = { "voldown", 0, 0, 0 },
};

typedef struct PmuEvent {
    int64_t at_ns;
    int btn;
    uint32_t hold_ms;
} PmuEvent;

typedef struct Nano7PmuState {
    uint8_t regs[256];
    uint8_t ptr;
    bool have_ptr;
    bool seen[256][2];          /* register logged (read / write) */
    unsigned trace_left;        /* log all accesses for a while after a button */
    bool trace;                 /* NANO7_PMU_TRACE: log every access */
    int64_t rtc_base;           /* counter value at virtual time 0 */
    bool rtc_from_snapshot;     /* rtc_base still to be set from the cache */
    PmuEvent script[PMU_SCRIPT_MAX];
    unsigned nscript, next;
    QEMUTimer *script_timer;
    QEMUTimer *release_timer[NANO7_BTN_COUNT];
} Nano7PmuState;

static Nano7PmuState *pmu_singleton;

static void pmu_line(Nano7PmuState *s)
{
    bool pending = s->regs[1] | s->regs[2] | s->regs[3] | s->regs[4];

    nano7_gpio_set_level(NANO7_GPIO_PMU_IRQ, !pending);
}

static void pmu_log(Nano7PmuState *s, uint8_t reg, bool wr, uint8_t v)
{
    if (s->trace || s->trace_left) {
        if (s->trace_left) {
            s->trace_left--;
        }
        fprintf(stderr, "[nano7-pmu] t=%.3f s %s reg 0x%02x %s 0x%02x\n",
                nano7_now_ns() / 1e9, wr ? "W" : "R",
                reg, wr ? "=" : "->", v);
        return;
    }
    if (!s->seen[reg][wr]) {
        s->seen[reg][wr] = true;
        fprintf(stderr, "[nano7-pmu] %s reg 0x%02x %s 0x%02x\n",
                wr ? "W" : "R", reg, wr ? "=" : "->", v);
    }
}

static void pmu_start(void *opaque, bool rx)
{
    Nano7PmuState *s = opaque;

    if (!rx) {
        s->have_ptr = false;
    }
}

/* ADC start: latch the channel, complete at once (bit4 reads clear) */
static void pmu_adc(Nano7PmuState *s, uint8_t v)
{
    unsigned raw = 0;

    s->regs[0x30] = v & ~0x10;
    if ((v & 0x10) && (v & 0xf) == 4) {
        raw = (PMU_BATT_MV - 2500) * 1023 / 2000;
    }
    if (v & 0x10) {
        s->regs[0x32] = raw >> 2;
        s->regs[0x31] = raw & 3;
    }
}

static bool pmu_write(void *opaque, uint8_t v)
{
    Nano7PmuState *s = opaque;
    uint8_t r;

    if (!s->have_ptr) {
        s->ptr = v;
        s->have_ptr = true;
        return true;
    }
    r = s->ptr++;
    pmu_log(s, r, true, v);
    if ((r >= 1 && r <= 4) || (r >= 0x7c && r <= 0x7f)) {
        s->regs[r] &= ~v;               /* event / wake status: W1C */
        pmu_line(s);
    } else if (r >= 5 && r <= 8) {
        /* levels are inputs */
    } else if (r >= 0x31 && r <= 0x32) {
        /* ADC result: read-only */
    } else if (r >= 0x3c && r <= 0x3f) {
        /* temperature measurement: read-only */
    } else if (r >= 0x40 && r <= 0x43) {
        /* RTC counter: read-only */
    } else if (r == 0x30) {
        pmu_adc(s, v);
    } else {
        s->regs[r] = v;
    }
    return true;
}

/* rtc=snapshot: start the RTC at the OS's cached time of day (taken as
 * UTC, the way the status bar shows RTC time), so both clock sources agree */
static void pmu_rtc_from_snapshot(Nano7PmuState *s)
{
    uint8_t c[12];
    GDateTime *dt;

    s->rtc_from_snapshot = false;
    address_space_read(&address_space_memory, PMU_TOD_CACHE,
                       MEMTXATTRS_UNSPECIFIED, c, sizeof(c));
    dt = g_date_time_new_utc(c[0] | c[1] << 8, c[2], c[3], c[8], c[9], c[10]);
    if (!dt) {
        fprintf(stderr, "[nano7-pmu] RTC: no valid time-of-day cache at "
                "0x%08x, keeping unix %" PRId64 "\n", PMU_TOD_CACHE,
                s->rtc_base + PMU_RTC_OFFSET);
        return;
    }
    s->rtc_base = g_date_time_to_unix(dt) - PMU_RTC_OFFSET;
    fprintf(stderr, "[nano7-pmu] RTC: snapshot time %04u-%02u-%02u %02u:%02u:"
            "%02u -> unix %" PRId64 "\n", c[0] | c[1] << 8, c[2], c[3], c[8],
            c[9], c[10], g_date_time_to_unix(dt));
    g_date_time_unref(dt);
}

static uint8_t pmu_read(void *opaque)
{
    Nano7PmuState *s = opaque;
    uint8_t r = s->ptr++;

    if (r == 0x40 && s->rtc_from_snapshot) {
        pmu_rtc_from_snapshot(s);
    }
    if (r == 0x40) {        /* latch the RTC counter for 0x40..0x43 */
        uint32_t c = s->rtc_base + nano7_now_ns() /
                                   NANOSECONDS_PER_SECOND;

        stl_le_p(&s->regs[0x40], c);
    }
    pmu_log(s, r, false, s->regs[r]);
    return s->regs[r];
}

void nano7_pmu_button(int btn, bool pressed)
{
    Nano7PmuState *s = pmu_singleton;
    const PmuBtn *b = &pmu_btns[btn];

    if (!s || btn < 0 || btn >= NANO7_BTN_COUNT) {
        return;
    }
    if (btn == NANO7_BTN_VOLUP || btn == NANO7_BTN_VOLDOWN) {
        /* GPIOButtonManagerTask (0x080de2cc): both edges, 10 ms debounce,
         * emits 0x08408570(8 up / 7 down, 1/0) */
        nano7_gpio_set_level(btn == NANO7_BTN_VOLUP ? NANO7_GPIO_VOL_UP :
                             NANO7_GPIO_VOL_DOWN, !pressed);
        fprintf(stderr, "[nano7-pmu] t=%.3f s button %s %s\n",
                nano7_now_ns() / 1e9, b->name,
                pressed ? "pressed" : "released");
        return;
    }
    if (pressed) {
        s->regs[b->lvl_reg] &= ~b->lvl_bit;
        s->regs[0x7e] |= b->ev_bit;
    } else {
        s->regs[b->lvl_reg] |= b->lvl_bit;
    }
    s->regs[3] |= b->ev_bit;
    s->trace_left = 64;
    fprintf(stderr, "[nano7-pmu] t=%.3f s button %s %s\n",
            nano7_now_ns() / 1e9, b->name,
            pressed ? "pressed" : "released");
    pmu_line(s);
}

static void pmu_release(Nano7PmuState *s, int btn)
{
    nano7_pmu_button(btn, false);
}

static void pmu_release_home(void *o)  { pmu_release(o, NANO7_BTN_HOME); }
static void pmu_release_power(void *o) { pmu_release(o, NANO7_BTN_POWER); }
static void pmu_release_play(void *o)  { pmu_release(o, NANO7_BTN_PLAY); }
static void pmu_release_volup(void *o) { pmu_release(o, NANO7_BTN_VOLUP); }
static void pmu_release_voldown(void *o)
{
    pmu_release(o, NANO7_BTN_VOLDOWN);
}

static void pmu_tap(Nano7PmuState *s, int btn, uint32_t hold_ms)
{
    nano7_pmu_button(btn, true);
    timer_mod_ns(s->release_timer[btn],
                 nano7_deadline(nano7_now_ns() +
                 (int64_t)hold_ms * 1000000));
}

static void pmu_script_tick(void *opaque)
{
    Nano7PmuState *s = opaque;
    int64_t now = nano7_now_ns();

    while (s->next < s->nscript && s->script[s->next].at_ns <= now) {
        pmu_tap(s, s->script[s->next].btn, s->script[s->next].hold_ms);
        s->next++;
    }
    if (s->next < s->nscript) {
        timer_mod_ns(s->script_timer,
                     nano7_deadline(s->script[s->next].at_ns));
    }
}

static void pmu_parse_script(Nano7PmuState *s, const char *script)
{
    g_auto(GStrv) items = g_strsplit_set(script, "/;", -1);
    int i, b;

    for (i = 0; items[i] && s->nscript < PMU_SCRIPT_MAX; i++) {
        g_auto(GStrv) f = g_strsplit(items[i], ":", 3);
        PmuEvent *e = &s->script[s->nscript];

        if (!f[0] || !f[1]) {
            fprintf(stderr, "[nano7-pmu] bad buttons item '%s'\n", items[i]);
            exit(1);
        }
        e->btn = -1;
        for (b = 0; b < NANO7_BTN_COUNT; b++) {
            if (!strcmp(f[1], pmu_btns[b].name)) {
                e->btn = b;
            }
        }
        if (e->btn < 0) {
            fprintf(stderr, "[nano7-pmu] unknown button '%s' (home, power, "
                    "play, volup, voldown)\n", f[1]);
            exit(1);
        }
        e->at_ns = (int64_t)(g_ascii_strtod(f[0], NULL) * 1e9);
        e->hold_ms = f[2] ? atoi(f[2]) : PMU_HOLD_MS;
        s->nscript++;
    }
    if (s->nscript) {
        timer_mod_ns(s->script_timer,
                     nano7_deadline(s->script[0].at_ns));
    }
}

/* keyboard (VNC / display window / monitor `sendkey`): h home, p power,
 * space play, Up / = / keypad + volume up, Down / - / keypad - volume down */
bool nano7_pmu_key(int qcode, bool down)
{
    int btn;

    switch (qcode) {
    case Q_KEY_CODE_H:     btn = NANO7_BTN_HOME;  break;
    case Q_KEY_CODE_P:     btn = NANO7_BTN_POWER; break;
    case Q_KEY_CODE_SPC:   btn = NANO7_BTN_PLAY;  break;
    case Q_KEY_CODE_UP:
    case Q_KEY_CODE_EQUAL:
    case Q_KEY_CODE_KP_ADD:   btn = NANO7_BTN_VOLUP;   break;
    case Q_KEY_CODE_DOWN:
    case Q_KEY_CODE_MINUS:
    case Q_KEY_CODE_KP_SUBTRACT: btn = NANO7_BTN_VOLDOWN; break;
    default:               return false;
    }
    nano7_pmu_button(btn, down);
    return true;
}

static const VMStateDescription vmstate_pmu = {
    .name = "nano7-pmu",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, Nano7PmuState, 256),
        VMSTATE_UINT8(ptr, Nano7PmuState),
        VMSTATE_BOOL(have_ptr, Nano7PmuState),
        VMSTATE_INT64(rtc_base, Nano7PmuState),
        VMSTATE_BOOL(rtc_from_snapshot, Nano7PmuState),
        VMSTATE_UINT32(next, Nano7PmuState),
        VMSTATE_TIMER_PTR(script_timer, Nano7PmuState),
        VMSTATE_TIMER_PTR_ARRAY(release_timer, Nano7PmuState,
                                NANO7_BTN_COUNT),
        VMSTATE_END_OF_LIST()
    }
};

void nano7_pmu_attach(Nano7I2CBus *bus, const char *script, const char *rtc)
{
    Nano7PmuState *s = g_new0(Nano7PmuState, 1);
    Nano7I2CSlave sl = {
        .addr = PMU_ADDR, .opaque = s,
        .start = pmu_start, .write = pmu_write, .read = pmu_read,
    };

    nano7_gpio_model_pin(NANO7_GPIO_PMU_IRQ, 1);   /* event line idle high */
    s->trace = getenv("NANO7_PMU_TRACE") != NULL;
    s->regs[7] = 0x30;      /* home, power released */
    s->regs[8] = 0x02;      /* play/pause released */
    s->regs[0x05] = 0x48;   /* external power present, charger good */
    s->regs[0x29] = 0x06;   /* charger enabled, 500 mA input limit */
    s->regs[0x3c] = 0x08;   /* temperature fields: A 500, r5 530, r6 500 */
    s->regs[0x3d] = 0x84;
    s->regs[0x3e] = 0x7d;
    s->regs[0x3f] = 0x7d;
    {
        bool snap = !rtc || !strcmp(rtc, "snapshot");
        int64_t now = (snap || !strcmp(rtc, "host")) ? (int64_t)time(NULL) :
                      g_ascii_strtoll(rtc, NULL, 0);

        s->rtc_from_snapshot = snap;

        stl_le_p(&s->regs[0x74], PMU_RTC_OFFSET);
        s->rtc_base = now - PMU_RTC_OFFSET;
        if (!snap) {
            fprintf(stderr, "[nano7-pmu] RTC: unix %" PRId64 " (counter %"
                    PRId64 " + offset 0x%08x)\n", now, s->rtc_base,
                    PMU_RTC_OFFSET);
        }
    }
    pmu_singleton = s;
    vmstate_register_any(NULL, &vmstate_pmu, s);
    s->script_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, pmu_script_tick, s);
    s->release_timer[NANO7_BTN_HOME] =
        timer_new_ns(QEMU_CLOCK_VIRTUAL, pmu_release_home, s);
    s->release_timer[NANO7_BTN_POWER] =
        timer_new_ns(QEMU_CLOCK_VIRTUAL, pmu_release_power, s);
    s->release_timer[NANO7_BTN_PLAY] =
        timer_new_ns(QEMU_CLOCK_VIRTUAL, pmu_release_play, s);
    s->release_timer[NANO7_BTN_VOLUP] =
        timer_new_ns(QEMU_CLOCK_VIRTUAL, pmu_release_volup, s);
    s->release_timer[NANO7_BTN_VOLDOWN] =
        timer_new_ns(QEMU_CLOCK_VIRTUAL, pmu_release_voldown, s);
    nano7_gpio_model_pin_both_edges(NANO7_GPIO_VOL_UP, 1);     /* idle high */
    nano7_gpio_model_pin_both_edges(NANO7_GPIO_VOL_DOWN, 1);
    nano7_i2c_add_slave(bus, &sl);
    if (script) {
        pmu_parse_script(s, script);
    }
}
