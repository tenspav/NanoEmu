/*
 * nano7_skin.c -- the device frame around the screen: an iPod nano 7G body
 * (slate glass front, aluminium edge) with its physical buttons, drawn for
 * the desktop front end (nano7_ui.c, NANO7_FRAME=1 / run.sh --frame).
 *
 * Geometry in LCD-pixel units (the 2.5" 240x432 panel is 202 ppi, so one
 * unit = 0.126 mm): body 39.6 x 76.5 mm = 314 x 607 units, corner radius
 * 44; screen inset 37 units at the sides, 66 from the top; the Home button
 * (54 units across) centred in the bottom bezel; Sleep/Wake on the top edge
 * towards the right, Volume + / Play-Pause / Volume - on the left edge, as
 * on the real device. The buttons are drawn proud of the body so they can
 * be clicked; small glyphs on the background name them.
 *
 * Everything is drawn at the target zoom (no upscaled bitmap): shapes are
 * signed-distance functions with one pixel of antialiasing, glyphs are
 * 4x4 supersampled. The static layer is cached per (zoom, HUD, pressed
 * buttons); a frame copies it and scales the panel in with whole factors.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include <math.h>
#include "nano7_skin.h"
#include "nano7_pmu.h"
#include "nano7_hud.h"

#define LCD_W       240
#define LCD_H       432
/* compact body (thin bezels) so the whole window fits a 13" screen at
 * 1.5x: 536 units tall = 804 points */
#define PAD_X       26.0                /* canvas margin left / right */
#define PAD_Y       14.0                /* canvas margin top / bottom */
#define BODY_W      268.0
#define BODY_H      508.0
#define BODY_R      36.0
#define EDGE        3.0                 /* aluminium band around the glass */
#define SCR_X       (PAD_X + 14.0)
#define SCR_Y       (PAD_Y + 26.0)
#define HOME_CX     (PAD_X + BODY_W / 2)
#define HOME_CY     (SCR_Y + LCD_H + (BODY_H - 26.0 - LCD_H) / 2)
#define HOME_R      20.0
#define BTN_OUT     5.5                 /* how far a button stands proud */

#define C_BG        0x1d1e21
#define C_EDGE_TOP  0xa9adb3
#define C_EDGE_BOT  0x7b7f86
#define C_GLASS     0x111215
#define C_GLASS_HI  0x1c1d21
#define C_BTN       0x9a9ea5
#define C_BTN_DOWN  0x5d6066
#define C_HOME      0x17181b
#define C_HOME_DOWN 0x2e3035
#define C_HOME_RING 0x3b3d43
#define C_HOME_ICON 0x8c8f95
#define C_GLYPH     0x74777d
#define C_GLYPH_DN  0xd0d3d8

typedef struct SkinBtn {
    int id;
    double x0, y0, x1, y1;              /* drawn pill (units) */
} SkinBtn;

/* side and top buttons; their inner half hides under the body */
static const SkinBtn skin_btns[] = {
    { NANO7_BTN_POWER,   PAD_X + 186, PAD_Y - BTN_OUT, PAD_X + 236,
      PAD_Y + 6 },
    { NANO7_BTN_VOLUP,   PAD_X - BTN_OUT, PAD_Y + 100, PAD_X + 6, PAD_Y + 150 },
    { NANO7_BTN_PLAY,    PAD_X - BTN_OUT, PAD_Y + 166, PAD_X + 6, PAD_Y + 196 },
    { NANO7_BTN_VOLDOWN, PAD_X - BTN_OUT, PAD_Y + 212, PAD_X + 6, PAD_Y + 262 },
};

static struct {
    uint32_t *layer;
    int z, w, h;
    unsigned pressed;
} cache;

void nano7_skin_size(int *w, int *h)
{
    *w = (int)(BODY_W + 2 * PAD_X);
    *h = (int)(BODY_H + 2 * PAD_Y);
}

/* ------------------------------------------------------------ drawing --- */
static double sd_rrect(double x, double y, double x0, double y0, double x1,
                       double y1, double r)
{
    double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    double hx = (x1 - x0) / 2 - r, hy = (y1 - y0) / 2 - r;
    double qx = fabs(x - cx) - hx, qy = fabs(y - cy) - hy;

    return hypot(MAX(qx, 0.0), MAX(qy, 0.0)) + MIN(MAX(qx, qy), 0.0) - r;
}

static uint32_t mix(uint32_t d, uint32_t c, double a)
{
    uint32_t o = 0;
    int sh;

    if (a <= 0) {
        return d;
    }
    if (a >= 1) {
        return c;
    }
    for (sh = 0; sh < 24; sh += 8) {
        double v = ((d >> sh) & 0xff) * (1 - a) + ((c >> sh) & 0xff) * a;

        o |= (uint32_t)lrint(v) << sh;
    }
    return o;
}

static uint32_t lerp_rgb(uint32_t a, uint32_t b, double t)
{
    return mix(a, b, MIN(MAX(t, 0.0), 1.0));
}

/* coverage of a shape with signed distance d (units) at zoom z */
static double cover(double d, int z)
{
    return MIN(MAX(0.5 - d * z, 0.0), 1.0);
}

typedef double (*SdFn)(double x, double y, const void *arg);

/* paint the shape sd over the canvas bounding box [x0,x1) x [y0,y1) */
static void paint(uint32_t *buf, int w, int h, int z, double x0, double y0,
                  double x1, double y1, SdFn sd, const void *arg,
                  uint32_t (*color)(double x, double y, const void *arg))
{
    int px0 = MAX(0, (int)floor(x0 * z) - 1), px1 = MIN(w, (int)ceil(x1 * z) + 1);
    int py0 = MAX(0, (int)floor(y0 * z) - 1), py1 = MIN(h, (int)ceil(y1 * z) + 1);
    int px, py;

    for (py = py0; py < py1; py++) {
        double y = (py + 0.5) / z;

        for (px = px0; px < px1; px++) {
            double x = (px + 0.5) / z;
            double a = cover(sd(x, y, arg), z);

            if (a > 0) {
                buf[py * w + px] = mix(buf[py * w + px], color(x, y, arg), a);
            }
        }
    }
}

typedef struct RR {
    double x0, y0, x1, y1, r;
    uint32_t c, c2;                     /* c2: vertical gradient end */
} RR;

static double rr_sd(double x, double y, const void *arg)
{
    const RR *q = arg;

    return sd_rrect(x, y, q->x0, q->y0, q->x1, q->y1, q->r);
}

static uint32_t rr_color(double x, double y, const void *arg)
{
    const RR *q = arg;

    return lerp_rgb(q->c, q->c2, (y - q->y0) / (q->y1 - q->y0));
}

static void rrect(uint32_t *buf, int w, int h, int z, double x0, double y0,
                  double x1, double y1, double r, uint32_t c, uint32_t c2)
{
    RR q = { x0, y0, x1, y1, MIN(r, MIN(x1 - x0, y1 - y0) / 2), c, c2 };

    paint(buf, w, h, z, x0, y0, x1, y1, rr_sd, &q, rr_color);
}

typedef struct Ring {
    double cx, cy, r, t;                /* t = 0: filled disc */
    uint32_t c;
} Ring;

static double ring_sd(double x, double y, const void *arg)
{
    const Ring *q = arg;
    double d = hypot(x - q->cx, y - q->cy) - q->r;

    return q->t > 0 ? fabs(d) - q->t / 2 : d;
}

static uint32_t ring_color(double x, double y, const void *arg)
{
    return ((const Ring *)arg)->c;
}

static void ring(uint32_t *buf, int w, int h, int z, double cx, double cy,
                 double r, double t, uint32_t c)
{
    Ring q = { cx, cy, r, t, c };
    double e = r + t;

    paint(buf, w, h, z, cx - e, cy - e, cx + e, cy + e, ring_sd, &q,
          ring_color);
}

/* hollow rounded square of stroke t (the Home button's icon) */
typedef struct RRStroke {
    double x0, y0, x1, y1, r, t;
    uint32_t c;
} RRStroke;

static double rrs_sd(double x, double y, const void *arg)
{
    const RRStroke *q = arg;

    return fabs(sd_rrect(x, y, q->x0, q->y0, q->x1, q->y1, q->r)) - q->t / 2;
}

static uint32_t rrs_color(double x, double y, const void *arg)
{
    return ((const RRStroke *)arg)->c;
}

/* filled triangle (play glyph), 4x4 supersampled */
static void triangle(uint32_t *buf, int w, int h, int z, const double v[6],
                     uint32_t c)
{
    double x0 = MIN(v[0], MIN(v[2], v[4])), x1 = MAX(v[0], MAX(v[2], v[4]));
    double y0 = MIN(v[1], MIN(v[3], v[5])), y1 = MAX(v[1], MAX(v[3], v[5]));
    int px, py, i, j;

    for (py = MAX(0, (int)floor(y0 * z)); py < MIN(h, (int)ceil(y1 * z)); py++) {
        for (px = MAX(0, (int)floor(x0 * z)); px < MIN(w, (int)ceil(x1 * z));
             px++) {
            int in = 0;

            for (j = 0; j < 4; j++) {
                for (i = 0; i < 4; i++) {
                    double x = (px + (i + 0.5) / 4) / z;
                    double y = (py + (j + 0.5) / 4) / z;
                    double e0 = (v[2] - v[0]) * (y - v[1]) -
                                (v[3] - v[1]) * (x - v[0]);
                    double e1 = (v[4] - v[2]) * (y - v[3]) -
                                (v[5] - v[3]) * (x - v[2]);
                    double e2 = (v[0] - v[4]) * (y - v[5]) -
                                (v[1] - v[5]) * (x - v[4]);

                    in += (e0 >= 0 && e1 >= 0 && e2 >= 0) ||
                          (e0 <= 0 && e1 <= 0 && e2 <= 0);
                }
            }
            buf[py * w + px] = mix(buf[py * w + px], c, in / 16.0);
        }
    }
}

static const SkinBtn *skin_btn(int id)
{
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(skin_btns); i++) {
        if (skin_btns[i].id == id) {
            return &skin_btns[i];
        }
    }
    return NULL;
}

/* the glyph naming a button, on the background next to it */
static void glyph(uint32_t *buf, int w, int h, int z, int id, bool down)
{
    const SkinBtn *b = skin_btn(id);
    uint32_t c = down ? C_GLYPH_DN : C_GLYPH;
    double cx, cy;

    if (!b) {
        return;
    }
    if (id == NANO7_BTN_POWER) {        /* power symbol above the button */
        cx = b->x0 - 10;                /* left of the button */
        cy = PAD_Y - 2;
        ring(buf, w, h, z, cx, cy, 4.2, 1.3, c);
        /* gap at the top of the ring, then the bar */
        rrect(buf, w, h, z, cx - 2.2, cy - 6.5, cx + 2.2, cy - 1.5, 0, C_BG,
              C_BG);
        rrect(buf, w, h, z, cx - 0.65, cy - 6.2, cx + 0.65, cy + 0.2, 0.65, c,
              c);
        return;
    }
    cx = b->x0 - 10;
    cy = (b->y0 + b->y1) / 2;
    switch (id) {
    case NANO7_BTN_VOLUP:
        rrect(buf, w, h, z, cx - 4, cy - 0.7, cx + 4, cy + 0.7, 0.7, c, c);
        rrect(buf, w, h, z, cx - 0.7, cy - 4, cx + 0.7, cy + 4, 0.7, c, c);
        break;
    case NANO7_BTN_VOLDOWN:
        rrect(buf, w, h, z, cx - 4, cy - 0.7, cx + 4, cy + 0.7, 0.7, c, c);
        break;
    case NANO7_BTN_PLAY: {
        const double v[6] = { cx - 4.5, cy - 4, cx - 4.5, cy + 4, cx + 0.5, cy };

        triangle(buf, w, h, z, v, c);
        rrect(buf, w, h, z, cx + 1.6, cy - 4, cx + 2.8, cy + 4, 0, c, c);
        rrect(buf, w, h, z, cx + 4.0, cy - 4, cx + 5.2, cy + 4, 0, c, c);
        break;
    }
    }
}

static void draw_layer(uint32_t *buf, int z, int w, int h, unsigned pressed)
{
    double bx0 = PAD_X, by0 = PAD_Y, bx1 = PAD_X + BODY_W, by1 = PAD_Y + BODY_H;
    RRStroke icon;
    unsigned i;
    int k;

    for (k = 0; k < w * h; k++) {
        buf[k] = C_BG;
    }
    /* buttons first: the body covers their inner half */
    for (i = 0; i < ARRAY_SIZE(skin_btns); i++) {
        const SkinBtn *b = &skin_btns[i];
        bool down = pressed & (1u << b->id);
        uint32_t c = down ? C_BTN_DOWN : C_BTN;

        rrect(buf, w, h, z, b->x0, b->y0, b->x1, b->y1, 3, c, mix(c, 0, 0.25));
        glyph(buf, w, h, z, b->id, down);
    }
    /* aluminium edge, then the glass front */
    rrect(buf, w, h, z, bx0, by0, bx1, by1, BODY_R, C_EDGE_TOP, C_EDGE_BOT);
    rrect(buf, w, h, z, bx0 + EDGE, by0 + EDGE, bx1 - EDGE, by1 - EDGE,
          BODY_R - EDGE, C_GLASS_HI, C_GLASS);
    /* screen opening (black until the first frame) */
    rrect(buf, w, h, z, SCR_X - 1, SCR_Y - 1, SCR_X + LCD_W + 1,
          SCR_Y + LCD_H + 1, 1.5, 0x000000, 0x000000);
    /* Home button */
    ring(buf, w, h, z, HOME_CX, HOME_CY, HOME_R, 0,
         pressed & (1u << NANO7_BTN_HOME) ? C_HOME_DOWN : C_HOME);
    ring(buf, w, h, z, HOME_CX, HOME_CY, HOME_R, 1.4, C_HOME_RING);
    icon = (RRStroke){ HOME_CX - 7.5, HOME_CY - 7.5, HOME_CX + 7.5,
                       HOME_CY + 7.5, 3.5, 1.4, C_HOME_ICON };
    paint(buf, w, h, z, icon.x0 - 2, icon.y0 - 2, icon.x1 + 2, icon.y1 + 2,
          rrs_sd, &icon, rrs_color);
}

void nano7_skin_draw(uint32_t *buf, int z, const uint32_t *lcd,
                     unsigned pressed)
{
    int cw, ch, w, h, x, y;

    nano7_skin_size(&cw, &ch);
    w = cw * z;
    h = ch * z;
    if (!cache.layer || cache.z != z || cache.pressed != pressed) {
        g_free(cache.layer);
        cache.layer = g_new(uint32_t, w * h);
        cache.z = z;
        cache.w = w;
        cache.h = h;
        cache.pressed = pressed;
        draw_layer(cache.layer, z, w, h, pressed);
    }
    memcpy(buf, cache.layer, sizeof(uint32_t) * w * h);
    if (lcd) {
        int sx = (int)SCR_X * z, sy = (int)SCR_Y * z;

        for (y = 0; y < LCD_H * z; y++) {
            const uint32_t *src = lcd + (y / z) * LCD_W;
            uint32_t *dst = buf + (sy + y) * w + sx;

            for (x = 0; x < LCD_W * z; x++) {
                dst[x] = src[x / z];
            }
        }
    }
}

/* ------------------------------------------------------------ pointer --- */
void nano7_skin_screen_xy(double x, double y, int *px, int *py)
{
    *px = MIN(MAX((int)floor(x - SCR_X), 0), LCD_W - 1);
    *py = MIN(MAX((int)floor(y - SCR_Y), 0), LCD_H - 1);
}

int nano7_skin_hit(double x, double y, int *px, int *py)
{
    unsigned i;

    if (x >= SCR_X && x < SCR_X + LCD_W && y >= SCR_Y && y < SCR_Y + LCD_H) {
        nano7_skin_screen_xy(x, y, px, py);
        return NANO7_SKIN_SCREEN;
    }
    if (hypot(x - HOME_CX, y - HOME_CY) <= HOME_R + 4) {
        return NANO7_BTN_HOME;
    }
    for (i = 0; i < ARRAY_SIZE(skin_btns); i++) {   /* generous margins */
        const SkinBtn *b = &skin_btns[i];
        bool top = b->id == NANO7_BTN_POWER;
        double mx = top ? 4 : 16, my = top ? 16 : 4;

        if (x >= b->x0 - mx && x < b->x1 + (top ? mx : 10) &&
            y >= b->y0 - my && y < b->y1 + (top ? 10 : my)) {
            return b->id;
        }
    }
    return NANO7_SKIN_NONE;
}

/* ------------------------------------------------------- side panel --- */
/*
 * The panel right of the device (NANO7_PANEL_W units wide): live numbers in
 * plain words (FPS, CPU / GPU clock and load, RAM, temperature) and labelled
 * buttons: clock profile Stock / Safe / Max, the iPod's buttons, and the
 * emulator's Pause / Reset / Mute / Rotate / Screenshot. Text is a 5x7 font
 * drawn at any size (4x4 supersampled). Redrawn only when its content or
 * the pressed button changes.
 */
#define P_PAD       12.0
#define P_IN        (NANO7_PANEL_W - 2 * P_PAD)     /* inner width, 176 */
#define C_P_CARD    0x26282c
#define C_P_TEXT    0xe8eaed
#define C_P_DIM     0x8a8f98
#define C_P_FPS     0x30d158
#define C_P_BTN     0x3a3d43
#define C_P_BTN_DN  0x5a5e66
#define C_P_ACCENT  0x0a84ff
#define C_P_WARN    0xff9f0a

static const struct {
    char c;
    uint8_t row[7];                     /* bit 4 = left column */
} p_font[] = {
    { 'A', { 0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 } },
    { 'B', { 0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e } },
    { 'C', { 0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e } },
    { 'D', { 0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e } },
    { 'E', { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f } },
    { 'F', { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10 } },
    { 'G', { 0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f } },
    { 'H', { 0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 } },
    { 'I', { 0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e } },
    { 'J', { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0c } },
    { 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } },
    { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f } },
    { 'M', { 0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11 } },
    { 'N', { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 } },
    { 'O', { 0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e } },
    { 'P', { 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10 } },
    { 'Q', { 0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d } },
    { 'R', { 0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11 } },
    { 'S', { 0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e } },
    { 'T', { 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
    { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e } },
    { 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04 } },
    { 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a } },
    { 'X', { 0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11 } },
    { 'Y', { 0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04 } },
    { 'Z', { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f } },
    { '0', { 0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e } },
    { '1', { 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e } },
    { '2', { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f } },
    { '3', { 0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e } },
    { '4', { 0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02 } },
    { '5', { 0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e } },
    { '6', { 0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e } },
    { '7', { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
    { '8', { 0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e } },
    { '9', { 0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c } },
    { '/', { 0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10 } },
    { '%', { 0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03 } },
    { '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c } },
    { '-', { 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00 } },
    { '+', { 0x00, 0x04, 0x04, 0x1f, 0x04, 0x04, 0x00 } },
    { ':', { 0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x0c, 0x00 } },
};

static const uint8_t *p_glyph(char c)
{
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(p_font); i++) {
        if (p_font[i].c == c) {
            return p_font[i].row;
        }
    }
    return NULL;                        /* space and unknown characters */
}

/* advance of one character at dot size s (5 dots + 1 gap) */
#define P_ADV(s)    (6 * (s))

static double p_text_w(const char *t, double s)
{
    size_t n = strlen(t);

    return n ? n * P_ADV(s) - s : 0;
}

/* text with its top-left at (x, y) units, dot size s units */
static void p_text(uint32_t *buf, int stride, int h, int z, double x,
                   double y, double s, const char *t, uint32_t c)
{
    double x1 = x + p_text_w(t, s), y1 = y + 7 * s;
    int px0 = MAX(0, (int)floor(x * z)), px1 = (int)ceil(x1 * z);
    int py0 = MAX(0, (int)floor(y * z)), py1 = MIN(h, (int)ceil(y1 * z));
    size_t n = strlen(t);
    int px, py, i, j;

    for (py = py0; py < py1; py++) {
        for (px = px0; px < px1 && px < stride; px++) {
            int in = 0;

            for (j = 0; j < 4; j++) {
                double uy = (py + (j + 0.5) / 4) / z - y;
                int row = (int)floor(uy / s);

                if (row < 0 || row > 6) {
                    continue;
                }
                for (i = 0; i < 4; i++) {
                    double ux = (px + (i + 0.5) / 4) / z - x;
                    int ci = (int)floor(ux / P_ADV(s));
                    int col = (int)floor((ux - ci * P_ADV(s)) / s);
                    const uint8_t *g;

                    if (ci < 0 || (size_t)ci >= n || col > 4 ||
                        !(g = p_glyph(t[ci]))) {
                        continue;
                    }
                    in += (g[row] >> (4 - col)) & 1;
                }
            }
            if (in) {
                buf[py * stride + px] = mix(buf[py * stride + px], c,
                                            in / 16.0);
            }
        }
    }
}

typedef struct PBtn {
    int id;
    double x, y, w, h;                  /* panel units */
    const char *label;
} PBtn;

/* panel layout (x relative to the panel, units); labels are defaults */
static const PBtn p_btns[] = {
    { NANO7_SKIN_P_STOCK, P_PAD,       194, 56, 30, "STOCK" },
    { NANO7_SKIN_P_SAFE,  P_PAD + 60,  194, 56, 30, "SAFE" },
    { NANO7_SKIN_P_MAX,   P_PAD + 120, 194, 56, 30, "MAX" },
    { NANO7_BTN_HOME,     P_PAD,       258, 86, 30, "HOME" },
    { NANO7_BTN_POWER,    P_PAD + 90,  258, 86, 30, "SLEEP" },
    { NANO7_BTN_VOLDOWN,  P_PAD,       292, 86, 30, "VOL -" },
    { NANO7_BTN_VOLUP,    P_PAD + 90,  292, 86, 30, "VOL +" },
    { NANO7_BTN_PLAY,     P_PAD,       326, 176, 30, "PLAY / PAUSE" },
    { NANO7_SKIN_P_PAUSE, P_PAD,       390, 86, 30, "PAUSE" },
    { NANO7_SKIN_P_RESET, P_PAD + 90,  390, 86, 30, "RESET" },
    { NANO7_SKIN_P_MUTE,  P_PAD,       424, 86, 30, "MUTE" },
    { NANO7_SKIN_P_ROTATE, P_PAD + 90, 424, 86, 30, "ROTATE" },
    { NANO7_SKIN_P_SHOT,  P_PAD,       458, 176, 30, "SCREENSHOT" },
};

static Nano7PanelInfo p_last;
static int p_last_z = -1, p_last_h, p_last_pressed;
static uint32_t *p_cache;

static void p_draw(uint32_t *buf, int w, int h, int z,
                   const Nano7PanelInfo *pi, int pressed)
{
    char line[48];
    unsigned i;
    int k;

    for (k = 0; k < w * h; k++) {
        buf[k] = C_BG;
    }
    /* numbers card */
    rrect(buf, w, h, z, P_PAD - 4, 10, P_PAD + P_IN + 4, 158, 8, C_P_CARD,
          C_P_CARD);
    snprintf(line, sizeof(line), "%d", (int)(pi->fps + 0.5f));
    p_text(buf, w, h, z, P_PAD + 4, 20, 4, line, C_P_FPS);
    p_text(buf, w, h, z, P_PAD + 8 + p_text_w(line, 4), 34, 2, "FPS",
           C_P_DIM);
    snprintf(line, sizeof(line), "CPU %3dMHZ %3d%%", (int)(pi->cpu_mhz + .5),
             (int)(pi->cpu * 100 + .5f));
    p_text(buf, w, h, z, P_PAD + 4, 60, 1.8, line, C_P_TEXT);
    snprintf(line, sizeof(line), "GPU %3dMHZ %3d%%", (int)(pi->gpu_mhz + .5),
             (int)(pi->gpu * 100 + .5f));
    p_text(buf, w, h, z, P_PAD + 4, 80, 1.8, line, C_P_TEXT);
    if (pi->ram_total) {
        snprintf(line, sizeof(line), "RAM %.1f/%.0fMB", pi->ram_used,
                 pi->ram_total);
    } else {
        snprintf(line, sizeof(line), "RAM --");
    }
    p_text(buf, w, h, z, P_PAD + 4, 100, 1.8, line, C_P_TEXT);
    snprintf(line, sizeof(line), "TEMP %dC  ROT %d", (int)(pi->temp + .5),
             pi->rot);
    p_text(buf, w, h, z, P_PAD + 4, 120, 1.8, line, C_P_TEXT);
    if (pi->paused) {
        p_text(buf, w, h, z, P_PAD + 4, 140, 1.8, "PAUSED", C_P_WARN);
    } else if (pi->muted) {
        p_text(buf, w, h, z, P_PAD + 4, 140, 1.8, "MUTED", C_P_WARN);
    }
    /* section titles */
    p_text(buf, w, h, z, P_PAD, 178, 1.4, "CLOCK PROFILE", C_P_DIM);
    p_text(buf, w, h, z, P_PAD, 242, 1.4, "IPOD BUTTONS", C_P_DIM);
    p_text(buf, w, h, z, P_PAD, 374, 1.4, "EMULATOR", C_P_DIM);
    for (i = 0; i < ARRAY_SIZE(p_btns); i++) {
        const PBtn *b = &p_btns[i];
        const char *label = b->label;
        bool active = (b->id == NANO7_SKIN_P_STOCK && pi->profile == 0) ||
                      (b->id == NANO7_SKIN_P_SAFE && pi->profile == 1) ||
                      (b->id == NANO7_SKIN_P_MAX && pi->profile == 2) ||
                      (b->id == NANO7_SKIN_P_PAUSE && pi->paused) ||
                      (b->id == NANO7_SKIN_P_MUTE && pi->muted);
        uint32_t bg = b->id == pressed ? C_P_BTN_DN :
                      active ? C_P_ACCENT : C_P_BTN;
        double s = 1.8;

        if (b->id == NANO7_SKIN_P_PAUSE && pi->paused) {
            label = "RESUME";
        } else if (b->id == NANO7_SKIN_P_MUTE && pi->muted) {
            label = "UNMUTE";
        }
        while (p_text_w(label, s) > b->w - 8 && s > 1) {
            s -= 0.1;
        }
        rrect(buf, w, h, z, b->x, b->y, b->x + b->w, b->y + b->h, 6, bg,
              mix(bg, 0, 0.15));
        p_text(buf, w, h, z, b->x + (b->w - p_text_w(label, s)) / 2,
               b->y + (b->h - 7 * s) / 2, s, label, C_P_TEXT);
    }
    p_text(buf, w, h, z, P_PAD, 504, 1.2, "KEYS: H HOME  P SLEEP", C_P_DIM);
    p_text(buf, w, h, z, P_PAD, 516, 1.2, "SPACE PLAY  UP/DOWN VOL",
           C_P_DIM);
}

void nano7_skin_panel_draw(uint32_t *buf, int stride, int z, int h_units,
                           const Nano7PanelInfo *pi, int pressed)
{
    int w = NANO7_PANEL_W * z, h = h_units * z, y;

    if (!p_cache || p_last_z != z || p_last_h != h_units ||
        p_last_pressed != pressed || memcmp(&p_last, pi, sizeof(*pi))) {
        g_free(p_cache);
        p_cache = g_new(uint32_t, w * h);
        p_draw(p_cache, w, h, z, pi, pressed);
        p_last = *pi;
        p_last_z = z;
        p_last_h = h_units;
        p_last_pressed = pressed;
    }
    for (y = 0; y < h; y++) {
        memcpy(buf + y * stride, p_cache + y * w, w * sizeof(uint32_t));
    }
}

int nano7_skin_panel_hit(double x, double y)
{
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(p_btns); i++) {
        const PBtn *b = &p_btns[i];

        if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h) {
            return b->id;
        }
    }
    return NANO7_SKIN_NONE;
}

void nano7_skin_screen_origin(int *x, int *y)
{
    *x = (int)SCR_X;
    *y = (int)SCR_Y;
}

uint32_t nano7_skin_bg(void)
{
    return C_BG;
}
