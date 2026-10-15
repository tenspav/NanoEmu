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
#if VARIANT == 77
#include "hb_heap.h"
#endif

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
 * and blend (the constant colour path), 20 = cull front, 21 = cull front
 * and back (the quad faces the camera: 12 keeps it, 20 and 21 drop it),
 * 22 = alpha test GREATER 0.5 (dark cells get alpha 64), 23 = the same
 * without glEnable(GL_ALPHA_TEST), 24 = alpha test LESS 0.5, 25 = probe:
 * clear green if glEnable(GL_ALPHA_TEST) raises no GL error, red if it does,
 * 26 = probe: after glAlphaFunc(GREATER, .5) + glEnable(GL_ALPHA_TEST) the
 * clear colour shows R = gc+0x140 bit 1, G = (gc+0x3d0 == 0x90000000),
 * B = context found (the driver's alpha enable / func fields),
 * 27 = GL_LINEAR min + mag filter, 28 = mag LINEAR only, 29 = min LINEAR only,
 * 30-33 = mipmapped: levels 64..1 uploaded by hand, each a solid colour
 * (red, green, blue, yellow, magenta, cyan, white) so they can be told apart
 * in GPU memory and on screen; a steep, receding plane with uv x 8 and no
 * colour array. Min filter 30 NEAREST_MIPMAP_NEAREST, 31 LINEAR_MIPMAP_LINEAR,
 * 32 LINEAR_MIPMAP_NEAREST, 33 NEAREST_MIPMAP_LINEAR (mag NEAREST),
 * 34 = near clipping: the checker quad as a floor (uv x 8, no colour
 * array) from 5 units behind the eye to 11 in front, eye 0.5 above it,
 * 35 = stencil probe: glClearStencil(0x55), glClear incl. the stencil bit,
 * glEnable(GL_STENCIL_TEST) around the quad, 36 = glShadeModel(GL_FLAT),
 * no texture (GL: each triangle takes its last vertex's colour - the strip
 * gives one blue and one white triangle), 37-40 = glColorMask around the
 * quad: 37 (1,0,1,1) green masked, 38 (1,1,1,0) alpha masked, 39 (0,0,0,0)
 * all masked, 40 (0,0,1,1) only blue + alpha written, 41 = glEnable(GL_FOG)
 * with the GL defaults (EXP, density 1, colour 0), 42-44 = fog with red
 * colour set on the real context (the driver has no glFog*): 42 LINEAR start
 * 2.5 end 5, 43 EXP density 0.3, 44 EXP2 density 0.3,
 * 45-49 = lighting with per-vertex normals tilted outwards, no colour array:
 * 45 GL_LIGHT0 defaults (untextured), 46 directional light from (1,1,1),
 * diffuse (1,.5,0), ambient (.1,.1,.3), specular 1 + material specular 1,
 * shininess 16, 47 point light at the eye, linear attenuation 0.3, 48 spot
 * at the eye down -z, cutoff 15, exponent 8, 49 = 45 with the texture,
 * 50 = 45 + GL_LIGHT1 directional from the left, red diffuse, 51 = only
 * GL_LIGHT1 enabled, 52 / 53 = 45 with GL_BYTE / GL_SHORT normals (same
 * directions, scaled to the type's range: must render like 45), 54 / 55 =
 * 45 flipped 180 deg about x (the back face looks at the camera): 54 one-
 * sided, 55 two-sided (gc+0x2d0 = 1 on the real context: the driver has
 * no glLightModel), 56 = the 4 vertices as GL_POINTS (untextured, colour
 * array), 57 = 56 as point sprites (GL_POINT_SPRITE_OES + GL_COORD_REPLACE_OES,
 * the checker texture), 58 = 57 with point size 24, 59 = 56 with point
 * size 24 (no glPointSize in this driver: written into the real context -
 * size gc+0x260 + dirty gc+0x1500 bit 25 for sprites, half size gc+0x258 /
 * +0x25c for GL_POINTS), 60 = glEnable(GL_POLYGON_OFFSET_FILL) (the driver has
 * no glPolygonOffset), 61-64 = multitexture: unit 1 = a 32x32 texture (left
 * half orange opaque, right half blue alpha 128), its own texcoords (x 2),
 * env 61 MODULATE, 62 ADD, 63 DECAL, 64 REPLACE,
 * 65 = texture env table: a 4 x 7 grid of quads in one frame, unit 0, each a
 * different GL_BLEND / GL_COMBINE setting (CFG[] below, in draw order), env
 * colour (0.25, 0.5, 0.75, 0.5), 66 = the same with table CFG2[] (three
 * parameters per entry: function, then up to two changes), 67 = alpha-only
 * table CFG3[] (RGB left at MODULATE; every alpha source / operand reset per
 * quad), 68 = wrap modes: 6 quads with uv -0.5 .. 1.5, wrap S / T = REPEAT /
 * REPEAT, CLAMP_TO_EDGE / REPEAT, REPEAT / CLAMP, CLAMP / CLAMP,
 * MIRRORED_REPEAT_OES / REPEAT, MIRRORED / MIRRORED, 69 = texture matrix:
 * 6 quads (uv 0..1), GL_TEXTURE matrix 0 identity, 1 translate (.25, .5),
 * 2 scale (2, 3), 3 rotate 30, 4 projective (q = 1 + .5 s), 5 unit 1 (the
 * 61-64 texture, MODULATE) translated by (.5, 0), 70 = v0 +
 * glEnable(GL_CLIP_PLANE0 / 1) (the driver has no glClipPlanef / x), 71 =
 * plane 0 = (1, 0, 0, 0) (keeps eye x >= 0) written into the real context
 * (gc+0xcd4 + 16 n, dirty gc+0x1500 bits 16 + n), 72 = 71 + plane 1 =
 * (0, 1, 0, 0), 73 = planes 1 = (0, 1, 0, 0.25) and 3 = (0, 0, 1, 5) only
 * (written at +0x10 / +0x30 after the enables), 74 = lines in one frame, 2D
 * ortho, untextured: 0 GL_LINES (2 segments) width 1, 1 GL_LINE_STRIP (4
 * points) width 1, 2 GL_LINE_LOOP (4 points) width 1, 3 GL_LINES width 4,
 * 4 GL_LINES with per-vertex colours, width 2, 75 = glDrawElements in one
 * frame, 2D ortho, checker + vertex colours: 0 GL_TRIANGLES ubyte indices,
 * 1 GL_TRIANGLES ushort, 2 GL_TRIANGLE_STRIP ubyte, 3 GL_TRIANGLES ubyte
 * reusing vertices out of order (the quad drawn with its corners permuted),
 * 76 = one glDrawElements of a 5 x 5 vertex grid (16 cells, 96 ushort
 * indices, GL_TRIANGLES) covering the screen, checker uv 0..1, 77 = v0 with
 * the clear colour = the OS heap's free bytes / 16 (hb_os_heap_free(), the
 * OS query 0x08417824) as 0xRRGGBB - a check of the emulator HUD's figure,
 * 78-87 = compressed textures via glCompressedTexImage2D, one 64x64 texture
 * drawn 3x (192x192 at 24,24; NEAREST, GL_REPLACE, 2D ortho); clear colour
 * green if the upload raised no GL error, red if it did. Data from an LCG
 * (seed 1) so a reference can rebuild it: 78 PVRTC 4bpp RGBA, 79 PVRTC 2bpp
 * RGBA, 80 PVRTC 4bpp RGB, 81 PVRTC 2bpp RGB (random blocks), 82 PALETTE4
 * RGBA8, 83 PALETTE8 RGBA8, 84 PALETTE4 R5G6B5, 85 PALETTE8 RGBA4, 86
 * PALETTE8 RGB8, 87 PALETTE4 RGB5_A1 (random palette, index (x*7 + y*3 +
 * (x*y >> 3)) mod 16 / 256) */
#define CTEX (VARIANT >= 78 && VARIANT <= 87)
#define MT (VARIANT >= 61 && VARIANT <= 64)
#ifndef GL_POLYGON_OFFSET_FILL
#define GL_POLYGON_OFFSET_FILL 0x8037
#endif
#ifndef GL_POINT_SPRITE_OES
#define GL_POINT_SPRITE_OES  0x8861
#define GL_COORD_REPLACE_OES 0x8862
#endif
#define LIT (VARIANT >= 45 && VARIANT <= 55)
#ifndef GL_BYTE
#define GL_BYTE  0x1400
#endif
#ifndef GL_SHORT
#define GL_SHORT 0x1402
#endif
#ifndef GL_FOG
#define GL_FOG 0x0B60
#endif
#ifndef GL_FLAT
#define GL_FLAT 0x1D00
#endif
#ifndef GL_STENCIL_TEST
#define GL_STENCIL_TEST 0x0B90
#endif
#define MIP (VARIANT >= 30 && VARIANT <= 33)
#ifndef GL_NEAREST_MIPMAP_NEAREST
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST  0x2701
#define GL_NEAREST_MIPMAP_LINEAR  0x2702
#define GL_LINEAR_MIPMAP_LINEAR   0x2703
#endif
#ifndef GL_ALPHA_TEST
#define GL_ALPHA_TEST 0x0BC0
#endif
#ifndef VARIANT
#define VARIANT 0
#endif

static uint8_t g_tex[TEX * TEX * 4];

#if VARIANT >= 42 && VARIANT <= 44
/* the OS has no glFog*: write the fog fields of the real context (getter
 * 0x083c2df0) - mode gc+0x32c, colour gc+0x334..0x340, density 0x344,
 * start 0x348, end 0x34c, 1 / (end - start) 0x350 - and mark them dirty the
 * way glEnable(GL_FOG) does (0x21 into gc+0x5d4) */
static void fog_setup(GLenum mode, float density, float start, float end)
{
    uint8_t *gc = ((uint8_t *(*)(void))(0x083c2df0u | 1u))();
    static const float red[4] = { 1.f, 0.f, 0.f, 1.f };
    int i;

    if (!gc) {
        return;
    }
    *(volatile uint32_t *)(gc + 0x32c) = mode;
    for (i = 0; i < 4; i++) {
        *(volatile float *)(gc + 0x334 + 4 * i) = red[i];
    }
    *(volatile float *)(gc + 0x344) = density;
    *(volatile float *)(gc + 0x348) = start;
    *(volatile float *)(gc + 0x34c) = end;
    *(volatile float *)(gc + 0x350) = 1.f / (end - start);
    *(volatile uint32_t *)(gc + 0x5d4) |= 0x21;
}
#endif

#if VARIANT >= 22 && VARIANT <= 24
/* glAlphaFunc on the context the OS GL methods really use: they ignore the
 * gc argument and fetch it with the getter 0x083c2df0 (a different object
 * than the SDK's gl_current_gc(), whose glAlphaFunc therefore has no effect).
 * Same packing as the SDK: ref * 256 (clamped to 255) in bits 28:21, func -
 * GL_NEVER in 31:29 at gc+0x3d0; dirty ISP/TSP bits 0|1 at gc+0x5d4. */
static void alpha_func(GLenum func, GLfloat ref)
{
    uint8_t *gc = ((uint8_t *(*)(void))(0x083c2df0u | 1u))();
    uint32_t r = (uint32_t)(ref * 256.f);

    if (!gc) {
        return;
    }
    if (r > 255) {
        r = 255;
    }
    *(volatile uint32_t *)(gc + 0x3d0) = r << 21 | (func - GL_NEVER) << 29;
    *(volatile uint32_t *)(gc + 0x5d4) |= 3;
}
#endif
static GLuint g_tex_id;
#if VARIANT >= 78 && VARIANT <= 87
static GLenum g_ctex_err;
void glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat,
                            GLsizei width, GLsizei height, GLint border,
                            GLsizei imageSize, const GLvoid *data);
#endif
#if MT || VARIANT == 69
static GLuint g_tex2_id;
static uint32_t g_tex2[32 * 32];
#endif
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
            p[3] = VARIANT >= 22 && VARIANT <= 24 && !border && !on ? 64 : 255;
        }
    }
    g_ready = 0;
}

static void setup_once(void)
{
    glGenTextures(1, &g_tex_id);
    glBindTexture(GL_TEXTURE_2D, g_tex_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    VARIANT == 27 || VARIANT == 29 ? GL_LINEAR :
                    VARIANT == 30 ? GL_NEAREST_MIPMAP_NEAREST :
                    VARIANT == 31 ? GL_LINEAR_MIPMAP_LINEAR :
                    VARIANT == 32 ? GL_LINEAR_MIPMAP_NEAREST :
                    VARIANT == 33 ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                    VARIANT == 27 || VARIANT == 28 ? GL_LINEAR : GL_NEAREST);
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
#elif CTEX
    {
        /* token, palette entry bytes (0 = PVRTC), index bits (4 / 8 / 2 or
         * 4 bpp for PVRTC) */
        static const unsigned fmt[10][3] = {
            { 0x8C02, 0, 4 }, { 0x8C03, 0, 2 }, { 0x8C00, 0, 4 },
            { 0x8C01, 0, 2 }, { 0x8B91, 4, 4 }, { 0x8B96, 4, 8 },
            { 0x8B92, 2, 4 }, { 0x8B98, 2, 8 }, { 0x8B95, 3, 8 },
            { 0x8B94, 2, 4 },
        };
        static uint8_t d[256 * 4 + TEX * TEX];
        const unsigned *f = fmt[VARIANT - 78];
        uint32_t lcg = 1;
        int n = 0, i, x, y;

        if (!f[1]) {                        /* PVRTC: random blocks */
            n = TEX * TEX * f[2] / 8;
            for (i = 0; i < n; i++) {
                lcg = lcg * 1103515245u + 12345u;
                d[i] = (uint8_t)(lcg >> 16);
            }
        } else {
            int pal = (1 << f[2]) * (int)f[1];

            for (i = 0; i < pal; i++) {
                lcg = lcg * 1103515245u + 12345u;
                d[i] = (uint8_t)(lcg >> 16);
            }
            n = pal;
            for (y = 0; y < TEX; y++) {
                for (x = 0; x < TEX; x++) {
                    int k = (x * 7 + y * 3 + (x * y >> 3)) & ((1 << f[2]) - 1);

                    if (f[2] == 8) {
                        d[n++] = (uint8_t)k;
                    } else if (x & 1) {     /* 4 bpp: first texel high */
                        d[n - 1] |= (uint8_t)k;
                    } else {
                        d[n++] = (uint8_t)(k << 4);
                    }
                }
            }
        }
        (void)glGetError();
        glCompressedTexImage2D(GL_TEXTURE_2D, 0, f[0], TEX, TEX, 0, n, d);
        g_ctex_err = glGetError();
    }
#elif MIP
    {
        static const uint32_t lc[7] = {
            0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xff00ffffu, 0xffff00ffu,
            0xffffff00u, 0xffffffffu,           /* ABGR in memory = RGBA */
        };
        uint32_t *t = (uint32_t *)g_tex;
        int l, i;

        for (l = 0; l < 7; l++) {
            for (i = 0; i < (TEX >> l) * (TEX >> l); i++) {
                t[i] = lc[l];
            }
            glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA, TEX >> l, TEX >> l, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, g_tex);
        }
    }
#else
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TEXW, TEXH, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, g_tex);
#endif
#if MT || VARIANT == 69
    {
        int i;

        for (i = 0; i < 32 * 32; i++) {     /* ABGR in memory = RGBA */
            g_tex2[i] = (i & 31) < 16 ? 0xff0080ffu : 0x80ff8000u;
        }
        glGenTextures(1, &g_tex2_id);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, g_tex2_id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 32, 32, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, g_tex2);
        glActiveTexture(GL_TEXTURE0);
    }
#endif
    g_ready = 1;
}

#if VARIANT >= 65 && VARIANT <= 67
#define GL_COMBINE          0x8570
#define GL_COMBINE_RGB      0x8571
#define GL_COMBINE_ALPHA    0x8572
#define GL_RGB_SCALE        0x8573
#define GL_ADD_SIGNED       0x8574
#define GL_INTERPOLATE      0x8575
#define GL_CONSTANT         0x8576
#define GL_PRIMARY_COLOR    0x8577
#define GL_PREVIOUS         0x8578
#define GL_SUBTRACT         0x84E7
#define GL_SRC0_RGB         0x8580
#define GL_SRC1_RGB         0x8581
#define GL_SRC2_RGB         0x8582
#define GL_SRC0_ALPHA       0x8588
#define GL_SRC1_ALPHA       0x8589
#define GL_SRC2_ALPHA       0x858A
#define GL_OPERAND1_ALPHA   0x8599
#define GL_OPERAND2_ALPHA   0x859A
#define GL_OPERAND0_RGB     0x8590
#define GL_OPERAND1_RGB     0x8591
#define GL_OPERAND2_RGB     0x8592
#define GL_OPERAND0_ALPHA   0x8598
#define GL_DOT3_RGB         0x86AE
#define GL_DOT3_RGBA        0x86AF
#define GL_ALPHA_SCALE      0x0D1C
#define GL_SRC_COLOR        0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA_       0x0302
#define GL_ONE_MINUS_SRC_ALPHA_ 0x0303

#if VARIANT == 65
/* one parameter changed from GL's COMBINE defaults per entry (pname 0 =
 * none); entry 0 is GL_BLEND */
static const GLenum CFG[28][2] = {
    { 0, 0 },                                   /* 0 GL_BLEND */
    { 0, 0 },                                   /* 1 COMBINE defaults */
    { GL_COMBINE_RGB, GL_REPLACE },             /* 2 */
    { GL_COMBINE_RGB, GL_ADD },                 /* 3 */
    { GL_COMBINE_RGB, GL_ADD_SIGNED },          /* 4 */
    { GL_COMBINE_RGB, GL_INTERPOLATE },         /* 5 */
    { GL_COMBINE_RGB, GL_SUBTRACT },            /* 6 */
    { GL_COMBINE_RGB, GL_DOT3_RGB },            /* 7 */
    { GL_COMBINE_RGB, GL_DOT3_RGBA },           /* 8 */
    { GL_SRC0_RGB, GL_CONSTANT },               /* 9 */
    { GL_SRC0_RGB, GL_PRIMARY_COLOR },          /* 10 */
    { GL_SRC1_RGB, GL_TEXTURE },                /* 11 */
    { GL_OPERAND0_RGB, GL_ONE_MINUS_SRC_COLOR },/* 12 */
    { GL_OPERAND0_RGB, GL_SRC_ALPHA_ },         /* 13 */
    { GL_OPERAND0_RGB, GL_ONE_MINUS_SRC_ALPHA_ },/* 14 */
    { GL_RGB_SCALE, 2 },                        /* 15 */
    { GL_RGB_SCALE, 4 },                        /* 16 */
    { GL_SRC0_RGB, GL_PREVIOUS },               /* 17 */
    { GL_COMBINE_ALPHA, GL_REPLACE },           /* 18 */
    { GL_COMBINE_ALPHA, GL_ADD },               /* 19 */
    { GL_COMBINE_ALPHA, GL_SUBTRACT },          /* 20 */
    { GL_COMBINE_ALPHA, GL_INTERPOLATE },       /* 21 */
    { GL_SRC0_ALPHA, GL_CONSTANT },             /* 22 */
    { GL_OPERAND0_ALPHA, GL_ONE_MINUS_SRC_ALPHA_ },/* 23 */
    { GL_ALPHA_SCALE, 2 },                      /* 24 */
    { GL_OPERAND1_RGB, GL_ONE_MINUS_SRC_COLOR },/* 25 */
    { GL_SRC2_RGB, GL_TEXTURE },                /* 26 */
    { GL_OPERAND2_RGB, GL_SRC_COLOR },          /* 27 */
};
#endif

#if VARIANT == 66
/* entry: { which (0 rgb / 1 alpha), function, pname, value, pname, value } */
static const GLenum CFG2[28][6] = {
    { 0, GL_ADD, GL_SRC0_RGB, GL_CONSTANT, 0, 0 },                  /* 0 */
    { 0, GL_ADD, GL_SRC1_RGB, GL_TEXTURE, 0, 0 },                   /* 1 */
    { 0, GL_ADD, GL_SRC0_RGB, GL_PRIMARY_COLOR, 0, 0 },             /* 2 */
    { 0, GL_ADD, GL_OPERAND0_RGB, GL_ONE_MINUS_SRC_COLOR, 0, 0 },   /* 3 */
    { 0, GL_ADD, GL_OPERAND1_RGB, GL_ONE_MINUS_SRC_COLOR, 0, 0 },   /* 4 */
    { 0, GL_ADD, GL_OPERAND0_RGB, GL_SRC_ALPHA_, 0, 0 },            /* 5 */
    { 0, GL_SUBTRACT, GL_SRC0_RGB, GL_CONSTANT, 0, 0 },             /* 6 */
    { 0, GL_SUBTRACT, GL_SRC1_RGB, GL_CONSTANT, 0, 0 },             /* 7 */
    { 0, GL_SUBTRACT, GL_OPERAND1_RGB, GL_ONE_MINUS_SRC_COLOR, 0, 0 },/* 8 */
    { 0, GL_INTERPOLATE, GL_SRC0_RGB, GL_CONSTANT, 0, 0 },          /* 9 */
    { 0, GL_INTERPOLATE, GL_SRC1_RGB, GL_TEXTURE, 0, 0 },           /* 10 */
    { 0, GL_INTERPOLATE, GL_SRC2_RGB, GL_PRIMARY_COLOR, 0, 0 },     /* 11 */
    { 0, GL_INTERPOLATE, GL_SRC2_RGB, GL_TEXTURE, 0, 0 },           /* 12 */
    { 0, GL_INTERPOLATE, GL_OPERAND2_RGB, GL_SRC_COLOR, 0, 0 },     /* 13 */
    { 0, GL_INTERPOLATE, GL_OPERAND2_RGB, GL_ONE_MINUS_SRC_ALPHA_, 0, 0 },/* 14 */
    { 0, GL_REPLACE, GL_SRC0_RGB, GL_CONSTANT, 0, 0 },              /* 15 */
    { 0, GL_REPLACE, GL_SRC0_RGB, GL_PRIMARY_COLOR, 0, 0 },         /* 16 */
    { 0, GL_REPLACE, GL_OPERAND0_RGB, GL_ONE_MINUS_SRC_COLOR, 0, 0 },/* 17 */
    { 0, GL_REPLACE, GL_OPERAND0_RGB, GL_SRC_ALPHA_, 0, 0 },        /* 18 */
    { 0, GL_ADD_SIGNED, GL_SRC0_RGB, GL_CONSTANT, 0, 0 },           /* 19 */
    { 0, GL_ADD, GL_RGB_SCALE, 2, 0, 0 },                           /* 20 */
    { 0, GL_MODULATE, GL_SRC1_RGB, GL_CONSTANT, 0, 0 },             /* 21 */
    { 1, GL_ADD, GL_SRC0_ALPHA, GL_CONSTANT, 0, 0 },                /* 22 */
    { 1, GL_MODULATE, GL_SRC1_ALPHA, GL_CONSTANT, 0, 0 },           /* 23 */
    { 1, GL_REPLACE, GL_SRC0_ALPHA, GL_PRIMARY_COLOR, 0, 0 },       /* 24 */
    { 1, GL_INTERPOLATE, GL_SRC2_ALPHA, GL_TEXTURE, 0, 0 },         /* 25 */
    { 0, GL_MODULATE, GL_SRC0_RGB, GL_CONSTANT,
      GL_SRC1_RGB, GL_TEXTURE },                                    /* 26 */
    { 0, GL_ADD, GL_SRC0_RGB, GL_CONSTANT, GL_SRC1_RGB, GL_TEXTURE },/* 27 */
};
#endif

#if VARIANT == 67
/* alpha: { function, src0, src1, src2, op0, op1, op2, scale } (0 = GL
 * default: TEXTURE, PREVIOUS, CONSTANT, SRC_ALPHA x3, 1) */
#define T_ GL_TEXTURE
#define K_ GL_CONSTANT
#define P_ GL_PRIMARY_COLOR
#define A_ GL_SRC_ALPHA_
#define NA GL_ONE_MINUS_SRC_ALPHA_
static const GLenum CFG3[28][8] = {
    { GL_MODULATE, 0, 0, 0, 0, 0, 0, 0 },          /* 0 */
    { GL_REPLACE, 0, 0, 0, 0, 0, 0, 0 },           /* 1 */
    { GL_REPLACE, K_, 0, 0, 0, 0, 0, 0 },          /* 2 */
    { GL_REPLACE, P_, 0, 0, 0, 0, 0, 0 },          /* 3 */
    { GL_MODULATE, K_, 0, 0, 0, 0, 0, 0 },         /* 4 */
    { GL_MODULATE, 0, K_, 0, 0, 0, 0, 0 },         /* 5 */
    { GL_MODULATE, 0, T_, 0, 0, 0, 0, 0 },         /* 6 */
    { GL_MODULATE, 0, 0, 0, NA, 0, 0, 0 },         /* 7 */
    { GL_MODULATE, 0, 0, 0, 0, NA, 0, 0 },         /* 8 */
    { GL_ADD, 0, 0, 0, 0, 0, 0, 0 },               /* 9 */
    { GL_ADD, K_, 0, 0, 0, 0, 0, 0 },              /* 10 */
    { GL_ADD, 0, T_, 0, 0, 0, 0, 0 },              /* 11 */
    { GL_ADD, 0, 0, 0, NA, 0, 0, 0 },              /* 12 */
    { GL_ADD, 0, 0, 0, 0, NA, 0, 0 },              /* 13 */
    { GL_SUBTRACT, 0, 0, 0, 0, 0, 0, 0 },          /* 14 */
    { GL_SUBTRACT, K_, 0, 0, 0, 0, 0, 0 },         /* 15 */
    { GL_SUBTRACT, 0, K_, 0, 0, 0, 0, 0 },         /* 16 */
    { GL_INTERPOLATE, 0, 0, T_, 0, 0, 0, 0 },      /* 17 */
    { GL_INTERPOLATE, 0, 0, 0, 0, 0, 0, 0 },       /* 18 */
    { GL_INTERPOLATE, 0, 0, P_, 0, 0, 0, 0 },      /* 19 */
    { GL_INTERPOLATE, K_, 0, T_, 0, 0, 0, 0 },     /* 20 */
    { GL_INTERPOLATE, 0, T_, 0, 0, 0, 0, 0 },      /* 21 */
    { GL_INTERPOLATE, 0, 0, 0, 0, 0, NA, 0 },      /* 22 */
    { GL_ADD_SIGNED, 0, 0, 0, 0, 0, 0, 0 },        /* 23 */
    { GL_MODULATE, 0, 0, 0, 0, 0, 0, 2 },          /* 24 */
    { GL_MODULATE, 0, 0, 0, 0, 0, 0, 4 },          /* 25 */
    { GL_ADD, 0, 0, 0, 0, 0, 0, 4 },               /* 26 */
    { GL_REPLACE, 0, 0, 0, NA, 0, 0, 0 },          /* 27 */
};
#endif

static void env_defaults(void)
{
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_MODULATE);
    glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_RGB, GL_TEXTURE);
    glTexEnvi(GL_TEXTURE_ENV, GL_SRC1_RGB, GL_PREVIOUS);
    glTexEnvi(GL_TEXTURE_ENV, GL_SRC2_RGB, GL_CONSTANT);
    glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_ALPHA, GL_TEXTURE);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND2_RGB, GL_SRC_ALPHA_);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA_);
    glTexEnvi(GL_TEXTURE_ENV, GL_RGB_SCALE, 1);
    glTexEnvi(GL_TEXTURE_ENV, GL_ALPHA_SCALE, 1);
    glTexEnvi(GL_TEXTURE_ENV, GL_SRC1_ALPHA, GL_PREVIOUS);
    glTexEnvi(GL_TEXTURE_ENV, GL_SRC2_ALPHA, GL_CONSTANT);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_ALPHA, GL_SRC_ALPHA_);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND2_ALPHA, GL_SRC_ALPHA_);
}

static void env_table_frame(int w, int h)
{
    static const GLfloat envc[4] = { .25f, .5f, .75f, .5f };
    static const GLfloat uvq[] = { 0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 1.f };
    static const GLubyte colq[] = {
        255, 64, 64, 255,   64, 255, 64, 255,   64, 64, 255, 255,
        255, 255, 255, 255,
    };
    float cw = w / 4.f, ch = h / 7.f;
    int i;

    glViewport(0, 0, w, h);
    glClearColor(0.05f, 0.08f, 0.25f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(0.f, (float)w, (float)h, 0.f, -1.f, 1.f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);
    glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_tex_id);
    glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, envc);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glColorPointer(4, GL_UNSIGNED_BYTE, 0, colq);
    glTexCoordPointer(2, GL_FLOAT, 0, uvq);
    for (i = 0; i < 28; i++) {
        float x = (i % 4) * cw + 2, y = (i / 4) * ch + 2;
        GLfloat v[8] = { x, y, x + cw - 4, y, x, y + ch - 4,
                         x + cw - 4, y + ch - 4 };

#if VARIANT == 65
        if (i == 0) {
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_BLEND);
        } else {
            env_defaults();
            if (CFG[i][0]) {
                glTexEnvi(GL_TEXTURE_ENV, CFG[i][0], (GLint)CFG[i][1]);
            }
        }
#elif VARIANT == 67
        {
            static const GLenum pn[7] = {
                GL_SRC0_ALPHA, GL_SRC1_ALPHA, GL_SRC2_ALPHA,
                GL_OPERAND0_ALPHA, GL_OPERAND1_ALPHA, GL_OPERAND2_ALPHA,
                GL_ALPHA_SCALE,
            };
            int k;

            env_defaults();
            glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, (GLint)CFG3[i][0]);
            for (k = 0; k < 7; k++) {
                if (CFG3[i][1 + k]) {
                    glTexEnvi(GL_TEXTURE_ENV, pn[k], (GLint)CFG3[i][1 + k]);
                }
            }
        }
#else
        env_defaults();
        glTexEnvi(GL_TEXTURE_ENV, CFG2[i][0] ? GL_COMBINE_ALPHA :
                  GL_COMBINE_RGB, (GLint)CFG2[i][1]);
        if (CFG2[i][2]) {
            glTexEnvi(GL_TEXTURE_ENV, CFG2[i][2], (GLint)CFG2[i][3]);
        }
        if (CFG2[i][4]) {
            glTexEnvi(GL_TEXTURE_ENV, CFG2[i][4], (GLint)CFG2[i][5]);
        }
#endif
        glVertexPointer(2, GL_FLOAT, 0, v);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_TEXTURE_2D);
}
#endif

#if VARIANT == 68
#ifndef GL_TEXTURE_WRAP_S
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#endif
#ifndef GL_REPEAT
#define GL_REPEAT 0x2901
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#define GL_MIRRORED_REPEAT_OES 0x8370

static void wrap_frame(int w, int h)
{
    static const GLenum WR[6][2] = {
        { GL_REPEAT, GL_REPEAT }, { GL_CLAMP_TO_EDGE, GL_REPEAT },
        { GL_REPEAT, GL_CLAMP_TO_EDGE }, { GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE },
        { GL_MIRRORED_REPEAT_OES, GL_REPEAT },
        { GL_MIRRORED_REPEAT_OES, GL_MIRRORED_REPEAT_OES },
    };
    static const GLfloat uvq[] = { -.5f, -.5f, 1.5f, -.5f, -.5f, 1.5f,
                                   1.5f, 1.5f };
    float cw = w / 2.f, ch = h / 3.f;
    int i;

    glViewport(0, 0, w, h);
    glClearColor(0.05f, 0.08f, 0.25f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(0.f, (float)w, (float)h, 0.f, -1.f, 1.f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_tex_id);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT, 0, uvq);
    for (i = 0; i < 6; i++) {
        float x = (i % 2) * cw + 4, y = (i / 2) * ch + 4;
        GLfloat v[8] = { x, y, x + cw - 8, y, x, y + ch - 8,
                         x + cw - 8, y + ch - 8 };

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint)WR[i][0]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLint)WR[i][1]);
        glVertexPointer(2, GL_FLOAT, 0, v);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_TEXTURE_2D);
}
#endif

#if VARIANT == 69
#ifndef GL_TEXTURE
#define GL_TEXTURE 0x1702
#endif
static void texmat_frame(int w, int h)
{
    static const GLfloat uvq[] = { 0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 1.f };
    static const GLfloat proj[16] = {      /* column-major: q = 1 + .5 s */
        1.f, 0.f, 0.f, .5f,   0.f, 1.f, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f,   0.f, 0.f, 0.f, 1.f,
    };
    float cw = w / 2.f, ch = h / 3.f;
    int i;

    glViewport(0, 0, w, h);
    glClearColor(0.05f, 0.08f, 0.25f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(0.f, (float)w, (float)h, 0.f, -1.f, 1.f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_tex_id);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT, 0, uvq);
    for (i = 0; i < 6; i++) {
        float x = (i % 2) * cw + 4, y = (i / 2) * ch + 4;
        GLfloat v[8] = { x, y, x + cw - 8, y, x, y + ch - 8,
                         x + cw - 8, y + ch - 8 };

        glMatrixMode(GL_TEXTURE);
        glLoadIdentity();
        if (i == 1) {
            glTranslatef(.25f, .5f, 0.f);
        } else if (i == 2) {
            glScalef(2.f, 3.f, 1.f);
        } else if (i == 3) {
            glRotatef(30.f, 0.f, 0.f, 1.f);
        } else if (i == 4) {
            glLoadMatrixf(proj);
        }
        if (i == 5) {               /* unit 1 with its own matrix */
            glActiveTexture(GL_TEXTURE1);
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, g_tex2_id);
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
            glMatrixMode(GL_TEXTURE);
            glLoadIdentity();
            glTranslatef(.5f, 0.f, 0.f);
            glClientActiveTexture(GL_TEXTURE1);
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glTexCoordPointer(2, GL_FLOAT, 0, uvq);
            glClientActiveTexture(GL_TEXTURE0);
            glActiveTexture(GL_TEXTURE0);
        }
        glMatrixMode(GL_MODELVIEW);
        glVertexPointer(2, GL_FLOAT, 0, v);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glActiveTexture(GL_TEXTURE1);
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    glDisable(GL_TEXTURE_2D);
    glClientActiveTexture(GL_TEXTURE1);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glClientActiveTexture(GL_TEXTURE0);
    glActiveTexture(GL_TEXTURE0);
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_TEXTURE_2D);
}
#endif

#if VARIANT == 74
#ifndef GL_LINE_LOOP
#define GL_LINE_LOOP  0x0002
#endif
#ifndef GL_LINE_STRIP
#define GL_LINE_STRIP 0x0003
#endif
static void lines_frame(int w, int h)
{
    static const GLubyte cols[] = { 255, 64, 64, 255,  64, 255, 64, 255,
                                    64, 64, 255, 255,  255, 255, 255, 255 };
    float ch = h / 5.f;
    int i;

    glViewport(0, 0, w, h);
    glClearColor(0.05f, 0.08f, 0.25f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(0.f, (float)w, (float)h, 0.f, -1.f, 1.f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);
    glEnableClientState(GL_VERTEX_ARRAY);
    for (i = 0; i < 5; i++) {
        float y = i * ch + 10, b = y + ch - 20;
        GLfloat v[8] = { 20.f, y, (float)w - 20.f, b,
                         (float)w - 20.f, y, 20.f, b };

        if (i == 4) {
            glEnableClientState(GL_COLOR_ARRAY);
            glColorPointer(4, GL_UNSIGNED_BYTE, 0, cols);
        } else {
            glColor4f(1.f, 1.f, .25f, 1.f);
        }
        glLineWidth(i == 3 ? 4.f : i == 4 ? 2.f : 1.f);
        glVertexPointer(2, GL_FLOAT, 0, v);
        glDrawArrays(i == 1 ? GL_LINE_STRIP : i == 2 ? GL_LINE_LOOP : GL_LINES,
                     0, 4);
    }
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glLineWidth(1.f);
}
#endif

#if VARIANT == 75
static void elements_frame(int w, int h)
{
    /* 8 vertices: quad A (left column) and quad B (right column) corners */
    static const GLfloat uvq[] = { 0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 1.f,
                                   0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 1.f };
    static const GLubyte colq[] = {
        255, 64, 64, 255,   64, 255, 64, 255,   64, 64, 255, 255,
        255, 255, 255, 255,
        255, 64, 64, 255,   64, 255, 64, 255,   64, 64, 255, 255,
        255, 255, 255, 255,
    };
    static const GLubyte tri_b[6] = { 0, 1, 2, 2, 1, 3 };
    static const uint16_t tri_s[6] = { 0, 1, 2, 2, 1, 3 };
    static const GLubyte strip_b[4] = { 0, 1, 2, 3 };
    static const GLubyte perm_b[6] = { 3, 2, 1, 1, 2, 0 };
    float cw = w / 2.f, ch = h / 2.f;
    int i;

    glViewport(0, 0, w, h);
    glClearColor(0.05f, 0.08f, 0.25f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(0.f, (float)w, (float)h, 0.f, -1.f, 1.f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_tex_id);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glColorPointer(4, GL_UNSIGNED_BYTE, 0, colq);
    glTexCoordPointer(2, GL_FLOAT, 0, uvq);
    for (i = 0; i < 4; i++) {
        float x = (i % 2) * cw + 6, y = (i / 2) * ch + 6;
        GLfloat v[16] = { x, y, x + cw - 12, y, x, y + ch - 12,
                          x + cw - 12, y + ch - 12 };

        glVertexPointer(2, GL_FLOAT, 0, v);
        if (i == 0) {
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_BYTE, tri_b);
        } else if (i == 1) {
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, tri_s);
        } else if (i == 2) {
            glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_BYTE, strip_b);
        } else {
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_BYTE, perm_b);
        }
    }
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_TEXTURE_2D);
}
#endif

#if CTEX
static void ctex_frame(int w, int h)
{
    static const GLfloat v[] = { 24.f, 24.f, 216.f, 24.f, 24.f, 216.f,
                                 216.f, 216.f };
    static const GLfloat uv[] = { 0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 1.f };

    glViewport(0, 0, w, h);
    glClearColor(g_ctex_err ? .8f : 0.f, g_ctex_err ? 0.f : .5f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(0.f, (float)w, (float)h, 0.f, -1.f, 1.f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_tex_id);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, v);
    glTexCoordPointer(2, GL_FLOAT, 0, uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_TEXTURE_2D);
}
#endif

#if VARIANT == 76
static void grid_frame(int w, int h)
{
    static GLfloat v[25 * 2], uv[25 * 2];
    static uint16_t idx[96];
    int i, j, k = 0;

    for (j = 0; j < 5; j++) {
        for (i = 0; i < 5; i++) {
            v[2 * (j * 5 + i)] = 10.f + i * (w - 20.f) / 4;
            v[2 * (j * 5 + i) + 1] = 10.f + j * (h - 20.f) / 4;
            uv[2 * (j * 5 + i)] = i / 4.f;
            uv[2 * (j * 5 + i) + 1] = j / 4.f;
        }
    }
    for (j = 0; j < 4; j++) {
        for (i = 0; i < 4; i++) {
            uint16_t a = j * 5 + i;

            idx[k++] = a;
            idx[k++] = a + 1;
            idx[k++] = a + 5;
            idx[k++] = a + 5;
            idx[k++] = a + 1;
            idx[k++] = a + 6;
        }
    }
    glViewport(0, 0, w, h);
    glClearColor(0.05f, 0.08f, 0.25f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(0.f, (float)w, (float)h, 0.f, -1.f, 1.f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_tex_id);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, v);
    glTexCoordPointer(2, GL_FLOAT, 0, uv);
    glDrawElements(GL_TRIANGLES, 96, GL_UNSIGNED_SHORT, idx);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_TEXTURE_2D);
}
#endif

void gl_app_frame(int w, int h, uint32_t frame)
{
    static const GLfloat pos[] = {
        -1.f, -1.f, 0.f,   1.f, -1.f, 0.f,   -1.f, 1.f, 0.f,   1.f, 1.f, 0.f,
    };
#if MIP || VARIANT == 34
    static const GLfloat uv[] = { 0.f, 0.f,  8.f, 0.f,  0.f, 8.f,  8.f, 8.f };
#else
    static const GLfloat uv[] = { 0.f, 0.f,  1.f, 0.f,  0.f, 1.f,  1.f, 1.f };
#endif
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
#if VARIANT == 68
    (void)frame;
    wrap_frame(w, h);
    return;
#endif
#if VARIANT == 69
    (void)frame;
    texmat_frame(w, h);
    return;
#endif
#if VARIANT == 74
    (void)frame;
    lines_frame(w, h);
    return;
#endif
#if VARIANT == 75
    (void)frame;
    elements_frame(w, h);
    return;
#endif
#if VARIANT == 76
    (void)frame;
    grid_frame(w, h);
    return;
#endif
#if CTEX
    (void)frame;
    ctex_frame(w, h);
    return;
#endif
#if VARIANT >= 65 && VARIANT <= 67
    (void)frame;
    env_table_frame(w, h);
    return;
#endif
    glViewport(0, 0, w, h);
#if VARIANT == 77
    {
        uint32_t f = hb_os_heap_free() >> 4;

        glClearColor(((f >> 16) & 255) / 255.f, ((f >> 8) & 255) / 255.f,
                     (f & 255) / 255.f, 1.f);
    }
#elif VARIANT == 4
    glClearColor(1.f, 0.5f, 0.25f, 1.f);
#elif VARIANT == 26
    {
        void *tls = ((void *(*)(void))(0x0841c1b4u | 1u))(), *ctx = 0;
        const uint8_t *gc = 0;

        glAlphaFunc(GL_GREATER, 0.5f);
        glEnable(GL_ALPHA_TEST);
        if (tls && (ctx = *(void **)((char *)tls + 12)) != 0) {
            gc = *(const uint8_t **)((char *)ctx + 0x24);
        }
        glClearColor(gc && (*(const uint32_t *)(gc + 0x140) & 2) ? 1.f : 0.f,
                     gc && *(const uint32_t *)(gc + 0x3d0) == 0x90000000u ?
                     1.f : 0.f, gc ? 1.f : 0.f, 1.f);
    }
#elif VARIANT == 25
    (void)glGetError();
    glEnable(GL_ALPHA_TEST);
    if (glGetError() != GL_NO_ERROR) {
        glClearColor(1.f, 0.f, 0.f, 1.f);
    } else {
        glClearColor(0.f, 1.f, 0.f, 1.f);
    }
    glDisable(GL_ALPHA_TEST);
#else
    glClearColor(0.05f, 0.08f, 0.25f, 1.f);
#endif
#if VARIANT == 35
    glClearStencil(0x55);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glEnable(GL_STENCIL_TEST);
#else
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
#endif

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustumf(-0.5f * aspect, 0.5f * aspect, -0.5f, 0.5f, 1.f, 10.f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
#if VARIANT == 34
    glTranslatef(0.f, -0.5f, -3.f);     /* floor z +5 .. -11 (eye at 0) */
    glRotatef(-90.f, 1.f, 0.f, 0.f);
    glScalef(4.f, 8.f, 1.f);
#elif MIP
    glTranslatef(0.f, -0.6f, -5.f);     /* z -8.9 .. -1.1: no near clip */
    glRotatef(-80.f, 1.f, 0.f, 0.f);
    glScalef(2.f, 4.f, 1.f);
#elif VARIANT != 5
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
#if VARIANT == 12 || VARIANT == 20 || VARIANT == 21
    glEnable(GL_CULL_FACE);
    glCullFace(VARIANT == 12 ? GL_BACK : VARIANT == 20 ? GL_FRONT :
               GL_FRONT_AND_BACK);
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
#if VARIANT == 22 || VARIANT == 23
    alpha_func(GL_GREATER, 0.5f);
#elif VARIANT == 24
    alpha_func(GL_LESS, 0.5f);
#endif
#if VARIANT == 22 || VARIANT == 24
    glEnable(GL_ALPHA_TEST);
#endif
#if VARIANT == 42
    fog_setup(0x2601, 1.f, 2.5f, 5.f);          /* GL_LINEAR */
#elif VARIANT == 43
    fog_setup(0x0800, 0.3f, 0.f, 1.f);          /* GL_EXP */
#elif VARIANT == 44
    fog_setup(0x0801, 0.3f, 0.f, 1.f);          /* GL_EXP2 */
#endif
#if VARIANT >= 41 && VARIANT <= 44
    glEnable(GL_FOG);
#endif
#if VARIANT == 60
    glEnable(GL_POLYGON_OFFSET_FILL);
#endif
#if VARIANT == 70
    glEnable(0x3000);           /* GL_CLIP_PLANE0 */
    glEnable(0x3001);
#endif
#if VARIANT == 73
    glEnable(0x3001);
    glEnable(0x3003);
    {
        uint8_t *gc = ((uint8_t *(*)(void))(0x083c2df0u | 1u))();

        if (gc) {
            float *pl = (float *)(gc + 0xcd4);

            pl[4] = 0.f; pl[5] = 1.f; pl[6] = 0.f; pl[7] = .25f;
            pl[12] = 0.f; pl[13] = 0.f; pl[14] = 1.f; pl[15] = 5.f;
            *(volatile uint32_t *)(gc + 0x1500) |= 0x3f0000u;
        }
    }
#endif
#if VARIANT == 71 || VARIANT == 72
    {
        uint8_t *gc = ((uint8_t *(*)(void))(0x083c2df0u | 1u))();

        if (gc) {
            float *pl = (float *)(gc + 0xcd4);

            pl[0] = 1.f; pl[1] = 0.f; pl[2] = 0.f; pl[3] = 0.f;
#if VARIANT == 72
            pl[4] = 0.f; pl[5] = 1.f; pl[6] = 0.f; pl[7] = 0.f;
#endif
            *(volatile uint32_t *)(gc + 0x1500) |= 0x3f0000u;
        }
    }
    glEnable(0x3000);
#if VARIANT == 72
    glEnable(0x3001);
#endif
#endif
#if VARIANT == 36
    glShadeModel(GL_FLAT);
#elif VARIANT == 37
    glColorMask(1, 0, 1, 1);
#elif VARIANT == 38
    glColorMask(1, 1, 1, 0);
#elif VARIANT == 39
    glColorMask(0, 0, 0, 0);
#elif VARIANT == 40
    glColorMask(0, 0, 1, 1);
#endif
#if LIT
    {
        static const GLfloat nrm[] = {
            -.5f, -.5f, .7071f,  .5f, -.5f, .7071f,
            -.5f, .5f, .7071f,   .5f, .5f, .7071f,
        };
#if VARIANT == 55
        {
            uint8_t *gc = ((uint8_t *(*)(void))(0x083c2df0u | 1u))();

            if (gc) {
                *(volatile uint32_t *)(gc + 0x2d0) = 1;  /* two-sided */
            }
        }
#endif
#if VARIANT == 54 || VARIANT == 55
        glRotatef(180.f, 1.f, 0.f, 0.f);    /* show the back face */
#endif
        glEnable(GL_LIGHTING);
#if VARIANT == 51
        glDisable(GL_LIGHT0);
#else
        glEnable(GL_LIGHT0);
#endif
#if VARIANT == 50 || VARIANT == 51
        {
            static const GLfloat red[] = { 1.f, 0.f, 0.f, 1.f };
            static const GLfloat left[] = { -1.f, 0.f, 0.f, 0.f };

            glPushMatrix();
            glLoadIdentity();
            glLightfv(GL_LIGHT0 + 1, GL_DIFFUSE, red);
            glLightfv(GL_LIGHT0 + 1, GL_POSITION, left);
            glPopMatrix();
            glEnable(GL_LIGHT0 + 1);
        }
#endif
        glEnableClientState(GL_NORMAL_ARRAY);
#if VARIANT == 52
        {
            /* x 127, padded to 4 bytes per normal so it is word-aligned */
            static const int8_t nb[] = {
                -64, -64, 90, 0,   64, -64, 90, 0,
                -64, 64, 90, 0,    64, 64, 90, 0,
            };
            glNormalPointer(GL_BYTE, 4, nb);
        }
#elif VARIANT == 53
        {
            static const int16_t ns[] = {   /* x 32767 */
                -16384, -16384, 23170,   16384, -16384, 23170,
                -16384, 16384, 23170,    16384, 16384, 23170,
            };
            glNormalPointer(GL_SHORT, 0, ns);
        }
#else
        glNormalPointer(GL_FLOAT, 0, nrm);
#endif
#if VARIANT >= 46 && VARIANT <= 48
        {
            static const GLfloat one[] = { 1.f, 1.f, 1.f, 1.f };
            static const GLfloat dif[] = { 1.f, .5f, 0.f, 1.f };
            static const GLfloat amb[] = { .1f, .1f, .3f, 1.f };
            static const GLfloat dir[] = { 1.f, 1.f, 1.f, 0.f };
            static const GLfloat eye[] = { 0.f, 0.f, 0.f, 1.f };
            static const GLfloat down[] = { 0.f, 0.f, -1.f };

            /* light positions are taken through the current modelview:
             * set them with the modelview at identity (eye space) */
            glPushMatrix();
            glLoadIdentity();
            glLightfv(GL_LIGHT0, GL_DIFFUSE, dif);
            glLightfv(GL_LIGHT0, GL_AMBIENT, amb);
            glLightfv(GL_LIGHT0, GL_SPECULAR, one);
            glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, one);
            glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 16.f);
#if VARIANT == 46
            glLightfv(GL_LIGHT0, GL_POSITION, dir);
#else
            glLightfv(GL_LIGHT0, GL_POSITION, eye);
#endif
#if VARIANT == 47
            glLightf(GL_LIGHT0, GL_LINEAR_ATTENUATION, .3f);
#elif VARIANT == 48
            glLightfv(GL_LIGHT0, GL_SPOT_DIRECTION, down);
            glLightf(GL_LIGHT0, GL_SPOT_CUTOFF, 15.f);
            glLightf(GL_LIGHT0, GL_SPOT_EXPONENT, 8.f);
#endif
            glPopMatrix();
        }
#endif
    }
#endif
#if VARIANT == 58 || VARIANT == 59
    {
        uint8_t *gc = ((uint8_t *(*)(void))(0x083c2df0u | 1u))();

        if (gc) {
            *(volatile float *)(gc + 0x260) = 24.f;
            *(volatile float *)(gc + 0x258) = 12.f;
            *(volatile float *)(gc + 0x25c) = 12.f;
            *(volatile uint32_t *)(gc + 0x1500) |= 1u << 25;
        }
    }
#endif
#if VARIANT == 57 || VARIANT == 58
    glEnable(GL_POINT_SPRITE_OES);
    glTexEnvi(GL_POINT_SPRITE_OES, GL_COORD_REPLACE_OES, 1);
#endif
#if VARIANT != 1 && VARIANT != 5 && VARIANT != 19 && VARIANT != 36 && \
    !(LIT && VARIANT != 49) && VARIANT != 56 && VARIANT != 59
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_tex_id);
#if VARIANT == 8
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
#else
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
#endif
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT, 0, uv);
#if MT
    {
        static const GLfloat uv2[] = { 0.f, 0.f, 2.f, 0.f, 0.f, 2.f, 2.f, 2.f };

        glActiveTexture(GL_TEXTURE1);
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, g_tex2_id);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE,
                  VARIANT == 61 ? GL_MODULATE : VARIANT == 62 ? GL_ADD :
                  VARIANT == 63 ? GL_DECAL : GL_REPLACE);
        glClientActiveTexture(GL_TEXTURE1);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glTexCoordPointer(2, GL_FLOAT, 0, uv2);
        glClientActiveTexture(GL_TEXTURE0);
        glActiveTexture(GL_TEXTURE0);
    }
#endif
#endif
    glEnableClientState(GL_VERTEX_ARRAY);
#if VARIANT != 2 && VARIANT != 5 && VARIANT != 19 && !MIP && VARIANT != 34 && \
    !LIT
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
#elif VARIANT >= 56 && VARIANT <= 59
    glVertexPointer(3, GL_FLOAT, 0, pos);
    glDrawArrays(GL_POINTS, 0, 4);
#else
    glVertexPointer(3, GL_FLOAT, 0, pos);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
#endif
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_TEXTURE_2D);
#if MT
    glActiveTexture(GL_TEXTURE1);
    glDisable(GL_TEXTURE_2D);
    glClientActiveTexture(GL_TEXTURE1);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glClientActiveTexture(GL_TEXTURE0);
    glActiveTexture(GL_TEXTURE0);
#endif
#if VARIANT == 57 || VARIANT == 58
    glDisable(GL_POINT_SPRITE_OES);
#endif
#if LIT
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisable(GL_LIGHTING);
#endif
#if VARIANT >= 37 && VARIANT <= 40
    glColorMask(1, 1, 1, 1);    /* glClear honours the mask */
#endif
}
