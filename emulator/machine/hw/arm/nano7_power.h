/*
 * nano7_power.h -- clocks decoded from CLKCON, clock profiles with the
 * thermal governor, a thermal model behind the PMU's temperature registers,
 * and the CPU busy sampler. See nano7_power.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NANO7_POWER_H
#define NANO7_POWER_H

#include <stdint.h>
#include <stdbool.h>

enum { NANO7_PROFILE_STOCK, NANO7_PROFILE_SAFE, NANO7_PROFILE_MAX,
       NANO7_PROFILE_COUNT };

/* NANO7_CLOCK_PROFILE (run.sh --clock-profile), NANO7_TEMP (--temp) */
void nano7_power_init(void);
double nano7_power_cpu_mhz(void);
double nano7_power_gpu_mhz(void);
/* cumulative 1 kHz samples and how many found the CPU halted (idle) */
void nano7_power_cpu_counts(uint64_t *samples, uint64_t *halted);
double nano7_power_temp(void);
/* -1 = guest-controlled clocks (no governor) */
int nano7_power_profile(void);
const char *nano7_power_profile_name(int profile);
void nano7_power_set_profile(int profile);
/* F11: STOCK -> SAFE -> MAX -> STOCK */
void nano7_power_cycle_profile(void);

#endif
