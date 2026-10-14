/*
 * nano7_pmu.h -- PMU (I2C1 slave 0x73) with the Home/Power/Play buttons for
 * the iPod nano 7G machine. See nano7_pmu.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NANO7_PMU_H
#define NANO7_PMU_H

#include "nano7_i2c.h"

enum { NANO7_BTN_HOME, NANO7_BTN_POWER, NANO7_BTN_PLAY, NANO7_BTN_COUNT };

/* script: "SEC:BUTTON[:HOLD_MS]/..." (virtual time from start; "/" or
 * ";" separated, -M splits on commas), e.g. "200:home:100/240:power";
 * NULL = none. Keys: h = home, p = power, space = play. */
/* rtc: "snapshot" / NULL = the OS's cached time of day at T0, "host" =
 * host clock, else Unix seconds (UTC) at start */
void nano7_pmu_attach(Nano7I2CBus *bus, const char *script, const char *rtc);
void nano7_pmu_button(int btn, bool pressed);

#endif
