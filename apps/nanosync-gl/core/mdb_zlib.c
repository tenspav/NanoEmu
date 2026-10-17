#include "mdb_zlib.h"

uint32_t mdb_adler32(const uint8_t *p, size_t n) {
    uint32_t a = 1, b = 0;
    while (n) {
        size_t k = n > 5552 ? 5552 : n; n -= k;
        while (k--) { a += *p++; b += a; }
        a %= 65521; b %= 65521;
    }
    return (b << 16) | a;
}

size_t mdb_zstore_bound(size_t n) { return 2 + n + 5 * (n / 65535 + 1) + 4; }
size_t mdb_zstore(const uint8_t *in, size_t n, uint8_t *out) {
    size_t o = 0, i = 0; uint32_t ad = mdb_adler32(in, n);
    out[o++] = 0x78; out[o++] = 0x01;
    do {
        size_t k = n - i > 65535 ? 65535 : n - i; int last = (i + k == n);
        out[o++] = (uint8_t)last; out[o++] = (uint8_t)k; out[o++] = (uint8_t)(k >> 8);
        out[o++] = (uint8_t)~k; out[o++] = (uint8_t)(~k >> 8);
        for (size_t j = 0; j < k; j++) out[o + j] = in[i + j];
        o += k; i += k;
    } while (i < n);
    out[o++] = ad >> 24; out[o++] = ad >> 16; out[o++] = ad >> 8; out[o++] = ad;
    return o;
}

/* ---- inflate (canonical Huffman by counts, puff style) ---- */
#define MAXBITS 15
typedef struct { uint16_t count[MAXBITS + 1]; uint16_t symbol[288]; } huff_t;
typedef struct { const uint8_t *in; size_t ilen, ipos; uint32_t bitbuf; int bitcnt; uint8_t *out; size_t ocap, opos; } st_t;

static int bits(st_t *s, int need) {
    long v = (long)s->bitbuf;
    while (s->bitcnt < need) {
        if (s->ipos >= s->ilen) return -1;
        v |= (long)s->in[s->ipos++] << s->bitcnt; s->bitcnt += 8;
    }
    s->bitbuf = (uint32_t)(v >> need); s->bitcnt -= need;
    return (int)(v & ((1L << need) - 1));
}
static int decode(st_t *s, const huff_t *h) {
    int code = 0, first = 0, index = 0, len;
    for (len = 1; len <= MAXBITS; len++) {
        int b = bits(s, 1); int cnt;
        if (b < 0) return -1;
        code |= b; cnt = h->count[len];
        if (code - cnt < first) return h->symbol[index + (code - first)];
        index += cnt; first += cnt; first <<= 1; code <<= 1;
    }
    return -2;
}
static int construct(huff_t *h, const uint8_t *length, int n) {
    int sym, len, left; uint16_t offs[MAXBITS + 1];
    for (len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (sym = 0; sym < n; sym++) h->count[length[sym]]++;
    if (h->count[0] == n) return 0;
    left = 1;
    for (len = 1; len <= MAXBITS; len++) { left <<= 1; left -= h->count[len]; if (left < 0) return left; }
    offs[1] = 0;
    for (len = 1; len < MAXBITS; len++) offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    for (sym = 0; sym < n; sym++) if (length[sym]) h->symbol[offs[length[sym]]++] = (uint16_t)sym;
    return left;
}
static const uint16_t LBASE[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const uint8_t LEXT[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const uint16_t DBASE[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const uint8_t DEXT[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static int codes(st_t *s, const huff_t *lc, const huff_t *dc) {
    int sym;
    for (;;) {
        sym = decode(s, lc);
        if (sym < 0) return -1;
        if (sym < 256) { if (s->opos >= s->ocap) return -2; s->out[s->opos++] = (uint8_t)sym; }
        else if (sym == 256) return 0;
        else {
            int len, d; size_t dist;
            sym -= 257; if (sym >= 29) return -1;
            len = bits(s, LEXT[sym]); if (len < 0) return -1; len += LBASE[sym];
            sym = decode(s, dc); if (sym < 0 || sym >= 30) return -1;
            d = bits(s, DEXT[sym]); if (d < 0) return -1; dist = (size_t)d + DBASE[sym];
            if (dist > s->opos) return -1;
            if (s->opos + (size_t)len > s->ocap) return -2;
            while (len--) { s->out[s->opos] = s->out[s->opos - dist]; s->opos++; }
        }
    }
}
static int fixed(st_t *s) {
    huff_t lc, dc; uint8_t l[288]; int i;
    for (i = 0; i < 144; i++) l[i] = 8;
    for (; i < 256; i++) l[i] = 9;
    for (; i < 280; i++) l[i] = 7;
    for (; i < 288; i++) l[i] = 8;
    construct(&lc, l, 288);
    for (i = 0; i < 30; i++) l[i] = 5;
    construct(&dc, l, 30);
    return codes(s, &lc, &dc);
}
static int dynamic(st_t *s) {
    static const uint8_t ORD[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    uint8_t lengths[320]; huff_t lc, dc; int nlen, ndist, ncode, i, err;
    nlen = bits(s, 5); ndist = bits(s, 5); ncode = bits(s, 4);
    if (nlen < 0 || ndist < 0 || ncode < 0) return -1;
    nlen += 257; ndist += 1; ncode += 4;
    if (nlen > 286 || ndist > 30) return -1;
    for (i = 0; i < ncode; i++) { int v = bits(s, 3); if (v < 0) return -1; lengths[ORD[i]] = (uint8_t)v; }
    for (; i < 19; i++) lengths[ORD[i]] = 0;
    if (construct(&lc, lengths, 19) != 0) return -1;
    i = 0;
    while (i < nlen + ndist) {
        int sym = decode(s, &lc), len, rep;
        if (sym < 0) return -1;
        if (sym < 16) lengths[i++] = (uint8_t)sym;
        else {
            len = 0;
            if (sym == 16) { if (i == 0) return -1; len = lengths[i - 1]; rep = bits(s, 2); if (rep < 0) return -1; rep += 3; }
            else if (sym == 17) { rep = bits(s, 3); if (rep < 0) return -1; rep += 3; }
            else { rep = bits(s, 7); if (rep < 0) return -1; rep += 11; }
            if (i + rep > nlen + ndist) return -1;
            while (rep--) lengths[i++] = (uint8_t)len;
        }
    }
    if (lengths[256] == 0) return -1;
    err = construct(&lc, lengths, nlen);
    if (err && (err < 0 || nlen != lc.count[0] + lc.count[1])) return -1;
    err = construct(&dc, lengths + nlen, ndist);
    if (err && (err < 0 || ndist != dc.count[0] + dc.count[1])) return -1;
    return codes(s, &lc, &dc);
}

int mdb_inflate(const uint8_t *in, size_t ilen, uint8_t *out, size_t ocap, size_t *olen, size_t *consumed) {
    st_t s; int last, type, err = 0;
    s.in = in; s.ilen = ilen; s.ipos = 0; s.bitbuf = 0; s.bitcnt = 0; s.out = out; s.ocap = ocap; s.opos = 0;
    if (ilen < 6 || (in[0] & 0x0f) != 8 || ((in[0] << 8 | in[1]) % 31) != 0 || (in[1] & 0x20)) return -10;
    s.ipos = 2;
    do {
        last = bits(&s, 1); type = bits(&s, 2);
        if (last < 0 || type < 0) return -11;
        if (type == 0) {
            unsigned len, nlen;
            s.bitbuf = 0; s.bitcnt = 0;
            if (s.ipos + 4 > s.ilen) return -12;
            len = s.in[s.ipos] | s.in[s.ipos + 1] << 8; nlen = s.in[s.ipos + 2] | s.in[s.ipos + 3] << 8;
            s.ipos += 4;
            if (len != (~nlen & 0xffff)) return -12;
            if (s.ipos + len > s.ilen || s.opos + len > s.ocap) return -12;
            for (unsigned j = 0; j < len; j++) s.out[s.opos++] = s.in[s.ipos++];
        } else if (type == 1) err = fixed(&s);
        else if (type == 2) err = dynamic(&s);
        else return -13;
        if (err) return err < -1 ? -14 : -15;
    } while (!last);
    s.bitbuf = 0; s.bitcnt = 0;
    if (s.ipos + 4 > s.ilen) return -16;
    { uint32_t ad = (uint32_t)s.in[s.ipos] << 24 | (uint32_t)s.in[s.ipos+1] << 16 | (uint32_t)s.in[s.ipos+2] << 8 | s.in[s.ipos+3];
      if (ad != mdb_adler32(out, s.opos)) return -17; }
    s.ipos += 4;
    *olen = s.opos; if (consumed) *consumed = s.ipos;
    return 0;
}
