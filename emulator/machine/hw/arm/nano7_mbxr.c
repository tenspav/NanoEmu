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
 *               one more word, and the vertices up to the end of the segment
 *   0x4/5/7...  a draw with unchanged state: the primitive word first
 *   0xf0000000  end of the scene's list
 * Vertex format: one nibble per attribute (0 position, 2 colour, 3 uv0),
 * the value is its size in words (5 = packed 0xAABBGGRR colour, 1 word).
 * Batches with ISP bit 22 run through the vertex program (VGP): the
 * driver's fixed-function programs compute clip = c0..c3 . (x, y, z, 1);
 * this renderer does that directly (high-level emulation of the VGP)
 * instead of executing the microcode, then divides by w and applies the
 * viewport. Other batches (the clears: full-screen triangles (0,0),
 * (480,0), (0,864)) carry pixel coordinates.
 * Texture layer word 1: address >> 7 in bits 23:0 (GPU virtual); texels
 * are twiddled (Morton order, y in the low bit). Size and format: see
 * mbxr_tex_params().
 * GPU addresses go through the MBX MMU: core +0x1000 + 4 * n holds the
 * physical address of page table n (1024 x 4-byte entries, 4 KB pages).
 * Render job registers: +0x65c target (GPU VA), +0x660 stride in pixels,
 * +0x654 / +0x658 last x / y in bits 31:16; ARGB8888.
 *
 * Model: per segment the draws are transformed and set up into triangles;
 * the render job rasterizes them (perspective-correct colour and uv,
 * depth test, nearest texel, modulate) into a host frame and writes it to
 * the target. Not modeled yet: blending, culling, stencil, fog, lighting
 * (programs other than the plain transform), mipmaps, filtering.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "nano7_mbxr.h"
#include "exec/memory.h"
#include "exec/address-spaces.h"
#include "qemu/host-utils.h"
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
} MbxVtx;

typedef struct MbxTri {
    MbxVtx v[3];
    uint32_t isp;
    int16_t tex;                /* index into r->texs, -1 = untextured */
    bool depth_only, no_depth_test;
} MbxTri;

struct MbxR {
    /* persistent TA state */
    uint32_t isp, b1, region, fmt, b6, b8;
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
    return r;
}

void mbxr_reset(MbxR *r)
{
    r->seg_len = 0;
    r->ntri = 0;
    r->ntex = 0;
}

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

static unsigned attr_words(unsigned n)
{
    return n == 5 ? 1 : n;      /* 5 = packed ubyte4 colour */
}

static void emit_tri(MbxR *r, const MbxVtx *a, const MbxVtx *b,
                     const MbxVtx *c, int tex, bool depth_only, bool vgp)
{
    MbxTri *t;

    if (r->ntri >= TRI_MAX) {
        warn_once(r, 0, "scene has more than %u triangles", TRI_MAX);
        return;
    }
    if (a->iw <= 0 || b->iw <= 0 || c->iw <= 0) {
        return;                 /* behind the eye: no near clipping yet */
    }
    t = &r->tri[r->ntri++];
    t->v[0] = *a;
    t->v[1] = *b;
    t->v[2] = *c;
    t->isp = r->isp;
    t->tex = tex;
    t->depth_only = depth_only;
    t->no_depth_test = !vgp;    /* clears: always pass */
}

static void draw(MbxR *r, uint32_t prim, const uint32_t *v, uint32_t nw)
{
    unsigned sz[8], off[8], stride = 0, i, n, k;
    bool vgp = r->isp & (1u << 22);
    bool has_col, has_uv, depth_only;
    int tex = -1;
    MbxVtx *vx;

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
    depth_only = !vgp && !has_col;
    if (has_uv && (r->layer[0].w[1] & 0xffffff) && r->ntex < TRI_MAX) {
        r->texs[r->ntex] = r->layer[0];
        tex = r->ntex++;
    }
    n = nw / stride;
    if (!n) {
        return;
    }
    vx = g_new(MbxVtx, n);
    for (i = 0; i < n; i++) {
        const uint32_t *p = v + i * stride;
        float x = f32(p[off[0]]), y = sz[0] > 1 ? f32(p[off[0] + 1]) : 0;
        float z = sz[0] > 2 ? f32(p[off[0] + 2]) : 0, w = 1, iw;
        MbxVtx *o = &vx[i];

        if (vgp) {
            float cx = 0, cy = 0, cz = 0, cw = 0, in[4] = { x, y, z, 1 };

            for (k = 0; k < 4; k++) {
                cx += r->cst[0][k] * in[k];
                cy += r->cst[1][k] * in[k];
                cz += r->cst[2][k] * in[k];
                cw += r->cst[3][k] * in[k];
            }
            w = cw;
            iw = w != 0 ? 1.f / w : 0;
            o->x = r->vp[0] + r->vp[1] * cx * iw;
            o->y = r->vp[2] + r->vp[3] * cy * iw;
            o->z = r->vp[4] + r->vp[5] * cz * iw;
        } else {
            iw = 1;
            o->x = x;
            o->y = y;
            o->z = z;
        }
        o->iw = iw;
        if (has_col) {
            uint32_t c = p[off[2]];

            for (k = 0; k < 4; k++) {
                o->c[k] = ((c >> (8 * k)) & 0xff) / 255.f * iw;
            }
        } else {
            for (k = 0; k < 4; k++) {
                o->c[k] = iw;   /* white */
            }
        }
        o->u = has_uv ? f32(p[off[3]]) * iw : 0;
        o->v = has_uv ? f32(p[off[3] + 1]) * iw : 0;
    }
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

/* parse one doorbell segment */
void mbxr_doorbell(MbxR *r)
{
    const uint32_t *s = r->seg;
    uint32_t n = r->seg_len, i = 0, b;

    r->seg_len = 0;
    while (i < n) {
        uint32_t w = s[i];

        if ((w >> 28) == 0x8) {                 /* program upload */
            i += 1 + ctpop32(w & 0xffff);       /* + descriptors */
            if (i < n && (s[i] >> 28) == 0x9) {
                uint32_t a = (s[i] >> 16) & 0xfff, e = s[i] & 0xfff;

                i += 1 + 2 * (e >= a ? e - a + 1 : 0);
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
            if (i + 2 <= n) {
                draw(r, s[i], &s[i + 2], n - (i + 2));
            }
            i = n;                              /* vertices to the end */
        } else if (w == 0xf0000000) {
            i++;
        } else if ((w >> 28) == 4 || (w >> 28) == 5 || (w >> 28) == 7) {
            /* draw with unchanged state: no 0x1 header, the primitive word
             * comes first (Blocks: 0x70060000) */
            if (i + 2 <= n) {
                draw(r, s[i], &s[i + 2], n - (i + 2));
            }
            i = n;
        } else {
            warn_once(r, 4, "unknown TA command word 0x%08x", w);
            break;
        }
    }
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

typedef struct HostTex {
    uint32_t key[3];
    unsigned w, h;
    uint32_t *texel;            /* linear ARGB8888 */
} HostTex;

/* size (log2) and format of a texture layer, from layer word 0 (gltest
 * variants: 64x64 ARGB8888 0x03319600, 32x16 ARGB8888 0x02119600, 64x64
 * RGB565 0x03305600): bits 27:24 log2(width) - 3, 23:20 log2(height) - 3,
 * 16:12 format (0x19 ARGB8888, 0x05 RGB565) */
static bool mbxr_tex_params(MbxR *r, const MbxTex *t, unsigned *lw,
                            unsigned *lh, unsigned *bpp)
{
    unsigned fmt = (t->w[0] >> 12) & 0x1f;

    *lw = ((t->w[0] >> 24) & 0xf) + 3;
    *lh = ((t->w[0] >> 20) & 0xf) + 3;
    switch (fmt) {
    case 0x19:
        *bpp = 32;
        break;
    case 0x05:
        *bpp = 16;
        break;
    default:
        warn_once(r, 8, "texture format 0x%02x not modeled", fmt);
        return false;
    }
    return *lw <= 11 && *lh <= 11;
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

static bool load_tex(MbxR *r, const uint32_t *core, const MbxTex *t,
                     HostTex *h)
{
    unsigned lw, lh, bpp, x, y;
    uint32_t va = (t->w[1] & 0xffffff) << 7, size;
    g_autofree uint8_t *raw = NULL;

    if (!mbxr_tex_params(r, t, &lw, &lh, &bpp)) {
        return false;
    }
    h->w = 1u << lw;
    h->h = 1u << lh;
    size = h->w * h->h * bpp / 8;
    raw = g_malloc(size);
    if (!gpu_rw(core, va, raw, size, false)) {
        return false;
    }
    h->texel = g_new(uint32_t, h->w * h->h);
    for (y = 0; y < h->h; y++) {
        for (x = 0; x < h->w; x++) {
            unsigned i = twiddle(x, y, lw, lh);
            uint32_t c;

            if (bpp == 32) {
                c = ldl_le_p(raw + i * 4);
            } else {
                uint32_t p = lduw_le_p(raw + i * 2), r5, g6, b5;

                r5 = (p >> 11) & 0x1f;
                g6 = (p >> 5) & 0x3f;
                b5 = p & 0x1f;
                c = 0xff000000u | (r5 << 3 | r5 >> 2) << 16 |
                    (g6 << 2 | g6 >> 4) << 8 | (b5 << 3 | b5 >> 2);
            }
            h->texel[y * h->w + x] = c;
        }
    }
    memcpy(h->key, t->w, 12);
    return true;
}

/* ------------------------------------------------------------------ */
/* rasterizer                                                          */

static inline float edge(float ax, float ay, float bx, float by, float px,
                         float py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

static void raster(MbxR *r, const MbxTri *t, const HostTex *tx, int W, int H)
{
    const MbxVtx *a = &t->v[0], *b = &t->v[1], *c = &t->v[2];
    float area = edge(a->x, a->y, b->x, b->y, c->x, c->y), ia;
    int x0, x1, y0, y1, x, y, k;

    if (fabsf(area) < 1e-6f) {
        return;
    }
    ia = 1.f / area;
    x0 = MAX(0, (int)floorf(MIN(a->x, MIN(b->x, c->x))));
    x1 = MIN(W - 1, (int)ceilf(MAX(a->x, MAX(b->x, c->x))));
    y0 = MAX(0, (int)floorf(MIN(a->y, MIN(b->y, c->y))));
    y1 = MIN(H - 1, (int)ceilf(MAX(a->y, MAX(b->y, c->y))));
    for (y = y0; y <= y1; y++) {
        float py = y + 0.5f;

        for (x = x0; x <= x1; x++) {
            float px = x + 0.5f;
            float w0 = edge(b->x, b->y, c->x, c->y, px, py) * ia;
            float w1 = edge(c->x, c->y, a->x, a->y, px, py) * ia;
            float w2 = edge(a->x, a->y, b->x, b->y, px, py) * ia;
            float z, iw, col[4];
            uint32_t out;

            if (w0 < 0 || w1 < 0 || w2 < 0) {
                continue;
            }
            z = w0 * a->z + w1 * b->z + w2 * c->z;
            if (!t->no_depth_test && !(z < r->zb[y * W + x])) {
                continue;
            }
            r->zb[y * W + x] = z;
            if (t->depth_only) {
                continue;
            }
            iw = w0 * a->iw + w1 * b->iw + w2 * c->iw;
            iw = iw != 0 ? 1.f / iw : 0;
            for (k = 0; k < 4; k++) {
                col[k] = (w0 * a->c[k] + w1 * b->c[k] + w2 * c->c[k]) * iw;
            }
            if (tx) {
                float u = (w0 * a->u + w1 * b->u + w2 * c->u) * iw;
                float v = (w0 * a->v + w1 * b->v + w2 * c->v) * iw;
                int tu = (int)floorf(u * tx->w), tv = (int)floorf(v * tx->h);
                uint32_t s;

                tu &= tx->w - 1;        /* repeat */
                tv &= tx->h - 1;
                s = tx->texel[tv * tx->w + tu];
                col[0] *= ((s >> 16) & 0xff) / 255.f;   /* modulate */
                col[1] *= ((s >> 8) & 0xff) / 255.f;
                col[2] *= (s & 0xff) / 255.f;
                col[3] *= (s >> 24) / 255.f;
            }
            for (k = 0; k < 4; k++) {
                col[k] = col[k] < 0 ? 0 : col[k] > 1 ? 1 : col[k];
            }
            out = (uint32_t)(col[3] * 255.f + .5f) << 24 |
                  (uint32_t)(col[0] * 255.f + .5f) << 16 |
                  (uint32_t)(col[1] * 255.f + .5f) << 8 |
                  (uint32_t)(col[2] * 255.f + .5f);
            r->fb[y * W + x] = out;
        }
    }
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
        HostTex *tx = NULL;

        if (t->tex >= 0) {
            uint32_t j;

            /* reuse an identical earlier layer of this scene */
            for (j = 0; j < (uint32_t)t->tex; j++) {
                if (ht[j].texel && !memcmp(ht[j].key, r->texs[t->tex].w, 12)) {
                    break;
                }
            }
            if (j == (uint32_t)t->tex && !ht[j].texel &&
                !load_tex(r, core, &r->texs[j], &ht[j])) {
                warn_once(r, 6, "texture at GPU VA 0x%08x unreadable",
                          (r->texs[j].w[1] & 0xffffff) << 7);
            }
            tx = ht[j].texel ? &ht[j] : NULL;
        }
        raster(r, t, tx, W, H);
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
}
