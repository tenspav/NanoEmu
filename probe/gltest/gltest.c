/*
 * gltest.c -- minimal OpenGL ES 1.1 test scene: the first target for the
 * emulator's PowerVR MBX model (emulator/machine/hw/arm/nano7_mbx.c).
 *
 * Every frame is a pure function of the frame counter, so a render can be
 * compared against a reference: clear to dark blue, then one 64x64 RGBA
 * checkerboard-textured quad (vertex colours modulate the texture: red,
 * green, blue, white corners) under a perspective projection, spinning 2
 * degrees per frame about Z and tilted 30 degrees about X. Depth test on,
 * no lighting, no blending. Uses only the SDK's flat GL entry points
 * (sdk/gl.h); runs inside the resident's injected GL view (GL_SURFACE).
 */
#include <stdint.h>
#include "gl.h"
#include "hb_gl_surface.h"

#if VARIANT == 6
#define TEXW 32
#define TEXH 16
#else
#define TEXW 64
#define TEXH 64
#endif
#define TEX 64

/* build variants for decoding the GPU stream (make VARIANT=n):
 * 0 = the scene above, 1 = no texture, 2 = no colour array,
 * 3 = GL_TRIANGLES (6 vertices), 4 = clear colour (1, 0.5, 0.25),
 * 5 = no texture and no colour, quad not rotated (identity modelview),
 * 6 = 32x16 RGBA texture, 7 = 64x64 RGB565 texture */
#ifndef VARIANT
#define VARIANT 0
#endif

static uint8_t g_tex[TEX * TEX * 4];
static GLuint g_tex_id;
static int g_ready;

/* checkerboard of 8x8 cells: white / grey, with a yellow 1-texel border so
 * texture-coordinate errors (wrap, half-texel offsets) are visible */
void gl_app_init(void)
{
    int x, y;

    for (y = 0; y < TEX; y++) {
        for (x = 0; x < TEX; x++) {
            uint8_t *p = &g_tex[(y * TEX + x) * 4];
            int border = x == 0 || y == 0 || x == TEX - 1 || y == TEX - 1;
            int on = ((x >> 3) ^ (y >> 3)) & 1;

            p[0] = border ? 255 : on ? 255 : 96;
            p[1] = border ? 220 : on ? 255 : 96;
            p[2] = border ? 0 : on ? 255 : 96;
            p[3] = 255;
        }
    }
    g_ready = 0;
}

static void setup_once(void)
{
    glGenTextures(1, &g_tex_id);
    glBindTexture(GL_TEXTURE_2D, g_tex_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
#if VARIANT == 7
    {
        static uint16_t t565[TEX * TEX];
        int i;

        for (i = 0; i < TEX * TEX; i++) {
            const uint8_t *p = &g_tex[i * 4];
            t565[i] = (uint16_t)((p[0] >> 3) << 11 | (p[1] >> 2) << 5 | p[2] >> 3);
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, TEX, TEX, 0, GL_RGB,
                     GL_UNSIGNED_SHORT_5_6_5, t565);
    }
#else
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TEXW, TEXH, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, g_tex);
#endif
    g_ready = 1;
}

void gl_app_frame(int w, int h, uint32_t frame)
{
    static const GLfloat pos[] = {
        -1.f, -1.f, 0.f,   1.f, -1.f, 0.f,   -1.f, 1.f, 0.f,   1.f, 1.f, 0.f,
    };
    static const GLfloat uv[] = { 0.f, 0.f,  1.f, 0.f,  0.f, 1.f,  1.f, 1.f };
    static const GLubyte col[] = {
        255, 64, 64, 255,   64, 255, 64, 255,   64, 64, 255, 255,
        255, 255, 255, 255,
    };
    GLfloat aspect = (GLfloat)w / (GLfloat)h;

    if (!g_ready) {
        setup_once();
    }
    glViewport(0, 0, w, h);
#if VARIANT == 4
    glClearColor(1.f, 0.5f, 0.25f, 1.f);
#else
    glClearColor(0.05f, 0.08f, 0.25f, 1.f);
#endif
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustumf(-0.5f * aspect, 0.5f * aspect, -0.5f, 0.5f, 1.f, 10.f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
#if VARIANT != 5
    glTranslatef(0.f, 0.f, -3.5f);
    glRotatef(-30.f, 1.f, 0.f, 0.f);
    glRotatef((GLfloat)((frame * 2) % 360), 0.f, 0.f, 1.f);
#endif

    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);
    glDisable(GL_BLEND);
#if VARIANT != 1 && VARIANT != 5
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_tex_id);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT, 0, uv);
#endif
    glEnableClientState(GL_VERTEX_ARRAY);
#if VARIANT != 2 && VARIANT != 5
    glEnableClientState(GL_COLOR_ARRAY);
    glColorPointer(4, GL_UNSIGNED_BYTE, 0, col);
#endif
#if VARIANT == 3
    {
        static const GLfloat pos6[] = {
            -1.f, -1.f, 0.f,  1.f, -1.f, 0.f,  -1.f, 1.f, 0.f,
            -1.f, 1.f, 0.f,   1.f, -1.f, 0.f,   1.f, 1.f, 0.f,
        };
        static const GLfloat uv6[] = { 0,0, 1,0, 0,1, 0,1, 1,0, 1,1 };
        static const GLubyte col6[] = {
            255,64,64,255, 64,255,64,255, 64,64,255,255,
            64,64,255,255, 64,255,64,255, 255,255,255,255,
        };
        glVertexPointer(3, GL_FLOAT, 0, pos6);
        glTexCoordPointer(2, GL_FLOAT, 0, uv6);
        glColorPointer(4, GL_UNSIGNED_BYTE, 0, col6);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
#else
    glVertexPointer(3, GL_FLOAT, 0, pos);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
#endif
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_TEXTURE_2D);
}
