/*
 * nano7_clock.h -- guest time base for the iPod nano 7G machine, so the
 * front end can fast-forward (F6 / Tab). See nano7_clock.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NANO7_CLOCK_H
#define NANO7_CLOCK_H

#include "qemu/timer.h"

/* guest nanoseconds: QEMU_CLOCK_VIRTUAL scaled by the current speed */
int64_t nano7_now_ns(void);
/* QEMU_CLOCK_VIRTUAL deadline for a guest-time deadline (for timer_mod_ns) */
int64_t nano7_deadline(int64_t guest_ns);
/* 1, 2, 4...: guest time runs this many times faster than real time;
 * 0 freezes guest time (pause) */
void nano7_clock_set_speed(int speed);
/* arm a QEMU_CLOCK_VIRTUAL timer for a guest-time deadline; re-armed on
 * every speed change */
void nano7_timer_mod(QEMUTimer *t, int64_t guest_deadline);
/* register a device's QEMU_CLOCK_VIRTUAL timer (guest_time: armed with
 * nano7_timer_mod) so speed changes and state loads can re-arm it */
void nano7_clock_track(QEMUTimer *t, bool guest_time);
/* after a state load, once QEMU_CLOCK_VIRTUAL was moved forward by delta to
 * stay monotonic: shift every registered device timer by the same amount */
void nano7_clock_after_load(int64_t delta);
int nano7_clock_speed(void);

#endif
