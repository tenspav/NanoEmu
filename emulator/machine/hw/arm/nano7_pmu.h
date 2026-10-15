/*
 * nano7_pmu.h -- PMU (I2C1 slave 0x73) with the Home/Power/Play buttons for
 * the iPod nano 7G machine. See nano7_pmu.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NANO7_PMU_H
#define NANO7_PMU_H

#include "nano7_i2c.h"

enum { NANO7_BTN_HOME, NANO7_BTN_POWER, NANO7_BTN_PLAY, NANO7_BTN_VOLUP,
       NANO7_BTN_VOLDOWN, NANO7_BTN_COUNT };

/* script: "SEC:BUTTON[:HOLD_MS]/..." (virtual time from start; "/" or
 * ";" separated, -M splits on commas), e.g. "200:home:100/240:power";
 * NULL = none; buttons home, power, play, volup, voldown. Keys: h home,
 * p power, space play, Up/=/+ volume up, Down/- volume down. */
/* rtc: "snapshot" / NULL = the OS's cached time of day at T0, "host" =
 * host clock, else Unix seconds (UTC) at start */
void nano7_pmu_attach(Nano7I2CBus *bus, const char *script, const char *rtc);
void nano7_pmu_button(int btn, bool pressed);
/* a key event (QKeyCode) from the front-end; true if it is a button key */
bool nano7_pmu_key(int qcode, bool down);
/* die temperature for the OS's measurement registers 0x3c..0x3f (OS read
 * 0x0818a4e8: C = ((r5 - r6) * 100 / a) * 4, a = r6 = 500, so r5 = 500 +
 * 1.25 C; the snapshot's 24 C = 0x08 0x84 0x7d 0x7d) */
void nano7_pmu_set_temp(float celsius);
/* set event status bits (regs 1..4) and pull the event line low, as a
 * device wired to the PMU's interrupt inputs would */
void nano7_pmu_raise(unsigned reg, uint8_t bits);

#endif
