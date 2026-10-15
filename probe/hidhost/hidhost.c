/*
 * hidhost.c -- Bluetooth gamepad demo (GL surface app). The HID host layer
 * (hh_bt.c) switches Bluetooth on, accepts a gamepad's HID channels from the
 * OS L2CAP layer and decodes its input reports; this file shows them every
 * frame: a square the left stick drives around a play field, 16 button
 * lamps, and a text panel (status line with a live seconds counter, axes,
 * raw report) rendered with the 8x8 font into a texture.
 *
 * GL surface rather than LVGL: the resident in the snap6 image predates the
 * LVGL surface heartbeat, so an LVGL app is drawn only on demand there;
 * GL surface apps get gl_app_frame() every frame.
 */
#include <stdint.h>
#include <stdbool.h>
#include "gl.h"
#include "hb_gl_surface.h"
#include "hb_sdk.h"
#include "hh_bt.h"

#define FIELD_X   10
#define FIELD_Y   124
#define FIELD_W   220
#define FIELD_H   220
#define SQ        24

#define TXT_W     256           /* text panel texture (top of the screen) */
#define TXT_H     128
#define TXT_LINES 10
#define TXT_COLS  30

static uint8_t g_txt[TXT_W * TXT_H * 4];
static char g_lines[TXT_LINES][TXT_COLS + 1];
static uint32_t g_line_rgb[TXT_LINES];
static GLuint g_tex;
static bool g_ready, g_bt_ok, g_power_asked;
static int32_t sq_x, sq_y;      /* 8.8 fixed point, field coords */
static uint32_t last_ms;

void gl_app_init(void)
{
    g_ready = false;
    g_bt_ok = hh_bt_init();
    sq_x = ((FIELD_W - SQ) / 2) << 8;
    sq_y = ((FIELD_H - SQ) / 2) << 8;
}

/* one text line into the texture: 8x8 glyphs, scale 1 or 2 */
static void text_line(int y, const char *s, uint32_t rgb, int scale)
{
    uint8_t r = rgb >> 16, g = rgb >> 8, b = rgb;
    int x, gx, gy;

    for (x = 0; *s && x + 8 * scale <= TXT_W; s++, x += 8 * scale) {
        const uint8_t *gl = hb_font8x8_glyph(*s);

        for (gy = 0; gy < 8 * scale; gy++) {
            uint8_t bits = gl[gy / scale];

            for (gx = 0; gx < 8 * scale; gx++) {
                uint8_t *p = &g_txt[((y + gy) * TXT_W + x + gx) * 4];

                if (bits & (0x80 >> (gx / scale))) {
                    p[0] = r; p[1] = g; p[2] = b;
                }
            }
        }
    }
}

/* rebuild the text texture if any line changed */
static void text_update(char lines[TXT_LINES][TXT_COLS + 1],
                        const uint32_t *rgb)
{
    bool dirty = !g_ready;
    int i, k;

    for (i = 0; i < TXT_LINES; i++) {
        for (k = 0; k <= TXT_COLS; k++) {
            if (g_lines[i][k] != lines[i][k]) {
                dirty = true;
            }
            g_lines[i][k] = lines[i][k];
            if (!lines[i][k]) {
                break;
            }
        }
        g_line_rgb[i] = rgb[i];
    }
    if (!dirty) {
        return;
    }
    for (i = 0; i < TXT_W * TXT_H; i++) {           /* background 0x05060a */
        g_txt[i * 4] = 5; g_txt[i * 4 + 1] = 6; g_txt[i * 4 + 2] = 10;
        g_txt[i * 4 + 3] = 255;
    }
    text_line(4, g_lines[0], g_line_rgb[0], 2);     /* title */
    for (i = 1; i < TXT_LINES; i++) {
        text_line(14 + i * 11, g_lines[i], g_line_rgb[i], 1);
    }
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TXT_W, TXT_H, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, g_txt);
}

static void quad(float x, float y, float w, float h, uint32_t rgb)
{
    GLfloat v[8] = { x, y, x + w, y, x, y + h, x + w, y + h };

    glColor4f((rgb >> 16 & 255) / 255.f, (rgb >> 8 & 255) / 255.f,
              (rgb & 255) / 255.f, 1.f);
    glVertexPointer(2, GL_FLOAT, 0, v);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void text_quad(void)
{
    static const GLfloat uv[8] = { 0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 1.f };
    GLfloat v[8] = { 0, 0, TXT_W, 0, 0, TXT_H, TXT_W, TXT_H };

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT, 0, uv);
    glColor4f(1.f, 1.f, 1.f, 1.f);
    glVertexPointer(2, GL_FLOAT, 0, v);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisable(GL_TEXTURE_2D);
}

static void build_text(char l[TXT_LINES][TXT_COLS + 1], uint32_t *rgb,
                       const hid_pad *pad, bool have, uint32_t frame)
{
    uint8_t r[8], a[6];
    char *p;
    int i, n;

    for (i = 0; i < TXT_LINES; i++) {
        l[i][0] = 0;
        rgb[i] = 0xb0b0b0;
    }
    put_s(l[0], "BT GAMEPAD");
    rgb[0] = 0xffffff;
    put_s(l[1], g_bt_ok ? hh_bt_status() : "ERROR: NO OS HEAP");
    rgb[1] = hh_bt_state() == HH_ERROR || !g_bt_ok ? 0xff6060 :
             hh_bt_state() == HH_CONNECTED ? 0x80ff80 : 0xffd060;
    if (have) {
        p = put_s(l[2], "X ");
        p = put_d(p, pad->axis[0]);
        p = put_s(p, "  Y ");
        put_d(p, pad->axis[1]);
        p = put_s(l[3], "Z ");
        p = put_d(p, pad->axis[2]);
        p = put_s(p, "  RZ ");
        put_d(p, pad->axis[5]);
        p = put_s(l[4], "BUTTONS ");
        put_x(p, pad->buttons, 4);
    } else {
        p = put_s(l[2], "NO REPORT YET  FRAME ");
        put_u(p, frame);
    }
    n = hh_bt_last_report(r, sizeof r);
    p = put_s(l[5], "RAW ");
    for (i = 0; i < n; i++) {
        p = put_x(p, r[i], 2);
        p = put_s(p, " ");
    }
    if (hh_bt_peer(a)) {
        p = put_s(l[6], "PEER ");
        for (i = 0; i < 6; i++) {
            p = put_x(p, a[i], 2);
            if (i < 5) {
                p = put_s(p, ":");
            }
        }
    }
    p = put_s(l[7], "HID SEC 1124  SDP ");
    put_u(p, hh_bt_descriptor_bytes());
    put_s(l[8], "STICK MOVES THE SQUARE");
    put_s(l[9], "A = RED  B = GREEN");
    rgb[8] = rgb[9] = 0x6080a0;
}

void gl_app_frame(int w, int h, uint32_t frame)
{
    char lines[TXT_LINES][TXT_COLS + 1];
    uint32_t rgb[TXT_LINES];
    uint32_t now = hb_time_uptime_ms();
    uint32_t dt = now - last_ms;
    hid_pad pad;
    bool have;
    int i;

    last_ms = now;
    if (dt > 100) {
        dt = 100;
    }
    if (!g_ready) {
        glGenTextures(1, &g_tex);
        glBindTexture(GL_TEXTURE_2D, g_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    }

    /* Bluetooth: power on once, then the registration / connection machine */
    if (g_bt_ok) {
        hh_bt_tick();
        if (hh_bt_state() == HH_OFF && !g_power_asked) {
            g_power_asked = true;
            hh_bt_power_on();
        }
    }
    have = hh_bt_pad(&pad);

    /* left stick = velocity: full deflection crosses the field in ~1 s */
    sq_x += (int32_t)pad.axis[0] * (int32_t)dt / 32767 * (FIELD_W - SQ) * 256 / 1000;
    sq_y += (int32_t)pad.axis[1] * (int32_t)dt / 32767 * (FIELD_H - SQ) * 256 / 1000;
    if (sq_x < 0) sq_x = 0;
    if (sq_y < 0) sq_y = 0;
    if (sq_x > (FIELD_W - SQ) << 8) sq_x = (FIELD_W - SQ) << 8;
    if (sq_y > (FIELD_H - SQ) << 8) sq_y = (FIELD_H - SQ) << 8;

    build_text(lines, rgb, &pad, have, frame);
    text_update(lines, rgb);
    g_ready = true;

    glViewport(0, 0, w, h);
    glClearColor(0.02f, 0.024f, 0.04f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(0.f, (GLfloat)w, (GLfloat)h, 0.f, -1.f, 1.f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glEnableClientState(GL_VERTEX_ARRAY);

    text_quad();
    quad(FIELD_X - 1, FIELD_Y - 1, FIELD_W + 2, FIELD_H + 2, 0x40506c);
    quad(FIELD_X, FIELD_Y, FIELD_W, FIELD_H, 0x141c2c);
    quad(FIELD_X + (sq_x >> 8), FIELD_Y + (sq_y >> 8), SQ, SQ,
         pad.buttons & 1 ? 0xff4040 : pad.buttons & 2 ? 0x40ff40 :
         pad.buttons ? 0xffd040 : 0x40a0ff);
    for (i = 0; i < 16; i++) {
        quad(14 + (i & 7) * 28, FIELD_Y + FIELD_H + 12 + (i >> 3) * 28, 20, 20,
             pad.buttons & (1u << i) ? 0xffd040 : 0x303848);
    }
    glDisableClientState(GL_VERTEX_ARRAY);
}
