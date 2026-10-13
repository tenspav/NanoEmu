/*
 * nano7_lcd.c -- 0x38900000 LCD controller of the iPod nano 7G (S5L8747):
 * two scan-out windows read from DRAM, plus the panel's tearing-effect
 * (TE) pulse that paces the OS display driver.
 *
 * RE (Session 18, snap6 build): the screenshot capturer 0x08240860 and
 * the commit 0x08080b70 (from 0x0807558c) use, per window w (0 at +0x20,
 * 1 at +0x40):
 *   +0x00 ctl     alpha bits 7:0, format bits 11:8 (4 RGB565, 7 ARGB8888),
 *                 flags bit16/bit21 (full-screen layer: 0x002107ff)
 *   +0x04 base    DRAM address (bit31 cleared)
 *   +0x08 stride  pixels (& 0xfff)
 *   +0x0c src     x bits 26:16, y bits 10:0
 *   +0x10 size    w bits 24:16, h bits 8:0
 *   +0x14 dst     x << 16 | y on the panel
 *   +0x04 (block) bit4 window 0 enable, bit5 window 1 enable
 * The commit only runs from the TE GPIO callback 0x08060b19 (GPIO 14 =
 * PDAT1 0x3cf00024 bit 6, group 1 bit 6, VIC IRQ 32): without TE pulses
 * new frames are rendered but never flipped. The LCD's own IRQ 13 has an
 * assert-stub handler: never raised here.
 *
 * Model: registers read back what was written. Every 16.67 ms GPIO 14
 * goes low for 1 ms (falling edge -> pending). At each TE the enabled
 * windows are composited (window 0 opaque, window 1 over it with its
 * alpha) into a 240x432 frame; with lcd dumps on (disp-dump=DIR) a
 * changed frame is written as DIR/screen-NNNNN.ppm.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "nano7_lcd.h"
#include "nano7_gpio.h"
#include "qemu/timer.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"
#include "exec/address-spaces.h"

#define LCD_SIZE        0x1000
#define LCD_TE_PERIOD   16666667    /* ns, 60 Hz */
#define LCD_TE_LOW      1000000     /* ns the TE line stays low */
#define LCD_DUMP_MAX    500

typedef struct Nano7LcdState {
    MemoryRegion mr;
    uint32_t regs[LCD_SIZE / 4];
    QEMUTimer *te_timer;
    bool te_low;
    char *dump_dir;
    uint32_t fb[NANO7_LCD_W * NANO7_LCD_H];   /* composited frame, XRGB */
    uint32_t last_hash;
    uint64_t tes, dumps, base_changes;
    bool warned_fmt;
    bool written[LCD_SIZE / 4];
} Nano7LcdState;

static Nano7LcdState *lcd_singleton;

static void lcd_summary(void)
{
    Nano7LcdState *s = lcd_singleton;

    if (s) {
        fprintf(stderr, "[nano7-lcd] summary: %" PRIu64 " TE pulses, %" PRIu64
                " window-base changes, %" PRIu64 " screen dumps; written regs:",
                s->tes, s->base_changes, s->dumps);
        for (unsigned i = 0; i < LCD_SIZE / 4; i++) {
            if (s->written[i]) {
                fprintf(stderr, " +0x%03x=%08x", i * 4, s->regs[i]);
            }
        }
        fputc('\n', stderr);
    }
}

static uint32_t lcd_pixel(uint32_t fmt, uint32_t addr)
{
    if (fmt == 4) {                                 /* RGB565 */
        uint16_t p = address_space_lduw_le(&address_space_memory, addr,
                                           MEMTXATTRS_UNSPECIFIED, NULL);
        uint32_t r = (p >> 11) & 0x1f, g = (p >> 5) & 0x3f, b = p & 0x1f;

        return 0xff000000 | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 |
               (b << 3 | b >> 2);
    }
    return address_space_ldl_le(&address_space_memory, addr,
                                MEMTXATTRS_UNSPECIFIED, NULL);   /* ARGB */
}

static void lcd_window(Nano7LcdState *s, int w)
{
    uint32_t *r = &s->regs[(0x20 + 0x20 * w) / 4];
    uint32_t ctl = r[0], base = r[1] & 0x7fffffff, stride = r[2] & 0xfff;
    uint32_t sx = (r[3] >> 16) & 0x7ff, sy = r[3] & 0x7ff;
    uint32_t ww = (r[4] >> 16) & 0x1ff, wh = r[4] & 0x1ff;
    uint32_t dx = r[5] >> 16, dy = r[5] & 0xffff;
    uint32_t fmt = (ctl >> 8) & 0xf, galpha = ctl & 0xff;
    unsigned bpp = fmt == 4 ? 2 : 4, x, y;

    if (fmt != 4 && fmt != 7) {
        if (!s->warned_fmt) {
            s->warned_fmt = true;
            fprintf(stderr, "[nano7-lcd] window %d format %u not modeled\n",
                    w, fmt);
        }
        return;
    }
    if (!base || !stride) {
        return;
    }
    for (y = 0; y < wh && dy + y < NANO7_LCD_H; y++) {
        for (x = 0; x < ww && dx + x < NANO7_LCD_W; x++) {
            uint32_t p = lcd_pixel(fmt, base + ((sy + y) * stride + sx + x) *
                                        bpp);
            uint32_t *o = &s->fb[(dy + y) * NANO7_LCD_W + dx + x];

            if (w == 0) {
                *o = p & 0xffffff;
            } else {
                unsigned a = ((p >> 24) * galpha + 127) / 255;
                uint32_t out = 0;
                int sh;

                for (sh = 0; sh < 24; sh += 8) {
                    out |= ((((p >> sh) & 0xff) * a +
                             ((*o >> sh) & 0xff) * (255 - a) + 127) / 255) << sh;
                }
                *o = out;
            }
        }
    }
}

static void lcd_dump(Nano7LcdState *s)
{
    g_autofree uint8_t *rgb = g_malloc(sizeof(s->fb) / 4 * 3);
    g_autofree char *path = NULL;
    uint32_t hash = 2166136261u;
    unsigned i;
    FILE *f;

    for (i = 0; i < ARRAY_SIZE(s->fb); i++) {
        rgb[i * 3] = s->fb[i] >> 16;
        rgb[i * 3 + 1] = s->fb[i] >> 8;
        rgb[i * 3 + 2] = s->fb[i];
        hash = (hash ^ s->fb[i]) * 16777619u;
    }
    if ((s->dumps && hash == s->last_hash) || s->dumps >= LCD_DUMP_MAX) {
        return;
    }
    s->last_hash = hash;
    path = g_strdup_printf("%s/screen-%05" PRIu64 ".ppm", s->dump_dir, s->tes);
    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[nano7-lcd] cannot write %s\n", path);
        return;
    }
    fprintf(f, "P6\n%u %u\n255\n", NANO7_LCD_W, NANO7_LCD_H);
    fwrite(rgb, 3, ARRAY_SIZE(s->fb), f);
    fclose(f);
    s->dumps++;
    fprintf(stderr, "[nano7-lcd] t=%.3f s TE %" PRIu64 " screen (win0 0x%08x) "
            "-> %s\n", qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9, s->tes,
            s->regs[0x24 / 4], path);
    fflush(stderr);
}

static void lcd_te(void *opaque)
{
    Nano7LcdState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (!s->te_low) {
        s->te_low = true;
        s->tes++;
        nano7_gpio_set_level(NANO7_GPIO_LCD_TE, 0);
        if (s->dump_dir) {
            memset(s->fb, 0, sizeof(s->fb));
            if (s->regs[0x04 / 4] & 0x10) {
                lcd_window(s, 0);
            }
            if (s->regs[0x04 / 4] & 0x20) {
                lcd_window(s, 1);
            }
            lcd_dump(s);
        }
        timer_mod_ns(s->te_timer, now + LCD_TE_LOW);
    } else {
        s->te_low = false;
        nano7_gpio_set_level(NANO7_GPIO_LCD_TE, 1);
        timer_mod_ns(s->te_timer, now + LCD_TE_PERIOD - LCD_TE_LOW);
    }
}

static uint64_t lcd_read(void *opaque, hwaddr off, unsigned size)
{
    Nano7LcdState *s = opaque;

    return s->regs[off / 4];
}

static void lcd_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    Nano7LcdState *s = opaque;

    if ((off == 0x24 || off == 0x44) && s->regs[off / 4] != v &&
        s->base_changes++ < 16) {
        fprintf(stderr, "[nano7-lcd] t=%.3f s pc=0x%08x window %d base -> "
                "0x%08x (ctl 0x%08x, en 0x%x)\n",
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9,
                current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0,
                off == 0x44, (uint32_t)v, s->regs[(off - 4) / 4],
                (s->regs[0x04 / 4] >> 4) & 3);
    }
    s->regs[off / 4] = v;
    s->written[off / 4] = true;
}

static const MemoryRegionOps lcd_ops = {
    .read = lcd_read,
    .write = lcd_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

void nano7_lcd_attach(MemoryRegion *parent, hwaddr base, const char *dump_dir)
{
    Nano7LcdState *s = g_new0(Nano7LcdState, 1);

    if (dump_dir) {
        s->dump_dir = g_strdup(dump_dir);
        g_mkdir_with_parents(dump_dir, 0755);
    }
    lcd_singleton = s;
    atexit(lcd_summary);
    nano7_gpio_model_pin(NANO7_GPIO_LCD_TE, 1);
    s->te_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, lcd_te, s);
    timer_mod_ns(s->te_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                 LCD_TE_PERIOD);
    memory_region_init_io(&s->mr, NULL, &lcd_ops, s, "nano7.lcd", LCD_SIZE);
    memory_region_add_subregion_overlap(parent, base, &s->mr, 0);
}
