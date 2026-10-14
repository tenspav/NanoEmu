/*
 * nano7_clock.c -- guest time base for the iPod nano 7G machine.
 *
 * The devices that give RetailOS its sense of time (timer block: 500 Hz
 * scheduler tick and free-running counters; LCD TE pulse; PMU RTC and
 * button script) read nano7_now_ns() and arm their QEMU timers through
 * nano7_deadline(). Normally both are QEMU_CLOCK_VIRTUAL; at speed N guest
 * time advances N times faster than virtual (= real) time, i.e. the guest
 * is fast-forwarded as far as the host CPU keeps up. The clock is piecewise
 * linear: a speed change re-anchors it so guest time stays continuous.
 * Guest-time timers are armed through nano7_timer_mod(), which remembers
 * their guest deadline, so a speed change (or a pause) re-arms every
 * pending one on the new scale. The RTC runs fast too while fast-forwarding.
 * All device timers are registered (nano7_clock_track) so a state load can
 * shift them: the UI keeps QEMU_CLOCK_VIRTUAL monotonic across loads (QEMU
 * restores the saved virtual time, which would leave QEMU's own timers,
 * e.g. VNC's key queue, stranded in the future).
 * Pause freezes guest time (speed 0): guest-time timers stop, while
 * QEMU_CLOCK_VIRTUAL keeps running for QEMU's own timers (e.g. the keyboard
 * queue that delivers the key that un-pauses).
 * Anchors and speed are part of save states.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "nano7_clock.h"
#include "qemu/timer.h"
#include "migration/vmstate.h"

typedef struct Nano7Clock {
    int32_t speed;
    int64_t anchor_virt;        /* QEMU_CLOCK_VIRTUAL at the last change */
    int64_t anchor_guest;       /* guest time at the last change */
} Nano7Clock;

static Nano7Clock clk = { .speed = 1 };

#define CLK_MAX_TIMERS  64

typedef struct ClkTimer {
    QEMUTimer *t;
    int64_t guest;              /* guest-time deadline */
    bool guest_time;            /* armed in guest time (nano7_timer_mod) */
} ClkTimer;

static ClkTimer clk_timers[CLK_MAX_TIMERS];
static unsigned clk_ntimers;

static const VMStateDescription vmstate_clock = {
    .name = "nano7-clock",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT32(speed, Nano7Clock),
        VMSTATE_INT64(anchor_virt, Nano7Clock),
        VMSTATE_INT64(anchor_guest, Nano7Clock),
        VMSTATE_END_OF_LIST()
    }
};

static void clock_register(void)
{
    static bool done;

    if (!done) {
        done = true;
        vmstate_register_any(NULL, &vmstate_clock, &clk);
    }
}

int64_t nano7_now_ns(void)
{
    int64_t v = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    return clk.anchor_guest + (v - clk.anchor_virt) * clk.speed;
}

int64_t nano7_deadline(int64_t guest_ns)
{
    clock_register();
    if (!clk.speed) {
        return INT64_MAX / 2;   /* frozen: re-armed by the next change */
    }
    return clk.anchor_virt + (guest_ns - clk.anchor_guest) / clk.speed;
}

static ClkTimer *clk_find(QEMUTimer *t, bool guest_time)
{
    unsigned i;

    for (i = 0; i < clk_ntimers && clk_timers[i].t != t; i++) {
    }
    if (i == clk_ntimers) {
        assert(clk_ntimers < CLK_MAX_TIMERS);
        clk_timers[clk_ntimers].t = t;
        clk_timers[clk_ntimers].guest_time = guest_time;
        clk_ntimers++;
    }
    return &clk_timers[i];
}

void nano7_clock_track(QEMUTimer *t, bool guest_time)
{
    clk_find(t, guest_time);
}

void nano7_timer_mod(QEMUTimer *t, int64_t guest_deadline)
{
    ClkTimer *c = clk_find(t, true);

    c->guest = guest_deadline;
    timer_mod_ns(t, nano7_deadline(guest_deadline));
}

void nano7_clock_set_speed(int speed)
{
    int64_t g = nano7_now_ns();
    unsigned i;

    clock_register();
    clk.anchor_virt = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    clk.anchor_guest = g;
    clk.speed = MAX(speed, 0);
    for (i = 0; i < clk_ntimers; i++) {     /* re-arm on the new scale */
        if (clk_timers[i].guest_time && timer_pending(clk_timers[i].t)) {
            timer_mod_ns(clk_timers[i].t, nano7_deadline(clk_timers[i].guest));
        }
    }
}

void nano7_clock_after_load(int64_t delta)
{
    unsigned i;

    for (i = 0; i < clk_ntimers; i++) {
        ClkTimer *c = &clk_timers[i];
        int64_t exp;

        if (!timer_pending(c->t)) {
            continue;
        }
        exp = timer_expire_time_ns(c->t);
        if (c->guest_time) {
            if (clk.speed) {    /* guest deadline from the restored timer */
                c->guest = clk.anchor_guest +
                           (exp - clk.anchor_virt) * clk.speed;
            }
        } else {
            timer_mod_ns(c->t, exp + delta);
        }
    }
    clk.anchor_virt += delta;   /* guest time continues where it was saved */
    for (i = 0; i < clk_ntimers; i++) {
        if (clk_timers[i].guest_time && timer_pending(clk_timers[i].t)) {
            timer_mod_ns(clk_timers[i].t, nano7_deadline(clk_timers[i].guest));
        }
    }
}

int nano7_clock_speed(void)
{
    return clk.speed;
}
