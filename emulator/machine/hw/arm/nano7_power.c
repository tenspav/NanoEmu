/*
 * nano7_power.c -- clocks, clock profiles and a thermal model for the iPod
 * nano 7G machine (Session 60).
 *
 * Clocks are decoded from what the guest programs into CLKCON (0x3c500000,
 * a register file, nano7_regfile.c; docs/research/gpu-clock.md and
 * cpu-deep-dive.md):
 *   CPU  CLKCON+0x00: bits 13:12 source (as the GPU's), bits 3:0 = div - 1:
 *        stock 0x40009001 = PLL1 684 / 2 = 342 MHz, 0x40009000 = 684 MHz
 *        (PLL1 stays locked at 684; DFS only, no voltage scaling). RetailOS
 *        does its own DFS here (IRAM 0x2200098a..: 0x1001 = 342 MHz busy,
 *        0x2001 = PLL2 / 2 = 133 MHz idle), so under RetailOS a profile's
 *        CPU clock holds only until the OS's next switch
 *   GPU  CLKCON+0x1c (domain 20): bits 29:28 source (0 24 MHz, 1 PLL1 684,
 *        2 PLL2 266, 3 PLL3 486), bits 19:16 / 23:20 = div1 - 1 / div2 - 1:
 *        stock PLL2 / 2 = 133 MHz; bits 15:0 are the DRAM domain (left
 *        untouched). RetailOS only flips the gate bits (sleep / wake), which
 *        the decode ignores: they stop the clock, they do not slow it.
 * nano7.c seeds both words with these stock values (the register file
 * otherwise fills with all-ones, which the OS read-modify-writes).
 * The emulated CPU runs at host speed regardless (QEMU TCG; its icount mode
 * cannot change rate at run time), so the CPU clock only feeds the HUD and
 * the thermal model; the GPU clock times the MBX model's render jobs
 * (nano7_mbx.c) and the HUD's GPU load.
 *
 * Profiles (the planned game OS's: STOCK 342 / 133, SAFE 684 / 171, MAX
 * 684 / 266) are applied by writing those registers as the guest would: F11
 * cycles them, run.sh --clock-profile NAME starts with one. Selecting a
 * profile from the emulator also starts the planned governor: above 65 C for
 * 2 s it drops one profile (MAX -> SAFE -> STOCK). Without a selection the
 * guest owns its clocks and no governor runs.
 *
 * Thermal model (illustrative, NOT calibrated against the device): every
 * 100 ms of guest time the die temperature relaxes (tau 45 s) towards 24 C
 * + 35 C * CPU busy * CPU MHz / 684 + 12 C * GPU busy * GPU MHz / 266, and
 * is published through the PMU's measurement registers (nano7_pmu.c, which
 * the OS reads at 0x0818a4e8, in 4 C steps). run.sh --temp C pins it.
 * CPU busy comes from an always-on 1 kHz sampler of the halted state (the
 * RetailOS idle thread waits in WFI); the usage HUD reads it too.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include <math.h>
#include "hw/core/cpu.h"
#include "qemu/timer.h"
#include "nano7_power.h"
#include "nano7_regfile.h"
#include "nano7_pmu.h"
#include "nano7_mbx.h"
#include "nano7_mbxr.h"
#include "nano7_clock.h"

#define PWR_SAMPLE_NS   1000000         /* CPU halted sampler: 1 ms */
#define PWR_THERM_NS    100000000       /* thermal step: 100 ms guest */
#define PWR_TAU_S       45.0
#define PWR_AMBIENT_C   24.0
#define PWR_TRIP_C      65.0
#define PWR_TRIP_S      2.0

#define PLL1_MHZ        684.0
#define PLL2_MHZ        266.0
#define PLL3_MHZ        486.0
#define OSC_MHZ         24.0

static const struct {
    const char *name;
    uint32_t cpu;               /* CLKCON+0x00 */
    uint32_t gpu;               /* CLKCON+0x1c bits 31:16 (bytes 2-3) */
} profiles[NANO7_PROFILE_COUNT] = {
    [NANO7_PROFILE_STOCK] = { "STOCK", 0x40009001u, 0xe0010000u }, /* 342/133 */
    [NANO7_PROFILE_SAFE]  = { "SAFE",  0x40009000u, 0xd0030000u }, /* 684/171 */
    [NANO7_PROFILE_MAX]   = { "MAX",   0x40009000u, 0xe0000000u }, /* 684/266 */
};

typedef struct Power {
    QEMUTimer *sampler, *therm;
    uint64_t samples, halted;   /* cumulative */
    int profile;                /* -1 = guest-controlled */
    double temp_c;
    bool temp_fixed;
    /* thermal step state */
    uint64_t t_samples, t_halted;
    MbxrStats t_st;
    int64_t t_ns;
    double hot_s;               /* time spent above the trip point */
} Power;

static Power pwr = { .profile = -1, .temp_c = PWR_AMBIENT_C };

double nano7_power_cpu_mhz(void)
{
    static const double src[4] = { OSC_MHZ, PLL1_MHZ, PLL2_MHZ, PLL3_MHZ };
    uint32_t v;

    if ((nano7_regfile_peek("clkcon", 0x00, &v) & 3) != 3) {
        return PLL1_MHZ / 2;
    }
    return src[(v >> 12) & 3] / ((v & 0xf) + 1);
}

double nano7_power_gpu_mhz(void)
{
    static const double src[4] = { OSC_MHZ, PLL1_MHZ, PLL2_MHZ, PLL3_MHZ };
    uint32_t v;

    if ((nano7_regfile_peek("clkcon", 0x1c, &v) & 0xc) != 0xc) {
        return PLL2_MHZ / 2;
    }
    return src[(v >> 28) & 3] /
           ((((v >> 16) & 0xf) + 1) * (((v >> 20) & 0xf) + 1));
}

void nano7_power_cpu_counts(uint64_t *samples, uint64_t *halted)
{
    *samples = pwr.samples;
    *halted = pwr.halted;
}

double nano7_power_temp(void)
{
    return pwr.temp_c;
}

int nano7_power_profile(void)
{
    return pwr.profile;
}

const char *nano7_power_profile_name(int p)
{
    return p >= 0 && p < NANO7_PROFILE_COUNT ? profiles[p].name : "GUEST";
}

void nano7_power_set_profile(int p)
{
    if (p < 0 || p >= NANO7_PROFILE_COUNT) {
        return;
    }
    pwr.profile = p;
    pwr.hot_s = 0;
    nano7_regfile_poke("clkcon", 0x00, profiles[p].cpu, 0xf);
    nano7_regfile_poke("clkcon", 0x1c, profiles[p].gpu, 0xc);
    fprintf(stderr, "[nano7-power] profile %s: CPU %.0f MHz, GPU %.0f MHz\n",
            profiles[p].name, nano7_power_cpu_mhz(), nano7_power_gpu_mhz());
}

void nano7_power_cycle_profile(void)
{
    nano7_power_set_profile(pwr.profile < 0 ? NANO7_PROFILE_STOCK :
                            (pwr.profile + 1) % NANO7_PROFILE_COUNT);
}

static void pwr_sample(void *opaque)
{
    pwr.samples++;
    if (first_cpu && first_cpu->halted) {
        pwr.halted++;
    }
    timer_mod_ns(pwr.sampler,
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + PWR_SAMPLE_NS);
}

static void pwr_therm(void *opaque)
{
    int64_t now = nano7_now_ns();
    double dt = (now - pwr.t_ns) / 1e9;
    MbxrStats st;

    nano7_mbx_stats(&st);
    if (dt > 0 && !pwr.temp_fixed) {
        uint64_t ds = pwr.samples - pwr.t_samples;
        double cpu = ds ? 1.0 - (double)(pwr.halted - pwr.t_halted) / ds : 0;
        double gpu = ((double)(st.frags - pwr.t_st.frags) +
                      (double)(st.vgp_ins - pwr.t_st.vgp_ins)) /
                     (nano7_power_gpu_mhz() * 1e6 * dt);
        double target = PWR_AMBIENT_C +
            35.0 * cpu * nano7_power_cpu_mhz() / PLL1_MHZ +
            12.0 * MIN(gpu, 1.0) * nano7_power_gpu_mhz() / PLL2_MHZ;

        pwr.temp_c += (target - pwr.temp_c) * (1.0 - exp(-dt / PWR_TAU_S));
        nano7_pmu_set_temp(pwr.temp_c);
    }
    if (dt > 0 && pwr.profile > NANO7_PROFILE_STOCK) {       /* governor */
        pwr.hot_s = pwr.temp_c > PWR_TRIP_C ? pwr.hot_s + dt : 0;
        if (pwr.hot_s >= PWR_TRIP_S) {
            fprintf(stderr, "[nano7-power] %.1f C > %.0f C: dropping "
                    "profile\n", pwr.temp_c, PWR_TRIP_C);
            nano7_power_set_profile(pwr.profile - 1);
        }
    }
    pwr.t_samples = pwr.samples;
    pwr.t_halted = pwr.halted;
    pwr.t_st = st;
    pwr.t_ns = now;
    nano7_timer_mod(pwr.therm, now + PWR_THERM_NS);
}

void nano7_power_init(void)
{
    const char *p = getenv("NANO7_CLOCK_PROFILE");
    const char *t = getenv("NANO7_TEMP");
    int i;

    pwr.sampler = timer_new_ns(QEMU_CLOCK_VIRTUAL, pwr_sample, NULL);
    timer_mod_ns(pwr.sampler,
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + PWR_SAMPLE_NS);
    pwr.therm = timer_new_ns(QEMU_CLOCK_VIRTUAL, pwr_therm, NULL);
    nano7_clock_track(pwr.therm, true);
    pwr.t_ns = nano7_now_ns();
    nano7_timer_mod(pwr.therm, pwr.t_ns + PWR_THERM_NS);
    if (t && *t) {
        pwr.temp_fixed = true;
        pwr.temp_c = g_ascii_strtod(t, NULL);
        nano7_pmu_set_temp(pwr.temp_c);
    }
    for (i = 0; p && i < NANO7_PROFILE_COUNT; i++) {
        if (!g_ascii_strcasecmp(p, profiles[i].name)) {
            nano7_power_set_profile(i);
        }
    }
    if (p && *p && pwr.profile < 0) {
        fprintf(stderr, "[nano7-power] unknown clock profile '%s' (stock / "
                "safe / max)\n", p);
    }
}
