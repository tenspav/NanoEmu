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
 * pins driven by a model (nano7_gpio_model_pin) are overlaid, e.g. GPIO
 * 0x56 = PMU event line (0x3cf00144 bit 6), GPIO 14 = panel TE (0x3cf00024
 * bit 6); other bits of those words read back what was written, as the
 * regfile did. A falling edge latches pending. NANO7_GPIO_TRACE=1 logs
 * group-register accesses.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "nano7_gpio.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"

#define GPIO_GROUPS     8
#define GPIO_PINS       256
#define GPIO_BANK_WORDS 8

static const int gpio_group_irq[GPIO_GROUPS] = { 33, 32, 31, 3, 2, -1, -1, -1 };

typedef struct GpioBankWord {   /* overlay of one bank's level word */
    MemoryRegion mr;
    unsigned bank;
    uint32_t written;           /* value last written (unmodeled bits) */
    uint32_t model_mask;        /* bits driven by models */
    uint32_t level;             /* their levels */
} GpioBankWord;

typedef struct Nano7GpioState {
    MemoryRegion irq_mr;        /* 0x39700080..0x397000ff */
    MemoryRegion *parent;
    uint32_t cfg[GPIO_GROUPS], pend[GPIO_GROUPS], mask[GPIO_GROUPS];
    uint32_t extra[GPIO_GROUPS];
    qemu_irq irq[GPIO_GROUPS];
    GpioBankWord banks[GPIO_BANK_WORDS];
    unsigned nbanks;
    uint8_t edge_logs[GPIO_PINS];
    bool trace;
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
    if (s->trace && s->log_lines++ < 2000) {
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
    GpioBankWord *w = opaque;

    return (w->written & ~w->model_mask) | (w->level & w->model_mask);
}

static void gpio_lvl_write(void *opaque, hwaddr off, uint64_t v,
                           unsigned size)
{
    GpioBankWord *w = opaque;

    w->written = v;
}

static const MemoryRegionOps gpio_lvl_ops = {
    .read = gpio_lvl_read,
    .write = gpio_lvl_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static GpioBankWord *gpio_bank(Nano7GpioState *s, unsigned bank)
{
    unsigned i;

    for (i = 0; i < s->nbanks; i++) {
        if (s->banks[i].bank == bank) {
            return &s->banks[i];
        }
    }
    return NULL;
}

void nano7_gpio_model_pin(unsigned n, int level)
{
    Nano7GpioState *s = gpio_singleton;
    GpioBankWord *w;

    if (!s || n >= GPIO_PINS) {
        return;
    }
    w = gpio_bank(s, n >> 3);
    if (!w) {
        g_autofree char *name = g_strdup_printf("nano7.gpio-lvl%u", n >> 3);

        assert(s->nbanks < GPIO_BANK_WORDS);
        w = &s->banks[s->nbanks++];
        w->bank = n >> 3;
        memory_region_init_io(&w->mr, NULL, &gpio_lvl_ops, w, name, 4);
        memory_region_add_subregion_overlap(s->parent,
                                            0x3cf00000 + w->bank * 0x20 + 4,
                                            &w->mr, 1);
    }
    w->model_mask |= 1u << (n & 7);
    w->level = (w->level & ~(1u << (n & 7))) | (level ? 1u << (n & 7) : 0);
}

bool nano7_gpio_irq_armed(unsigned n)
{
    Nano7GpioState *s = gpio_singleton;
    unsigned g, bit;

    if (!s) {
        return false;
    }
    gpio_map(n, &g, &bit);
    return g < GPIO_GROUPS && (s->mask[g] >> bit & 1);
}

void nano7_gpio_set_level(unsigned n, int level)
{
    Nano7GpioState *s = gpio_singleton;
    GpioBankWord *w = s && n < GPIO_PINS ? gpio_bank(s, n >> 3) : NULL;
    uint32_t bit = 1u << (n & 7);
    unsigned g, gbit;

    if (!w || !(w->model_mask & bit)) {
        return;
    }
    if ((w->level & bit) && !level) {
        gpio_map(n, &g, &gbit);
        s->pend[g] |= 1u << gbit;
        if (s->edge_logs[n] < 8) {
            s->edge_logs[n]++;
            fprintf(stderr, "[nano7-gpio] GPIO 0x%x falling edge -> group %u "
                    "bit %u pending (mask 0x%08x, IRQ %d)%s\n", n, g, gbit,
                    s->mask[g], gpio_group_irq[g],
                    s->edge_logs[n] == 8 ? " (further edges not logged)" : "");
        }
        gpio_update(s);
    }
    w->level = level ? w->level | bit : w->level & ~bit;
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
    s->parent = parent;
    s->trace = getenv("NANO7_GPIO_TRACE") != NULL;
    gpio_singleton = s;
    memory_region_init_io(&s->irq_mr, NULL, &gpio_irq_ops, s,
                          "nano7.gpio-irq", 0x80);
    memory_region_add_subregion_overlap(parent, 0x39700080, &s->irq_mr, 1);
}
