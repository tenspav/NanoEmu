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
 * Other registers are a plain register file (0 at reset). Every register
 * touched is logged once.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "nano7_pmu.h"
#include "nano7_gpio.h"
#include "qemu/timer.h"
#include "ui/input.h"

#define PMU_ADDR        0x73
#define PMU_HOLD_MS     100
#define PMU_SCRIPT_MAX  32

typedef struct PmuBtn {
    const char *name;
    uint8_t lvl_reg, lvl_bit, ev_bit;   /* ev_bit: in reg3 and reg 0x7e */
} PmuBtn;

static const PmuBtn pmu_btns[NANO7_BTN_COUNT] = {
    [NANO7_BTN_HOME]  = { "home",  7, 1u << 4, 1u << 0 },
    [NANO7_BTN_POWER] = { "power", 7, 1u << 5, 1u << 1 },
    [NANO7_BTN_PLAY]  = { "play",  8, 1u << 1, 1u << 3 },
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
    unsigned trace_left;        /* log every access for a while after a button */
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
    if (s->trace_left) {
        s->trace_left--;
        fprintf(stderr, "[nano7-pmu] t=%.3f s %s reg 0x%02x %s 0x%02x\n",
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9, wr ? "W" : "R",
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
    } else {
        s->regs[r] = v;
    }
    return true;
}

static uint8_t pmu_read(void *opaque)
{
    Nano7PmuState *s = opaque;
    uint8_t r = s->ptr++;

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
    if (pressed) {
        s->regs[b->lvl_reg] &= ~b->lvl_bit;
        s->regs[0x7e] |= b->ev_bit;
    } else {
        s->regs[b->lvl_reg] |= b->lvl_bit;
    }
    s->regs[3] |= b->ev_bit;
    s->trace_left = 64;
    fprintf(stderr, "[nano7-pmu] t=%.3f s button %s %s\n",
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9, b->name,
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

static void pmu_tap(Nano7PmuState *s, int btn, uint32_t hold_ms)
{
    nano7_pmu_button(btn, true);
    timer_mod_ns(s->release_timer[btn],
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                 (int64_t)hold_ms * 1000000);
}

static void pmu_script_tick(void *opaque)
{
    Nano7PmuState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    while (s->next < s->nscript && s->script[s->next].at_ns <= now) {
        pmu_tap(s, s->script[s->next].btn, s->script[s->next].hold_ms);
        s->next++;
    }
    if (s->next < s->nscript) {
        timer_mod_ns(s->script_timer, s->script[s->next].at_ns);
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
                    "play)\n", f[1]);
            exit(1);
        }
        e->at_ns = (int64_t)(g_ascii_strtod(f[0], NULL) * 1e9);
        e->hold_ms = f[2] ? atoi(f[2]) : PMU_HOLD_MS;
        s->nscript++;
    }
    if (s->nscript) {
        timer_mod_ns(s->script_timer, s->script[0].at_ns);
    }
}

/* keyboard (QEMU display window or monitor `sendkey`): h, p, space */
static void pmu_key_event(DeviceState *dev, QemuConsole *src,
                          InputEvent *evt)
{
    InputKeyEvent *key = evt->u.key.data;
    int btn;

    switch (qemu_input_key_value_to_qcode(key->key)) {
    case Q_KEY_CODE_H:     btn = NANO7_BTN_HOME;  break;
    case Q_KEY_CODE_P:     btn = NANO7_BTN_POWER; break;
    case Q_KEY_CODE_SPC:   btn = NANO7_BTN_PLAY;  break;
    default:               return;
    }
    nano7_pmu_button(btn, key->down);
}

static const QemuInputHandler pmu_key_handler = {
    .name  = "nano7 buttons",
    .mask  = INPUT_EVENT_MASK_KEY,
    .event = pmu_key_event,
};

void nano7_pmu_attach(Nano7I2CBus *bus, const char *script)
{
    Nano7PmuState *s = g_new0(Nano7PmuState, 1);
    Nano7I2CSlave sl = {
        .addr = PMU_ADDR, .opaque = s,
        .start = pmu_start, .write = pmu_write, .read = pmu_read,
    };
    QemuInputHandlerState *hs;

    s->regs[7] = 0x30;      /* home, power released */
    s->regs[8] = 0x02;      /* play/pause released */
    pmu_singleton = s;
    s->script_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, pmu_script_tick, s);
    s->release_timer[NANO7_BTN_HOME] =
        timer_new_ns(QEMU_CLOCK_VIRTUAL, pmu_release_home, s);
    s->release_timer[NANO7_BTN_POWER] =
        timer_new_ns(QEMU_CLOCK_VIRTUAL, pmu_release_power, s);
    s->release_timer[NANO7_BTN_PLAY] =
        timer_new_ns(QEMU_CLOCK_VIRTUAL, pmu_release_play, s);
    nano7_i2c_add_slave(bus, &sl);
    if (script) {
        pmu_parse_script(s, script);
    }
    hs = qemu_input_handler_register(NULL, &pmu_key_handler);
    qemu_input_handler_activate(hs);
}
