/* NanoSync - cover art: see ns_art.h */
#include "ns_art.h"

const ns_fmt NS_FMT[NS_FMT_N] = { {1010, 240, 240, 480, 480u * 240u}, {1013, 50, 50, 100, 100u * 50u}, {1015, 58, 58, 116, 116u * 58u}, {1016, 57, 57, 116, 116u * 57u} };

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint32_t ss32(const uint8_t *p) { return ((uint32_t)(p[0] & 0x7f) << 21) | ((uint32_t)(p[1] & 0x7f) << 14) | ((uint32_t)(p[2] & 0x7f) << 7) | (p[3] & 0x7f); }
static uint32_t le32(const uint8_t *p) { return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wl32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void wl64(uint8_t *p, uint64_t v) { wl32(p, (uint32_t)v); wl32(p + 4, (uint32_t)(v >> 32)); }
static void zero(void *d, uint32_t n) { uint8_t *a = d; while (n--) *a++ = 0; }
static void cpy(void *d, const void *s, uint32_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; }
static void move(void *d, const void *s, uint32_t n) { uint8_t *a = d; const uint8_t *b = s; if (a < b) { while (n--) *a++ = *b++; } else { a += n; b += n; while (n--) *--a = *--b; } }

/* ------------------------------------------------------------------------------------------ ID3 */
int ns_id3_cover(const uint8_t *p, uint32_t n, uint32_t *off, uint32_t *len) {
    uint32_t ver, flags, end, pos, bo = 0, bl = 0; int best = 0, bt = -1, unsup = 0;
    if (n < 10 || p[0] != 'I' || p[1] != 'D' || p[2] != '3') return 0;
    ver = p[3]; flags = p[5];
    if (ver < 3 || ver > 4) return 0;
    end = 10u + ss32(p + 6); if (end > n) end = n;
    pos = 10;
    if (flags & 0x40) { if (pos + 4 > end) return 0; pos += ver == 4 ? ss32(p + pos) : be32(p + pos) + 4u; }
    while (pos + 10 <= end) {
        uint32_t sz, d, fl2; if (!p[pos]) break;
        sz = ver == 4 ? ss32(p + pos + 4) : be32(p + pos + 4); fl2 = p[pos + 9]; d = pos + 10;
        if (sz > end - d) break;
        if (p[pos] == 'A' && p[pos + 1] == 'P' && p[pos + 2] == 'I' && p[pos + 3] == 'C') {
            uint32_t bad = ver == 4 ? (fl2 & 0x0Eu) : (fl2 & 0xC0u), q = d + (ver == 4 && (fl2 & 1) ? 4u : 0u), qe = d + sz, m, enc, type;
            if (bad || (flags & 0x80)) unsup = 1;
            else if (q + 4 <= qe) {
                enc = p[q++]; m = q; while (q < qe && p[q]) q++;
                if (q + 2 < qe) {
                    int link = (q - m == 3 && p[m] == '-' && p[m + 1] == '-' && p[m + 2] == '>');
                    q++; type = p[q++];
                    if (enc == 1 || enc == 2) { while (q + 1 < qe && !(p[q] == 0 && p[q + 1] == 0)) q += 2; q += 2; }
                    else { while (q < qe && p[q]) q++; q++; }
                    if (!link && q < qe && ((!best) || (type == 3 && bt != 3))) { best = 1; bt = (int)type; bo = q; bl = qe - q; }
                }
            }
        }
        pos = d + sz;
    }
    if (!best) return unsup ? -1 : 0;
    *off = bo; *len = bl;
    return (bl >= 3 && p[bo] == 0xFF && p[bo + 1] == 0xD8) ? 1 : -1;
}

/* ------------------------------------------------------------------------------------------ decoder */
static size_t cv_in(JDEC *jd, uint8_t *buf, size_t nb) {
    ns_cv *c = (ns_cv *)jd->device; uint32_t r = c->srclen - c->srcpos;
    if (nb > r) nb = r;
    if (buf) cpy(buf, c->src + c->srcpos, (uint32_t)nb);
    c->srcpos += (uint32_t)nb; return nb;
}
static void fit(uint32_t sw, uint32_t sh, uint32_t fw, uint32_t fh, uint32_t *dw, uint32_t *dh) {
    if (sw <= fw && sh <= fh) { *dw = sw; *dh = sh; return; }                  /* no upscaling */
    if ((uint64_t)sw * fh >= (uint64_t)sh * fw) { *dw = fw; *dh = (sh * fw + sw / 2) / sw; if (!*dh) *dh = 1; if (*dh > fh) *dh = fh; }
    else { *dh = fh; *dw = (sw * fh + sh / 2) / sh; if (!*dw) *dw = 1; if (*dw > fw) *dw = fw; }
}
static int pg_sof(const uint8_t *d, uint32_t n);
static int pg_segment(ns_cv *cv, uint8_t *aux, uint32_t auxsz);
static int pg_step(ns_cv *cv, uint32_t n);
uint32_t ns_cv_scratch_need(void) { return 4u * NS_CV_MAXW * 6u + 405u * 8u + 22u * 405u * 12u + 2048u; }
/* 1-axis area reduction map: source pixel i covers [i*d/s, (i+1)*d/s) in destination units; weights in 1/16: w0 goes to destination m[i], 16-w0 to the next one */
static void axis_map(uint32_t s, uint32_t d, uint16_t *m, uint8_t *w, uint32_t *tot) {
    uint32_t i;
    for (i = 0; i < d; i++) tot[i] = 0;
    for (i = 0; i < s; i++) {
        uint32_t dx = (uint32_t)(((uint64_t)i * d) / s), w0 = 16;
        if (dx + 1 < d && (uint64_t)(i + 1) * d > (uint64_t)(dx + 1) * s) {      /* crosses the destination pixel boundary */
            w0 = (uint32_t)(((uint64_t)((dx + 1) * s - i * d) * 16u + d / 2u) / d);
            if (w0 > 16) w0 = 16;
        }
        m[i] = (uint16_t)dx; w[i] = (uint8_t)w0; tot[dx] += w0; if (w0 < 16) tot[dx + 1] += 16 - w0;
    }
}
int ns_cv_begin(ns_cv *c, const uint8_t *img, uint32_t len, uint8_t *pool, uint32_t poolsz, uint8_t *scratch, uint32_t scsz, uint8_t *const out[NS_FMT_N], uint8_t *aux, uint32_t auxsz, uint8_t *aux2, uint32_t aux2sz) {
    uint32_t t, i, wd0, hd0; uint8_t *sp = scratch, *se = scratch + scsz; JRESULT rc;
    zero(c, sizeof *c); c->src = img; c->srclen = len;
    if (pg_sof(img, len)) {                                                    /* progressive JPEG: low frequencies, output at 1/4 */
        ns_pg *g = &c->pg; int r; c->prog = 1; g->d = img; g->n = len; g->aux[0] = aux; g->auxsz[0] = auxsz; g->aux[1] = aux2; g->auxsz[1] = aux2sz;
        if (!aux || auxsz < 1024u) return -2;
        while (!g->sof) { r = pg_segment(c, aux, auxsz); if (r == -3) return -3; if (r) return -1; }
        c->W0 = g->W; c->H0 = g->H; c->sc = 1;
        c->Ws = ((c->W0 - 1) >> 1) + 1; c->Hs = ((c->H0 - 1) >> 1) + 1;
        if (c->Ws < 200u && c->Hs < 200u) return -3;                          /* too small */
    } else {
        rc = jd_prepare(&c->jd, cv_in, pool, poolsz, c);
        if (rc == JDR_MEM1 || rc == JDR_MEM2) return -2;
        if (rc != JDR_OK) return -1;
        c->W0 = c->jd.width; c->H0 = c->jd.height; if (!c->W0 || !c->H0) return -1;
        fit(c->W0, c->H0, NS_FMT[0].w, NS_FMT[0].h, &wd0, &hd0);
        /* descale only while >= 2x the final size remains (area reduction needs spare samples to stay sharp) */
        c->sc = 0; for (i = 3; i > 0; i--) if (((c->W0 - 1) >> i) + 1 >= 2u * wd0 && ((c->H0 - 1) >> i) + 1 >= 2u * hd0) { c->sc = (uint8_t)i; break; }
        while (c->sc < 3 && (((c->W0 - 1) >> c->sc) + 1 > NS_CV_MAXW || ((c->H0 - 1) >> c->sc) + 1 > NS_CV_MAXW)) c->sc++;
        c->Ws = ((c->W0 - 1) >> c->sc) + 1; c->Hs = ((c->H0 - 1) >> c->sc) + 1;
        c->mx = (uint32_t)c->jd.msx * 8u; c->my = (uint32_t)c->jd.msy * 8u;
    }
    if (c->Ws > NS_CV_MAXW || c->Hs > NS_CV_MAXW) return -3;
    c->bandrows = c->prog ? c->pg.vmax * 4u : (((c->my + (1u << c->sc) - 1) >> c->sc) ? ((c->my + (1u << c->sc) - 1) >> c->sc) : 1u);
    for (t = 0; t < NS_FMT_N; t++) {
        uint32_t fw = NS_FMT[t].w, fh = NS_FMT[t].h, wd, hd, rb;
        fit(c->Ws, c->Hs, fw, fh, &wd, &hd);
        c->t[t].Wd = wd; c->t[t].Hd = hd; c->t[t].ox = (fw - wd) / 2; c->t[t].oy = (fh - hd) / 2;
        rb = (c->bandrows * hd + c->Hs - 1) / c->Hs + 4; if (rb > hd + 1) rb = hd + 1; c->t[t].RB = rb;
        if ((uint32_t)(se - sp) < c->Ws * 3u + c->Hs * 3u + wd * 4u + hd * 4u + rb * wd * 12u + 64u) return -2;
        c->t[t].mx0 = (uint16_t *)(void *)sp; sp += (c->Ws * 2u + 3u) & ~3u;
        c->t[t].wx0 = sp; sp += (c->Ws + 3u) & ~3u;
        c->t[t].my0 = (uint16_t *)(void *)sp; sp += (c->Hs * 2u + 3u) & ~3u;
        c->t[t].wy0 = sp; sp += (c->Hs + 3u) & ~3u;
        c->t[t].cw = (uint32_t *)(void *)sp; sp += wd * 4u;
        c->t[t].rw = (uint32_t *)(void *)sp; sp += hd * 4u;
        c->t[t].acc = (uint32_t *)(void *)sp; sp += rb * wd * 12u;
        zero(c->t[t].acc, rb * wd * 12u);
        axis_map(c->Ws, wd, c->t[t].mx0, c->t[t].wx0, c->t[t].cw);
        axis_map(c->Hs, hd, c->t[t].my0, c->t[t].wy0, c->t[t].rw);
        c->t[t].out = out[t]; zero(out[t], NS_FMT[t].psize);
    }
    if (!c->prog) { c->jd.scale = 0; c->jd.rst = 0; c->jd.rsc = 0; c->jd.dcv[0] = c->jd.dcv[1] = c->jd.dcv[2] = 0; }
    return 0;
}
static void cv_emit(ns_cv *c, uint32_t t, uint32_t dy) {
    uint32_t k = dy % c->t[t].RB, wd = c->t[t].Wd, dx; uint32_t *a = c->t[t].acc + k * wd * 3u;
    uint8_t *row = c->t[t].out + (c->t[t].oy + dy) * NS_FMT[t].stride + c->t[t].ox * 2u;
    for (dx = 0; dx < wd; dx++, a += 3) {
        uint32_t n = c->t[t].rw[dy] * c->t[t].cw[dx];
        if (n) { uint32_t r = (a[0] + n / 2) / n, g = (a[1] + n / 2) / n, b = (a[2] + n / 2) / n, v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3); row[2 * dx] = (uint8_t)v; row[2 * dx + 1] = (uint8_t)(v >> 8); }
        a[0] = a[1] = a[2] = 0;
    }
}
static void cv_flush(ns_cv *c, uint32_t t, uint32_t upto) { while (c->t[t].flushed < upto && c->t[t].flushed < c->t[t].Hd) cv_emit(c, t, c->t[t].flushed++); }
static int cv_out(JDEC *jd, void *bitmap, JRECT *rc);
static int cv_acc(ns_cv *c, const uint8_t *bm, JRECT *rc);
/* The LVGL TJpgDec: (1) delivers pixels in B,G,R order; (2) does NOT do 1/2, 1/4 descaling (smaller rect only, full-size data => scrambled image):
   so it always decodes at full size and the 2^sc x 2^sc average descaling is done here. */
static int cv_out(JDEC *jd, void *bitmap, JRECT *rc) {
    ns_cv *c = (ns_cv *)jd->device; uint8_t *bm = (uint8_t *)bitmap; uint32_t w = (uint32_t)rc->right - rc->left + 1u, h = (uint32_t)rc->bottom - rc->top + 1u, i;
    if (!c->sc) { for (i = 0; i < w * h; i++) { uint8_t t = bm[3 * i]; bm[3 * i] = bm[3 * i + 2]; bm[3 * i + 2] = t; } return cv_acc(c, bm, rc); }
    {
        uint32_t f = 1u << c->sc, w2 = (w + f - 1) >> c->sc, h2 = (h + f - 1) >> c->sc, x, y, xx, yy; uint8_t tmp[8 * 8 * 3]; JRECT r2;
        for (y = 0; y < h2; y++) for (x = 0; x < w2; x++) {
            uint32_t s0 = 0, s1 = 0, s2 = 0, n = 0;
            for (yy = y * f; yy < y * f + f && yy < h; yy++) for (xx = x * f; xx < x * f + f && xx < w; xx++) { const uint8_t *p = bm + (yy * w + xx) * 3u; s0 += p[2]; s1 += p[1]; s2 += p[0]; n++; }
            tmp[(y * w2 + x) * 3] = (uint8_t)((s0 + n / 2) / n); tmp[(y * w2 + x) * 3 + 1] = (uint8_t)((s1 + n / 2) / n); tmp[(y * w2 + x) * 3 + 2] = (uint8_t)((s2 + n / 2) / n);
        }
        r2.left = (uint16_t)(rc->left >> c->sc); r2.top = (uint16_t)(rc->top >> c->sc); r2.right = (uint16_t)(r2.left + w2 - 1); r2.bottom = (uint16_t)(r2.top + h2 - 1);
        return cv_acc(c, tmp, &r2);
    }
}
static int cv_acc(ns_cv *c, const uint8_t *bm, JRECT *rc) {
    uint32_t w = (uint32_t)rc->right - rc->left + 1u, r, t, cx;
    for (r = rc->top; r <= rc->bottom && r < c->Hs; r++) {
        const uint8_t *rowp = bm + (r - rc->top) * w * 3u;
        for (t = 0; t < NS_FMT_N; t++) {
            uint32_t dy = c->t[t].my0[r], wy0 = c->t[t].wy0[r], wy1 = 16u - wy0, RB = c->t[t].RB, wd = c->t[t].Wd;
            uint32_t *b0 = c->t[t].acc + (dy % RB) * wd * 3u, *b1 = wy1 ? c->t[t].acc + ((dy + 1) % RB) * wd * 3u : 0;
            if (dy < c->t[t].flushed || dy + (wy1 ? 1u : 0u) >= c->t[t].flushed + RB) { c->err = 1; return 1; }
            for (cx = rc->left; cx <= rc->right && cx < c->Ws; cx++) {
                const uint8_t *px = rowp + (cx - rc->left) * 3u; uint32_t dx = c->t[t].mx0[cx], wx0 = c->t[t].wx0[cx], wx1 = 16u - wx0, wgt;
                uint32_t *a = b0 + dx * 3u;
                wgt = wx0 * wy0; a[0] += px[0] * wgt; a[1] += px[1] * wgt; a[2] += px[2] * wgt;
                if (wx1) { wgt = wx1 * wy0; a[3] += px[0] * wgt; a[4] += px[1] * wgt; a[5] += px[2] * wgt; }
                if (b1) {
                    a = b1 + dx * 3u;
                    wgt = wx0 * wy1; a[0] += px[0] * wgt; a[1] += px[1] * wgt; a[2] += px[2] * wgt;
                    if (wx1) { wgt = wx1 * wy1; a[3] += px[0] * wgt; a[4] += px[1] * wgt; a[5] += px[2] * wgt; }
                }
            }
        }
    }
    return 1;
}
/* ---------------------------------------------------------------------------------------- progressive JPEG (low frequencies only) */
/* slot = row*4 + column (u = column = horizontal frequency, v = row). zigzag -> slot and slot -> zigzag */
static const int8_t PG_SLOT[25] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, -1, 13, 10, 7, -1, -1, -1, 11, 14, -1, -1, -1, -1, -1, 15};
static const uint8_t PG_ZZ[NS_PG_NCO] = {0, 1, 5, 6, 2, 4, 7, 13, 3, 8, 12, 17, 9, 11, 18, 24};
static const int8_t PG_CAC[16] = {-1, 0, -1, -1, 1, 2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};      /* chroma: only (0,1), (1,0), (1,1) */
static uint32_t pg_u16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static int pg_sof(const uint8_t *d, uint32_t n) {          /* 1 = progressive (SOF2), 0 = other/baseline */
    uint32_t pos = 2;
    while (pos + 4 <= n && d[pos] == 0xFF) {
        uint8_t m = d[pos + 1];
        if (m == 0xFF) { pos++; continue; }
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { pos += 2; continue; }
        if (m == 0xC2) return 1;
        if (m == 0xC0 || m == 0xC1 || m == 0xDA) return 0;
        pos += 2 + pg_u16(d + pos + 2);
    }
    return 0;
}
static int pg_bit(ns_pg *g) {
    if (!g->bitcnt) {
        uint32_t b = 0;
        if (!g->hit && g->pos < g->n) {
            b = g->d[g->pos++];
            if (b == 0xFF) { if (g->pos < g->n && g->d[g->pos] == 0) g->pos++; else { g->pos--; g->hit = 1; b = 0; } }
        } else g->hit = 1;
        g->bitbuf = b; g->bitcnt = 8;
    }
    g->bitcnt--; return (int)((g->bitbuf >> g->bitcnt) & 1u);
}
static int32_t pg_bits(ns_pg *g, uint32_t k) { int32_t v = 0; while (k--) v = (v << 1) | pg_bit(g); return v; }
static int pg_huff(ns_pg *g, uint32_t cls, uint32_t id) {
    int32_t code = 0; uint32_t l;
    for (l = 1; l <= 16; l++) { code = (code << 1) | pg_bit(g); if (g->ht[cls][id].maxcode[l] >= 0 && code <= g->ht[cls][id].maxcode[l]) return g->ht[cls][id].vals[(g->ht[cls][id].valptr[l] + code - g->ht[cls][id].mincode[l]) & 255]; }
    return -1;
}
static int32_t pg_ext(int32_t v, uint32_t s) { return (s && v < (1 << (s - 1))) ? v - (1 << s) + 1 : v; }
static void pg_align_restart(ns_pg *g) {                  /* end of restart interval: align, skip RSTn, reset predictions */
    g->bitcnt = 0; g->hit = 0;
    while (g->pos + 1 < g->n && !(g->d[g->pos] == 0xFF && g->d[g->pos + 1] >= 0xD0 && g->d[g->pos + 1] <= 0xD7)) { if (g->d[g->pos] == 0xFF && g->d[g->pos + 1] != 0 && g->d[g->pos + 1] != 0xFF) return; g->pos++; }
    if (g->pos + 1 < g->n) g->pos += 2;
    g->pred[0] = g->pred[1] = g->pred[2] = 0; g->eobrun = 0;
}
static int pg_next_marker(ns_pg *g) {                      /* position at FF xx (xx != 0, != RST) */
    g->bitcnt = 0; g->hit = 0;
    while (g->pos + 1 < g->n) { if (g->d[g->pos] == 0xFF && g->d[g->pos + 1] != 0 && g->d[g->pos + 1] != 0xFF && !(g->d[g->pos + 1] >= 0xD0 && g->d[g->pos + 1] <= 0xD7)) return 0; g->pos++; }
    return -1;
}
/* one header segment; 0 = ok, 1 = ready to render (EOI), 2 = starts a scan, <0 error */
static int pg_segment(ns_cv *cv, uint8_t *aux, uint32_t auxsz) {
    ns_pg *g = &cv->pg; uint8_t m; uint32_t len, i, j; const uint8_t *p;
    if (pg_next_marker(g)) return 1;
    m = g->d[g->pos + 1];
    if (m == 0xD9) return 1;
    if (m == 0xD8 || m == 0x01) { g->pos += 2; return 0; }
    if (g->pos + 4 > g->n) return 1;
    len = pg_u16(g->d + g->pos + 2); if (len < 2 || g->pos + 2 + len > g->n) return -1;
    p = g->d + g->pos + 4; len -= 2;
    switch (m) {
    case 0xDB: while (len >= 1) { uint32_t pq = p[0] >> 4, tq = p[0] & 15, sz = pq ? 128u : 64u; if (tq > 3 || len < 1 + sz) return -1;
            for (i = 0; i < NS_PG_NCO; i++) g->qt[tq][i] = (uint16_t)(pq ? pg_u16(p + 1 + 2 * PG_ZZ[i]) : p[1 + PG_ZZ[i]]);
            p += 1 + sz; len -= 1 + sz; } break;
    case 0xC4: while (len >= 17) { uint32_t cls = p[0] >> 4, id = p[0] & 15, tot = 0, code = 0, k = 0; if (cls > 1 || id > 3) return -1;
            for (i = 1; i <= 16; i++) tot += p[i];
            if (tot > 256 || len < 17 + tot) return -1;
            for (i = 0; i < tot; i++) g->ht[cls][id].vals[i] = p[17 + i];
            for (i = 1; i <= 16; i++) { g->ht[cls][id].valptr[i] = (int32_t)k; g->ht[cls][id].mincode[i] = (int32_t)code; code += p[i]; k += p[i]; g->ht[cls][id].maxcode[i] = p[i] ? (int32_t)code - 1 : -1; code <<= 1; }
            g->ht[cls][id].ok = 1; p += 17 + tot; len -= 17 + tot; } break;
    case 0xDD: if (len >= 2) g->ri = pg_u16(p); break;
    case 0xC2: {
        (void)aux; (void)auxsz;
        if (len < 6 || p[0] != 8) return -1;
        g->H = pg_u16(p + 1); g->W = pg_u16(p + 3); g->nc = p[5];
        if (!g->W || !g->H || (g->nc != 1 && g->nc != 3) || len < 6u + 3u * g->nc) return -1;
        g->hmax = g->vmax = 1;
        for (i = 0; i < g->nc; i++) { g->c[i].id = p[6 + 3 * i]; g->c[i].h = p[7 + 3 * i] >> 4; g->c[i].v = p[7 + 3 * i] & 15; g->c[i].tq = p[8 + 3 * i] & 3;
            if (g->c[i].h < 1 || g->c[i].h > 2 || g->c[i].v < 1 || g->c[i].v > 2) return -1;
            if (g->c[i].h > g->hmax) g->hmax = g->c[i].h;
            if (g->c[i].v > g->vmax) g->vmax = g->c[i].v; }
        g->mcux = (g->W + 8 * g->hmax - 1) / (8 * g->hmax); g->mcuy = (g->H + 8 * g->vmax - 1) / (8 * g->vmax);
        for (i = 0; i < g->nc; i++) {
            uint32_t cw = (g->W * g->c[i].h + g->hmax - 1) / g->hmax, ch = (g->H * g->c[i].v + g->vmax - 1) / g->vmax;
            g->c[i].bw = g->mcux * g->c[i].h; g->c[i].bh = g->mcuy * g->c[i].v; g->c[i].cbw = (cw + 7) / 8; g->c[i].cbh = (ch + 7) / 8;
            g->c[i].nac = i == 0 ? (uint8_t)(NS_PG_NCO - 1u) : 3u;
        }
        {   /* DC (int16) and AC (int8) of each component: first fit into the two regions */
            uint8_t *rp[2]; uint32_t rl[2], r, ok = 1; rp[0] = g->aux[0]; rl[0] = g->auxsz[0] & ~1u; rp[1] = g->aux[1]; rl[1] = g->aux[1] ? (g->auxsz[1] & ~1u) : 0;
            for (i = 0; i < g->nc && ok; i++) {
                uint32_t nb = g->c[i].bw * g->c[i].bh, sz[2]; sz[0] = nb * 2u; sz[1] = (nb * g->c[i].nac + 1u) & ~1u;
                for (j = 0; j < 2; j++) {
                    uint8_t *p2 = 0; for (r = 0; r < 2 && !p2; r++) if (rl[r] >= sz[j]) { p2 = rp[r]; rp[r] += sz[j]; rl[r] -= sz[j]; }
                    if (!p2) { ok = 0; break; }
                    { uint32_t q; for (q = 0; q < sz[j]; q++) p2[q] = 0; }
                    if (j == 0) g->c[i].dc = (int16_t *)(void *)p2; else g->c[i].ac = (int8_t *)(void *)p2;
                }
            }
            if (!ok) return -3;
        }
        g->sof = 1; } break;
    case 0xDA: {
        uint32_t ns;
        if (!g->sof || len < 1) return -1;
        ns = p[0]; if (ns < 1 || ns > 3 || len < 1 + 2 * ns + 3) return -1;
        for (i = 0; i < ns; i++) { for (j = 0; j < g->nc && g->c[j].id != p[1 + 2 * i]; j++) ; if (j >= g->nc) return -1; g->ci[i] = (uint8_t)j; g->c[j].td = p[2 + 2 * i] >> 4; g->c[j].ta = p[2 + 2 * i] & 15; }
        g->ns = (uint8_t)ns; g->Ss = p[1 + 2 * ns]; g->Se = p[2 + 2 * ns]; g->Ah = p[3 + 2 * ns] >> 4; g->Al = p[3 + 2 * ns] & 15;
        g->pos += 2 + 2 + len;                                              /* entropy-coded data starts here */
        g->bitcnt = 0; g->hit = 0; g->eobrun = 0; g->pred[0] = g->pred[1] = g->pred[2] = 0; g->idx = 0; g->rleft = g->ri;
        g->skip = (g->Ss > 24) || (g->Ss > 0 && g->Ah > 0) || g->Se > 63 || g->Al > 7;          /* AC beyond zz24 or AC refinement: ignored */
        if (g->Ss == 0 && g->Se != 0) g->skip = 1;
        g->total = g->ns > 1 ? g->mcux * g->mcuy : g->c[g->ci[0]].cbw * g->c[g->ci[0]].cbh;
        return 2; }
    default: if (m >= 0xC3 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) return -1; break;
    }
    g->pos += 2 + 2 + len; (void)cv; return 0;
}
/* up to nu units of the current scan (MCU in interleaved DC scans, block in the others); 1 = finished */
static int pg_scan(ns_cv *cv, uint32_t nu) {
    ns_pg *g = &cv->pg;
    while (nu-- && g->idx < g->total) {
        if (g->ri && g->idx && !g->rleft) { pg_align_restart(g); g->rleft = g->ri; }
        if (g->ri) g->rleft--;
        if (g->Ss == 0) {                                                   /* DC (first pass or refinement) */
            uint32_t ci, vy, hx, mxx = g->idx % g->mcux, myy = g->idx / g->mcux;
            for (ci = 0; ci < g->ns; ci++) {
                uint32_t k = g->ci[ci], hh = g->ns > 1 ? g->c[k].h : 1, vv = g->ns > 1 ? g->c[k].v : 1;
                for (vy = 0; vy < vv; vy++) for (hx = 0; hx < hh; hx++) {
                    uint32_t bx, by; int16_t *b;
                    if (g->ns > 1) { bx = mxx * hh + hx; by = myy * vv + vy; } else { bx = g->idx % g->c[k].cbw; by = g->idx / g->c[k].cbw; }
                    if (bx >= g->c[k].bw || by >= g->c[k].bh) continue;
                    b = g->c[k].dc + by * g->c[k].bw + bx;
                    if (g->Ah == 0) { int s = pg_huff(g, 0, g->c[k].td); int32_t df; if (s < 0 || s > 15) return -1; df = s ? pg_ext(pg_bits(g, (uint32_t)s), (uint32_t)s) : 0; g->pred[k] += df; *b = (int16_t)(g->pred[k] * (1 << g->Al)); }
                    else if (pg_bit(g)) *b = (int16_t)(*b | (1 << g->Al));
                }
                if (g->ns == 1) break;
            }
        } else {                                                            /* AC, first pass: 1 component */
            uint32_t k0 = g->ci[0], bx = g->idx % g->c[k0].cbw, by = g->idx / g->c[k0].cbw, k, nac = g->c[k0].nac;
            int8_t *b = g->c[k0].ac + (by * g->c[k0].bw + bx) * nac;
            if (g->eobrun) { g->eobrun--; }
            else for (k = g->Ss; k <= g->Se; k++) {
                int rs = pg_huff(g, 1, g->c[k0].ta), r, s; if (rs < 0) return -1;
                r = rs >> 4; s = rs & 15;
                if (!s) { if (r < 15) { g->eobrun = (1u << r) - 1u; if (r) g->eobrun += (uint32_t)pg_bits(g, (uint32_t)r); break; } k += 15; continue; }
                k += (uint32_t)r; if (k > 63) return -1;
                int ai = (k <= 24 && PG_SLOT[k] > 0) ? (k0 == g->ci[0] && g->ci[0] == 0 ? PG_SLOT[k] - 1 : PG_CAC[PG_SLOT[k]]) : -1;
                if (ai >= 0 && (uint32_t)ai < nac) {
                    int32_t v = pg_ext(pg_bits(g, (uint32_t)s), (uint32_t)s) * (1 << g->Al);
                    if (g->Al && v) v += v > 0 ? (1 << (g->Al - 1)) : -(1 << (g->Al - 1));      /* without refinement: the true value is in the middle of the interval */
                    b[ai] = (int8_t)(v > 127 ? 127 : (v < -127 ? -127 : v));
                } else pg_bits(g, (uint32_t)s);
            }
        }
        g->idx++;
    }
    return g->idx >= g->total ? 1 : 0;
}
/* 4x4 IDCT of an 8x8 block = average of each 2x2 cell from the coefficients (u,v) < 4 (the rest are 0): p(a,b) = 1/4 * sum C(u)C(v) F(u,v) m_u(a) m_v(b),
   m_u(a) = average of cos((2x+1)u*pi/16) over the 2 pixels of cell a. Table A[u][a] = 256*C(u)*m_u(a). */
static const int16_t PG_A[4][4] = {{181, 181, 181, 181}, {232, 96, -96, -232}, {167, -167, -167, 167}, {81, -197, 197, -81}};
static void pg_idct4(int16_t dc, const int8_t *ac, uint32_t luma, const uint16_t *q, uint8_t out[16]) {
    int32_t f[4][4], t[4][4], u, v, a, b;                                  /* f[v][u] */
    for (v = 0; v < 4; v++) for (u = 0; u < 4; u++) f[v][u] = 0;
    f[0][0] = dc * (int32_t)q[0];
    if (luma) { for (v = 0; v < 4; v++) for (u = 0; u < 4; u++) { int s = v * 4 + u; if (s) f[v][u] = ac[s - 1] * (int32_t)q[s]; } }
    else { f[0][1] = ac[0] * (int32_t)q[1]; f[1][0] = ac[1] * (int32_t)q[4]; f[1][1] = ac[2] * (int32_t)q[5]; }
    for (v = 0; v < 4; v++) for (a = 0; a < 4; a++) { int32_t s = 0; for (u = 0; u < 4; u++) s += PG_A[u][a] * f[v][u]; t[v][a] = s >> 6; }
    for (b = 0; b < 4; b++) for (a = 0; a < 4; a++) {
        int32_t s = 0, r; for (v = 0; v < 4; v++) s += PG_A[v][b] * t[v][a];
        r = ((s + (1 << 11)) >> 12) + 128; out[b * 4 + a] = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
    }
}
static int pg_render_mcu(ns_cv *cv) {
    ns_pg *g = &cv->pg; uint32_t pw = g->hmax * 4u, ph = g->vmax * 4u, i, j, k, hx, vy; uint8_t bm[8 * 8 * 3]; JRECT r;
    uint8_t blk[3][2][2][16];                                              /* [component][block row][block column][4x4] */
    for (k = 0; k < g->nc; k++) for (vy = 0; vy < g->c[k].v; vy++) for (hx = 0; hx < g->c[k].h; hx++) {
        uint32_t bx = g->mx_i * g->c[k].h + hx, by = g->my_i * g->c[k].v + vy, bi = by * g->c[k].bw + bx;
        pg_idct4(g->c[k].dc[bi], g->c[k].ac + bi * g->c[k].nac, k == 0, g->qt[g->c[k].tq], blk[k][vy][hx]);
    }
    for (j = 0; j < ph; j++) for (i = 0; i < pw; i++) {
        int v[3] = {128, 128, 128};
        for (k = 0; k < g->nc; k++) {
            uint32_t cx = i * g->c[k].h / g->hmax, cy = j * g->c[k].v / g->vmax;            /* component sample (4x4 grid per block) */
            v[k] = blk[k][cy >> 2][cx >> 2][(cy & 3) * 4 + (cx & 3)];
        }
        {   int y = v[0], cb = v[1] - 128, cr = v[2] - 128, R, G, B;
            if (g->nc == 1) { cb = cr = 0; }
            R = y + ((359 * cr) >> 8); G = y - ((88 * cb + 183 * cr) >> 8); B = y + ((454 * cb) >> 8);
            bm[(j * pw + i) * 3] = (uint8_t)(R < 0 ? 0 : R > 255 ? 255 : R); bm[(j * pw + i) * 3 + 1] = (uint8_t)(G < 0 ? 0 : G > 255 ? 255 : G); bm[(j * pw + i) * 3 + 2] = (uint8_t)(B < 0 ? 0 : B > 255 ? 255 : B); }
    }
    r.left = (uint16_t)(g->mx_i * pw); r.top = (uint16_t)(g->my_i * ph); r.right = (uint16_t)(r.left + pw - 1); r.bottom = (uint16_t)(r.top + ph - 1);
    if (r.left >= cv->Ws || r.top >= cv->Hs) return 0;
    if (r.right >= cv->Ws) { /* crop: repack the visible columns */
        uint32_t w2 = cv->Ws - r.left, jj, ii; uint8_t tmp[8 * 8 * 3];
        for (jj = 0; jj < ph; jj++) for (ii = 0; ii < w2; ii++) { tmp[(jj * w2 + ii) * 3] = bm[(jj * pw + ii) * 3]; tmp[(jj * w2 + ii) * 3 + 1] = bm[(jj * pw + ii) * 3 + 1]; tmp[(jj * w2 + ii) * 3 + 2] = bm[(jj * pw + ii) * 3 + 2]; }
        r.right = (uint16_t)(cv->Ws - 1); if (r.bottom >= cv->Hs) r.bottom = (uint16_t)(cv->Hs - 1);
        for (jj = 0; jj < w2 * ph * 3; jj++) bm[jj] = tmp[jj];
        cv->err = 0; cv_acc(cv, bm, &r); return cv->err ? -1 : 0;
    }
    if (r.bottom >= cv->Hs) r.bottom = (uint16_t)(cv->Hs - 1);
    cv->err = 0; cv_acc(cv, bm, &r); return cv->err ? -1 : 0;
}
static int pg_step(ns_cv *cv, uint32_t n) {
    ns_pg *g = &cv->pg; uint32_t t;
    while (n--) {
        if (g->phase == 0) { int r = pg_segment(cv, cv->pg.aux[0], cv->pg.auxsz[0]); if (r == 2) g->phase = g->skip ? 3 : 1; else if (r == 1) { if (!g->sof) return -1; g->phase = 2; g->mx_i = g->my_i = 0; } else if (r < 0) return -1; }
        else if (g->phase == 1) { int r = pg_scan(cv, 24); if (r < 0) return -1; if (r == 1) g->phase = 3; }
        else if (g->phase == 3) { g->phase = 0; }                                    /* scan ignored or finished: continue to the next marker */
        else {                                                                       /* render one MCU */
            if (pg_render_mcu(cv) < 0) return -1;
            g->mx_i++;
            if (g->mx_i >= g->mcux) {
                uint32_t y0; g->mx_i = 0; g->my_i++;
                if (g->my_i >= g->mcuy) { for (t = 0; t < NS_FMT_N; t++) cv_flush(cv, t, cv->t[t].Hd); return 1; }
                y0 = g->my_i * g->vmax * 4u; for (t = 0; t < NS_FMT_N; t++) cv_flush(cv, t, y0 * cv->t[t].Hd / cv->Hs);
            }
        }
    }
    return 0;
}
int ns_cv_step(ns_cv *c, uint32_t nmcu) {
    uint32_t t;
    if (c->prog) return pg_step(c, nmcu);
    while (nmcu--) {
        JRESULT rc;
        if (c->jd.nrst && c->jd.rst++ == c->jd.nrst) { rc = jd_restart(&c->jd, c->jd.rsc++); if (rc != JDR_OK) return -1; c->jd.rst = 1; }
        rc = jd_mcu_load(&c->jd); if (rc != JDR_OK) return -1;
        c->err = 0; rc = jd_mcu_output(&c->jd, cv_out, c->x, c->y); if (rc != JDR_OK || c->err) return -1;
        c->x += c->mx;
        if (c->x >= c->W0) {
            c->x = 0; c->y += c->my;
            if (c->y >= c->H0) { for (t = 0; t < NS_FMT_N; t++) cv_flush(c, t, c->t[t].Hd); return 1; }
            { uint32_t y0 = c->y >> c->sc; for (t = 0; t < NS_FMT_N; t++) cv_flush(c, t, y0 * c->t[t].Hd / c->Hs); }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------ ArtworkDB */
typedef struct { uint32_t next_id, maxid, off[4], tl[4], hl[4]; uint32_t have; } adb_info;
static int adb_parse(const uint8_t *d, uint32_t n, adb_info *o) {
    uint32_t hl, off, i; zero(o, sizeof *o);
    if (n < 0x20 || d[0] != 'm' || d[1] != 'h' || d[2] != 'f' || d[3] != 'd') return -1;
    hl = le32(d + 4); if (hl > n) return -1;
    o->next_id = le32(d + 0x1C); off = hl;
    while (off + 16 <= n) {
        uint32_t shl, stl, typ, lst, cnt, cc;
        if (!(d[off] == 'm' && d[off + 1] == 'h' && d[off + 2] == 's' && d[off + 3] == 'd')) return -1;
        shl = le32(d + off + 4); stl = le32(d + off + 8); typ = le32(d + off + 12);
        if (stl < shl || off + stl > n || typ < 1 || typ > 3) return -1;
        o->off[typ] = off; o->tl[typ] = stl; o->hl[typ] = shl; o->have |= 1u << typ;
        lst = off + shl; if (lst + 12 > n) return -1;
        cnt = le32(d + lst + 8); cc = lst + le32(d + lst + 4);
        for (i = 0; i < cnt; i++) {
            uint32_t tl; if (cc + 24 > n) return -1; tl = le32(d + cc + 8); if (tl < 16 || cc + tl > n) return -1;
            if (typ == 1) { uint32_t id = le32(d + cc + 0x10); if (id > o->maxid) o->maxid = id; }
            else if (typ == 3) { uint32_t fid = le32(d + cc + 0x10), sz = le32(d + cc + 0x14), k; for (k = 0; k < NS_FMT_N; k++) if (NS_FMT[k].id == fid) { if (sz != NS_FMT[k].psize) return -2; o->maxid = o->maxid; } }
            cc += tl;
        }
        off += stl;
    }
    return 0;
}
int ns_adb_info(const uint8_t *old, uint32_t old_len, uint32_t *next_id, uint32_t *max_img_id) {
    adb_info a; int r;
    if (!old || !old_len) { *next_id = 0; *max_img_id = 0; return 0; }
    r = adb_parse(old, old_len, &a); if (r) return r;
    if (!(a.have & 2u) || !(a.have & 8u)) return -1;
    *next_id = a.next_id; *max_img_id = a.maxid; return 0;
}
static uint32_t hdr(uint8_t *o, const char *tag, uint32_t hl, uint32_t tl, uint32_t f0c) { zero(o, hl); o[0] = (uint8_t)tag[0]; o[1] = (uint8_t)tag[1]; o[2] = (uint8_t)tag[2]; o[3] = (uint8_t)tag[3]; wl32(o + 4, hl); wl32(o + 8, tl); wl32(o + 12, f0c); return hl; }
#define MHII_LEN (152u + NS_FMT_N * (24u + 76u + 64u) + 120u)
static uint32_t put_mhii(uint8_t *o, uint32_t id, uint64_t owner, uint32_t srcsz, const uint32_t offs[NS_FMT_N][8], uint32_t j) {
    uint32_t p = hdr(o, "mhii", 152, MHII_LEN, NS_FMT_N + 1), k, i;
    wl32(o + 0x10, id); wl64(o + 0x14, owner); wl32(o + 0x30, srcsz);
    for (k = 0; k < NS_FMT_N; k++) {
        uint8_t *c = o + p, *m = c + 24, *s = m + 76; char nm[16]; uint32_t nl = 0, f = NS_FMT[k].id;
        hdr(c, "mhod", 24, 24 + 76 + 64, 2);
        hdr(m, "mhni", 76, 76 + 64, 1);
        wl32(m + 0x10, f); wl32(m + 0x14, offs[k][j]); wl32(m + 0x18, NS_FMT[k].psize); wl32(m + 0x1C, 0); wl32(m + 0x20, (uint32_t)NS_FMT[k].h | ((uint32_t)NS_FMT[k].w << 16)); wl32(m + 0x28, NS_FMT[k].psize);
        hdr(s, "mhod", 24, 24 + 12 + 28, 3); wl32(s + 24, 28); wl32(s + 28, 2); wl32(s + 32, 0);
        nm[nl++] = ':'; nm[nl++] = 'F'; nm[nl++] = (char)('0' + f / 1000); nm[nl++] = (char)('0' + f / 100 % 10); nm[nl++] = (char)('0' + f / 10 % 10); nm[nl++] = (char)('0' + f % 10);
        nm[nl++] = '_'; nm[nl++] = '1'; nm[nl++] = '.'; nm[nl++] = 'i'; nm[nl++] = 't'; nm[nl++] = 'h'; nm[nl++] = 'm'; nm[nl++] = 'b';
        for (i = 0; i < nl; i++) { s[36 + 2 * i] = (uint8_t)nm[i]; s[37 + 2 * i] = 0; }
        p += 24 + 76 + 64;
    }
    hdr(o + p, "mhod", 24, 120, 6); zero(o + p + 24, 96); o[p + 24] = 'm'; o[p + 25] = 'h'; o[p + 26] = 'a'; o[p + 27] = 'f'; wl32(o + p + 28, 96); wl32(o + p + 32, 60);
    return MHII_LEN;
}
static uint32_t put_mhif(uint8_t *o, uint32_t k) { hdr(o, "mhif", 124, 124, 0); wl32(o + 0x10, NS_FMT[k].id); wl32(o + 0x14, NS_FMT[k].psize); return 124; }
static uint32_t put_ds(uint8_t *o, uint32_t typ, const char *ltag, uint32_t count, uint32_t bodylen) {
    hdr(o, "mhsd", 96, 96 + 92 + bodylen, typ); hdr(o + 96, ltag, 92, count, 0); wl32(o + 96 + 8, count); return 96 + 92;
}
uint32_t ns_adb_build(const uint8_t *old, uint32_t old_len, uint32_t n, const uint32_t *ids, const uint64_t *owner, const uint32_t *srcsz,
                      const uint32_t offs[NS_FMT_N][8], uint32_t next_id, uint8_t *out, uint32_t cap) {
    uint32_t j, k, pos;
    if (!old || !old_len) {                                                     /* create */
        uint32_t imgs = n * MHII_LEN, fm = NS_FMT_N * 124u, total = 132u + (96 + 92 + imgs) + (96 + 92) + (96 + 92 + fm);
        if (total > cap) return 0;
        hdr(out, "mhfd", 132, total, 0); wl32(out + 0x10, 6); wl32(out + 0x14, 3); wl32(out + 0x18, 0); wl32(out + 0x1C, next_id); wl32(out + 0x30, 2);
        pos = 132; pos += put_ds(out + pos, 1, "mhli", n, imgs);
        for (j = 0; j < n; j++) pos += put_mhii(out + pos, ids[j], owner[j], srcsz[j], offs, j);
        pos += put_ds(out + pos, 2, "mhla", 0, 0);
        pos += put_ds(out + pos, 3, "mhlf", NS_FMT_N, fm);
        for (k = 0; k < NS_FMT_N; k++) pos += put_mhif(out + pos, k);
        return pos;
    } else {                                                                    /* append to the existing one */
        adb_info a; uint32_t addf = 0, nf = 0, add1 = n * MHII_LEN, p1, p3, tot; uint8_t *tmp;
        if (adb_parse(old, old_len, &a) || !(a.have & 2u) || !(a.have & 8u)) return 0;
        {   /* which MHIF are missing */
            uint32_t lst = a.off[3] + a.hl[3], cnt = le32(old + lst + 8), cc = lst + le32(old + lst + 4), i, mask = 0;
            for (i = 0; i < cnt; i++) { uint32_t fid = le32(old + cc + 0x10); for (k = 0; k < NS_FMT_N; k++) if (NS_FMT[k].id == fid) mask |= 1u << k; cc += le32(old + cc + 8); }
            for (k = 0; k < NS_FMT_N; k++) if (!(mask & (1u << k))) { addf += 124; nf++; }
            (void)tmp;
            tot = old_len + add1 + addf; if (tot > cap) return 0;
            cpy(out, old, old_len);
            /* dataset 3 first (it comes later), then dataset 1 */
            if (a.off[3] > a.off[1]) { p3 = a.off[3] + a.tl[3]; p1 = a.off[1] + a.tl[1]; } else { return 0; }
            move(out + p3 + addf, out + p3, old_len - p3);
            { uint32_t w = p3; for (k = 0; k < NS_FMT_N; k++) if (!(mask & (1u << k))) w += put_mhif(out + w, k);
              wl32(out + a.off[3] + a.hl[3] + 8, le32(out + a.off[3] + a.hl[3] + 8) + nf); wl32(out + a.off[3] + 8, a.tl[3] + addf); }
            move(out + p1 + add1, out + p1, old_len + addf - p1);
            { uint32_t w = p1; for (j = 0; j < n; j++) w += put_mhii(out + w, ids[j], owner[j], srcsz[j], offs, j);
              wl32(out + a.off[1] + a.hl[1] + 8, le32(out + a.off[1] + a.hl[1] + 8) + n); wl32(out + a.off[1] + 8, a.tl[1] + add1); }
            /* dataset 3 was shifted by the block of dataset 1: only adjust the total size and the next id */
            wl32(out + 8, tot); wl32(out + 0x1C, next_id);
            return tot;
        }
    }
}
