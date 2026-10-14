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
 * Timers armed before a change fire at their old real time once; periodic
 * ones re-arm on the new scale. The RTC runs fast too while fast-forwarding.
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
    return clk.anchor_virt + (guest_ns - clk.anchor_guest) / clk.speed;
}

void nano7_clock_set_speed(int speed)
{
    int64_t g = nano7_now_ns();

    clock_register();
    clk.anchor_virt = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    clk.anchor_guest = g;
    clk.speed = MAX(speed, 1);
}

int nano7_clock_speed(void)
{
    return clk.speed;
}
