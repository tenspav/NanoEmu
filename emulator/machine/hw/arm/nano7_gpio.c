/*
 * nano7_gpio.c -- GPIO interrupt groups and modeled GPIO input levels for
 * the iPod nano 7G (S5L8747).
 *
 * Interrupt groups live inside the 0x39700000 block (whose low registers
 * are the power-gate controller, kept by the regfile underneath); RE'd from
 * IRAM 0x22001180/0x220011a4 and ISR 0x083a33be (Session 17):
 *   GPIO n: bank b = n >> 3, pin p = n & 7
 *   group g = b == 0 ? 0 : 1 + ((b - 1) >> 2)
 *   bit     = b == 0 ? p + 2 : p + 8 * ((b - 1) & 3)
 *   +0x80 + 4g config, +0xa0 + 4g pending (W1C), +0xc0 + 4g mask,
 *   +0xe0 + 4g enable-style (stored)
 *   group -> VIC IRQ: g0 33, g1 32, g2 31, g3 3, g4 2
 * The ISR calls cb[g][bit] for pending & mask, then writes pending back.
 *
 * Input levels: GPIO n reads at 0x3cf00000 + b * 0x20 + 4, bit p. Only
 * pins driven by a model are overlaid (GPIO 0x56 = PMU event line,
 * 0x3cf00144 bit 6, idle high); other bits of that word read back what
 * was written, as the regfile did. A falling edge latches pending.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "nano7_gpio.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"

#define GPIO_GROUPS     8

static const int gpio_group_irq[GPIO_GROUPS] = { 33, 32, 31, 3, 2, -1, -1, -1 };

typedef struct Nano7GpioState {
    MemoryRegion irq_mr;        /* 0x39700080..0x397000ff */
    MemoryRegion lvl_mr;        /* 0x3cf00144: bank 10 level word */
    uint32_t cfg[GPIO_GROUPS], pend[GPIO_GROUPS], mask[GPIO_GROUPS];
    uint32_t extra[GPIO_GROUPS];
    qemu_irq irq[GPIO_GROUPS];
    uint32_t bank10;            /* written value of 0x3cf00144 */
    int pmu_level;              /* GPIO 0x56 */
    unsigned log_lines;
} Nano7GpioState;

static Nano7GpioState *gpio_singleton;

static void gpio_map(unsigned n, unsigned *g, unsigned *bit)
{
    unsigned b = n >> 3, p = n & 7;

    *g = b == 0 ? 0 : 1 + ((b - 1) >> 2);
    *bit = b == 0 ? p + 2 : p + 8 * ((b - 1) & 3);
}

static void gpio_update(Nano7GpioState *s)
{
    int g;

    for (g = 0; g < GPIO_GROUPS; g++) {
        if (s->irq[g]) {
            qemu_set_irq(s->irq[g], (s->pend[g] & s->mask[g]) != 0);
        }
    }
}

/* group-register accesses, logged (capped) to follow the button path */
static void gpio_trace(Nano7GpioState *s, bool wr, hwaddr off, uint32_t v)
{
    if (s->log_lines++ < 200) {
        fprintf(stderr, "[nano7-gpio] pc=0x%08x %s 0x%08x %s 0x%08x\n",
                current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0,
                wr ? "W" : "R", (uint32_t)(0x39700080 + off), wr ? "=" : "->",
                v);
    }
}

static uint64_t gpio_irq_read(void *opaque, hwaddr off, unsigned size)
{
    Nano7GpioState *s = opaque;
    unsigned g = (off & 0x1f) / 4;
    uint32_t v;

    switch (off & ~0x1f) {
    case 0x00: v = s->cfg[g]; break;
    case 0x20: v = s->pend[g]; break;
    case 0x40: v = s->mask[g]; break;
    default:   v = s->extra[g]; break;
    }
    gpio_trace(s, false, off, v);
    return v;
}

static void gpio_irq_write(void *opaque, hwaddr off, uint64_t v,
                           unsigned size)
{
    Nano7GpioState *s = opaque;
    unsigned g = (off & 0x1f) / 4;

    gpio_trace(s, true, off, v);
    switch (off & ~0x1f) {
    case 0x00: s->cfg[g] = v; break;
    case 0x20: s->pend[g] &= ~(uint32_t)v; break;
    case 0x40: s->mask[g] = v; break;
    default:   s->extra[g] = v; break;
    }
    gpio_update(s);
}

static const MemoryRegionOps gpio_irq_ops = {
    .read = gpio_irq_read,
    .write = gpio_irq_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static uint64_t gpio_lvl_read(void *opaque, hwaddr off, unsigned size)
{
    Nano7GpioState *s = opaque;

    return (s->bank10 & ~(1u << 6)) | (s->pmu_level ? 1u << 6 : 0);
}

static void gpio_lvl_write(void *opaque, hwaddr off, uint64_t v,
                           unsigned size)
{
    Nano7GpioState *s = opaque;

    s->bank10 = v;
}

static const MemoryRegionOps gpio_lvl_ops = {
    .read = gpio_lvl_read,
    .write = gpio_lvl_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

void nano7_gpio_set_level(unsigned n, int level)
{
    Nano7GpioState *s = gpio_singleton;
    unsigned g, bit;

    if (!s || n != NANO7_GPIO_PMU_IRQ) {
        return;
    }
    level = !!level;
    if (s->pmu_level && !level) {
        gpio_map(n, &g, &bit);
        s->pend[g] |= 1u << bit;
        if (s->log_lines++ < 64) {
            fprintf(stderr, "[nano7-gpio] GPIO 0x%x falling edge -> group %u "
                    "bit %u pending (mask 0x%08x, IRQ %d)\n", n, g, bit,
                    s->mask[g], gpio_group_irq[g]);
        }
        gpio_update(s);
    }
    s->pmu_level = level;
}

void nano7_gpio_attach(MemoryRegion *parent, qemu_irq *vic_in)
{
    Nano7GpioState *s = g_new0(Nano7GpioState, 1);
    int g;

    for (g = 0; g < GPIO_GROUPS; g++) {
        if (gpio_group_irq[g] >= 0) {
            s->irq[g] = vic_in[gpio_group_irq[g]];
        }
    }
    s->pmu_level = 1;
    gpio_singleton = s;
    memory_region_init_io(&s->irq_mr, NULL, &gpio_irq_ops, s,
                          "nano7.gpio-irq", 0x80);
    memory_region_add_subregion_overlap(parent, 0x39700080, &s->irq_mr, 1);
    memory_region_init_io(&s->lvl_mr, NULL, &gpio_lvl_ops, s,
                          "nano7.gpio-lvl", 4);
    memory_region_add_subregion_overlap(parent, 0x3cf00144, &s->lvl_mr, 1);
}
