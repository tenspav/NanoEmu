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
 * 6 = 32x16 RGBA texture, 7 = 64x64 RGB565 texture,
 * 8 = GL_REPLACE, 9 = blend SRC_ALPHA / ONE_MINUS_SRC_ALPHA with vertex
 * alpha 128, 10 = blend ONE / ONE, 11 = depth func LEQUAL, 12 = cull back,
 * 13 = depth test off, 14 = RGBA4444 texture, 15 = RGBA5551 texture,
 * 16 = LUMINANCE_ALPHA texture, 17 = ALPHA texture (vertex colours, blend),
 * 18 = LUMINANCE texture, 19 = no colour array, glColor4f(1, .5, .25, .5)
 * and blend (the constant colour path) */
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
#elif VARIANT == 14 || VARIANT == 15
    {
        static uint16_t t16[TEX * TEX];
        int i;

        /* alpha: 255 on the checker's light cells, ~half on the dark ones */
        for (i = 0; i < TEX * TEX; i++) {
            const uint8_t *p = &g_tex[i * 4];
            int a = p[0] == 96 ? 128 : 255;
#if VARIANT == 14
            t16[i] = (uint16_t)((p[0] >> 4) << 12 | (p[1] >> 4) << 8 |
                                (p[2] >> 4) << 4 | a >> 4);
#else
            t16[i] = (uint16_t)((p[0] >> 3) << 11 | (p[1] >> 3) << 6 |
                                (p[2] >> 3) << 1 | (a == 255));
#endif
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TEX, TEX, 0, GL_RGBA,
                     VARIANT == 14 ? GL_UNSIGNED_SHORT_4_4_4_4 :
                     GL_UNSIGNED_SHORT_5_5_5_1, t16);
    }
#elif VARIANT >= 16 && VARIANT <= 18
    {
        static uint8_t t8[TEX * TEX * 2];
        int i;

        for (i = 0; i < TEX * TEX; i++) {
            const uint8_t *p = &g_tex[i * 4];
#if VARIANT == 16
            t8[2 * i] = p[1];                   /* luminance */
            t8[2 * i + 1] = p[0] == 96 ? 128 : 255;
#else
            t8[i] = p[1];
#endif
        }
#if VARIANT == 16
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, TEX, TEX, 0,
                     GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, t8);
#elif VARIANT == 17
        glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, TEX, TEX, 0, GL_ALPHA,
                     GL_UNSIGNED_BYTE, t8);
#else
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, TEX, TEX, 0,
                     GL_LUMINANCE, GL_UNSIGNED_BYTE, t8);
#endif
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
#if VARIANT == 9
    static const GLubyte cola[] = {
        255, 64, 64, 128,   64, 255, 64, 128,   64, 64, 255, 128,
        255, 255, 255, 128,
    };
#endif
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

#if VARIANT == 13
    glDisable(GL_DEPTH_TEST);
#else
    glEnable(GL_DEPTH_TEST);
#endif
#if VARIANT == 11
    glDepthFunc(GL_LEQUAL);
#endif
#if VARIANT == 12
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
#else
    glDisable(GL_CULL_FACE);
#endif
    glDisable(GL_LIGHTING);
#if VARIANT == 9 || VARIANT == 14 || VARIANT == 15 || VARIANT == 16 || \
    VARIANT == 17 || VARIANT == 19
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
#elif VARIANT == 10
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
#else
    glDisable(GL_BLEND);
#endif
#if VARIANT == 19
    glColor4f(1.f, 0.5f, 0.25f, 0.5f);
#endif
#if VARIANT != 1 && VARIANT != 5 && VARIANT != 19
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_tex_id);
#if VARIANT == 8
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
#else
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
#endif
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT, 0, uv);
#endif
    glEnableClientState(GL_VERTEX_ARRAY);
#if VARIANT != 2 && VARIANT != 5 && VARIANT != 19
    glEnableClientState(GL_COLOR_ARRAY);
#if VARIANT == 9
    glColorPointer(4, GL_UNSIGNED_BYTE, 0, cola);
#else
    glColorPointer(4, GL_UNSIGNED_BYTE, 0, col);
#endif
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
