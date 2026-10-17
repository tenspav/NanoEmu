#include "mdb_text.h"

/* ---- UTF-8 / UTF-16 ---- */
uint32_t mdb_u8next(const char **pp) {
    const uint8_t *p = (const uint8_t *)*pp; uint32_t c = *p;
    if (c < 0x80) { *pp += 1; return c; }
    if (c >= 0xC2 && c < 0xE0 && (p[1] & 0xC0) == 0x80) { *pp += 2; return ((c & 0x1F) << 6) | (p[1] & 0x3F); }
    if (c >= 0xE0 && c < 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *pp += 3; return ((c & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    }
    if (c >= 0xF0 && c < 0xF5 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        *pp += 4; return ((c & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) | ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    }
    *pp += 1; return 0xFFFD;
}
size_t mdb_u8put(char *o, uint32_t c) {
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) { o[0] = (char)(0xE0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 0x3F)); o[2] = (char)(0x80 | (c & 0x3F)); return 3; }
    o[0] = (char)(0xF0 | (c >> 18)); o[1] = (char)(0x80 | ((c >> 12) & 0x3F)); o[2] = (char)(0x80 | ((c >> 6) & 0x3F)); o[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}
size_t mdb_u8_to_u16le(const char *s, uint8_t *out) {
    size_t n = 0;
    while (*s) {
        uint32_t c = mdb_u8next(&s);
        if (c >= 0x10000) {
            c -= 0x10000;
            if (out) { uint32_t h = 0xD800 + (c >> 10), l = 0xDC00 + (c & 0x3FF);
                out[n] = (uint8_t)h; out[n+1] = (uint8_t)(h >> 8); out[n+2] = (uint8_t)l; out[n+3] = (uint8_t)(l >> 8); }
            n += 4;
        } else { if (out) { out[n] = (uint8_t)c; out[n+1] = (uint8_t)(c >> 8); } n += 2; }
    }
    return n;
}
size_t mdb_u16le_to_u8(const uint8_t *in, size_t nbytes, char *out) {
    size_t i = 0, n = 0;
    while (i + 1 < nbytes) {
        uint32_t c = in[i] | ((uint32_t)in[i+1] << 8); i += 2;
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < nbytes) {
            uint32_t l = in[i] | ((uint32_t)in[i+1] << 8);
            if (l >= 0xDC00 && l < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (l - 0xDC00); i += 2; }
        }
        if (out) n += mdb_u8put(out + n, c); else { char t[4]; n += mdb_u8put(t, c); }
    }
    if (out) out[n] = 0;
    return n;
}

/* ---- normalization: NFKD (Latin-1) + casefold (ASCII, Latin-1, Greek, Cyrillic) ---- */
/* base letter and combining mark of U+00C0..U+00FF; 0 = no decomposition */
#include "mdb_text_tab.h"
static uint32_t lower1(uint32_t c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c < 0x80) return c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    if (c >= 0x100 && c <= 0x17F && c != 0x130 && c != 0x131 && c != 0x138 && c != 0x149 && c != 0x17F && c != 0x178) {
        /* Latin Extended-A: even/odd pairs (even = upper case) and the 0x139..0x148 / 0x179..0x17E block (odd = upper case) */
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c + 1 : c;
        return (c & 1) ? c : c + 1;
    }
    return c;
}
static size_t put_cf(char *o, uint32_t c) {
    size_t n = 0;
    if (c == 0xDF) { o[0] = 's'; o[1] = 's'; return 2; }
    if (c == 0xB5) c = 0x3BC;
    if (c == 0x149) { size_t k = mdb_u8put(o, 0x2BC); o[k] = 'n'; return k + 1; }
    if (c == 0x178) c = 0xFF;
    if (c == 0x130) { o[0] = 'i'; return 1 + mdb_u8put(o + 1, 0x307); }
    return n + mdb_u8put(o, lower1(c));
}
static size_t put_cp_nfkd_cf(char *o, uint32_t c) {
    if (c >= DEC_LO && c < DEC_HI && DEC_TAB[c - DEC_LO][0]) {
        size_t n = 0; int k;
        for (k = 0; k < 3 && DEC_TAB[c - DEC_LO][k]; k++) n += put_cf(o + n, DEC_TAB[c - DEC_LO][k]);
        return n;
    }
    return put_cf(o, c);
}
static int casestart(const char *s, const char *art) {
    for (; *art; s++, art++) {
        char c = *s; if (c >= 'A' && c <= 'Z') c += 32;
        if (c != *art) return 0;
    }
    return 1;
}
size_t mdb_norm_key(const char *s, char *out) {
    size_t n = 0; size_t skip = 0;
    if (casestart(s, "the ")) skip = 4; else if (casestart(s, "an ")) skip = 3; else if (casestart(s, "a ")) skip = 2;
    s += skip;
    while (*s) {
        uint32_t c = mdb_u8next(&s);
        if (out) n += put_cp_nfkd_cf(out + n, c); else { char t[8]; n += put_cp_nfkd_cf(t, c); }
    }
    if (out) out[n] = 0;
    return n;
}
size_t mdb_casefold(const char *s, char *out) {
    size_t n = 0;
    while (*s) { uint32_t c = mdb_u8next(&s); if (out) n += put_cf(out + n, c); else { char t[8]; n += put_cf(t, c); } }
    if (out) out[n] = 0;
    return n;
}
static int is_alnum(uint32_t c) {
    if (c < 0x80) return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (c < 0xC0) return c == 0xB5 || c == 0xAA || c == 0xBA;
    if (c == 0xD7 || c == 0xF7) return 0;
    if (c >= 0x300 && c <= 0x36F) return 0;
    if ((c >= 0x2000 && c <= 0x2BFF) || (c >= 0x3000 && c <= 0x303F) || (c >= 0xFE00 && c <= 0xFE6F) || (c >= 0xFF00 && c <= 0xFF0F) || (c >= 0xD800 && c < 0xE000)) return 0;
    return 1;
}
uint32_t mdb_jump_letter(const char *key) {
    while (*key) {
        uint32_t c = mdb_u8next(&key);
        if (!is_alnum(c)) continue;
        if (c >= '0' && c <= '9') return '0';
        if (c >= 'a' && c <= 'z') return c - 32;
        if (c < 0x80) return c;
        if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
        if (c == 0xFF) return 0x178;
        if (c == 0xB5) return 0x39C;
        if (c == 0x131) return 'I';
        if (c == 0x17F) return 'S';
        if (c == 0x149) return 0x2BC;
        if (c >= 0x100 && c <= 0x17F && c != 0x138) {
            int lowerodd = !((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E));
            if (c == 0x130) return c;
            if ((c & 1) == (lowerodd ? 1 : 0)) return c - 1;   /* lower case -> upper case */
            return c;
        }
        if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 32;
        if (c == 0x3C2) return 0x3A3;
        if (c >= 0x430 && c <= 0x44F) return c - 32;
        if (c >= 0x450 && c <= 0x45F) return c - 80;
        if (c > 0xFFFF) return '0';
        return c;
    }
    return '0';
}
