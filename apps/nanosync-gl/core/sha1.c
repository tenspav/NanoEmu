#include "sha1.h"
#define ROL(x,n) (((x)<<(n))|((x)>>(32-(n))))
static void blk(uint32_t *h, const uint8_t *p) {
    uint32_t w[80], a, b, c, d, e, t; int i;
    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4*i]<<24 | (uint32_t)p[4*i+1]<<16 | (uint32_t)p[4*i+2]<<8 | p[4*i+3];
    for (; i < 80; i++) { t = w[i-3]^w[i-8]^w[i-14]^w[i-16]; w[i] = ROL(t,1); }
    a=h[0];b=h[1];c=h[2];d=h[3];e=h[4];
    for (i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i<20)      { f=(b&c)|(~b&d); k=0x5A827999u; }
        else if (i<40) { f=b^c^d;        k=0x6ED9EBA1u; }
        else if (i<60) { f=(b&c)|(b&d)|(c&d); k=0x8F1BBCDCu; }
        else           { f=b^c^d;        k=0xCA62C1D6u; }
        t = ROL(a,5)+f+e+k+w[i]; e=d; d=c; c=ROL(b,30); b=a; a=t;
    }
    h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;
}
void mdb_sha1_init(mdb_sha1_t *c) {
    c->h[0]=0x67452301u;c->h[1]=0xEFCDAB89u;c->h[2]=0x98BADCFEu;c->h[3]=0x10325476u;c->h[4]=0xC3D2E1F0u;
    c->n=0; c->used=0;
}
void mdb_sha1_update(mdb_sha1_t *c, const void *d, size_t n) {
    const uint8_t *p = d; c->n += n;
    while (n) {
        uint32_t k = 64 - c->used; if (k > n) k = (uint32_t)n;
        for (uint32_t i = 0; i < k; i++) c->buf[c->used+i] = p[i];
        c->used += k; p += k; n -= k;
        if (c->used == 64) { blk(c->h, c->buf); c->used = 0; }
    }
}
void mdb_sha1_final(mdb_sha1_t *c, uint8_t out[20]) {
    uint64_t bits = c->n * 8; uint8_t pad = 0x80, z = 0; int i;
    mdb_sha1_update(c, &pad, 1);
    while (c->used != 56) mdb_sha1_update(c, &z, 1);
    for (i = 7; i >= 0; i--) { uint8_t b = (uint8_t)(bits >> (8*i)); mdb_sha1_update(c, &b, 1); }
    for (i = 0; i < 5; i++) { out[4*i]=c->h[i]>>24; out[4*i+1]=c->h[i]>>16; out[4*i+2]=c->h[i]>>8; out[4*i+3]=c->h[i]; }
}
void mdb_sha1(const void *d, size_t n, uint8_t out[20]) {
    mdb_sha1_t c; mdb_sha1_init(&c); mdb_sha1_update(&c, d, n); mdb_sha1_final(&c, out);
}
