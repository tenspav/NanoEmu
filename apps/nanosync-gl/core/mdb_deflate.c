/* Simple deflate compressor: a single FIXED Huffman block + LZ77 (short chain). Integers only.
   Output: zlib stream (78 01 ... big-endian adler32), structurally like zlib level 1. */
#ifndef MDB_NO_MALLOC
#include <stdlib.h>
#endif
#include "mdb_deflate.h"
#include "mdb_zlib.h"

typedef struct { uint8_t *o; size_t pos, cap; uint32_t acc; int nb; int err; } bw_t;
static void put(bw_t *w, uint32_t v, int n) {            /* LSB first */
    w->acc |= v << w->nb; w->nb += n;
    while (w->nb >= 8) { if (w->pos >= w->cap) { w->err = 1; w->nb = 0; w->acc = 0; return; } w->o[w->pos++] = (uint8_t)w->acc; w->acc >>= 8; w->nb -= 8; }
}
static uint32_t rev(uint32_t c, int n) { uint32_t r = 0; while (n--) { r = (r << 1) | (c & 1); c >>= 1; } return r; }
static void huff(bw_t *w, uint32_t c, int n) { put(w, rev(c, n), n); }
static void lit(bw_t *w, unsigned s) {
    if (s < 144) huff(w, 0x30 + s, 8);
    else if (s < 256) huff(w, 0x190 + (s - 144), 9);
    else if (s < 280) huff(w, s - 256, 7);
    else huff(w, 0xC0 + (s - 280), 8);
}
static const uint16_t LB[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const uint8_t  LE[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const uint16_t DB[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const uint8_t  DE[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
static void match(bw_t *w, unsigned len, unsigned dist) {
    int i = 28, d = 29;
    while (LB[i] > len) i--;
    lit(w, 257 + i); if (LE[i]) put(w, len - LB[i], LE[i]);
    while (DB[d] > dist) d--;
    huff(w, d, 5); if (DE[d]) put(w, dist - DB[d], DE[d]);
}
size_t mdb_zdeflate_bound(size_t n) { return n + n / 8 + 64; }   /* worst case: 9 bits/literal */
#define HB 15
#define MAXCH 24
static uint32_t hash3(const uint8_t *p) { return (((uint32_t)p[0] << 10) ^ ((uint32_t)p[1] << 5) ^ p[2]) * 2654435761u >> (32 - HB); }
/* Same output as the malloc-based mdb_zdeflate, byte for byte, but without malloc: work >= MDB_DEFLATE_WORK bytes.
   The prev table is a ring of 32768 entries (the search already limited the distance to 32768). */
#define WMASK 32767u
size_t mdb_zdeflate_w(const uint8_t *in, size_t n, uint8_t *out, size_t cap, void *work, size_t worksz) {
    bw_t w; size_t i = 0; int32_t *head, *prev; uint32_t ad;
    if (!work || worksz < MDB_DEFLATE_WORK) return 0;
    head = (int32_t *)work; prev = head + (1u << HB);
    w.o = out; w.pos = 0; w.cap = cap; w.acc = 0; w.nb = 0; w.err = 0;
    for (i = 0; i < (1u << HB); i++) head[i] = -1;
    put(&w, 0x78, 8); put(&w, 0x01, 8);
    put(&w, 1, 1); put(&w, 1, 2);                  /* BFINAL=1 BTYPE=01 (fixed Huffman) */
    i = 0;
    while (i < n) {
        unsigned bl = 0, bd = 0; size_t adv, k;
        if (i + 3 <= n) {
            int32_t c = head[hash3(in + i)]; int ch = MAXCH;
            while (c >= 0 && ch-- && i - (size_t)c <= 32768) {
                unsigned l = 0, mx = n - i > 258 ? 258 : (unsigned)(n - i);
                while (l < mx && in[c + l] == in[i + l]) l++;
                if (l > bl) { bl = l; bd = (unsigned)(i - c); if (l == mx) break; }
                c = prev[(uint32_t)c & WMASK];
            }
            if (bl < 3) bl = 0;
        }
        adv = bl ? bl : 1;
        if (bl) match(&w, bl, bd); else lit(&w, in[i]);
        for (k = 0; k < adv; k++) {
            if (i + k + 3 <= n) { uint32_t h = hash3(in + i + k); prev[(uint32_t)(i + k) & WMASK] = head[h]; head[h] = (int32_t)(i + k); }
        }
        i += adv;
    }
    lit(&w, 256);
    if (w.nb) put(&w, 0, 8 - w.nb);
    ad = mdb_adler32(in, n);
    put(&w, ad >> 24, 8); put(&w, (ad >> 16) & 255, 8); put(&w, (ad >> 8) & 255, 8); put(&w, ad & 255, 8);
    return w.err ? 0 : w.pos;
}
#ifndef MDB_NO_MALLOC
size_t mdb_zdeflate(const uint8_t *in, size_t n, uint8_t *out, size_t cap) {
    void *wk = malloc(MDB_DEFLATE_WORK); size_t r;
    if (!wk) return 0;
    r = mdb_zdeflate_w(in, n, out, cap, wk, MDB_DEFLATE_WORK);
    free(wk); return r;
}
#endif
