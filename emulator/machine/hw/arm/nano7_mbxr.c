/*
 * nano7_mbxr.c -- software renderer for the PowerVR MBX Lite model of the
 * iPod nano 7G (S5L8747), Session 32.
 *
 * Stream (RE of the OS GL ES driver + captures of probe/gltest, see
 * docs/research/mbx-gltest-frame.txt). The driver writes the TA FIFO
 * (0x3b800000) in segments, each committed by a doorbell (0x3bc00000).
 * Commands, by the top bits of the word in command position:
 *   0x8000mmmm  vertex program upload: one descriptor word per set bit of
 *               m, then 0x90SS00EE and (EE-SS+1) instruction pairs
 *   0xa0SS00EE  vertex program constants SS..EE, 4 floats each
 *   0x1000mmmm  draw: state blocks for the set bits of the dirty mask m, in
 *               bit order - b0 ISP/TSP word, b1 one word, b2 texture layer 0
 *               (3 words), b3 layer 1 (3 words), b4 tile region word, b5
 *               vertex format, b6 one word, b7 viewport (6 floats: x offset,
 *               x scale, y offset, y scale, z offset, z scale), b8 one word -
 *               then the primitive word (top nibble 4 strip, 5 fan, 7 list),
 *               the control word (bit 30: vertex program), and the
 *               vertices up to the end of the segment
 *   0x4/5/7...  a draw with unchanged state: the primitive word first
 *   0xf0000000  end of the scene's list
 * Vertex format: one nibble per attribute (0 position, 2 colour, 3 uv0),
 * the value is its size in words (5 = packed 0xAABBGGRR colour, 1 word).
 * Vertex processing (Session 56): the uploaded vertex program is executed
 * by an interpreter (vgp_run(), see there for the decoded ISA); the
 * high-level emulation below - transform, lighting, fog, texture matrix,
 * point-sprite expansion - remains as the NANO7_MBXR_VGP=hle fallback, the
 * values for draws whose stages were not uploaded yet after a resume, and
 * the NANO7_MBXR_VGPCHECK cross-check.
 * Batches with bit 30 in the word after the primitive word run through
 * the vertex program (VGP) - transformed draws send 0xf002xxxx there, the
 * pass-through clears 0xb00b0800 (Session 40; earlier sessions keyed this
 * on ISP bit 22, which is really GL_SMOOTH): the driver's fixed-function
 * programs compute clip = c0..c3 . (x, y, z, 1);
 * this renderer does that directly (high-level emulation of the VGP)
 * instead of executing the microcode, clips against the near plane, then
 * divides by w and applies the viewport. Other batches (the clears: full-screen triangles (0,0),
 * (480,0), (0,864)) carry pixel coordinates.
 * Texture layer word 1: address >> 7 in bits 23:0 (GPU virtual), bit 31
 * bilinear filter (Session 36: set when either GL_TEXTURE_MIN_FILTER or
 * MAG_FILTER is GL_LINEAR - the driver has one bit for both); texels
 * are twiddled (Morton order, y in the low bit). Size and format: see
 * mbxr_tex_params().
 * GPU addresses go through the MBX MMU: core +0x1000 + 4 * n holds the
 * physical address of page table n (1024 x 4-byte entries, 4 KB pages).
 * Render job registers: +0x65c target (GPU VA), +0x660 stride in pixels,
 * +0x654 / +0x658 last x / y in bits 31:16; ARGB8888.
 *
 * Model: per segment the draws are transformed and set up into triangles;
 * the render job rasterizes them (perspective-correct colour and uv,
 * depth compare / write mask, nearest or bilinear texel (Session 36) with
 * mipmaps (Session 37), texture combine (modulate / replace), blending
 * (Session 33)) into a host frame and writes it to the target, with face
 * culling (Session 34), alpha test (Session 35), flat shading (Session 40),
 * the colour mask (Session 41), fog (Session 42) and fixed-function
 * lighting (Session 43). Not modeled: vertex programs other than the
 * driver's fixed-function ones; stencil does not exist in this driver
 * (Session 39).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "nano7_mbxr.h"
#include "exec/memory.h"
#include "exec/address-spaces.h"
#include "qemu/host-utils.h"
#include "migration/vmstate.h"
#include <math.h>

#define SEG_MAX         (1u << 20)      /* words per segment */
#define TRI_MAX         65536
#define NCONST          256
#define FB_MAX_W        1024
#define FB_MAX_H        1024

typedef struct MbxTex {
    uint32_t w[3];              /* layer words as sent */
} MbxTex;

typedef struct MbxVtx {
    float x, y, z, iw;          /* screen x, y, depth 0..1, 1/w */
    float c[4];                 /* r g b a (0..1), divided by w */
    float u, v;                 /* divided by w */
    float f;                    /* fog factor (1 = no fog), divided by w */
    float u1, v1;               /* texture unit 1, divided by w */
} MbxVtx;

typedef struct MbxTri {
    MbxVtx v[3];
    uint32_t isp;
    uint32_t atest;             /* draw block 1: alpha test, see ATEST_* */
    int16_t tex;                /* index into r->texs, -1 = untextured */
    int16_t tex1;               /* texture unit 1 (layer 1), -1 = off */
    bool depth_only, no_depth_test;
    bool flat;                  /* GL_FLAT: colour of the provoking vertex */
    float fc[4];
    uint32_t keep;              /* ARGB8888 bits kept from the frame (mask) */
    bool fog;                   /* ISP_FOG: blend towards FOG_RGB by v.f */
} MbxTri;

/* ISP/TSP word fields (gltest variants 9-13, docs/research/mbx-gltest-
 * frame.txt): blend factors in the PowerVR2 TSP encoding (0 zero, 1 one,
 * 2 other colour, 3 1 - other colour, 4 src alpha, 5 1 - src alpha, 6 dst
 * alpha, 7 1 - dst alpha; "other" = dst for the source factor, src for the
 * destination factor) - opaque draws carry ONE / ZERO; bit 13 is set with
 * GL_BLEND; bits 11:9 depth compare (GL order: 0 never, 1 less, 2 equal,
 * 3 lequal, 4 greater, 5 notequal, 6 gequal, 7 always; LESS is the GL
 * default, glDisable(GL_DEPTH_TEST) gives 7); bit 7 no depth write (set with
 * the depth test off); bit 17 textured */
#define ISP_SRC(isp)        ((isp) >> 29)
#define ISP_DST(isp)        (((isp) >> 26) & 7)
#define ISP_ZCMP(isp)       (((isp) >> 9) & 7)
#define ISP_NO_ZWRITE       (1u << 7)
/* bit 22 = Gouraud shading: glShadeModel (0x0848141f) puts 0x400000 into
 * the ISP source field gc+0x2bc for GL_SMOOTH, 0 for GL_FLAT (Session 39) */
#define ISP_GOURAUD         (1u << 22)
/* the word after the primitive word: bit 30 = run the vertex program */
#define DRAW_VGP            (1u << 30)

/*
 * glColorMask (Session 41; OS handler 0x08480e5a -> gc+0x3e0, 1 = channel
 * masked: 0x80 R, 0x40 G, 0x20 B, 0x100 A): the dirty flush (0x0810bef4)
 * ORs gc+0x3e0 into texture layer word 0 - for an untextured draw with a
 * partial mask it emits a dummy layer 0 just to carry it - and the ISP build
 * sets ISP bit 14 for a partial mask, bit 8 when all four channels are
 * masked (gltest variants 37-40: layer word 0 0x03319640 G, 0x03319700 A,
 * 0x033197e0 all, 0x033196c0 R + G). Bit 14 also comes with an active alpha
 * test; the layer bits are 0 then.
 */
#define ISP_NO_COLOR        (1u << 8)
#define ISP_MASKED          (1u << 14)
#define LAYER_CMASK(w0)     (((w0) >> 5) & 0xf)     /* bit 0 B .. bit 3 A */

/*
 * Fog (Session 42). The OS driver has no glFog* (vtable slots 0xa4..0xd4
 * are empty); glEnable(GL_FOG) sets ISP bit 24, and apps can only reach the
 * parameters by writing the context (gc+0x32c mode, +0x334 colour, +0x344
 * density, +0x348 start, +0x34c end, +0x350 1 / (end - start)). The vertex
 * program always carries a fog stage (descriptor 0x4000) chosen by the mode;
 * with fog on the driver also uploads the modelview rows as c8..c11, and c91
 * = (end, 1 / (end - start), density * log2(e), density * sqrt(log2(e))).
 * The stage is recognised by its last instruction word (gltest variants
 * 41-44): LINEAR 0x0a09005b, EXP 0x0a020005, EXP2 0x0a020006. Factor per
 * vertex from the eye distance d = |eye z| (GL rules): LINEAR (end - d) /
 * (end - start), EXP 2^(-c91.z d), EXP2 2^(-(c91.w d)^2), clamped; per
 * pixel rgb = f * colour + (1 - f) * fog colour, after texturing. The fog
 * colour is never sent to the GPU (no FIFO word or register changes with
 * it), so the hardware keeps its own - modeled as black, GL's default.
 */
#define ISP_FOG             (1u << 24)
#define FOG_RGB             0.f
#define CST_FOG             91
#define CST_EYE             8           /* modelview rows c8..c11 */
enum { FOG_NONE, FOG_LINEAR, FOG_EXP, FOG_EXP2 };

/*
 * Lighting (Session 43; RE of the driver's light setters 0x08192a6c /
 * material 0x080eded4 / constant upload 0x081558f8 + gltest variants
 * 45-51). Constants, raw (the vertex program forms the products):
 *   c8..c11 modelview rows, c12..c14 normal matrix rows,
 *   c18 / c19 colour-material weights per term (x ambient, y diffuse,
 *   z specular, w emission: material = c18 * material + c19 * vertex
 *   colour), c20 scene colour = (emission + material ambient * model
 *   ambient) * 256/255 with alpha = material diffuse alpha, c22..c25
 *   material ambient / diffuse / specular / emission, c26.x shininess,
 *   c27 + 7k per ENABLED light, directional lights first:
 *     +0 half vector (w 0) / eye position (w 1), +1 direction to the light /
 *     spot direction, +2 ambient, +3 diffuse, +4 specular, positional only:
 *     +5 (spot exponent, cos cutoff, 1 = spot), +6 attenuation (kc, kl, kq).
 * Normals are vertex attribute slot 1 (3 floats; the driver feeds the
 * current normal as a stride-0 attribute when there is no normal array),
 * so a draw is lit when its format has slot 1 and the "any light" stage
 * (program stage bit 10) is the lit fragment (descriptor top byte 0x05,
 * 0x01 when no light is on). Stage 8 / 9 descriptors carry the directional
 * / positional loop count - 1 in bits 19:16; since 0 means 0 or 1 light,
 * a block is only taken when its +0 w matches its kind. No light-model
 * setter exists: infinite viewer, one-sided. Normals are renormalised
 * (GL_NORMALIZE has no consumer in the driver; an assumption).
 */
#define CST_NRM             12
#define VGP_PROG_MAX        256     /* instruction slots (2 words each) */
#define VPDUMP_FRAME        30
/* c92 = (scale, bias) of the normal attribute: (1, 0) for floats, GL's
 * signed-normalised (2c + 1) / (2^b - 1) for short4 normals: (2/255, 1/255)
 * from GL_BYTE, (2/65535, 1/65535) from GL_SHORT (Session 44, gltest 52/53) */
#define CST_NRM_DECODE      92
#define ATTR_SHORT4         8
/* texture unit 1 (Session 48): control word bit 13 (bit 12 = unit 0),
 * texture layer 1 (draw block 3), texcoords in vertex slot 4 */
#define CTL_TEX1            (1u << 13)
#define CTL_UCLIP           ((1u << 20) | (1u << 5))
#define UV1_SLOT            4

/*
 * Texture matrix (Session 51, gltest 69): unit n's GL_TEXTURE matrix is
 * uploaded as c85 + 3n .. c87 + 3n = the rows producing s, t, q (applied to
 * (s, t, 0, 1)); the vertex-program stage of bit 12 + n is a plain copy (1
 * instruction, e.g. 0x00806969) for identity, 2 instructions (0x05506a69)
 * for an affine matrix, 5 (0x05506d69: three dots, a reciprocal of q, a
 * multiply) when the q row is not (0, 0, 0, 1) - the divide by q is per
 * vertex in the hardware, so that is what is modeled (not GL's per-pixel
 * divide).
 */
#define CST_TEXMAT          85
#define STAGE_TEX0          12

/*
 * Points and point sprites (Session 46; gltest variants 56 / 57). The driver
 * has no glPointSize (vtable slots 0x1a4..0x1c0 null): the size is GL's 1
 * unless an app writes the context.
 *   GL_POINTS: primitive word top nibble 0xe (0xe0020000), then one float =
 *   half the point size in pixels (0x3f000000 = 0.5 for size 1), then the
 *   control word and the vertices; each vertex is a screen-aligned square.
 *   Point sprites (GL_POINT_SPRITE_OES + GL_COORD_REPLACE_OES): the driver
 *   sends every point as 4 vertices at the same position, each with a
 *   slot-7 attribute (format nibble 7, one word: corner u16 x | u16 y << 16:
 *   (1,0) (0,0) (1,1) (0,1)) as a strip (0x40020000, control word
 *   0xf002101c); the vertex program copies the corner to the texcoord.
 *   The driver emits the corners from a table ({1,0,0x10001,0x10000}, Y-
 *   flipped {0,1,0x10000,0x10001} when primitive bit 19 is set) and the
 *   vertex program offsets them by the size from constants c99 =
 *   (attenuation a, b, c, 0.5) and c100 = (min 0, max 32, size 1) (the
 *   driver's defaults; nothing can change them but a write to the context:
 *   size gc+0x260, half size of GL_POINTS gc+0x258 / +0x25c). Modeled:
 *   size = clamp(c100.z * sqrt(1 / (a + b d + c d^2)), min, max) with d the
 *   eye distance (GL point parameters), each corner at centre +
 *   (2 corner - 1) * size / 2 in pixels with uv = the corner.
 */
#define CST_PT_ATT          99
#define CST_PT_SIZE         100
#define PRIM_POINTS         0xe
/* lines (Session 53, gltest 74): primitive nibble 6 = a line strip, then a
 * float = half the line width in pixels (glLineWidth / 2), the control word
 * and the vertices; GL_LINES = one 2-vertex strip per segment (doorbell-0
 * restarts), GL_LINE_STRIP = one strip, GL_LINE_LOOP = one strip with the
 * first vertex repeated at the end. Each segment is drawn as GL's wide-line
 * rectangle: offset by +-half width in y for an x-major line, in x for a
 * y-major one. */
#define PRIM_LINES          0x6
#define ATTR_SHORT2         7
#define SPRITE_SLOT         7
#define CST_CM_MAT          18
#define CST_CM_VTX          19
#define CST_SCENE           20
#define CST_MAT             22          /* ambient, diffuse, specular, emission */
#define CST_SHININESS       26
#define CST_LIGHT           27
#define LIGHT_STRIDE        7
#define MAX_LIGHTS          8
#define STAGE_DIR           8
#define STAGE_POS           9
#define STAGE_ANY           10
#define STAGE_LOOP(d)       ((((d) >> 16) & 0xf) + 1)

/* primitive word bits 19:18 = face culling (gltest variants 0/12/20):
 * 0 none (0x40020000), 1 GL_BACK (0x40060000), 2 GL_FRONT (0x400a0000); for
 * GL_FRONT_AND_BACK the driver sends no draw. Front = counter-clockwise in
 * GL window space (y up), i.e. negative edge() area in this renderer's y-down
 * screen space. */
#define PRIM_CULL(prim)     (((prim) >> 18) & 3)

/* draw block 1 = alpha test (OS state build 0x08102784, gltest variants
 * 22-24): bits 31:29 compare in GL order (as ISP_ZCMP), bits 28:21 reference
 * (ref * 256, clamped to 255), compared with the fragment's 8-bit alpha
 * after the texture combine. The driver sends 0xe0000000 (always) while
 * GL_ALPHA_TEST is off, and also sets ISP bit 14 while it is on; a test that
 * can never pass comes as 0 (never) with ISP bits 8:7. A failed fragment
 * writes neither colour nor depth. */
#define ATEST_CMP(b1)       ((b1) >> 29)
#define ATEST_REF(b1)       (((b1) >> 21) & 0xff)

struct MbxR {
    /* persistent TA state */
    uint32_t isp, b1, region, fmt, b6, b8;
    unsigned cull;              /* PRIM_CULL() of the draw being set up */
    uint32_t keep;              /* colour mask of the draw being set up */
    int tex1;                   /* texture unit 1 of the draw being set up */
    unsigned fog_mode;          /* FOG_*: the uploaded fog program stage */
    bool uclip;                 /* the draw clips against the user plane */
    uint32_t stage[16];         /* last program descriptor per stage bit */
    uint32_t stage_valid;       /* stages uploaded since reset / resume */
    uint32_t prog[VGP_PROG_MAX][2]; /* vertex program memory (Session 56) */
    FILE *vpdump;               /* NANO7_MBXR_VPDUMP: per-draw VGP ground truth */
    bool vgp_hle;               /* NANO7_MBXR_VGP=hle: Sessions 32-54 HLE */
    bool vgp_check;             /* NANO7_MBXR_VGPCHECK: interpreter vs HLE */
    float chk[5];               /* max |diff| p, colour, uv, uv1, fog */
    MbxrStats st;               /* workload counters (usage HUD) */
    uint64_t chk_n;
    float point_half;           /* half point size / line width (px) */
    /* doorbell 0 = primitive restart: the next segment holds only more
     * vertices of the same draw (point sprites: one strip of 4 per point) */
    bool cont;
    uint32_t cont_prim, cont_ctl;
    MbxTex layer[2];
    float vp[6];
    float cst[NCONST][4];
    /* segment being received */
    uint32_t *seg;
    uint32_t seg_len;
    /* the scene */
    MbxTri *tri;
    uint32_t ntri;
    MbxTex *texs;
    uint32_t ntex;
    /* host frame */
    uint32_t *fb;
    float *zb;
    uint64_t frames, warned;
    /* NANO7_MBXR_STATS: distinct draw states (isp, b1, fmt, tex word 0) */
    bool stats;
    uint64_t dbg_frame;         /* NANO7_MBXR_FRAME: debug dumps (300) */
    int probe_x, probe_y;       /* NANO7_MBXR_PROBE=x,y: fragments there */
    uint32_t dump_isp;          /* NANO7_MBXR_DUMP=isp: print one segment */
    const char *dump_fb;        /* NANO7_MBXR_DUMPFB=file: frame 300 (or NANO7_MBXR_FRAME) as PAM */
    uint32_t st_key[64][5], st_n[64], st_cnt;
    /* save state: byte sizes of the used part of seg / tri / texs */
    uint32_t vm_seg_bytes, vm_tri_bytes, vm_tex_bytes;
};

static float f32(uint32_t w)
{
    float f;

    memcpy(&f, &w, 4);
    return f;
}

static void warn_once(MbxR *r, unsigned bit, const char *fmt, uint32_t v)
{
    if (!(r->warned & (1ull << bit))) {
        r->warned |= 1ull << bit;
        fprintf(stderr, "[nano7-mbxr] ");
        fprintf(stderr, fmt, v);
        fputc('\n', stderr);
    }
}

MbxR *mbxr_new(void)
{
    MbxR *r = g_new0(MbxR, 1);

    r->seg = g_new(uint32_t, SEG_MAX);
    r->tri = g_new(MbxTri, TRI_MAX);
    r->texs = g_new(MbxTex, TRI_MAX);
    r->fb = g_new(uint32_t, FB_MAX_W * FB_MAX_H);
    r->zb = g_new(float, FB_MAX_W * FB_MAX_H);
    r->stats = getenv("NANO7_MBXR_STATS") != NULL;
    r->probe_x = r->probe_y = -1;
    if (getenv("NANO7_MBXR_PROBE")) {
        sscanf(getenv("NANO7_MBXR_PROBE"), "%d,%d", &r->probe_x, &r->probe_y);
    }
    r->dbg_frame = getenv("NANO7_MBXR_FRAME") ?
                   strtoull(getenv("NANO7_MBXR_FRAME"), NULL, 0) : 300;
    r->point_half = 0.5f;       /* GL default point size 1 */
    r->dump_fb = getenv("NANO7_MBXR_DUMPFB");
    r->vgp_check = getenv("NANO7_MBXR_VGPCHECK") != NULL;
    r->vgp_hle = getenv("NANO7_MBXR_VGP") &&
                 !strcmp(getenv("NANO7_MBXR_VGP"), "hle");
    if (getenv("NANO7_MBXR_VPDUMP")) {
        r->vpdump = fopen(getenv("NANO7_MBXR_VPDUMP"), "w");
    }
    if (getenv("NANO7_MBXR_DUMP")) {
        r->dump_isp = strtoul(getenv("NANO7_MBXR_DUMP"), NULL, 16);
    }
    return r;
}

void mbxr_reset(MbxR *r)
{
    r->seg_len = 0;
    r->ntri = 0;
    r->ntex = 0;
    r->cont = false;
    r->stage_valid = 0;         /* program memory: re-uploaded per frame */
}

/* ------------------------------------------------------------------ */
/* save state                                                          */

/* Everything the OS driver does not resend per frame (draw state, the
 * vertex program and constants, texture layers), plus the segment and
 * scene in flight. Floats and the scene arrays are saved as host bytes:
 * save states are only loaded by the same binary. Not saved: the host
 * frame / depth buffers (cleared by every render job), debug and HUD
 * counters. */
static int mbxr_pre_save(void *opaque)
{
    MbxR *r = opaque;

    r->vm_seg_bytes = r->seg_len * sizeof(uint32_t);
    r->vm_tri_bytes = r->ntri * sizeof(MbxTri);
    r->vm_tex_bytes = r->ntex * sizeof(MbxTex);
    return 0;
}

static bool mbxr_sizes_ok(void *opaque, int version_id)
{
    MbxR *r = opaque;

    return r->vm_seg_bytes <= SEG_MAX * sizeof(uint32_t) &&
           r->vm_seg_bytes % sizeof(uint32_t) == 0 &&
           r->vm_tri_bytes <= TRI_MAX * sizeof(MbxTri) &&
           r->vm_tri_bytes % sizeof(MbxTri) == 0 &&
           r->vm_tex_bytes <= TRI_MAX * sizeof(MbxTex) &&
           r->vm_tex_bytes % sizeof(MbxTex) == 0;
}

static int mbxr_post_load(void *opaque, int version_id)
{
    MbxR *r = opaque;

    r->seg_len = r->vm_seg_bytes / sizeof(uint32_t);
    r->ntri = r->vm_tri_bytes / sizeof(MbxTri);
    r->ntex = r->vm_tex_bytes / sizeof(MbxTex);
    return 0;
}

const VMStateDescription vmstate_mbxr = {
    .name = "nano7-mbxr",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = mbxr_pre_save,
    .post_load = mbxr_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(isp, MbxR),
        VMSTATE_UINT32(b1, MbxR),
        VMSTATE_UINT32(region, MbxR),
        VMSTATE_UINT32(fmt, MbxR),
        VMSTATE_UINT32(b6, MbxR),
        VMSTATE_UINT32(b8, MbxR),
        VMSTATE_UINT32(cull, MbxR),
        VMSTATE_UINT32(keep, MbxR),
        VMSTATE_INT32(tex1, MbxR),
        VMSTATE_UINT32(fog_mode, MbxR),
        VMSTATE_BOOL(uclip, MbxR),
        VMSTATE_UINT32_ARRAY(stage, MbxR, 16),
        VMSTATE_UINT32(stage_valid, MbxR),
        VMSTATE_UINT32_2DARRAY(prog, MbxR, VGP_PROG_MAX, 2),
        VMSTATE_BUFFER_UNSAFE(point_half, MbxR, 0, sizeof(float)),
        VMSTATE_BOOL(cont, MbxR),
        VMSTATE_UINT32(cont_prim, MbxR),
        VMSTATE_UINT32(cont_ctl, MbxR),
        VMSTATE_BUFFER_UNSAFE(layer, MbxR, 0, sizeof(MbxTex) * 2),
        VMSTATE_BUFFER_UNSAFE(vp, MbxR, 0, sizeof(float) * 6),
        VMSTATE_BUFFER_UNSAFE(cst, MbxR, 0, sizeof(float) * NCONST * 4),
        VMSTATE_UINT32(vm_seg_bytes, MbxR),
        VMSTATE_UINT32(vm_tri_bytes, MbxR),
        VMSTATE_UINT32(vm_tex_bytes, MbxR),
        VMSTATE_VALIDATE("nano7-mbxr sizes", mbxr_sizes_ok),
        VMSTATE_VBUFFER_UINT32(seg, MbxR, 0, NULL, vm_seg_bytes),
        VMSTATE_VBUFFER_UINT32(tri, MbxR, 0, NULL, vm_tri_bytes),
        VMSTATE_VBUFFER_UINT32(texs, MbxR, 0, NULL, vm_tex_bytes),
        VMSTATE_END_OF_LIST()
    }
};

void mbxr_scene_begin(MbxR *r)
{
    r->ntri = 0;
    r->ntex = 0;
}

void mbxr_fifo_word(MbxR *r, uint32_t w)
{
    if (r->seg_len < SEG_MAX) {
        r->seg[r->seg_len++] = w;
    }
}

/* ------------------------------------------------------------------ */
/* vertices -> triangles                                               */

/* words of one attribute from its format nibble: n floats, 5 = packed
 * ubyte4 colour, 8 = short4 (x | y << 16, z | w << 16; GL_BYTE / GL_SHORT
 * normals, converted to shorts by the driver, w = 1) */
static unsigned attr_words(unsigned n)
{
    return n == 5 || n == ATTR_SHORT2 ? 1 : n == 8 ? 2 : n;
}

/* a vertex before the perspective divide: clip-space position (pixel
 * coordinates and w = 1 for batches without the vertex program) and the
 * plain attributes */
typedef struct ClipVtx {
    float p[4];
    float c[4];
    float u, v;
    float f;                    /* fog factor */
    float ed;                   /* eye distance (point sprites) */
    float u1, v1;               /* texture unit 1 */
    float ud;                   /* user clip plane distance (>= 0 kept) */
    uint32_t corner;            /* sprite corner word (slot 7) */
} ClipVtx;

static void store_tri(MbxR *r, const MbxVtx *a, const MbxVtx *b,
                      const MbxVtx *c, int tex, bool depth_only, bool vgp,
                      const float *fc)
{
    MbxTri *t;

    if (r->ntri >= TRI_MAX) {
        warn_once(r, 0, "scene has more than %u triangles", TRI_MAX);
        return;
    }
    if (a->iw <= 0 || b->iw <= 0 || c->iw <= 0) {
        return;                 /* w <= 0 left after clipping: degenerate */
    }
    if (r->cull) {
        float area = (b->x - a->x) * (c->y - a->y) -
                     (b->y - a->y) * (c->x - a->x);
        bool front = area < 0;

        if (r->cull == 3 || (r->cull == 1 && !front) ||
            (r->cull == 2 && front)) {
            return;
        }
    }
    t = &r->tri[r->ntri++];
    t->v[0] = *a;
    t->v[1] = *b;
    t->v[2] = *c;
    t->isp = r->isp;
    t->atest = r->b1;
    t->tex = tex;
    t->tex1 = r->tex1;
    t->depth_only = depth_only;
    t->no_depth_test = !vgp;    /* clears: always pass */
    t->flat = !(r->isp & ISP_GOURAUD);
    t->keep = r->keep;
    t->fog = (r->isp & ISP_FOG) && vgp && r->fog_mode != FOG_NONE;
    memcpy(t->fc, fc, sizeof(t->fc));
}

/* perspective divide + viewport; attributes are stored divided by w for
 * perspective-correct interpolation */
static void project(MbxR *r, const ClipVtx *cv, bool vgp, MbxVtx *o)
{
    float iw = 1;
    int k;

    if (vgp) {
        iw = cv->p[3] != 0 ? 1.f / cv->p[3] : 0;
        o->x = r->vp[0] + r->vp[1] * cv->p[0] * iw;
        o->y = r->vp[2] + r->vp[3] * cv->p[1] * iw;
        o->z = r->vp[4] + r->vp[5] * cv->p[2] * iw;
    } else {
        o->x = cv->p[0];
        o->y = cv->p[1];
        o->z = cv->p[2];
    }
    o->iw = iw;
    for (k = 0; k < 4; k++) {
        o->c[k] = cv->c[k] * iw;
    }
    o->u = cv->u * iw;
    o->u1 = cv->u1 * iw;
    o->v1 = cv->v1 * iw;
    o->f = cv->f * iw;
    o->v = cv->v * iw;
}

static void clip_lerp(const ClipVtx *a, const ClipVtx *b, float f, ClipVtx *o)
{
    int k;

    for (k = 0; k < 4; k++) {
        o->p[k] = a->p[k] + (b->p[k] - a->p[k]) * f;
        o->c[k] = a->c[k] + (b->c[k] - a->c[k]) * f;
    }
    o->u = a->u + (b->u - a->u) * f;
    o->u1 = a->u1 + (b->u1 - a->u1) * f;
    o->v1 = a->v1 + (b->v1 - a->v1) * f;
    o->ud = a->ud + (b->ud - a->ud) * f;
    o->f = a->f + (b->f - a->f) * f;
    o->v = a->v + (b->v - a->v) * f;
}

/*
 * Near-plane clipping (Session 38), GL convention z >= -w (the viewport z
 * row maps NDC -1..1 to depth 0..1): Sutherland-Hodgman on the triangle in
 * clip space, keeping the vertex order (so the winding the culling sees is
 * unchanged), then a fan of the 3 or 4 vertices. Only the near plane is
 * clipped: x / y are bounded by the rasterizer's screen box, and far
 * fragments (depth > 1) fail the depth test against the cleared 1.0.
 * Batches without the vertex program are already in pixels.
 */
/* clip distance of a vertex against plane k: 0 = near (z >= -w), 1 = the
 * user clip plane (VGP clip-distance output, Session 57) */
static inline float clip_dist(const ClipVtx *v, int k)
{
    return k == 0 ? v->p[2] + v->p[3] : v->ud;
}

/* Sutherland-Hodgman of polygon in[0..n) against plane k into out; vertex
 * order kept (so the cull winding is unchanged) */
static int clip_poly(const ClipVtx *in, int n, int k, ClipVtx *out)
{
    int i, m = 0;

    for (i = 0; i < n; i++) {
        const ClipVtx *a = &in[i], *b = &in[(i + 1) % n];
        float da = clip_dist(a, k), db = clip_dist(b, k);

        if (da >= 0) {
            out[m++] = *a;
        }
        if ((da >= 0) != (db >= 0)) {
            clip_lerp(a, b, da / (da - db), &out[m++]);
        }
    }
    return m;
}

static void emit_tri(MbxR *r, const ClipVtx *a, const ClipVtx *b,
                     const ClipVtx *c, int tex, bool depth_only, bool vgp)
{
    /* GL provoking vertex for flat shading: the last one of the triangle
     * (strips / fans / lists keep it last in this emit order), taken before
     * clipping can replace it */
    const float *fc = c->c;
    ClipVtx poly[2][8];
    MbxVtx pv[8];
    int i, k, n = 3, cur = 0;

    poly[0][0] = *a;
    poly[0][1] = *b;
    poly[0][2] = *c;
    if (!vgp) {
        for (i = 0; i < 3; i++) {
            project(r, &poly[0][i], false, &pv[i]);
        }
        store_tri(r, &pv[0], &pv[1], &pv[2], tex, depth_only, false, fc);
        return;
    }
    for (k = 0; k < (r->uclip ? 2 : 1) && n >= 3; k++) {
        bool all_in = true;

        for (i = 0; i < n; i++) {
            all_in &= clip_dist(&poly[cur][i], k) >= 0;
        }
        if (!all_in) {
            n = clip_poly(poly[cur], n, k, poly[cur ^ 1]);
            cur ^= 1;
        }
    }
    if (n < 3) {
        return;                 /* entirely clipped away */
    }
    for (i = 0; i < n; i++) {
        project(r, &poly[cur][i], true, &pv[i]);
    }
    for (i = 2; i < n; i++) {
        store_tri(r, &pv[0], &pv[i - 1], &pv[i], tex, depth_only, true, fc);
    }
}

static float dot3(const float *a, const float *b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void norm3(float *v)
{
    float l = sqrtf(dot3(v, v));

    if (l > 0) {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    }
}

static bool lit_draw(MbxR *r)
{
    return (r->stage[STAGE_ANY] >> 24) == 0x05;
}

/* GL ES 1.1 fixed-function lighting of one vertex (object-space position
 * and normal, vertex colour vc for colour material) into out[4] */
static void light_vertex(MbxR *r, const float *pos, const float *nrm,
                         const float *vc, float *out)
{
    float (*c)[4] = r->cst;
    float ep[3], n[3], mat[4][4];
    unsigned ndir = STAGE_LOOP(r->stage[STAGE_DIR]);
    unsigned npos = STAGE_LOOP(r->stage[STAGE_POS]), l = 0, cnt, k, t;

    for (k = 0; k < 3; k++) {
        ep[k] = c[CST_EYE + k][0] * pos[0] + c[CST_EYE + k][1] * pos[1] +
                c[CST_EYE + k][2] * pos[2] + c[CST_EYE + k][3];
        n[k] = dot3(c[CST_NRM + k], nrm);
    }
    norm3(n);
    for (t = 0; t < 4; t++) {           /* colour material */
        for (k = 0; k < 4; k++) {
            mat[t][k] = c[CST_MAT + t][k] * c[CST_CM_MAT][t] +
                        vc[k] * c[CST_CM_VTX][t];
        }
    }
    for (k = 0; k < 3; k++) {
        out[k] = c[CST_SCENE][k] * (255.f / 256.f);
    }
    out[3] = mat[1][3];                 /* material diffuse alpha */
    for (cnt = 0; cnt < ndir + npos && l < MAX_LIGHTS; cnt++) {
        const float *b = c[CST_LIGHT + LIGHT_STRIDE * l];
        bool positional = cnt >= ndir;
        float L[3], H[3], ndl, ndh, att = 1, spot = 1, sp = 0;

        if ((b[3] != 0) != positional) {
            if (!positional) {
                cnt = ndir - 1;         /* fewer directional lights */
            }
            continue;
        }
        if (!positional) {
            memcpy(L, c[CST_LIGHT + LIGHT_STRIDE * l + 1], sizeof(L));
            memcpy(H, b, sizeof(H));
        } else {
            const float *sd = c[CST_LIGHT + LIGHT_STRIDE * l + 1];
            const float *se = c[CST_LIGHT + LIGHT_STRIDE * l + 5];
            const float *ka = c[CST_LIGHT + LIGHT_STRIDE * l + 6];
            float d;

            for (k = 0; k < 3; k++) {
                L[k] = b[k] - ep[k];
            }
            d = sqrtf(dot3(L, L));
            norm3(L);
            att = ka[0] + ka[1] * d + ka[2] * d * d;
            att = att > 0 ? 1.f / att : 1;
            if (se[2] != 0) {           /* spot */
                float sdl = -dot3(L, sd);

                spot = sdl < se[1] ? 0 : powf(MAX(sdl, 0), se[0]);
            }
            H[0] = L[0];
            H[1] = L[1];
            H[2] = L[2] + 1;            /* infinite viewer */
            norm3(H);
        }
        ndl = dot3(n, L);
        ndh = dot3(n, H);
        if (ndl > 0 && ndh > 0) {
            sp = powf(ndh, c[CST_SHININESS][0]);
        }
        ndl = MAX(ndl, 0);
        for (k = 0; k < 3; k++) {
            out[k] += att * spot *
                      (c[CST_LIGHT + LIGHT_STRIDE * l + 2][k] * mat[0][k] +
                       ndl * c[CST_LIGHT + LIGHT_STRIDE * l + 3][k] *
                       mat[1][k] +
                       sp * c[CST_LIGHT + LIGHT_STRIDE * l + 4][k] *
                       mat[2][k]);
        }
        l++;
    }
    for (k = 0; k < 4; k++) {
        out[k] = out[k] < 0 ? 0 : out[k] > 1 ? 1 : out[k];
    }
}

/* one screen-aligned square of half side h (pixels) around cv: a plain
 * point keeps its own uv; a sprite (spr = its 4 strip vertices) places
 * each vertex by its corner word and takes uv = the corner */
static void emit_point(MbxR *r, const ClipVtx *cv, float h,
                       const ClipVtx *spr, int tex, bool depth_only,
                       bool vgp)
{
    static const uint32_t plain[4] = { 0x00000000u, 0x00000001u,
                                       0x00010000u, 0x00010001u };
    MbxVtx c, q[4];
    int k;

    if (vgp && (cv->p[2] + cv->p[3] < 0 || (r->uclip && cv->ud < 0))) {
        return;                 /* near plane / user clip plane */
    }
    project(r, cv, vgp, &c);
    for (k = 0; k < 4; k++) {
        uint32_t w = spr ? spr[k].corner : plain[k];
        float cx = w & 0xffff, cy = w >> 16;

        q[k] = c;
        q[k].x = c.x + (2 * cx - 1) * h;
        q[k].y = c.y + (2 * cy - 1) * h;
        if (spr) {
            q[k].u = cx * c.iw;
            q[k].v = cy * c.iw;
        }
    }
    /* strip order: (0,1,2), (2,1,3) */
    store_tri(r, &q[0], &q[1], &q[2], tex, depth_only, vgp, cv->c);
    store_tri(r, &q[2], &q[1], &q[3], tex, depth_only, vgp, cv->c);
}

/* one line segment a-b of half width h (pixels): near-clipped in clip
 * space, projected, then GL's wide-line rectangle as two triangles; the
 * flat colour is the second vertex's (GL's provoking vertex for lines) */
static void emit_line(MbxR *r, const ClipVtx *a, const ClipVtx *b, float h,
                      int tex, bool depth_only, bool vgp)
{
    ClipVtx e[2] = { *a, *b };
    MbxVtx p[2], q[4];
    float da, db, dx, dy;
    int k;

    for (k = 0; vgp && k < (r->uclip ? 2 : 1); k++) {
        ClipVtx s0 = e[0], s1 = e[1];

        da = clip_dist(&s0, k);
        db = clip_dist(&s1, k);
        if (da < 0 && db < 0) {
            return;
        }
        if (da < 0) {
            clip_lerp(&s0, &s1, da / (da - db), &e[0]);
        } else if (db < 0) {
            clip_lerp(&s0, &s1, da / (da - db), &e[1]);
        }
    }
    project(r, &e[0], vgp, &p[0]);
    project(r, &e[1], vgp, &p[1]);
    dx = p[1].x - p[0].x;
    dy = p[1].y - p[0].y;
    for (k = 0; k < 4; k++) {
        q[k] = p[k >> 1];
        if (fabsf(dx) >= fabsf(dy)) {
            q[k].y += k & 1 ? h : -h;       /* x-major: vertical extent */
        } else {
            q[k].x += k & 1 ? h : -h;
        }
    }
    store_tri(r, &q[0], &q[1], &q[2], tex, depth_only, vgp, b->c);
    store_tri(r, &q[2], &q[1], &q[3], tex, depth_only, vgp, b->c);
}

/* GL point-parameter size of a sprite, from c99 / c100 */
static float sprite_size(MbxR *r, float d)
{
    const float *a = r->cst[CST_PT_ATT], *ps = r->cst[CST_PT_SIZE];
    float k = a[0] + a[1] * d + a[2] * d * d;
    float sz = ps[2] * (k > 0 ? sqrtf(1.f / k) : 1.f);

    return MIN(MAX(sz, ps[0]), ps[1] > 0 ? ps[1] : sz);
}

/* the texture matrix of unit n on (s, t), when its program stage is more
 * than a copy (descriptor: last - first instruction > 0) */
static void tex_matrix(MbxR *r, int n, float *s, float *t)
{
    uint32_t d = r->stage[STAGE_TEX0 + n];
    const float (*m)[4] = (const float (*)[4])r->cst[CST_TEXMAT + 3 * n];
    float in[2] = { *s, *t }, q;

    if (((d >> 8) & 0xff) <= (d & 0xff)) {
        return;                 /* identity: a plain copy stage */
    }
    q = m[2][0] * in[0] + m[2][1] * in[1] + m[2][3];
    q = q != 0 ? 1.f / q : 1.f;
    *s = (m[0][0] * in[0] + m[0][1] * in[1] + m[0][3]) * q;
    *t = (m[1][0] * in[0] + m[1][1] * in[1] + m[1][3]) * q;
}

/*
 * VGP microcode interpreter (Session 56). The driver's fixed-function
 * programs are pre-encoded fragment templates (.data 0x0874f680..; IDs by
 * stage, table 0x08750738) uploaded per stage; this executes the uploaded
 * program memory instead of high-level emulation. Encoding (decoded from the
 * templates, validated against the HLE outputs of gltest 0..74):
 *   word A: 31:24 / 23:16 / 15:8 = swizzle of src2 / src1 / src0 (2 bits per
 *     component, e4 = xyzw); bits 7:5 negate src2 / src1 / src0 (bit 5 =
 *     src0); 4:0 opcode: 01 MOV, 02 ADD, 03 MAD, 04 MUL, 05 RCP, 06 RSQ,
 *     07 DP3, 08 DP4, 09 MIN, 0a MAX, 0c SGE, 0f LIT, 10 DST, 12 ARL (a0 =
 *     floor), 13 EX2; scalar ops (RCP / RSQ / EX2) read the LAST component of
 *     the swizzle (bits 7:6: e4 -> .w).
 *   word B: 31:26 destination (0 position, 1 colour - BGRA, 2 fog (.w),
 *     3 texcoord 0, 4 texcoord 1, 5 clip distances, 16 + n temp rn), 25:22
 *     write mask (x = bit 22), per source k: file at 17:16 + 2k (0 input,
 *     1 constant, 2 temp), index at 3:0 + 4k; constants: c[base + (idx8 &
 *     0x7f) + (idx8 & 0x80 ? a0 : 0)], idx8 = bits 15:12 << 4 | index.
 *   stage descriptor: 7:0 first, 15:8 last instruction, 23:20 constant base
 *     / 16, 19:16 loop count - 1 (light stages 8 / 9 only).
 * The control word's bits 15:0 select the stages, run in bit order. Inputs:
 * vertex-format slots (floats; 5 = ubyte4 as (b0, b1, b2, b3) / 255, 7 =
 * u16 x2, 8 = s16 x4), default (0, 0, 0, 1).
 */
#define VGP_OUT             6
#define VGP_TEMPS           48

static void vgp_inputs(uint32_t fmt, const uint32_t *p, float in[8][4])
{
    unsigned k, i, off = 0;

    for (k = 0; k < 8; k++) {
        unsigned nib = (fmt >> (4 * k)) & 0xf;

        in[k][0] = in[k][1] = in[k][2] = 0;
        in[k][3] = 1;
        switch (nib) {
        case 0:
            break;
        case 5:
            for (i = 0; i < 4; i++) {
                in[k][i] = ((p[off] >> (8 * i)) & 0xff) / 255.f;
            }
            off++;
            break;
        case ATTR_SHORT2:
            in[k][0] = p[off] & 0xffff;
            in[k][1] = p[off] >> 16;
            off++;
            break;
        case ATTR_SHORT4:
            in[k][0] = (int16_t)p[off];
            in[k][1] = (int16_t)(p[off] >> 16);
            in[k][2] = (int16_t)p[off + 1];
            in[k][3] = (int16_t)(p[off + 1] >> 16);
            off += 2;
            break;
        default:
            for (i = 0; i < nib && i < 4; i++) {
                in[k][i] = f32(p[off + i]);
            }
            off += nib;
            break;
        }
    }
}

static void vgp_run(MbxR *r, uint32_t ctl, const float in[8][4],
                    float out[VGP_OUT][4])
{
    float t[VGP_TEMPS][4];
    int a0 = 0;
    unsigned bit, k, i;

    memset(t, 0, sizeof(t));
    for (k = 0; k < VGP_OUT; k++) {
        out[k][0] = out[k][1] = out[k][2] = 0;
        out[k][3] = 1;
    }
    for (bit = 0; bit < 16; bit++) {
        uint32_t d = r->stage[bit];
        unsigned st = d & 0xff, en = (d >> 8) & 0xff, base, loops, l, pc;

        if (!(ctl & (1u << bit))) {
            continue;
        }
        base = ((d >> 20) & 0xf) * 16;
        loops = bit == STAGE_DIR || bit == STAGE_POS ? ((d >> 16) & 0xf) + 1
                                                       : 1;
        for (l = 0; l < loops; l++) {
            for (pc = st; pc <= en && pc < VGP_PROG_MAX; pc++) {
                uint32_t A = r->prog[pc][0], B = r->prog[pc][1];

                r->st.vgp_ins++;
                unsigned op = A & 0x1f, dst = B >> 26, mask = (B >> 22) & 0xf;
                float s[3][4], res[4], *d4;

                for (k = 0; k < 3; k++) {
                    unsigned f = (B >> (16 + 2 * k)) & 3;
                    unsigned idx = (B >> (4 * k)) & 0xf;
                    unsigned swz = (A >> (8 + 8 * k)) & 0xff;
                    const float *v;
                    static const float zero[4];

                    if (f == 0) {
                        v = in[idx & 7];
                    } else if (f == 1) {
                        unsigned i8 = ((B >> 12) & 0xf) << 4 | idx;

                        v = r->cst[(base + (i8 & 0x7f) +
                                    (i8 & 0x80 ? a0 : 0)) & (NCONST - 1)];
                    } else if (f == 2) {
                        v = t[idx];
                    } else {
                        v = zero;
                    }
                    for (i = 0; i < 4; i++) {
                        s[k][i] = v[(swz >> (2 * i)) & 3];
                        if (A & (1u << (5 + k))) {
                            s[k][i] = -s[k][i];
                        }
                    }
                }
                switch (op) {
                case 0x01:
                    memcpy(res, s[0], sizeof(res));
                    break;
                case 0x02:
                    for (i = 0; i < 4; i++) {
                        res[i] = s[0][i] + s[1][i];
                    }
                    break;
                case 0x03:
                    for (i = 0; i < 4; i++) {
                        res[i] = s[0][i] * s[1][i] + s[2][i];
                    }
                    break;
                case 0x04:
                    for (i = 0; i < 4; i++) {
                        res[i] = s[0][i] * s[1][i];
                    }
                    break;
                case 0x05:
                    res[0] = 1.f / s[0][3];
                    res[1] = res[2] = res[3] = res[0];
                    break;
                case 0x06:
                    res[0] = 1.f / sqrtf(fabsf(s[0][3]));
                    res[1] = res[2] = res[3] = res[0];
                    break;
                case 0x07:
                    res[0] = s[0][0] * s[1][0] + s[0][1] * s[1][1] +
                             s[0][2] * s[1][2];
                    res[1] = res[2] = res[3] = res[0];
                    break;
                case 0x08:
                    res[0] = s[0][0] * s[1][0] + s[0][1] * s[1][1] +
                             s[0][2] * s[1][2] + s[0][3] * s[1][3];
                    res[1] = res[2] = res[3] = res[0];
                    break;
                case 0x09:
                    for (i = 0; i < 4; i++) {
                        res[i] = MIN(s[0][i], s[1][i]);
                    }
                    break;
                case 0x0a:
                    for (i = 0; i < 4; i++) {
                        res[i] = MAX(s[0][i], s[1][i]);
                    }
                    break;
                case 0x0c:
                    for (i = 0; i < 4; i++) {
                        res[i] = s[0][i] >= s[1][i] ? 1.f : 0.f;
                    }
                    break;
                case 0x0f:                      /* LIT */
                    res[0] = res[3] = 1;
                    res[1] = MAX(s[0][0], 0.f);
                    res[2] = s[0][0] > 0 ?
                             (s[0][1] > 0 ? powf(s[0][1], s[0][3]) :
                              s[0][3] == 0 ? 1.f : 0.f) : 0.f;
                    break;
                case 0x10:                      /* DST */
                    res[0] = 1;
                    res[1] = s[0][1] * s[1][1];
                    res[2] = s[0][2];
                    res[3] = s[1][3];
                    break;
                case 0x12:                      /* ARL */
                    a0 = (int)floorf(s[0][0]);
                    continue;
                case 0x13:
                    res[0] = exp2f(s[0][3]);
                    res[1] = res[2] = res[3] = res[0];
                    break;
                default:
                    warn_once(r, 13, "VGP opcode 0x%02x not modeled", op);
                    continue;
                }
                d4 = dst >= 16 ? (dst - 16 < VGP_TEMPS ? t[dst - 16] : NULL) :
                     dst < VGP_OUT ? out[dst] : NULL;
                for (i = 0; d4 && i < 4; i++) {
                    if (mask & (1u << i)) {
                        d4[i] = res[i];
                    }
                }
            }
        }
    }
}

/* the colour output (BGRA, scaled by 256/255 by the programs) as the TA
 * converts it: floor to 8 bits, then this renderer's RGBA floats */
static float vgp_col(float x)
{
    int b = (int)floorf(MAX(x, 0.f) * 255.f + 1e-4f);

    return MIN(b, 255) / 255.f;
}

/* NANO7_MBXR_VPDUMP: one JSON line per transformed draw of frame
 * VPDUMP_FRAME - program, stage descriptors, constants, raw vertices and the
 * HLE outputs - the ground truth for the vertex program interpreter */
static void vp_dump(MbxR *r, uint32_t prim, uint32_t ctl, const uint32_t *v,
                    unsigned stride, unsigned nv, const ClipVtx *vx)
{
    FILE *f = r->vpdump;
    unsigned i, k;

    fprintf(f, "{\"prim\":%u,\"ctl\":%u,\"fmt\":%u,\"isp\":%u,"
            "\"stride\":%u,\"stage\":[", prim, ctl, r->fmt, r->isp, stride);
    for (i = 0; i < 16; i++) {
        fprintf(f, "%s%u", i ? "," : "", r->stage[i]);
    }
    fprintf(f, "],\"prog\":[");
    for (i = 0; i < VGP_PROG_MAX; i++) {
        fprintf(f, "%s[%u,%u]", i ? "," : "", r->prog[i][0], r->prog[i][1]);
    }
    fprintf(f, "],\"cst\":[");
    for (i = 0; i < NCONST; i++) {
        fprintf(f, "%s[%.9g,%.9g,%.9g,%.9g]", i ? "," : "", r->cst[i][0],
                r->cst[i][1], r->cst[i][2], r->cst[i][3]);
    }
    fprintf(f, "],\"vtx\":[");
    for (i = 0; i < nv; i++) {
        fprintf(f, "%s[", i ? "," : "");
        for (k = 0; k < stride; k++) {
            fprintf(f, "%s%u", k ? "," : "", v[i * stride + k]);
        }
        fprintf(f, "]");
    }
    fprintf(f, "],\"out\":[");
    for (i = 0; i < nv; i++) {
        const ClipVtx *o = &vx[i];

        fprintf(f, "%s{\"p\":[%.9g,%.9g,%.9g,%.9g],\"c\":[%.9g,%.9g,%.9g,"
                "%.9g],\"uv\":[%.9g,%.9g],\"uv1\":[%.9g,%.9g],\"f\":%.9g}",
                i ? "," : "", o->p[0], o->p[1], o->p[2], o->p[3], o->c[0],
                o->c[1], o->c[2], o->c[3], o->u, o->v, o->u1, o->v1, o->f);
    }
    fprintf(f, "]}\n");
    fflush(f);
}

static void draw(MbxR *r, uint32_t prim, uint32_t ctl, const uint32_t *v,
                 uint32_t nw)
{
    unsigned sz[8], off[8], stride = 0, i, n, k;
    bool vgp = ctl & DRAW_VGP;

    /* user clip plane (Session 57): control-word stage bit 5 (template ID 39:
     * clip distance oext.z = eye position . c93) and the clip enable bit 20;
     * the only plane the driver's templates support is GL_CLIP_PLANE0 */
    r->uclip = vgp && !r->vgp_hle && (ctl & CTL_UCLIP) == CTL_UCLIP;
    bool has_col, has_uv, has_uv1, depth_only;
    int tex = -1;
    ClipVtx *vx;

    for (i = 0; i < 8; i++) {
        sz[i] = attr_words((r->fmt >> (4 * i)) & 0xf);
        off[i] = stride;
        stride += sz[i];
    }
    if (!sz[0] || !stride) {
        warn_once(r, 1, "draw with vertex format 0x%x", r->fmt);
        return;
    }
    has_col = sz[2] != 0;
    has_uv = sz[3] >= 2;
    depth_only = (!vgp && !has_col) || (r->isp & ISP_NO_COLOR);
    r->keep = 0;
    if (r->isp & ISP_MASKED) {
        unsigned m = LAYER_CMASK(r->layer[0].w[0]);

        r->keep = (m & 1 ? 0x000000ffu : 0) | (m & 2 ? 0x0000ff00u : 0) |
                  (m & 4 ? 0x00ff0000u : 0) | (m & 8 ? 0xff000000u : 0);
    }
    if ((has_uv || sz[SPRITE_SLOT]) && (r->layer[0].w[1] & 0xffffff) &&
        r->ntex < TRI_MAX) {
        r->texs[r->ntex] = r->layer[0];
        tex = r->ntex++;
    }
    /* texture unit 1: control word bit 13, layer 1, texcoords in slot 4 */
    has_uv1 = vgp && (ctl & CTL_TEX1) && sz[UV1_SLOT] >= 2 &&
              (r->layer[1].w[1] & 0xffffff) && r->ntex < TRI_MAX;
    r->tex1 = -1;
    if (has_uv1) {
        r->texs[r->ntex] = r->layer[1];
        r->tex1 = r->ntex++;
    }
    n = nw / stride;
    if (!n) {
        return;
    }
    if (r->stats) {
        uint32_t key[5] = { r->isp, r->b1, r->fmt,
                            tex >= 0 ? r->layer[0].w[0] : 0, ctl }, j;

        for (j = 0; j < r->st_cnt && memcmp(r->st_key[j], key, 20); j++) {
        }
        if (j == r->st_cnt && j < 64) {
            memcpy(r->st_key[j], key, 20);
            r->st_n[j] = 0;
            r->st_cnt++;
        }
        if (j < 64) {
            r->st_n[j] += n;
        }
    }
    vx = g_new(ClipVtx, n);
    for (i = 0; i < n; i++) {
        const uint32_t *p = v + i * stride;
        float x = f32(p[off[0]]), y = sz[0] > 1 ? f32(p[off[0] + 1]) : 0;
        float z = sz[0] > 2 ? f32(p[off[0] + 2]) : 0;
        ClipVtx *o = &vx[i];

        if (vgp) {
            float in[4] = { x, y, z, 1 };
            int j;

            for (j = 0; j < 4; j++) {
                o->p[j] = 0;
                for (k = 0; k < 4; k++) {
                    o->p[j] += r->cst[j][k] * in[k];
                }
            }
        } else {
            o->p[0] = x;
            o->p[1] = y;
            o->p[2] = z;
            o->p[3] = 1;
        }
        if (has_col) {
            uint32_t c = p[off[2]];

            for (k = 0; k < 4; k++) {
                o->c[k] = ((c >> (8 * k)) & 0xff) / 255.f;
            }
        } else if (vgp) {
            /* no colour array: the driver's program outputs constant c15 =
             * glColor (ubyte / 255 scaled by 256 / 255, e.g. 1.0039) */
            for (k = 0; k < 4; k++) {
                float c = r->cst[15][k] * (255.f / 256.f);

                o->c[k] = c < 0 ? 0 : c > 1 ? 1 : c;
            }
        } else {
            for (k = 0; k < 4; k++) {
                o->c[k] = 1;    /* white */
            }
        }
        if (vgp && sz[1] && lit_draw(r)) {
            unsigned nib = (r->fmt >> 4) & 0xf;
            float pos[3] = { x, y, z }, nrm[3], lc[4];
            bool known = true;

            if (nib == 3) {
                for (k = 0; k < 3; k++) {
                    nrm[k] = f32(p[off[1] + k]);
                }
            } else if (nib == ATTR_SHORT4) {
                const float *dc = r->cst[CST_NRM_DECODE];
                int16_t q[3] = { p[off[1]], p[off[1]] >> 16, p[off[1] + 1] };

                for (k = 0; k < 3; k++) {
                    nrm[k] = q[k] * dc[0] + dc[1];
                }
            } else {
                warn_once(r, 10, "lit draw with normal format nibble %u not "
                          "modeled: left unlit", nib);
                known = false;
            }
            if (known) {
                light_vertex(r, pos, nrm, o->c, lc);
                memcpy(o->c, lc, sizeof(lc));
            }
        }
        o->ud = 1;                  /* HLE: no user clip plane */
        o->u = has_uv ? f32(p[off[3]]) : 0;
        o->u1 = has_uv1 ? f32(p[off[UV1_SLOT]]) : 0;
        o->v1 = has_uv1 ? f32(p[off[UV1_SLOT] + 1]) : 0;
        o->f = 1;
        o->corner = sz[SPRITE_SLOT] ? p[off[SPRITE_SLOT]] : 0;
        o->ed = 0;
        if (vgp && sz[SPRITE_SLOT]) {
            float e[3];

            for (k = 0; k < 3; k++) {
                e[k] = r->cst[CST_EYE + k][0] * x + r->cst[CST_EYE + k][1] * y +
                       r->cst[CST_EYE + k][2] * z + r->cst[CST_EYE + k][3];
            }
            o->ed = sqrtf(dot3(e, e));
        }
        if (vgp && (r->isp & ISP_FOG) && r->fog_mode != FOG_NONE) {
            const float *e = r->cst[CST_EYE + 2], *fp = r->cst[CST_FOG];
            float d = fabsf(e[0] * x + e[1] * y + e[2] * z + e[3]), t;

            switch (r->fog_mode) {
            case FOG_LINEAR:
                o->f = (fp[0] - d) * fp[1];
                break;
            case FOG_EXP:
                o->f = exp2f(-fp[2] * d);
                break;
            default:
                t = fp[3] * d;
                o->f = exp2f(-t * t);
                break;
            }
            o->f = o->f < 0 ? 0 : o->f > 1 ? 1 : o->f;
        }
        o->v = has_uv ? f32(p[off[3] + 1]) : 0;
        if (vgp && has_uv) {
            tex_matrix(r, 0, &o->u, &o->v);
        }
        if (vgp && has_uv1) {
            tex_matrix(r, 1, &o->u1, &o->v1);
        }
    }
    if (r->vpdump && vgp && r->frames == VPDUMP_FRAME) {
        vp_dump(r, prim, ctl, v, stride, MIN(n, 8u), vx);
    }
    /* run the uploaded program per vertex - unless a stage it needs has not
     * been uploaded yet (after a snapshot resume the OS loaded the program
     * memory long before T0; the first draws then keep the HLE values) */
    if (!r->vgp_hle && (ctl & 0xffff & ~r->stage_valid) == 0) {
        for (i = 0; i < n; i++) {
            float in[8][4], out[VGP_OUT][4];
            ClipVtx *o = &vx[i];

            vgp_inputs(r->fmt, v + i * stride, in);
            vgp_run(r, ctl, in, out);
            r->st.verts++;
            if (r->vgp_check && !sz[SPRITE_SLOT]) {
                float dc = 0, dp = 0;
                int k2;

                for (k2 = 0; k2 < 4; k2++) {
                    float pv = fabsf(out[0][k2] - o->p[k2]);

                    dp = MAX(dp, pv / MAX(1.f, fabsf(o->p[k2])));
                }
                dc = MAX(MAX(fabsf(vgp_col(out[1][2]) - o->c[0]),
                             fabsf(vgp_col(out[1][1]) - o->c[1])),
                         MAX(fabsf(vgp_col(out[1][0]) - o->c[2]),
                             fabsf(vgp_col(out[1][3]) - o->c[3])));
                if ((dp > 1e-3 || dc > 2.f / 255) && !(r->warned & (1ull << 14))) {
                    r->warned |= 1ull << 14;
                    fprintf(stderr, "[nano7-mbxr] vgpcheck first diff: ctl "
                            "%08x fmt %x vgp %d stage %08x out p %g %g %g %g c "
                            "%g %g %g %g | hle p %g %g %g %g c %g %g %g %g\n",
                            ctl, r->fmt, vgp, r->stage[11], out[0][0],
                            out[0][1], out[0][2], out[0][3], out[1][2],
                            out[1][1], out[1][0], out[1][3], o->p[0], o->p[1],
                            o->p[2], o->p[3], o->c[0], o->c[1], o->c[2],
                            o->c[3]);
                }
                r->chk[0] = MAX(r->chk[0], dp);
                if (has_col || vgp) {
                    r->chk[1] = MAX(r->chk[1], dc);
                }
                if (has_uv) {
                    r->chk[2] = MAX(r->chk[2], MAX(fabsf(out[3][0] - o->u),
                                                   fabsf(out[3][1] - o->v)));
                }
                if (has_uv1) {
                    r->chk[3] = MAX(r->chk[3],
                                    MAX(fabsf(out[4][0] - o->u1),
                                        fabsf(out[4][1] - o->v1)));
                }
                if (r->isp & ISP_FOG) {
                    r->chk[4] = MAX(r->chk[4], fabsf(MIN(MAX(out[2][3], 0.f),
                                                         1.f) - o->f));
                }
                r->chk_n++;
            }
            memcpy(o->p, out[0], sizeof(o->p));
            o->c[0] = vgp_col(out[1][2]);
            o->c[1] = vgp_col(out[1][1]);
            o->c[2] = vgp_col(out[1][0]);
            o->c[3] = vgp_col(out[1][3]);
            o->u = out[3][0];
            o->v = out[3][1];
            o->u1 = out[4][0];
            o->v1 = out[4][1];
            o->f = (r->isp & ISP_FOG) ? MIN(MAX(out[2][3], 0.f), 1.f) : 1.f;
            o->ud = r->uclip ? out[5][2] : 1.f;
        }
    }
    r->cull = 0;                /* points / sprites / lines: no culling */
    if ((prim >> 28) == PRIM_LINES) {
        for (i = 0; i + 1 < n; i++) {
            emit_line(r, &vx[i], &vx[i + 1], r->point_half, tex, depth_only,
                      vgp);
        }
        g_free(vx);
        return;
    }
    if ((prim >> 28) == PRIM_POINTS) {
        for (i = 0; i < n; i++) {
            emit_point(r, &vx[i], r->point_half, NULL, tex, depth_only, vgp);
        }
        g_free(vx);
        return;
    }
    if (sz[SPRITE_SLOT] && r->vgp_hle) {   /* HLE: one point per 4 */
        for (i = 0; i + 3 < n; i += 4) {
            emit_point(r, &vx[i], sprite_size(r, vx[i].ed) / 2, &vx[i], tex,
                       depth_only, vgp);
        }
        g_free(vx);
        return;
    }
    r->cull = PRIM_CULL(prim);
    switch (prim >> 28) {
    case 4:                     /* strip */
        for (i = 2; i < n; i++) {
            if (i & 1) {
                emit_tri(r, &vx[i - 1], &vx[i - 2], &vx[i], tex, depth_only,
                         vgp);
            } else {
                emit_tri(r, &vx[i - 2], &vx[i - 1], &vx[i], tex, depth_only,
                         vgp);
            }
        }
        break;
    case 5:                     /* fan */
        for (i = 2; i < n; i++) {
            emit_tri(r, &vx[0], &vx[i - 1], &vx[i], tex, depth_only, vgp);
        }
        break;
    case 7:                     /* list */
        for (i = 0; i + 2 < n; i += 3) {
            emit_tri(r, &vx[i], &vx[i + 1], &vx[i + 2], tex, depth_only,
                     vgp);
        }
        break;
    default:
        warn_once(r, 2, "primitive word 0x%08x not modeled", prim);
        break;
    }
    g_free(vx);
}

/* primitive word, [points: half size], control word, vertices */
static void draw_cmd(MbxR *r, const uint32_t *s, uint32_t n)
{
    if ((s[0] >> 28) == PRIM_POINTS || (s[0] >> 28) == PRIM_LINES) {
        if (n >= 3) {
            r->point_half = f32(s[1]);
            r->cont_prim = s[0];
            r->cont_ctl = s[2];
            draw(r, s[0], s[2], &s[3], n - 3);
        }
    } else if (n >= 2) {
        r->cont_prim = s[0];
        r->cont_ctl = s[1];
        draw(r, s[0], s[1], &s[2], n - 2);
    }
}

void mbxr_get_stats(const MbxR *r, MbxrStats *st)
{
    *st = r->st;
}

/* parse one doorbell segment; v = the doorbell value (1 commit, 0 commit
 * with the draw left open: the next segment continues it) */
void mbxr_doorbell(MbxR *r, uint32_t v)
{
    const uint32_t *s = r->seg;
    uint32_t n = r->seg_len, i = 0, b;

    r->seg_len = 0;
    if (r->cont) {
        r->cont = v == 0;
        draw(r, r->cont_prim, r->cont_ctl, s, n);
        return;
    }
    r->cont_prim = 0;
    while (i < n) {
        uint32_t w = s[i];

        if ((w >> 28) == 0x8) {                 /* program upload */
            uint32_t j = i + 1, bit;

            for (bit = 0; bit < 16; bit++) {    /* descriptors, bit order */
                if ((w & (1u << bit)) && j < n) {
                    r->stage[bit] = s[j++];
                    r->stage_valid |= 1u << bit;
                }
            }
            i += 1 + ctpop32(w & 0xffff);       /* + descriptors */
            if (i < n && (s[i] >> 28) == 0x9) {
                uint32_t a = (s[i] >> 16) & 0xfff, e = s[i] & 0xfff;
                uint32_t len = 2 * (e >= a ? e - a + 1 : 0);

                if ((w & 0xffff) == 0x4000 && len && i + len < n) {
                    uint32_t last = s[i + len];         /* fog stage */

                    r->fog_mode = last == 0x0a09005b ? FOG_LINEAR :
                                  last == 0x0a020005 ? FOG_EXP :
                                  last == 0x0a020006 ? FOG_EXP2 : FOG_NONE;
                    if (r->fog_mode == FOG_NONE) {
                        warn_once(r, 9, "fog program stage ending 0x%08x "
                                  "not recognised", last);
                    }
                }
                for (j = 0; a + j <= e && a + j < VGP_PROG_MAX &&
                            i + 2 + 2 * j < n; j++) {
                    r->prog[a + j][0] = s[i + 1 + 2 * j];
                    r->prog[a + j][1] = s[i + 2 + 2 * j];
                }
                i += 1 + len;
            }
        } else if ((w >> 24) == 0xa0) {         /* constants */
            uint32_t a = (w >> 16) & 0xff, e = w & 0xff, c;

            i++;
            for (c = a; c <= e && i + 4 <= n; c++, i += 4) {
                r->cst[c][0] = f32(s[i]);
                r->cst[c][1] = f32(s[i + 1]);
                r->cst[c][2] = f32(s[i + 2]);
                r->cst[c][3] = f32(s[i + 3]);
            }
        } else if ((w >> 28) == 0x1) {          /* draw */
            uint32_t m = w & 0xfff;

            i++;
            for (b = 0; b <= 8 && i < n; b++) {
                if (!(m & (1u << b))) {
                    continue;
                }
                switch (b) {
                case 0: r->isp = s[i++]; break;
                case 1: r->b1 = s[i++]; break;
                case 2: case 3:
                    if (i + 3 <= n) {
                        memcpy(r->layer[b - 2].w, &s[i], 12);
                    }
                    i += 3;
                    break;
                case 4: r->region = s[i++]; break;
                case 5: r->fmt = s[i++]; break;
                case 6: r->b6 = s[i++]; break;
                case 7:
                    if (i + 6 <= n) {
                        int k;

                        for (k = 0; k < 6; k++) {
                            r->vp[k] = f32(s[i + k]);
                        }
                    }
                    i += 6;
                    break;
                case 8: r->b8 = s[i++]; break;
                }
            }
            if (m & ~0x1ffu) {
                warn_once(r, 3, "draw dirty mask 0x%03x: unknown blocks", m);
            }
            if (r->dump_isp && r->isp == r->dump_isp) {
                uint32_t k;

                r->dump_isp = 0;                /* once */
                fprintf(stderr, "[nano7-mbxr] segment of isp %08x:", r->isp);
                for (k = 0; k < n; k++) {
                    fprintf(stderr, "%s%08x", k % 8 ? " " : "\n  ", s[k]);
                }
                fputc('\n', stderr);
            }
            draw_cmd(r, &s[i], n - i);
            i = n;                              /* vertices to the end */
        } else if (w == 0xf0000000) {
            i++;
        } else if ((w >> 28) == 4 || (w >> 28) == 5 || (w >> 28) == 7 ||
                   (w >> 28) == PRIM_POINTS || (w >> 28) == PRIM_LINES) {
            /* draw with unchanged state: no 0x1 header, the primitive word
             * comes first (Blocks: 0x70060000) */
            draw_cmd(r, &s[i], n - i);
            i = n;
        } else {
            warn_once(r, 4, "unknown TA command word 0x%08x", w);
            break;
        }
    }
    r->cont = v == 0 && r->cont_prim;   /* a draw left open */
}

/* ------------------------------------------------------------------ */
/* memory through the MBX MMU                                          */

static bool mmu_pa(const uint32_t *core, uint32_t va, uint32_t *pa)
{
    uint32_t root = core[(0x1000 >> 2) + ((va >> 22) & 7)], pte;

    if (!root || (va >> 25)) {
        return false;
    }
    pte = address_space_ldl_le(&address_space_memory,
                               (root & ~0xfffu) + ((va >> 12) & 0x3ff) * 4,
                               MEMTXATTRS_UNSPECIFIED, NULL);
    if (pte < 0x08000000 || pte >= 0x0c000000) {
        return false;
    }
    *pa = (pte & ~0xfffu) | (va & 0xfff);
    return true;
}

/* copy len bytes from / to GPU virtual memory, page by page */
static bool gpu_rw(const uint32_t *core, uint32_t va, void *buf, uint32_t len,
                   bool write)
{
    uint8_t *p = buf;

    while (len) {
        uint32_t pa, n = MIN(len, 0x1000 - (va & 0xfff));

        if (!mmu_pa(core, va, &pa)) {
            return false;
        }
        if (write) {
            address_space_write(&address_space_memory, pa,
                                MEMTXATTRS_UNSPECIFIED, p, n);
        } else {
            address_space_read(&address_space_memory, pa,
                               MEMTXATTRS_UNSPECIFIED, p, n);
        }
        va += n;
        p += n;
        len -= n;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* textures                                                            */

#define MIP_MAX         12

/*
 * Texture layer word 1 bits 31:26 = filter (gltest variants 0, 27-33; one
 * bit serves both GL_TEXTURE_MIN_FILTER and MAG_FILTER):
 *   NEAREST 0x12......, LINEAR 0x92, NEAREST_MIPMAP_NEAREST 0x32,
 *   LINEAR_MIPMAP_NEAREST 0xb2, NEAREST_MIPMAP_LINEAR 0x4e,
 *   LINEAR_MIPMAP_LINEAR 0xce
 * bit 31 bilinear within a level, bit 29 mipmapped (nearest level), bit 30
 * mipmapped (blend of the two nearest levels; bits 27:26 come with it).
 * Mip levels follow level 0 contiguously, largest first, each twiddled on
 * its own, down to 1x1 (64x64 ARGB8888: +0, +16384, +20480, +21504, +21760,
 * +21824, +21840; no padding seen).
 */
#define TEX_BILINEAR    (1u << 31)
#define TEX_MIP_LINEAR  (1u << 30)
#define TEX_MIP_NEAREST (1u << 29)

typedef struct HostTex {
    uint32_t key[3];
    unsigned w, h;              /* level 0 */
    uint32_t *texel;            /* linear ARGB8888, all levels */
    bool linear;                /* bilinear filter (word 1 bit 31) */
    bool clamp_s, clamp_t;      /* GL_CLAMP_TO_EDGE (word 0 bits 31 / 29) */
    bool mip, mip_linear;
    unsigned nlev, lw[MIP_MAX], lh[MIP_MAX], off[MIP_MAX];
} HostTex;

/* size (log2) and format of a texture layer, from layer word 0 (gltest
 * variants: 64x64 ARGB8888 0x03319600, 32x16 ARGB8888 0x02119600, 64x64
 * RGB565 0x03305600): bits 27:24 log2(width) - 3, 23:20 log2(height) - 3,
 * 16:12 format - 0x19 ARGB8888, 0x05 RGB565, 0x09 ARGB4444, 0x01 ARGB1555,
 * 0x11 AL88 (luminance-alpha), 0x15 8-bit (luminance or alpha) */
/* wrap (Session 50, gltest 68): layer word 0 bit 31 = GL_CLAMP_TO_EDGE on S,
 * bit 29 on T, clear = GL_REPEAT. GL_MIRRORED_REPEAT_OES is rejected by the
 * driver (state unchanged), so ES 1.1 here has only these two; bits 30 / 28
 * (perhaps hardware mirror) are never set. */
#define TEX_CLAMP_S         (1u << 31)
#define TEX_CLAMP_T         (1u << 29)

/* texture format of layer word 0: bits 16:12 without bit 13, which is the
 * DOT3 flag (Session 49: GL_DOT3_RGB made 0x19 read as 0x1b) */
#define TEX_FMT(w0)         (((w0) >> 12) & 0x1d)

static bool mbxr_tex_params(MbxR *r, const MbxTex *t, unsigned *lw,
                            unsigned *lh, unsigned *bpp)
{
    unsigned fmt = TEX_FMT(t->w[0]);

    *lw = ((t->w[0] >> 24) & 0xf) + 3;
    *lh = ((t->w[0] >> 20) & 0xf) + 3;
    switch (fmt) {
    case 0x19:
        *bpp = 32;
        break;
    case 0x05: case 0x09: case 0x01: case 0x11:
        *bpp = 16;
        break;
    case 0x15:
        *bpp = 8;
        break;
    default:
        warn_once(r, 8, "texture format 0x%02x not modeled", fmt);
        return false;
    }
    return *lw <= 11 && *lh <= 11;
}

static inline uint32_t x4(uint32_t v)
{
    return v << 4 | v;
}

static inline uint32_t x5(uint32_t v)
{
    return v << 3 | v >> 2;
}

/* one texel of a 16 / 8 bpp format -> ARGB8888 */
static uint32_t texel16(unsigned fmt, uint32_t p)
{
    uint32_t a, l;

    switch (fmt) {
    case 0x09:                  /* ARGB4444 */
        return x4(p >> 12) << 24 | x4((p >> 8) & 0xf) << 16 |
               x4((p >> 4) & 0xf) << 8 | x4(p & 0xf);
    case 0x01:                  /* ARGB1555 */
        return (p & 0x8000 ? 0xff000000u : 0) | x5((p >> 10) & 0x1f) << 16 |
               x5((p >> 5) & 0x1f) << 8 | x5(p & 0x1f);
    case 0x11:                  /* AL88: alpha high, luminance low */
        a = p >> 8;
        l = p & 0xff;
        return a << 24 | l << 16 | l << 8 | l;
    default:                    /* RGB565 */
        return 0xff000000u | x5((p >> 11) & 0x1f) << 16 |
               (((p >> 5) & 0x3f) << 2 | ((p >> 5) & 0x3f) >> 4) << 8 |
               x5(p & 0x1f);
    }
}

static unsigned twiddle(unsigned x, unsigned y, unsigned lw, unsigned lh)
{
    unsigned i = 0, b, o = 0, m = MIN(lw, lh);

    for (b = 0; b < m; b++) {
        i |= ((y >> b) & 1) << o++;
        i |= ((x >> b) & 1) << o++;
    }
    /* non-square: the remaining bits of the longer side go on top */
    if (lw > m) {
        i |= (x >> m) << o;
    } else if (lh > m) {
        i |= (y >> m) << o;
    }
    return i;
}

/*
 * PVRTC (Session 62, gltest 78-81): layer word 0 bit 17 set; bit 15 set =
 * 4 bpp (4x4-texel blocks; the 5-bit field then reads 0x19), clear = 2 bpp
 * (8x4 blocks, field 0x01). GL's RGB and RGBA variants give the same word
 * (RGB only changes the combiner: alpha from the vertex). The driver
 * (glCompressedTexImage2D 0x08483794) copies the data verbatim; the
 * hardware reads it as standard PVRTC v1: 64-bit words, modulation data
 * then colour data, in twiddled word order, at least 2 x 2 words per level.
 * Decoded as Imagination's reference decoder does (colours bilinearly
 * upscaled from the word centres with wrap-around, integer weights), so it
 * matches an independent decoder bit for bit. Mip levels (unverified: the
 * driver uploads one level per call) are assumed to follow each other.
 * Paletted textures (GL_PALETTE4 / 8_*) need nothing here: the driver
 * expands them on the CPU to 8888 / 565 / 4444 / 5551.
 */
#define TEX_PVRTC       (1u << 17)
#define TEX_PVRTC_4BPP  (1u << 15)

typedef struct PvCol {
    int r, g, b, a;             /* 5-bit colour, 4-bit alpha */
} PvCol;

/* colour A = bits 15:1 of the colour word (bit 0 = modulation mode),
 * colour B = bits 31:16; bit 15 of each half = opaque */
static PvCol pvrtc_col(uint32_t c, bool b)
{
    PvCol p;

    if (b) {
        uint32_t h = c >> 16;

        if (h & 0x8000) {       /* RGB 555 */
            p = (PvCol){ (h >> 10) & 31, (h >> 5) & 31, h & 31, 15 };
        } else {                /* ARGB 3444 */
            p = (PvCol){ ((h & 0xf00) >> 7) | ((h & 0xf00) >> 11),
                         ((h & 0xf0) >> 3) | ((h & 0xf0) >> 7),
                         ((h & 0xf) << 1) | ((h & 0xf) >> 3),
                         (h & 0x7000) >> 11 };
        }
    } else if (c & 0x8000) {    /* RGB 554 */
        p = (PvCol){ (c >> 10) & 31, (c >> 5) & 31,
                     (c & 0x1e) | ((c & 0x1e) >> 4), 15 };
    } else {                    /* ARGB 3443 */
        p = (PvCol){ ((c & 0xf00) >> 7) | ((c & 0xf00) >> 11),
                     ((c & 0xf0) >> 3) | ((c & 0xf0) >> 7),
                     ((c & 0xe) << 1) | ((c & 0xe) >> 2),
                     (c & 0x7000) >> 11 };
    }
    return p;
}

/* bytes of one PVRTC level */
static unsigned pvrtc_size(unsigned lw, unsigned lh, bool bpp4)
{
    unsigned bw = MAX(2u, (1u << lw) / (bpp4 ? 4 : 8));
    unsigned bh = MAX(2u, (1u << lh) / 4);

    return bw * bh * 8;
}

/* one level: raw words -> w x h ARGB8888 (out, row-major) */
static void pvrtc_decode(const uint8_t *raw, unsigned lw, unsigned lh,
                         bool bpp4, uint32_t *out)
{
    static const int rep[4] = { 0, 3, 5, 8 };
    const unsigned ww = bpp4 ? 4 : 8;
    const unsigned w = 1u << lw, h = 1u << lh;
    const unsigned bw = MAX(2u, w / ww), bh = MAX(2u, h / 4);
    const unsigned pw = bw * ww, ph = bh * 4;   /* padded size */
    unsigned lbw = ctz32(bw), lbh = ctz32(bh);
    g_autofree PvCol *ca = g_new(PvCol, bw * bh);
    g_autofree PvCol *cb = g_new(PvCol, bw * bh);
    g_autofree uint8_t *val = g_new(uint8_t, pw * ph);  /* raw 0..3 */
    g_autofree uint8_t *mode = g_new(uint8_t, pw * ph);
    unsigned bx, by, x, y;

    for (by = 0; by < bh; by++) {
        for (bx = 0; bx < bw; bx++) {
            const uint8_t *wd = raw + twiddle(bx, by, lbw, lbh) * 8;
            uint32_t m = ldl_le_p(wd), c = ldl_le_p(wd + 4);
            unsigned k = by * bw + bx, md = c & 1;

            ca[k] = pvrtc_col(c, false);
            cb[k] = pvrtc_col(c, true);
            if (bpp4) {
                /* weights 0 3 5 8; mode 1: 0 4 4+punch-through 8 */
                for (y = 0; y < 4; y++) {
                    for (x = 0; x < 4; x++, m >>= 2) {
                        unsigned i = (by * 4 + y) * pw + bx * 4 + x;

                        val[i] = m & 3;
                        mode[i] = md;
                    }
                }
            } else if (md) {
                /* half the texels stored (checkerboard), the others
                 * interpolated: 1 = H and V, 2 = H only, 3 = V only */
                unsigned im = 1;

                if (m & 1) {
                    im = m & (1u << 20) ? 3 : 2;
                    m = m & (1u << 21) ? m | 1u << 20 : m & ~(1u << 20);
                }
                m = m & 2 ? m | 1 : m & ~1u;
                for (y = 0; y < 4; y++) {
                    for (x = 0; x < 8; x++) {
                        unsigned i = (by * 4 + y) * pw + bx * 8 + x;

                        mode[i] = im;
                        if (!((x ^ y) & 1)) {
                            val[i] = m & 3;
                            m >>= 2;
                        }
                    }
                }
            } else {            /* 1 bit per texel: 0 or 8 */
                for (y = 0; y < 4; y++) {
                    for (x = 0; x < 8; x++, m >>= 1) {
                        unsigned i = (by * 4 + y) * pw + bx * 8 + x;

                        val[i] = m & 1 ? 3 : 0;
                        mode[i] = 0;
                    }
                }
            }
        }
    }
    for (y = 0; y < MIN(h, ph); y++) {
        for (x = 0; x < MIN(w, pw); x++) {
            /* colours: bilinear between the 4 word centres around */
            int fx = (int)x - (int)ww / 2, fy = (int)y - 2;
            int bx0 = (fx + (int)pw) / (int)ww % (int)bw;
            int by0 = (fy + (int)ph) / 4 % (int)bh;
            int dx = (fx + (int)pw) % (int)ww, dy = (fy + (int)ph) % 4;
            int bx1 = (bx0 + 1) % bw, by1 = (by0 + 1) % bh;
            const PvCol *col[2][4] = {
                { &ca[by0 * bw + bx0], &ca[by0 * bw + bx1],
                  &ca[by1 * bw + bx0], &ca[by1 * bw + bx1] },
                { &cb[by0 * bw + bx0], &cb[by0 * bw + bx1],
                  &cb[by1 * bw + bx0], &cb[by1 * bw + bx1] },
            };
            int up[2][4], k, j, wt, i = y * pw + x;
            bool punch = false;

            for (k = 0; k < 2; k++) {
                for (j = 0; j < 4; j++) {
                    const int *pp = &col[k][0]->r, *pq = &col[k][1]->r;
                    const int *pr = &col[k][2]->r, *ps = &col[k][3]->r;
                    int hp = pp[j] * (int)ww + dx * (pq[j] - pp[j]);
                    int hr = pr[j] * (int)ww + dx * (ps[j] - pr[j]);
                    int v = 4 * hp + dy * (hr - hp);

                    if (j < 3) {
                        up[k][j] = bpp4 ? (v >> 6) + (v >> 1)
                                        : (v >> 7) + (v >> 2);
                    } else {
                        up[k][j] = bpp4 ? (v >> 4) + v : (v >> 5) + (v >> 1);
                    }
                }
            }
            /* modulation weight 0..8 */
            if (bpp4) {
                static const int m1[4] = { 0, 4, 4, 8 };

                wt = mode[i] ? m1[val[i]] : rep[val[i]];
                punch = mode[i] && val[i] == 2;
            } else if (!mode[i] || !((x ^ y) & 1)) {
                wt = rep[val[i]];
            } else {
                int l = rep[val[y * pw + (x + pw - 1) % pw]];
                int r2 = rep[val[y * pw + (x + 1) % pw]];
                int u = rep[val[(y + ph - 1) % ph * pw + x]];
                int d = rep[val[(y + 1) % ph * pw + x]];

                wt = mode[i] == 1 ? (l + r2 + u + d + 2) / 4 :
                     mode[i] == 2 ? (l + r2 + 1) / 2 : (u + d + 1) / 2;
            }
            {
                uint32_t c[4];

                for (j = 0; j < 4; j++) {
                    c[j] = (up[0][j] * (8 - wt) + up[1][j] * wt) / 8;
                }
                if (punch) {
                    c[3] = 0;
                }
                out[y * w + x] = c[3] << 24 | c[0] << 16 | c[1] << 8 | c[2];
            }
        }
    }
}

static bool load_tex_pvrtc(MbxR *r, const uint32_t *core, const MbxTex *t,
                           HostTex *h)
{
    bool bpp4 = t->w[0] & TEX_PVRTC_4BPP;
    unsigned lw = ((t->w[0] >> 24) & 0xf) + 3, lh = ((t->w[0] >> 20) & 0xf) + 3;
    unsigned l, size = 0, ntex = 0, roff[MIP_MAX];
    uint32_t va = (t->w[1] & 0xffffff) << 7;
    g_autofree uint8_t *raw = NULL;

    if (lw > 11 || lh > 11) {
        return false;
    }
    h->w = 1u << lw;
    h->h = 1u << lh;
    h->mip = t->w[1] & (TEX_MIP_NEAREST | TEX_MIP_LINEAR);
    h->mip_linear = t->w[1] & TEX_MIP_LINEAR;
    h->nlev = h->mip ? MAX(lw, lh) + 1 : 1;
    for (l = 0; l < h->nlev; l++) {
        h->lw[l] = lw > l ? lw - l : 0;
        h->lh[l] = lh > l ? lh - l : 0;
        h->off[l] = ntex;
        ntex += 1u << (h->lw[l] + h->lh[l]);
        roff[l] = size;
        size += pvrtc_size(h->lw[l], h->lh[l], bpp4);
    }
    raw = g_malloc(size);
    if (!gpu_rw(core, va, raw, size, false)) {
        return false;
    }
    h->texel = g_new(uint32_t, ntex);
    for (l = 0; l < h->nlev; l++) {
        pvrtc_decode(raw + roff[l], h->lw[l], h->lh[l], bpp4,
                     h->texel + h->off[l]);
    }
    if (r->stats && r->frames == r->dbg_frame) {
        fprintf(stderr, "[nano7-mbxr] tex %08x %08x %08x PVRTC %ubpp "
                "%ux%u, %u levels\n", t->w[0], t->w[1], t->w[2],
                bpp4 ? 4 : 2, h->w, h->h, h->nlev);
    }
    memcpy(h->key, t->w, 12);
    h->linear = t->w[1] & TEX_BILINEAR;
    h->clamp_s = t->w[0] & TEX_CLAMP_S;
    h->clamp_t = t->w[0] & TEX_CLAMP_T;
    return true;
}

static bool load_tex(MbxR *r, const uint32_t *core, const MbxTex *t,
                     HostTex *h)
{
    unsigned lw, lh, bpp, x, y, l, ntex = 0;
    uint32_t va = (t->w[1] & 0xffffff) << 7, size = 0;
    g_autofree uint8_t *raw = NULL;

    if (t->w[0] & TEX_PVRTC) {
        return load_tex_pvrtc(r, core, t, h);
    }
    if (!mbxr_tex_params(r, t, &lw, &lh, &bpp)) {
        return false;
    }
    h->w = 1u << lw;
    h->h = 1u << lh;
    h->mip = t->w[1] & (TEX_MIP_NEAREST | TEX_MIP_LINEAR);
    h->mip_linear = t->w[1] & TEX_MIP_LINEAR;
    h->nlev = h->mip ? MAX(lw, lh) + 1 : 1;
    for (l = 0; l < h->nlev; l++) {
        h->lw[l] = lw > l ? lw - l : 0;
        h->lh[l] = lh > l ? lh - l : 0;
        h->off[l] = ntex;
        ntex += 1u << (h->lw[l] + h->lh[l]);
    }
    size = ntex * bpp / 8;
    raw = g_malloc(size);
    if (!gpu_rw(core, va, raw, size, false)) {
        return false;
    }
    if (r->stats && r->frames == r->dbg_frame) {
        /* raw texels at (0,0) (1,1) (9,1): gltest's border, dark, light */
        static const unsigned pt[3][2] = { { 0, 0 }, { 1, 1 }, { 9, 1 } };
        unsigned k, j;

        fprintf(stderr, "[nano7-mbxr] tex %08x %08x %08x raw:", t->w[0],
                t->w[1], t->w[2]);
        for (k = 0; k < 3; k++) {
            unsigned i = twiddle(pt[k][0], pt[k][1], lw, lh) * bpp / 8;

            fprintf(stderr, " (%u,%u)=", pt[k][0], pt[k][1]);
            for (j = 0; j < MAX(bpp / 8, 1); j++) {
                fprintf(stderr, "%02x", raw[i + j]);
            }
        }
        fputc('\n', stderr);
        if (getenv("NANO7_MBXR_TEXSCAN")) {   /* runs of equal words */
            uint32_t *m = g_new(uint32_t, 0x4000), prev = 0, start = 0;

            if (gpu_rw(core, va - 0x8000, m, 0x10000, false)) {
                for (k = 0; k <= 0x4000; k++) {
                    if (k == 0x4000 || (k && m[k] != prev)) {
                        if (k - start >= 1) {
                            fprintf(stderr, "[nano7-mbxr]  %+6d: %08x x %u\n",
                                    (int)(start * 4) - 0x8000, prev,
                                    k - start);
                        }
                        start = k;
                    }
                    if (k < 0x4000) {
                        prev = m[k];
                    }
                }
            }
            g_free(m);
        }
    }
    h->texel = g_new(uint32_t, ntex);
    for (l = 0; l < h->nlev; l++) {
        unsigned w = 1u << h->lw[l], hh = 1u << h->lh[l];

        for (y = 0; y < hh; y++) {
            for (x = 0; x < w; x++) {
                unsigned i = h->off[l] + twiddle(x, y, h->lw[l], h->lh[l]);
                uint32_t c;

                if (bpp == 32) {
                    c = ldl_le_p(raw + i * 4);
                } else if (bpp == 16) {
                    c = texel16(TEX_FMT(t->w[0]),
                                lduw_le_p(raw + i * 2));
                } else {
                    c = raw[i] * 0x01010101u; /* 8-bit: every channel */
                }
                h->texel[h->off[l] + y * w + x] = c;
            }
        }
    }
    memcpy(h->key, t->w, 12);
    h->linear = t->w[1] & TEX_BILINEAR;
    h->clamp_s = t->w[0] & TEX_CLAMP_S;
    h->clamp_t = t->w[0] & TEX_CLAMP_T;
    return true;
}

/* ------------------------------------------------------------------ */
/* rasterizer                                                          */

static inline float edge(float ax, float ay, float bx, float by, float px,
                         float py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

static float blend_factor(unsigned f, bool src, const float *s,
                          const float *d, int k)
{
    const float *o = src ? d : s;       /* "other" colour */

    switch (f) {
    case 0: return 0;
    case 1: return 1;
    case 2: return o[k];
    case 3: return 1 - o[k];
    case 4: return s[3];
    case 5: return 1 - s[3];
    case 6: return d[3];
    default: return 1 - d[3];
    }
}

static bool depth_pass(unsigned f, float z, float zb)
{
    switch (f) {
    case 0: return false;
    case 1: return z < zb;
    case 2: return z == zb;
    case 3: return z <= zb;
    case 4: return z > zb;
    case 5: return z != zb;
    case 6: return z >= zb;
    default: return true;
    }
}

/*
 * Texture combine (Sessions 48 / 49), from layer word 2: two combiners,
 * colour in bits 31:16 (+ bit 15), alpha in bits 14:0. Decoded from ~80
 * driver codes (gltest 0, 8, 17, 18, 61-67: GL_MODULATE / REPLACE / ADD /
 * DECAL / BLEND and GL_COMBINE functions, sources, operands, scales); each
 * computes  X1 * Y1 + X2 * Y2  (all four after optional complement):
 *   colour half h (bits 15:0 of w2 >> 16):
 *     X1 15:14 (0 K, 1 C, 2 T, 3 one), Y1 12:11 (0 P, 1 C, 2 T, 3 K) with
 *     alpha-replicate bit 13, X2 10:9 (0 P, 1 C, 2 T, 3 one), Y2 7:6 (as
 *     Y1) with alpha-replicate bit 8, complement X1 bit 5 / Y1 4 / Y2 2,
 *     bit 3 drops product 2, bit 1 subtracts 0.5 (GL_ADD_SIGNED);
 *   alpha half a (bits 14:0): X1 14:13 (0 P, 1 C, 2 T, 3 one), Y1 12:11,
 *     X2 10:9, Y2 8:7 (Y: 0 P, 1 C, 2 T, 3 K), complement X1 bit 6 / Y1 5 /
 *     Y2 3, bit 4 drops product 2, bit 2 subtracts 0.5;
 *   result scale (alpha bits 1:0; colour: h bit 0 << 1 | w2 bit 15):
 *     0 x1, 1 x2, 2 x4, 3 = 1 - result (how the driver builds GL_SUBTRACT:
 *     1 - ((1 - a) + b)).
 * T = this unit's texel, P = the primary (vertex) colour, C = the previous
 * stage (P for unit 0), K = the texture env colour. Examples: MODULATE
 * 0x86084610 = T * P, GL_BLEND 0x84e0 = (1 - T) * P + T * K, DECAL (unit 1)
 * 0xb384 = T * T.a + C * (1 - T.a). DOT3 (layer word 0 bit 13): 4 * sum((X1
 * - .5) * (Y1 - .5)) into rgb, and alpha too when the colour Y2 field is 1
 * (GL_DOT3_RGBA). K: glTexEnvfv(GL_TEXTURE_ENV_COLOR) is stored in the
 * context (texture unit +0x10..+0x1c, packed at +0x0c) but nothing of it
 * reaches the FIFO, the MBX registers or GPU memory (searched in the trace
 * and a RAM snapshot) - like the fog colour, the hardware keeps its own;
 * modeled as GL's default (0, 0, 0, 0), unverified.
 */
#define LAYER0_DOT3         (1u << 13)
static const float TEX_ENV_K[4] = { 0, 0, 0, 0 };

/* xsrc: 0 = a Y source (0 P, 1 C, 2 T, 3 K), 1 = X1, 2 = X2 (0 P - or K
 * for the colour X1 -, 1 C, 2 T, 3 one) */
static float tc_src(unsigned code, int xsrc, bool alpha_half, int k,
                    const float *T, const float *P, const float *C)
{
    switch (code) {
    case 0:
        return xsrc == 1 && !alpha_half ? TEX_ENV_K[k] : P[k];
    case 1:
        return C[k];
    case 2:
        return T[k];
    default:
        return xsrc ? 1.f : TEX_ENV_K[k];
    }
}

static float tc_eval(unsigned x1, bool cx1, unsigned y1, bool ay1, bool cy1,
                     unsigned x2, unsigned y2, bool ay2, bool cy2, bool drop2,
                     bool bias, unsigned scale, bool alpha_half, int k,
                     const float *T, const float *P, const float *C)
{
    float a = tc_src(x1, 1, alpha_half, k, T, P, C);
    float b = tc_src(y1, 0, alpha_half, ay1 ? 3 : k, T, P, C);
    float r;

    a = cx1 ? 1 - a : a;
    b = cy1 ? 1 - b : b;
    r = a * b;
    if (!drop2) {
        float c = tc_src(x2, 2, alpha_half, k, T, P, C);
        float d = tc_src(y2, 0, alpha_half, ay2 ? 3 : k, T, P, C);

        r += c * (cy2 ? 1 - d : d);
    }
    if (bias) {
        r -= .5f;
    }
    switch (scale) {
    case 1: r *= 2; break;
    case 2: r *= 4; break;
    case 3: r = 1 - r; break;
    }
    return r < 0 ? 0 : r > 1 ? 1 : r;
}

/* one texture stage: col (the previous stage, C) <- combine of texel s with
 * the primary colour prim */
static void combine(const HostTex *tx, uint32_t s, const float *prim,
                    float *col)
{
    uint32_t w2 = tx->key[2], h = w2 >> 16, a = w2 & 0x7fff;
    unsigned cscale = (h & 1) << 1 | ((w2 >> 15) & 1);
    float T[4] = { ((s >> 16) & 0xff) / 255.f, ((s >> 8) & 0xff) / 255.f,
                   (s & 0xff) / 255.f, (s >> 24) / 255.f };
    float C[4], out[4];
    int k;

    memcpy(C, col, sizeof(C));
    if (tx->key[0] & LAYER0_DOT3) {
        float d = 0;

        for (k = 0; k < 3; k++) {
            float x = tc_src((h >> 14) & 3, 1, false, k, T, prim, C);
            float y = tc_src((h >> 11) & 3, 0, false, k, T, prim, C);

            d += (x - .5f) * (y - .5f);
        }
        d = 4 * d;
        d = d < 0 ? 0 : d > 1 ? 1 : d;
        out[0] = out[1] = out[2] = d;
        out[3] = ((h >> 6) & 3) == 1 ? d :
                 tc_eval((a >> 13) & 3, a & 0x40, (a >> 11) & 3, true,
                         a & 0x20, (a >> 9) & 3, (a >> 7) & 3, true,
                         a & 0x08, a & 0x10, a & 0x04, a & 3, true, 3,
                         T, prim, C);
    } else {
        for (k = 0; k < 3; k++) {
            out[k] = tc_eval((h >> 14) & 3, h & 0x20, (h >> 11) & 3,
                             h & 0x2000, h & 0x10, (h >> 9) & 3,
                             (h >> 6) & 3, h & 0x100, h & 0x04, h & 0x08,
                             h & 0x02, cscale, false, k, T, prim, C);
        }
        out[3] = tc_eval((a >> 13) & 3, a & 0x40, (a >> 11) & 3, true,
                         a & 0x20, (a >> 9) & 3, (a >> 7) & 3, true,
                         a & 0x08, a & 0x10, a & 0x04, a & 3, true, 3,
                         T, prim, C);
    }
    memcpy(col, out, sizeof(out));
}

/* texel of level l at texture coordinates (u, v), repeat wrap: nearest, or
 * the 2x2 bilinear blend around the sample point (texel centres at +0.5) */
#define TEX_TIE_BIAS        1e-4f

/* texel index along one axis: repeat (wrap) or clamp to the edge texel */
static inline int tex_wrap(int i, int n, bool clamp)
{
    return clamp ? (i < 0 ? 0 : i >= n ? n - 1 : i) : (i & (n - 1));
}

static uint32_t sample_level(const HostTex *tx, unsigned l, float u, float v)
{
    int w = 1 << tx->lw[l], h = 1 << tx->lh[l];
    const uint32_t *t = tx->texel + tx->off[l];
    bool cs = tx->clamp_s, ct = tx->clamp_t;
    int x0, y0, x1, y1, k;
    float fx, fy, acc[4] = { 0 };
    uint32_t q[4], out = 0;

    if (!tx->linear) {
        /* exact texel-boundary ties go to the upper texel, consistently
         * (TEX_TIE_BIAS > the float spacing of u * w, ~4e-6 at 64) */
        return t[tex_wrap((int)floorf(v * h + TEX_TIE_BIAS), h, ct) * w +
                 tex_wrap((int)floorf(u * w + TEX_TIE_BIAS), w, cs)];
    }
    fx = u * w - 0.5f;
    fy = v * h - 0.5f;
    x0 = (int)floorf(fx);
    y0 = (int)floorf(fy);
    fx -= x0;
    fy -= y0;
    x1 = tex_wrap(x0 + 1, w, cs);
    y1 = tex_wrap(y0 + 1, h, ct);
    x0 = tex_wrap(x0, w, cs);
    y0 = tex_wrap(y0, h, ct);
    q[0] = t[y0 * w + x0];
    q[1] = t[y0 * w + x1];
    q[2] = t[y1 * w + x0];
    q[3] = t[y1 * w + x1];
    for (k = 0; k < 4; k++) {
        float wq = (k & 1 ? fx : 1 - fx) * (k & 2 ? fy : 1 - fy);
        int c;

        for (c = 0; c < 4; c++) {
            acc[c] += wq * ((q[k] >> (8 * c)) & 0xff);
        }
    }
    for (k = 0; k < 4; k++) {
        out |= (uint32_t)MIN(255, (int)(acc[k] + .5f)) << (8 * k);
    }
    return out;
}

/* texel at (u, v) with level of detail lambda = log2(texels per pixel of
 * level 0), GL rules: lambda <= 0 magnifies from level 0; mipmapped
 * minification picks the nearest level, or blends the two around lambda */
static uint32_t sample(const HostTex *tx, float u, float v, float lambda)
{
    unsigned top = tx->nlev - 1, l0, l1, k;
    uint32_t a, b, out = 0;
    float f;

    if (!tx->mip || lambda <= 0) {
        return sample_level(tx, 0, u, v);
    }
    if (!tx->mip_linear) {
        l0 = lambda <= 0.5f ? 0 : (unsigned)ceilf(lambda + 0.5f) - 1;
        return sample_level(tx, MIN(l0, top), u, v);
    }
    l0 = MIN((unsigned)lambda, top);
    l1 = MIN(l0 + 1, top);
    f = l0 == top ? 0 : lambda - (unsigned)lambda;
    a = sample_level(tx, l0, u, v);
    if (f == 0) {
        return a;
    }
    b = sample_level(tx, l1, u, v);
    for (k = 0; k < 4; k++) {
        float ca = (a >> (8 * k)) & 0xff, cb = (b >> (8 * k)) & 0xff;

        out |= (uint32_t)(ca + (cb - ca) * f + .5f) << (8 * k);
    }
    return out;
}

/* perspective-correct (u, v) of triangle t at pixel position (px, py) */
static inline double edge_d(double ax, double ay, double bx, double by,
                            double px, double py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/*
 * Perspective-correct (u, v) of unit `unit` at pixel (px, py), in double
 * precision (Session 55): in float the barycentric rounding differs from
 * row to row, so where u * width lands on a texel boundary nearest sampling
 * flipped between the two texels along a straight edge (serrated checker
 * edges, ~3 % of the pixels of an axis-aligned quad). The hardware's fixed-
 * point coordinates are consistent along an edge; double + the tie bias in
 * sample_level() reproduce that. ia (float) is unused here.
 */
static void uv_at(const MbxTri *t, int unit, float ia, float px, float py,
                  float *u, float *v)
{
    const MbxVtx *a = &t->v[0], *b = &t->v[1], *c = &t->v[2];
    double area = edge_d(a->x, a->y, b->x, b->y, c->x, c->y);
    double w0, w1, w2, iw, inv;

    (void)ia;
    if (area == 0) {
        *u = *v = 0;
        return;
    }
    inv = 1.0 / area;
    w0 = edge_d(b->x, b->y, c->x, c->y, px, py) * inv;
    w1 = edge_d(c->x, c->y, a->x, a->y, px, py) * inv;
    w2 = edge_d(a->x, a->y, b->x, b->y, px, py) * inv;
    iw = w0 * a->iw + w1 * b->iw + w2 * c->iw;
    iw = iw != 0 ? 1.0 / iw : 0;
    if (unit) {
        *u = (w0 * a->u1 + w1 * b->u1 + w2 * c->u1) * iw;
        *v = (w0 * a->v1 + w1 * b->v1 + w2 * c->v1) * iw;
    } else {
        *u = (w0 * a->u + w1 * b->u + w2 * c->u) * iw;
        *v = (w0 * a->v + w1 * b->v + w2 * c->v) * iw;
    }
}

/* texel of texture unit `unit` at pixel (px, py), with its LOD */
static uint32_t tex_fetch(const MbxTri *t, const HostTex *tx, int unit,
                          float ia, float px, float py)
{
    float u, v, lambda = 0;

    uv_at(t, unit, ia, px, py, &u, &v);
    if (tx->mip) {
        /* screen-space derivatives from the neighbours */
        float ux, vx, uy, vy, rx, ry;

        uv_at(t, unit, ia, px + 1, py, &ux, &vx);
        uv_at(t, unit, ia, px, py + 1, &uy, &vy);
        rx = hypotf((ux - u) * tx->w, (vx - v) * tx->h);
        ry = hypotf((uy - u) * tx->w, (vy - v) * tx->h);
        lambda = log2f(MAX(MAX(rx, ry), 1e-6f));
    }
    return sample(tx, u, v, lambda);
}

/*
 * Coverage (Session 67): edge functions on vertices snapped to 1/16 pixel,
 * in exact integer arithmetic, with the top-left rule for samples exactly
 * on an edge: such a pixel belongs to the triangle that has the edge on its
 * left (inside to the right of it) or, for a horizontal edge, on its top
 * (inside below). Of two triangles sharing an edge exactly one draws a
 * pixel on it, so translucent meshes are not blended twice along their
 * edges and quads cover exactly their [x0, x1) x [y0, y1) pixels (before:
 * any sample with all edge values >= 0 was drawn, by every triangle
 * touching it). gltest 88 checks it. Interpolation still uses the float
 * barycentrics.
 */
#define SUBPIX          16

typedef struct EdgeI {
    int64_t a, b, c;            /* e(px, py) = a * px + b * py + c */
    bool tl;                    /* top-left: e == 0 counts as inside */
} EdgeI;

/* edge P -> Q of a triangle with orientation sign s (+1 / -1), positive
 * inside; coordinates in 1/16 pixel */
static void edge_setup(EdgeI *e, int64_t px, int64_t py, int64_t qx,
                       int64_t qy, int s)
{
    e->a = -(qy - py) * s;
    e->b = (qx - px) * s;
    e->c = -(e->a * px + e->b * py);
    /* the inside normal (a, b): left edge = inside to the right (a > 0);
     * top edge = horizontal with the inside below (a == 0, b > 0) */
    e->tl = e->a > 0 || (e->a == 0 && e->b > 0);
}

static inline bool edge_in(const EdgeI *e, int64_t sx, int64_t sy)
{
    int64_t v = e->a * sx + e->b * sy + e->c;

    return v > 0 || (v == 0 && e->tl);
}

static void raster(MbxR *r, const MbxTri *t, const HostTex *tx,
                   const HostTex *tx1, int W, int H)
{
    const MbxVtx *a = &t->v[0], *b = &t->v[1], *c = &t->v[2];
    float area = edge(a->x, a->y, b->x, b->y, c->x, c->y), ia;
    unsigned zcmp = t->no_depth_test ? 7 : ISP_ZCMP(t->isp);
    unsigned sf = ISP_SRC(t->isp), df = ISP_DST(t->isp);
    bool zwrite = t->no_depth_test || !(t->isp & ISP_NO_ZWRITE);
    bool blend = !(sf == 1 && df == 0);
    unsigned acmp = t->no_depth_test ? 7 : ATEST_CMP(t->atest);
    float aref = ATEST_REF(t->atest);
    int x0, x1, y0, y1, x, y, k;
    /* integer coverage while the products fit (|coordinate| < 1e7 px;
     * beyond that, far outside the frame, the float test as before) */
    bool fix = fabsf(a->x) < 1e7f && fabsf(a->y) < 1e7f &&
               fabsf(b->x) < 1e7f && fabsf(b->y) < 1e7f &&
               fabsf(c->x) < 1e7f && fabsf(c->y) < 1e7f;
    int64_t ax = fix ? llrintf(a->x * SUBPIX) : 0;
    int64_t ay = fix ? llrintf(a->y * SUBPIX) : 0;
    int64_t bx = fix ? llrintf(b->x * SUBPIX) : 0;
    int64_t by = fix ? llrintf(b->y * SUBPIX) : 0;
    int64_t cx = fix ? llrintf(c->x * SUBPIX) : 0;
    int64_t cy = fix ? llrintf(c->y * SUBPIX) : 0;
    int64_t iarea = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    EdgeI e0, e1, e2;
    int sgn;

    if (fabsf(area) < 1e-6f || (fix && !iarea) ||
        !isfinite(a->x + a->y + b->x + b->y + c->x + c->y)) {
        return;
    }
    sgn = iarea >= 0 ? 1 : -1;
    edge_setup(&e0, bx, by, cx, cy, sgn);
    edge_setup(&e1, cx, cy, ax, ay, sgn);
    edge_setup(&e2, ax, ay, bx, by, sgn);
    ia = 1.f / area;
    x0 = MAX(0, (int)floorf(MIN(a->x, MIN(b->x, c->x))));
    x1 = MIN(W - 1, (int)ceilf(MAX(a->x, MAX(b->x, c->x))));
    y0 = MAX(0, (int)floorf(MIN(a->y, MIN(b->y, c->y))));
    y1 = MIN(H - 1, (int)ceilf(MAX(a->y, MAX(b->y, c->y))));
    for (y = y0; y <= y1; y++) {
        float py = y + 0.5f;

        int64_t sy = (int64_t)y * SUBPIX + SUBPIX / 2;

        for (x = x0; x <= x1; x++) {
            int64_t sx = (int64_t)x * SUBPIX + SUBPIX / 2;
            float px = x + 0.5f;
            float w0, w1, w2;
            float z, iw, col[4];
            uint32_t out;

            if (fix && (!edge_in(&e0, sx, sy) || !edge_in(&e1, sx, sy) ||
                        !edge_in(&e2, sx, sy))) {
                continue;
            }
            w0 = edge(b->x, b->y, c->x, c->y, px, py) * ia;
            w1 = edge(c->x, c->y, a->x, a->y, px, py) * ia;
            w2 = edge(a->x, a->y, b->x, b->y, px, py) * ia;
            if (!fix && (w0 < 0 || w1 < 0 || w2 < 0)) {
                continue;
            }
            r->st.frags++;
            z = w0 * a->z + w1 * b->z + w2 * c->z;
            if (!depth_pass(zcmp, z, r->zb[y * W + x])) {
                continue;
            }
            if (t->depth_only) {
                if (zwrite) {
                    r->zb[y * W + x] = z;
                }
                continue;
            }
            iw = w0 * a->iw + w1 * b->iw + w2 * c->iw;
            iw = iw != 0 ? 1.f / iw : 0;
            for (k = 0; k < 4; k++) {
                col[k] = t->flat ? t->fc[k] :
                         (w0 * a->c[k] + w1 * b->c[k] + w2 * c->c[k]) * iw;
            }
            if (tx || tx1) {
                float prim[4];

                memcpy(prim, col, sizeof(prim));
                if (tx) {
                    combine(tx, tex_fetch(t, tx, 0, ia, px, py), prim, col);
                }
                if (tx1) {
                    combine(tx1, tex_fetch(t, tx1, 1, ia, px, py), prim, col);
                }
            }
            if (t->fog) {
                float ff = (w0 * a->f + w1 * b->f + w2 * c->f) * iw;

                ff = ff < 0 ? 0 : ff > 1 ? 1 : ff;
                for (k = 0; k < 3; k++) {
                    col[k] = ff * col[k] + (1 - ff) * FOG_RGB;
                }
            }
            for (k = 0; k < 4; k++) {
                col[k] = col[k] < 0 ? 0 : col[k] > 1 ? 1 : col[k];
            }
            if (acmp != 7 &&
                !depth_pass(acmp, (int)(col[3] * 255.f + .5f), aref)) {
                continue;               /* alpha test failed */
            }
            if (zwrite) {
                r->zb[y * W + x] = z;
            }
            if (blend) {
                uint32_t d32 = r->fb[y * W + x];
                float d[4] = { ((d32 >> 16) & 0xff) / 255.f,
                               ((d32 >> 8) & 0xff) / 255.f,
                               (d32 & 0xff) / 255.f, (d32 >> 24) / 255.f };
                float o[4];

                for (k = 0; k < 4; k++) {
                    o[k] = col[k] * blend_factor(sf, true, col, d, k) +
                           d[k] * blend_factor(df, false, col, d, k);
                }
                for (k = 0; k < 4; k++) {
                    col[k] = o[k] < 0 ? 0 : o[k] > 1 ? 1 : o[k];
                }
            }
            out = (uint32_t)(col[3] * 255.f + .5f) << 24 |
                  (uint32_t)(col[0] * 255.f + .5f) << 16 |
                  (uint32_t)(col[1] * 255.f + .5f) << 8 |
                  (uint32_t)(col[2] * 255.f + .5f);
            if (x == r->probe_x && y == r->probe_y &&
                r->frames + 1 == r->dbg_frame) {
                fprintf(stderr, "[nano7-mbxr] probe (%d,%d) isp %08x tex %d "
                        "%08x %08x: tri (%.3f,%.3f uv %.4f,%.4f) (%.3f,%.3f "
                        "uv %.4f,%.4f) (%.3f,%.3f uv %.4f,%.4f) bary %.4f "
                        "%.4f %.4f -> %08x over %08x\n", x, y, t->isp,
                        t->tex, t->tex >= 0 ? r->texs[t->tex].w[0] : 0,
                        t->tex >= 0 ? r->texs[t->tex].w[1] : 0,
                        a->x, a->y, a->u / a->iw, a->v / a->iw,
                        b->x, b->y, b->u / b->iw, b->v / b->iw,
                        c->x, c->y, c->u / c->iw, c->v / c->iw, w0, w1, w2,
                        out, r->fb[y * W + x]);
            }
            r->fb[y * W + x] = (out & ~t->keep) |
                               (r->fb[y * W + x] & t->keep);
        }
    }
}

/* the host copy of scene texture idx, loaded once; an identical earlier
 * layer of the scene is reused */
static HostTex *host_tex(MbxR *r, const uint32_t *core, HostTex *ht, int idx)
{
    uint32_t j;

    if (idx < 0) {
        return NULL;
    }
    for (j = 0; j < (uint32_t)idx; j++) {
        if (ht[j].texel && !memcmp(ht[j].key, r->texs[idx].w, 12)) {
            break;
        }
    }
    if (j == (uint32_t)idx && !ht[j].texel &&
        !load_tex(r, core, &r->texs[j], &ht[j])) {
        warn_once(r, 6, "texture at GPU VA 0x%08x unreadable",
                  (r->texs[j].w[1] & 0xffffff) << 7);
    }
    return ht[j].texel ? &ht[j] : NULL;
}

void mbxr_render(MbxR *r, const uint32_t *core)
{
    uint32_t target = core[0x65c >> 2], stride = core[0x660 >> 2] & 0xffff;
    int W = (core[0x654 >> 2] >> 16) + 1, H = (core[0x658 >> 2] >> 16) + 1;
    HostTex *ht = NULL;
    uint32_t i, y;

    if (!target || !stride || W > FB_MAX_W || H > FB_MAX_H || W > (int)stride) {
        warn_once(r, 5, "render job target 0x%08x not modeled", target);
        return;
    }
    for (i = 0; i < (uint32_t)(W * H); i++) {
        r->fb[i] = 0xff000000u;
        r->zb[i] = 1.f;
    }
    if (r->ntex) {
        ht = g_new0(HostTex, r->ntex);
    }
    for (i = 0; i < r->ntri; i++) {
        const MbxTri *t = &r->tri[i];

        raster(r, t, host_tex(r, core, ht, t->tex),
               host_tex(r, core, ht, t->tex1), W, H);
    }
    for (y = 0; y < (uint32_t)H; y++) {
        if (!gpu_rw(core, target + y * stride * 4, &r->fb[y * W], W * 4,
                    true)) {
            warn_once(r, 7, "render target GPU VA 0x%08x not mapped", target);
            break;
        }
    }
    for (i = 0; ht && i < r->ntex; i++) {
        g_free(ht[i].texel);
    }
    g_free(ht);
    r->frames++;
    r->st.frames++;
    /* a '%' in the name ("fb%05u.pam"): every NANO7_MBXR_FRAME-th frame */
    if (r->dump_fb && (strchr(r->dump_fb, '%') ?
                       r->frames % r->dbg_frame == 0 :
                       r->frames == r->dbg_frame)) {
        g_autofree char *fn = strchr(r->dump_fb, '%') ?
            g_strdup_printf(r->dump_fb, (unsigned)r->frames) :
            g_strdup(r->dump_fb);
        FILE *f = fopen(fn, "wb");              /* RGBA, before display */

        if (f) {
            fprintf(f, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\n"
                    "TUPLTYPE RGB_ALPHA\nENDHDR\n", W, H);
            for (i = 0; i < (uint32_t)(W * H); i++) {
                uint8_t px[4] = { r->fb[i] >> 16, r->fb[i] >> 8, r->fb[i],
                                  r->fb[i] >> 24 };

                fwrite(px, 1, 4, f);
            }
            fclose(f);
        }
    }
    if (r->vgp_check && r->frames == r->dbg_frame) {
        fprintf(stderr, "[nano7-mbxr] vgpcheck %" PRIu64 " vertices: max diff "
                "pos %g (rel) colour %g uv %g uv1 %g fog %g\n", r->chk_n,
                r->chk[0], r->chk[1] * 255.f, r->chk[2], r->chk[3],
                r->chk[4]);
    }
    if (r->stats && r->frames == r->dbg_frame) {
        for (i = 0; i < r->st_cnt; i++) {
            fprintf(stderr, "[nano7-mbxr] state isp %08x b1 %08x fmt %04x "
                    "tex0 %08x ctl %08x: %u vertices\n", r->st_key[i][0],
                    r->st_key[i][1], r->st_key[i][2], r->st_key[i][3],
                    r->st_key[i][4], r->st_n[i]);
        }
        fprintf(stderr, "[nano7-mbxr] viewport x %g + %g, y %g + %g, "
                "z %g + %g * ndc\n", r->vp[0], r->vp[1], r->vp[2], r->vp[3],
                r->vp[4], r->vp[5]);
    }
}
