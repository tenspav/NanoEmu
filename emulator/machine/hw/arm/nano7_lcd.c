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
 * The panel (a command-mode MIPI DSI panel with its own frame memory) is
 * also written directly through the DSI controller at 0x3d800000 by the
 * homebrew SDK (hb_mipi.c: hb_fill_rect, hb_mipi_blit_rgb888) and the
 * homebrew launcher (code at 0x08006xxx): payload words at +0x38 (bytes
 * LSB first), then a packet header at +0x34 (type 0x29 = DCS long write,
 * length << 8) sends them; +0x44 FIFO control is polled for bits 22/20
 * (header/payload FIFO empty) and bit 21 (payload FIFO full); +0x10 config
 * bit28 is cleared per packet. DCS 0x2a / 0x2b set the column / page
 * window, 0x2c / 0x3c write RGB888 pixels from the window start / onward.
 *
 * Model: registers read back what was written. Every 16.67 ms GPIO 14
 * goes low for 1 ms (falling edge -> pending). At each TE the enabled
 * windows are composited (window 0 opaque, window 1 over it with its
 * alpha); a changed composite is copied into the panel memory (GRAM), so
 * direct DSI drawing survives until the OS flips a new frame. GRAM goes to
 * nano7_ui.c (the QEMU console, rotation / zoom) at the TE after a change.
 * The DSI FIFOs always read empty; packets complete at once. With
 * disp-dump=DIR a changed frame is also written as DIR/screen-NNNNN.ppm.
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
#include "nano7_ui.h"
#include "nano7_clock.h"
#include "migration/vmstate.h"

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
    uint32_t fb[NANO7_LCD_W * NANO7_LCD_H];   /* panel GRAM, XRGB: shown */
    uint32_t comp[NANO7_LCD_W * NANO7_LCD_H]; /* last window composite */
    bool dirty;                 /* GRAM changed since the last TE */
    MemoryRegion dsim_mr;       /* 0x3d800000 MIPI DSI controller */
    uint32_t dsim_regs[LCD_SIZE / 4];
    uint8_t pkt[0x4000];        /* payload FIFO */
    unsigned pkt_len;
    unsigned win_x0, win_x1, win_y0, win_y1, cur_x, cur_y;
    uint64_t dsi_packets, dsi_pixels;
    uint32_t last_hash;
    uint64_t tes, dumps, base_changes;
    bool warned_fmt;
    bool written[LCD_SIZE / 4];
    uint64_t updates;           /* frames pushed to the console */
} Nano7LcdState;

static Nano7LcdState *lcd_singleton;

static void lcd_summary(void)
{
    Nano7LcdState *s = lcd_singleton;

    if (s) {
        fprintf(stderr, "[nano7-lcd] summary: %" PRIu64 " TE pulses, %" PRIu64
                " window-base changes, %" PRIu64 " console updates, %" PRIu64
                " screen dumps, %" PRIu64 " DSI packets (%" PRIu64 " px); "
                "written regs:", s->tes, s->base_changes, s->updates,
                s->dumps, s->dsi_packets, s->dsi_pixels);
        for (unsigned i = 0; i < LCD_SIZE / 4; i++) {
            if (s->written[i]) {
                fprintf(stderr, " +0x%03x=%08x", i * 4, s->regs[i]);
            }
        }
        fputc('\n', stderr);
    }
}

static uint32_t lcd_pixel(uint32_t fmt, const uint8_t *p)
{
    if (fmt == 4) {                                 /* RGB565 */
        uint16_t v = lduw_le_p(p);
        uint32_t r = (v >> 11) & 0x1f, g = (v >> 5) & 0x3f, b = v & 0x1f;

        return 0xff000000 | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 |
               (b << 3 | b >> 2);
    }
    return ldl_le_p(p);                             /* ARGB8888 */
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
    g_autofree uint8_t *row = NULL;

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
    row = g_malloc(ww * bpp);
    for (y = 0; y < wh && dy + y < NANO7_LCD_H; y++) {
        address_space_read(&address_space_memory,
                           base + ((sy + y) * stride + sx) * bpp,
                           MEMTXATTRS_UNSPECIFIED, row, ww * bpp);
        for (x = 0; x < ww && dx + x < NANO7_LCD_W; x++) {
            uint32_t p = lcd_pixel(fmt, row + x * bpp);
            uint32_t *o = &s->comp[(dy + y) * NANO7_LCD_W + dx + x];

            if (w == 0) {
                *o = p & 0xffffff;
            } else {
                unsigned a = ((p >> 24) * galpha + 127) / 255;
                uint32_t out = 0;
                int sh;

                for (sh = 0; sh < 24; sh += 8) {
                    unsigned c = (((p >> sh) & 0xff) * a +
                                  ((*o >> sh) & 0xff) * (255 - a) + 127) / 255;

                    out |= c << sh;
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
    unsigned i;
    FILE *f;

    if (s->dumps >= LCD_DUMP_MAX) {
        return;
    }
    for (i = 0; i < ARRAY_SIZE(s->fb); i++) {
        rgb[i * 3] = s->fb[i] >> 16;
        rgb[i * 3 + 1] = s->fb[i] >> 8;
        rgb[i * 3 + 2] = s->fb[i];
    }
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
            "-> %s\n", nano7_now_ns() / 1e9, s->tes,
            s->regs[0x24 / 4], path);
    fflush(stderr);
}

/* Composite the enabled windows; push the frame out if it changed. */
static void lcd_scanout(Nano7LcdState *s)
{
    uint32_t hash = 2166136261u;
    unsigned i;

    memset(s->comp, 0, sizeof(s->comp));
    if (s->regs[0x04 / 4] & 0x10) {
        lcd_window(s, 0);
    }
    if (s->regs[0x04 / 4] & 0x20) {
        lcd_window(s, 1);
    }
    for (i = 0; i < ARRAY_SIZE(s->comp); i++) {
        hash = (hash ^ s->comp[i]) * 16777619u;
    }
    if (s->tes == 1 || hash != s->last_hash) {
        s->last_hash = hash;
        memcpy(s->fb, s->comp, sizeof(s->fb));
        s->dirty = true;
    }
    if (!s->dirty) {
        return;
    }
    s->dirty = false;
    nano7_ui_frame(s->fb);
    s->updates++;
    if (s->dump_dir) {
        lcd_dump(s);
    }
}

static void lcd_te(void *opaque)
{
    Nano7LcdState *s = opaque;
    int64_t now = nano7_now_ns();

    if (!s->te_low) {
        s->te_low = true;
        s->tes++;
        nano7_gpio_set_level(NANO7_GPIO_LCD_TE, 0);
        lcd_scanout(s);
        nano7_timer_mod(s->te_timer, now + LCD_TE_LOW);
    } else {
        s->te_low = false;
        nano7_gpio_set_level(NANO7_GPIO_LCD_TE, 1);
        nano7_timer_mod(s->te_timer, now + LCD_TE_PERIOD - LCD_TE_LOW);
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
                nano7_now_ns() / 1e9,
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

/* ------------------------------------------------ MIPI DSI controller --- */
#define DSIM_CONFIG     0x10
#define DSIM_PKTHDR     0x34
#define DSIM_PAYLOAD    0x38
#define DSIM_FIFOCTRL   0x44
#define DSIM_EMPTY      (1u << 22 | 1u << 20)   /* header + payload FIFO */
#define DSIM_FULL       (1u << 21)

static void dsi_pixel(Nano7LcdState *s, const uint8_t *rgb)
{
    if (s->cur_x < NANO7_LCD_W && s->cur_y < NANO7_LCD_H) {
        s->fb[s->cur_y * NANO7_LCD_W + s->cur_x] =
            rgb[0] << 16 | rgb[1] << 8 | rgb[2];
    }
    s->dsi_pixels++;
    if (++s->cur_x > s->win_x1) {
        s->cur_x = s->win_x0;
        if (++s->cur_y > s->win_y1) {
            s->cur_y = s->win_y0;
        }
    }
}

/* one DCS long write: command byte + parameters */
static void dsi_dcs(Nano7LcdState *s, const uint8_t *p, unsigned len)
{
    unsigned i;

    switch (p[0]) {
    case 0x2a:                  /* SET_COLUMN_ADDRESS */
        if (len >= 5) {
            s->win_x0 = p[1] << 8 | p[2];
            s->win_x1 = p[3] << 8 | p[4];
        }
        break;
    case 0x2b:                  /* SET_PAGE_ADDRESS */
        if (len >= 5) {
            s->win_y0 = p[1] << 8 | p[2];
            s->win_y1 = p[3] << 8 | p[4];
        }
        break;
    case 0x2c:                  /* WRITE_MEMORY_START */
        s->cur_x = s->win_x0;
        s->cur_y = s->win_y0;
        /* fall through */
    case 0x3c:                  /* WRITE_MEMORY_CONTINUE */
        for (i = 1; i + 3 <= len; i += 3) {
            dsi_pixel(s, p + i);
        }
        s->dirty = true;
        break;
    default:
        break;
    }
}

static uint64_t dsim_read(void *opaque, hwaddr off, unsigned size)
{
    Nano7LcdState *s = opaque;

    if (off == DSIM_FIFOCTRL) {
        return (s->dsim_regs[off / 4] | DSIM_EMPTY) & ~DSIM_FULL;
    }
    return s->dsim_regs[off / 4];
}

static void dsim_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    Nano7LcdState *s = opaque;

    switch (off) {
    case DSIM_PAYLOAD:
        if (s->pkt_len + 4 <= sizeof(s->pkt)) {
            stl_le_p(s->pkt + s->pkt_len, v);
            s->pkt_len += 4;
        }
        break;
    case DSIM_PKTHDR: {
        unsigned type = v & 0x3f, len = (v >> 8) & 0xffff;

        if ((type == 0x29 || type == 0x39) && len && len <= s->pkt_len) {
            dsi_dcs(s, s->pkt, len);
        }
        if (s->dsi_packets++ < 8) {
            fprintf(stderr, "[nano7-lcd] t=%.3f s DSI packet type 0x%02x len "
                    "%u cmd 0x%02x\n", nano7_now_ns() /
                    1e9, type, len, s->pkt_len ? s->pkt[0] : 0);
        }
        s->pkt_len = 0;
        break;
    }
    }
    s->dsim_regs[off / 4] = v;
}

static const MemoryRegionOps dsim_ops = {
    .read = dsim_read,
    .write = dsim_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static int lcd_post_load(void *opaque, int version_id)
{
    Nano7LcdState *s = opaque;

    s->dirty = true;            /* push the restored GRAM at the next TE */
    return 0;
}

/* after a state load: show the restored frame now, even while paused (the
 * TE pulse that would push it runs on guest time, frozen by a pause) */
void nano7_lcd_refresh(void)
{
    if (lcd_singleton) {
        nano7_ui_frame(lcd_singleton->fb);
    }
}

static const VMStateDescription vmstate_lcd = {
    .name = "nano7-lcd",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = lcd_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Nano7LcdState, LCD_SIZE / 4),
        VMSTATE_BOOL_ARRAY(written, Nano7LcdState, LCD_SIZE / 4),
        VMSTATE_TIMER_PTR(te_timer, Nano7LcdState),
        VMSTATE_BOOL(te_low, Nano7LcdState),
        VMSTATE_UINT32_ARRAY(fb, Nano7LcdState, NANO7_LCD_W * NANO7_LCD_H),
        VMSTATE_UINT32_ARRAY(comp, Nano7LcdState, NANO7_LCD_W * NANO7_LCD_H),
        VMSTATE_UINT32(last_hash, Nano7LcdState),
        VMSTATE_UINT32_ARRAY(dsim_regs, Nano7LcdState, LCD_SIZE / 4),
        VMSTATE_UINT8_ARRAY(pkt, Nano7LcdState, 0x4000),
        VMSTATE_UINT32(pkt_len, Nano7LcdState),
        VMSTATE_UINT32(win_x0, Nano7LcdState),
        VMSTATE_UINT32(win_x1, Nano7LcdState),
        VMSTATE_UINT32(win_y0, Nano7LcdState),
        VMSTATE_UINT32(win_y1, Nano7LcdState),
        VMSTATE_UINT32(cur_x, Nano7LcdState),
        VMSTATE_UINT32(cur_y, Nano7LcdState),
        VMSTATE_UINT64(tes, Nano7LcdState),
        VMSTATE_END_OF_LIST()
    }
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
    vmstate_register_any(NULL, &vmstate_lcd, s);
    nano7_gpio_model_pin(NANO7_GPIO_LCD_TE, 1);
    s->te_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, lcd_te, s);
    nano7_clock_track(s->te_timer, true);
    nano7_timer_mod(s->te_timer, nano7_now_ns() +
                 LCD_TE_PERIOD);
    memory_region_init_io(&s->mr, NULL, &lcd_ops, s, "nano7.lcd", LCD_SIZE);
    memory_region_add_subregion_overlap(parent, base, &s->mr, 0);
    s->win_x1 = NANO7_LCD_W - 1;
    s->win_y1 = NANO7_LCD_H - 1;
    memory_region_init_io(&s->dsim_mr, NULL, &dsim_ops, s, "nano7.dsim",
                          LCD_SIZE);
    memory_region_add_subregion_overlap(parent, 0x3d800000, &s->dsim_mr, 0);
}
