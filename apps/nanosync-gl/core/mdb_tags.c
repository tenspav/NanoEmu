/* MP3 tag reader: ID3v2.2/2.3/2.4 + ID3v1 + MPEG frame header (Xing/Info/VBRI). Integers only.
   Tag interpretation follows the usual tagger conventions (aliases, priority, whitespace strip, numbers) as exposed by common
   tag libraries (upgrade to v2.4, genre expansion, ID3v1 at lowest priority). */
#include "mdb_tags.h"
#include "mdb_text.h"
#include "mdb_genres.h"

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint32_t synchsafe(const uint8_t *p) { return ((uint32_t)p[0] << 21) | ((uint32_t)p[1] << 14) | ((uint32_t)p[2] << 7) | p[3]; }
static uint32_t slen(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }
static int seq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int scmpi(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (int)(uint8_t)*a - (int)(uint8_t)*b; }
static void put(char *dst, const char *src) { uint32_t i = 0; while (src[i] && i < MDB_TAG_STR - 1) { dst[i] = src[i]; i++; } dst[i] = 0; }
static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

#define BIG 4096          /* short temporary text */
#define LONGCAP (1u << 17) /* comment/lyrics limit (UTF-8 bytes) */
#define MAXF 600
typedef struct {
    char id[5]; uint8_t enc, dead; const uint8_t *d; uint32_t n;   /* d,n = payload after the encoding byte (enc 0xFF = synthetic UTF-8; POPM: raw) */
    uint32_t toff;                                                 /* start of text (TXXX/COMM/USLT: after desc) */
    const char *desc; char lang[4];
} fr_t;
typedef struct {
    fr_t f[MAXF]; uint32_t nf; mdb_tags_t *t; char *xb; uint32_t xcap, xused;
} cx_t;

static char *xalloc(cx_t *c, uint32_t n) {
    char *p; if (c->xused + n > c->xcap) return 0;
    p = c->xb + c->xused; c->xused += n; return p;
}
static char *dupx(cx_t *c, const char *s) {
    char *p = xalloc(c, slen(s) + 1); uint32_t k; if (!p) return 0;
    for (k = 0; (p[k] = s[k]); k++) ;
    return p;
}

/* ---------- whitespace (Python str.strip semantics) */
static int is_ws(uint32_t c) {
    return (c >= 9 && c <= 13) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
           c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}
static int blank(const char *s) { while (*s) { const char *q = s; if (!is_ws(mdb_u8next(&q))) return 0; s = q; } return 1; }
static void strip(char *s) {
    const char *p = s, *start = 0, *end = s; char *o = s;
    while (*p) { const char *q = p; uint32_t c = mdb_u8next(&q); if (!is_ws(c)) { if (!start) start = p; end = q; } p = q; }
    if (!start) { s[0] = 0; return; }
    p = start; while (p < end) *o++ = *p++;
    *o = 0;
}

/* ---------- decode one terminated item starting at *pos; returns 1 if an item was produced */
static int dec_item(const uint8_t *d, uint32_t n, uint32_t *pos, uint8_t enc, char *out, uint32_t cap) {
    uint32_t i = *pos, o = 0;
    out[0] = 0;
    if (i >= n) return 0;
    if (enc == 0 || enc == 3 || enc == 0xFF) {
        for (; i < n && d[i]; i++) {
            if (enc == 0) { if (o + 4 < cap) o += (uint32_t)mdb_u8put(out + o, d[i]); }
            else if (o + 1 < cap) out[o++] = (char)d[i];
        }
        if (i < n) i++;
    } else {
        int be = (enc == 2);
        if (i + 1 < n && d[i] == 0xFF && d[i + 1] == 0xFE && enc == 1) { be = 0; i += 2; }
        else if (i + 1 < n && d[i] == 0xFE && d[i + 1] == 0xFF) { be = 1; i += 2; }
        while (i + 1 < n) {
            uint32_t c = be ? (uint32_t)(d[i] << 8 | d[i + 1]) : (uint32_t)(d[i] | d[i + 1] << 8); i += 2;
            if (!c) break;
            if (c >= 0xD800 && c < 0xDC00 && i + 1 < n) {
                uint32_t l = be ? (uint32_t)(d[i] << 8 | d[i + 1]) : (uint32_t)(d[i] | d[i + 1] << 8);
                if (l >= 0xDC00 && l < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (l - 0xDC00); i += 2; }
            }
            if (o + 8 < cap) o += (uint32_t)mdb_u8put(out + o, c);
        }
        if (i < n && i + 1 >= n) i = n;
    }
    out[o] = 0; *pos = i; return 1;
}
/* first non-empty item of the frame text (strip if dostrip) */
static int frame_text(const fr_t *f, int dostrip, char *out, uint32_t cap) {
    uint32_t pos = f->toff;
    while (dec_item(f->d, f->n, &pos, f->enc, out, cap)) {
        if (!blank(out)) { if (dostrip) strip(out); return 1; }
    }
    out[0] = 0; return 0;
}

/* ---------- genre (TCON genre expansion, two passes) */
#define NGEN ((uint32_t)(sizeof MDB_GENRES / sizeof MDB_GENRES[0]))
static int isdig(const char *s) { if (!*s) return 0; for (; *s; s++) if (*s < '0' || *s > '9') return 0; return 1; }
static const char *gname(const char *id) {
    if (seq(id, "CR")) return "Cover";
    if (seq(id, "RX")) return "Remix";
    if (isdig(id) && slen(id) < 9) { uint32_t v = 0; const char *p = id; while (*p) v = v * 10 + (uint32_t)(*p++ - '0'); if (v < NGEN) return MDB_GENRES[v]; }
    return "Unknown";
}
typedef struct { char v[8][256]; uint32_t n; } glist_t;
static void gadd(glist_t *l, const char *s) { if (l->n < 8) put(l->v[l->n++], s); }
static void genres_of(const glist_t *in, glist_t *out) {
    uint32_t k;
    out->n = 0;
    for (k = 0; k < in->n; k++) {
        const char *v = in->v[k];
        if (isdig(v) && slen(v) < 9) {
            uint32_t x = 0; const char *p = v; while (*p) x = x * 10 + (uint32_t)(*p++ - '0');
            if (x < 256) { gadd(out, x < NGEN ? MDB_GENRES[x] : "Unknown"); continue; }
        }
        if (seq(v, "CR")) { gadd(out, "Cover"); continue; }
        if (seq(v, "RX")) { gadd(out, "Remix"); continue; }
        if (v[0]) {
            const char *p = v; char ids[8][16]; uint32_t nn = 0, base = out->n, j; char rest[256];
            while (p[0] == '(') {
                const char *q = p + 1; uint32_t l = 0; char id[16];
                while (*q && *q != ')' && l < 15) id[l++] = *q++;
                id[l] = 0;
                if (*q != ')' || !(isdig(id) || seq(id, "RX") || seq(id, "CR"))) break;
                if (nn < 8) { for (j = 0; id[j]; j++) ids[nn][j] = id[j]; ids[nn][j] = 0; nn++; }
                p = q + 1;
            }
            for (j = 0; j < nn; j++) gadd(out, gname(ids[j]));
            { uint32_t r = 0; while (p[r] && p[r] != '\n' && r < 255) { rest[r] = p[r]; r++; } rest[r] = 0; }   /* the optional text group does not cross \n */
            if (rest[0]) {
                const char *gn = rest[0] == '(' ? rest + 1 : rest; int dup = 0;
                for (j = base; j < out->n; j++) if (seq(out->v[j], gn)) dup = 1;
                if (!dup) gadd(out, gn);
            }
        }
    }
}
static int genre_first(const fr_t *f, char *out) {
    glist_t a, b, c2; uint32_t pos = f->toff; char tmp[256];
    a.n = 0;
    while (a.n < 8 && dec_item(f->d, f->n, &pos, f->enc, tmp, sizeof tmp)) gadd(&a, tmp);
    genres_of(&a, &b);          /* re-assigning the genres re-escapes them; the net effect is one pass */
    (void)c2;
    if (!b.n) return 0;
    put(out, b.v[0]); return 1;
}

/* ---------- frame lookup (key/name semantics) */
static void normkey(const char *s, char *o) {      /* remove ' ', '_' and '-' after casefold (ASCII) */
    for (; *s; s++) { if (*s == ' ' || *s == '_' || *s == '-') continue; *o++ = lc(*s); }
    *o = 0;
}
static int plain(const fr_t *f) { return !f->dead && f->id[0] == 'T' && !seq(f->id, "TXXX"); }
static void keyof(const fr_t *f, char *key) {
    uint32_t k = 0; const char *p = seq(f->id, "TXXX") ? "TXXX:" : f->id;
    for (; p[k]; k++) key[k] = p[k];
    if (seq(f->id, "TXXX")) { uint32_t m = 0; while (f->desc[m] && k < 290) key[k++] = f->desc[m++]; }
    key[k] = 0;
}
static int find_alias(cx_t *c, const char *alias) {
    char na[300], nk[300], key[300], bestkey[300]; int best = -1; uint32_t i;
    if (slen(alias) >= 280) return -1;
    for (i = c->nf; i-- > 0;) if (plain(&c->f[i]) && seq(c->f[i].id, alias)) return (int)i;       /* alias among the keys */
    normkey(alias, na);
    for (i = 0; i < c->nf; i++) {         /* lookup by normalized key: first in key order */
        const fr_t *f = &c->f[i];
        if (f->dead || !(plain(f) || (seq(f->id, "TXXX") && f->desc && slen(f->desc) < 280))) continue;
        normkey(plain(f) ? f->id : f->desc, nk);
        if (!seq(nk, na)) continue;
        keyof(f, key);
        if (best < 0 || scmpi(key, bestkey) <= 0) { best = (int)i; { uint32_t k = 0; for (; key[k]; k++) bestkey[k] = key[k]; bestkey[k] = 0; } }
    }
    return best;
}
#define X(f, ...) static const char *const AL_##f[] = {__VA_ARGS__, 0};
#include "mdb_tags_alias.h"
#undef X
enum {
#define X(f, ...) F_##f,
#include "mdb_tags_alias.h"
#undef X
    F_N
};
static const char *const *const AL[F_N] = {
#define X(f, ...) AL_##f,
#include "mdb_tags_alias.h"
#undef X
};
/* field value: first alias with non-empty text (lyrics: no strip) */
static int field(cx_t *c, int fld, char *out, uint32_t cap) {
    const char *const *a = AL[fld]; out[0] = 0;
    for (; *a; a++) {
        int i = find_alias(c, *a);
        if (i < 0) continue;
        if (frame_text(&c->f[i], fld != F_lyrics, out, cap)) return 1;
    }
    return 0;
}

/* ---------- numeric/boolean/date interpretation */
static int numtxt(const char *s, uint32_t max, uint32_t *v) {      /* number: ascii digits only, <= 10 digits, <= max */
    uint64_t x = 0; uint32_t l = 0;
    if (!*s) return 0;
    for (; *s; s++, l++) { if (*s < '0' || *s > '9' || l >= 10) return 0; x = x * 10 + (uint32_t)(*s - '0'); }
    if (x > max) return 0;
    *v = (uint32_t)x; return 1;
}
static int flagtxt(const char *s, uint32_t *v) {
    char t[16]; uint32_t i;
    if (slen(s) > 6) return 0;
    for (i = 0; s[i]; i++) t[i] = lc(s[i]);
    t[i] = 0;
    if (seq(t, "1") || seq(t, "true") || seq(t, "yes")) { *v = 1; return 1; }
    if (seq(t, "0") || seq(t, "false") || seq(t, "no")) { *v = 0; return 1; }
    return 0;
}
static int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
    int64_t era, yoe, doy, doe; y -= m <= 2; era = (y >= 0 ? y : y - 399) / 400; yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; doe = yoe * 365 + yoe / 4 - yoe / 100 + doy; return era * 146097 + doe - 719468;
}
static int dig(const char *s, int n) { int i; for (i = 0; i < n; i++) if (s[i] < '0' || s[i] > '9') return 0; return 1; }
static int num_n(const char *s, int n) { int v = 0, i; for (i = 0; i < n; i++) v = v * 10 + (s[i] - '0'); return v; }
/* release date: YYYY | YYYY-MM | ISO 8601 subset (YYYY-MM-DD, YYYYMMDD, [T| ]HH[:MM[:SS[.f]]], Z/+HH:MM). 0 = invalid */
static int64_t release_date(const char *s) {
    int y, mo = 1, d = 1, hh = 0, mi = 0, ss = 0, sign = 0, th = 0, tm = 0; const char *p = s; int64_t secs, off;
    uint32_t l = slen(s);
    if (l == 4 && dig(s, 4)) { y = num_n(s, 4); goto have; }
    if (l == 7 && dig(s, 4) && s[4] == '-' && dig(s + 5, 2)) { y = num_n(s, 4); mo = num_n(s + 5, 2); goto have; }
    if (l < 8 || !dig(s, 4)) return 0;
    y = num_n(s, 4);
    if (s[4] == '-') { if (!(dig(s + 5, 2) && s[7] == '-' && dig(s + 8, 2))) return 0; mo = num_n(s + 5, 2); d = num_n(s + 8, 2); p = s + 10; }
    else { if (!dig(s + 4, 4)) return 0; mo = num_n(s + 4, 2); d = num_n(s + 6, 2); p = s + 8; }
    if (*p) {
        if (*p != 'T' && *p != ' ' && *p != 't') return 0;
        p++; if (!dig(p, 2)) return 0; hh = num_n(p, 2); p += 2;
        if (*p == ':') { p++; if (!dig(p, 2)) return 0; mi = num_n(p, 2); p += 2;
            if (*p == ':') { p++; if (!dig(p, 2)) return 0; ss = num_n(p, 2); p += 2; if (*p == '.') { p++; if (!dig(p, 1)) return 0; while (dig(p, 1)) p++; } } }
        if (*p == 'Z' || *p == 'z') p++;
        else if (*p == '+' || *p == '-') { sign = *p == '-' ? -1 : 1; p++; if (!dig(p, 2)) return 0; th = num_n(p, 2); p += 2; if (*p == ':') p++; if (dig(p, 2)) { tm = num_n(p, 2); p += 2; } }
        if (*p) return 0;
    }
have:
    if (y < 1 || mo < 1 || mo > 12 || d < 1 || hh > 23 || mi > 59 || ss > 59) return 0;
    { static const int dm[12] = {31,28,31,30,31,30,31,31,30,31,30,31}; int dim = dm[mo - 1]; if (mo == 2 && ((y % 4 == 0 && y % 100) || y % 400 == 0)) dim = 29; if (d > dim) return 0; }
    off = (int64_t)sign * (th * 3600 + tm * 60);
    secs = days_from_civil(y, mo, d) * 86400 + hh * 3600 + mi * 60 + ss - off;
    if (secs <= -2082844800LL || secs > 2212122495LL) return 0;
    return secs;
}

/* ---------- frame collection */
static const char *const V22[][2] = {
    {"TT2","TIT2"},{"TP1","TPE1"},{"TAL","TALB"},{"TCO","TCON"},{"TYE","TYER"},{"TRK","TRCK"},{"TPA","TPOS"},{"TCM","TCOM"},{"TP2","TPE2"},
    {"COM","COMM"},{"ULT","USLT"},{"TXX","TXXX"},{"TBP","TBPM"},{"TT1","TIT1"},{"TT3","TIT3"},{"TCR","TCOP"},{"TPB","TPUB"},{"TCP","TCMP"},
    {"TDA","TDAT"},{"TIM","TIME"},{"POP","POPM"},{"TSP","TSOP"},{"TSA","TSOA"},{"TST","TSOT"},{"TS2","TSO2"},{"TSC","TSOC"},
    {"TP3","TPE3"},{"TEN","TENC"},{"TLA","TLAN"},{"TKE","TKEY"},{"TLE","TLEN"},{"TMT","TMED"},{"TOA","TOPE"},{"TOT","TOAL"},{"TRC","TSRC"},{"TXT","TEXT"},{"TOR","TORY"}
};
static fr_t *newfr(cx_t *c, const char *id) {
    fr_t *f; uint32_t k;
    if (c->nf >= MAXF) return 0;
    f = &c->f[c->nf++]; for (k = 0; k < 5; k++) f->id[k] = k < 4 ? id[k] : 0;
    f->dead = 0; f->desc = 0; f->lang[0] = 0; f->d = 0; f->n = 0; f->enc = 0; f->toff = 0; return f;
}
static void addframe(cx_t *c, const char *id, const uint8_t *d, uint32_t n) {
    fr_t *f; uint32_t pos; char tmp[BIG];
    int istxt = id[0] == 'T', ispopm = seq(id, "POPM"), iscl = seq(id, "COMM") || seq(id, "USLT");
    if (!(istxt || ispopm || iscl) || n < 1) return;
    if (!ispopm && d[0] > 3) return;
    if (!(f = newfr(c, id))) return;
    if (ispopm) { f->d = d; f->n = n; return; }
    f->enc = d[0]; f->d = d + 1; f->n = n - 1;
    if (seq(id, "TXXX")) {
        pos = 0; dec_item(f->d, f->n, &pos, f->enc, tmp, sizeof tmp);
        f->desc = dupx(c, tmp); f->toff = pos;
        if (!f->desc) c->nf--;
    } else if (iscl) {
        if (f->n < 3) { c->nf--; return; }
        f->lang[0] = (char)f->d[0]; f->lang[1] = (char)f->d[1]; f->lang[2] = (char)f->d[2]; f->lang[3] = 0;
        pos = 3; dec_item(f->d, f->n, &pos, f->enc, tmp, sizeof tmp);
        f->desc = dupx(c, tmp); f->toff = pos;
        if (!f->desc) c->nf--;
    }
}
static void synth(cx_t *c, const char *id, const char *txt) {      /* synthetic UTF-8 text frame */
    char *b = dupx(c, txt); fr_t *f;
    if (!b || !(f = newfr(c, id))) return;
    f->d = (const uint8_t *)b; f->n = slen(txt) + 1; f->enc = 0xFF;
}
static void parse_id3v2(cx_t *c, const uint8_t *b, size_t len, uint32_t tagsize) {
    uint8_t ver = b[3], flags = b[5]; size_t p = 10, end = 10 + tagsize;
    if (end > len) end = len;
    if (flags & 0x40 && ver >= 3) { uint32_t ext = ver == 4 ? synchsafe(b + p) : be32(b + p) + 4; p += ext; }
    while (p + (ver == 2 ? 6 : 10) <= end) {
        char id[5]; uint32_t sz, hdr = ver == 2 ? 6 : 10, k; const uint8_t *d; uint16_t fl = 0; int skip = 0;
        id[0] = (char)b[p]; id[1] = (char)b[p + 1]; id[2] = (char)b[p + 2]; id[3] = ver == 2 ? 0 : (char)b[p + 3]; id[4] = 0;
        if (!id[0]) break;
        sz = ver == 2 ? (uint32_t)(b[p + 3] << 16 | b[p + 4] << 8 | b[p + 5]) : ver == 4 ? synchsafe(b + p + 4) : be32(b + p + 4);
        if (ver != 2) fl = (uint16_t)(b[p + 8] << 8 | b[p + 9]);
        if (p + hdr + sz > end) break;
        d = b + p + hdr;
        if (ver == 2) {
            for (k = 0; k < sizeof V22 / sizeof V22[0]; k++)
                if (id[0] == V22[k][0][0] && id[1] == V22[k][0][1] && id[2] == V22[k][0][2]) { uint32_t m; for (m = 0; m < 4; m++) id[m] = V22[k][1][m]; id[4] = 0; break; }
        }
        if (ver == 4) { if (fl & 0x0001) { if (sz >= 4) { d += 4; sz -= 4; } else skip = 1; } if (fl & 0x000C) skip = 1; }    /* compression/encryption; per-frame unsync: ignored */
        if (ver == 3 && (fl & 0x00C0)) skip = 1;
        if (!skip) addframe(c, id, d, sz);
        p += hdr + sz;
    }
}
static void v1text(const uint8_t *s, uint32_t n, char *out) {
    uint32_t e = 0, i, o = 0; while (e < n && s[e]) e++;
    while (e > 0 && s[e - 1] == ' ') e--;
    for (i = 0; i < e; i++) o += (uint32_t)mdb_u8put(out + o, s[i]);
    out[o] = 0;
}
static int has_key(cx_t *c, const char *id) { uint32_t i; for (i = 0; i < c->nf; i++) if (!c->f[i].dead && seq(c->f[i].id, id)) return 1; return 0; }
static void parse_id3v1(cx_t *c, const uint8_t *t) {
    char tmp[200]; int v11 = t[125] == 0 && t[126] != 0;
    v1text(t + 3, 30, tmp); if (tmp[0] && !has_key(c, "TIT2")) synth(c, "TIT2", tmp);
    v1text(t + 33, 30, tmp); if (tmp[0] && !has_key(c, "TPE1")) synth(c, "TPE1", tmp);
    v1text(t + 63, 30, tmp); if (tmp[0] && !has_key(c, "TALB")) synth(c, "TALB", tmp);
    v1text(t + 93, 4, tmp); if (tmp[0] && !has_key(c, "TDRC") && !has_key(c, "TYER")) synth(c, "TDRC", tmp);
    if (v11 && !has_key(c, "TRCK")) { char n[8]; uint32_t v = t[126], l = 0, k; char r[8]; while (v) { r[l++] = (char)('0' + v % 10); v /= 10; } for (k = 0; k < l; k++) n[k] = r[l - 1 - k]; n[l] = 0; synth(c, "TRCK", n); }
    if (t[127] < NGEN && !has_key(c, "TCON")) synth(c, "TCON", MDB_GENRES[t[127]]);
    v1text(t + 97, v11 ? 28 : 30, tmp);
    if (tmp[0]) { fr_t *f = newfr(c, "COMM"); char *b = dupx(c, tmp); if (f && b) { f->desc = "ID3v1 Comment"; f->lang[0] = 'e'; f->lang[1] = 'n'; f->lang[2] = 'g'; f->d = (const uint8_t *)b; f->n = slen(tmp) + 1; f->enc = 0xFF; } else if (f) c->nf--; }
}
static void kill_ids(cx_t *c, const char *id) { uint32_t i; for (i = 0; i < c->nf; i++) if (seq(c->f[i].id, id)) c->f[i].dead = 1; }
static int first_of(cx_t *c, const char *id) { uint32_t i; int r = -1; for (i = 0; i < c->nf; i++) if (!c->f[i].dead && seq(c->f[i].id, id)) r = (int)i; return r; }
static void first_text(cx_t *c, int i, char *out, uint32_t cap) {
    uint32_t pos; out[0] = 0; if (i < 0) return;
    pos = c->f[i].toff; dec_item(c->f[i].d, c->f[i].n, &pos, c->f[i].enc, out, cap);     /* first item (empty ones are not skipped) */
}
/* upgrade to v2.4: TYER/TDAT/TIME -> TDRC (only if there is no TDRC) */
static void upd24(cx_t *c) {
    int iy = first_of(c, "TYER");
    if (iy >= 0 && first_of(c, "TDRC") < 0) {
        char ty[64], td[64], tt[64], ts[64]; uint32_t l = 0, k; int hastd, hastt;
        first_text(c, iy, ty, sizeof ty); first_text(c, first_of(c, "TDAT"), td, sizeof td); first_text(c, first_of(c, "TIME"), tt, sizeof tt);
        hastd = slen(td) == 4 && dig(td, 4); hastt = slen(tt) == 4 && dig(tt, 4);
        ts[0] = 0;
        if (slen(ty) == 4 && dig(ty, 4)) {
            for (k = 0; k < 4; k++) ts[l++] = ty[k];
            if (hastd) { ts[l++] = '-'; ts[l++] = td[2]; ts[l++] = td[3]; ts[l++] = '-'; ts[l++] = td[0]; ts[l++] = td[1]; }
            if (hastd && hastt) { ts[l++] = 'T'; ts[l++] = tt[0]; ts[l++] = tt[1]; ts[l++] = ':'; ts[l++] = tt[2]; ts[l++] = tt[3]; ts[l++] = ':'; ts[l++] = '0'; ts[l++] = '0'; }
        } else if (slen(ty) == 10 && dig(ty, 4) && ty[4] == '-' && dig(ty + 5, 2) && ty[7] == '-' && dig(ty + 8, 2)) {
            for (k = 0; k < 10; k++) ts[l++] = ty[k];
            if (hastd) { ts[5] = td[2]; ts[6] = td[3]; ts[8] = td[0]; ts[9] = td[1]; }
            if (hastt) { ts[l++] = 'T'; ts[l++] = tt[0]; ts[l++] = tt[1]; ts[l++] = ':'; ts[l++] = tt[2]; ts[l++] = tt[3]; ts[l++] = ':'; ts[l++] = '0'; ts[l++] = '0'; }
        }
        ts[l] = 0;
        if (ts[0]) synth(c, "TDRC", ts);
    }
    kill_ids(c, "TYER"); kill_ids(c, "TDAT"); kill_ids(c, "TIME");
}

/* COMM/USLT ordered by (has desc, desc, lang): first with non-empty text. skipitun ignores "itun*" comments. */
static int best_cl(cx_t *c, const char *id, int skipitun, char *out, uint32_t cap, int dostrip, uint32_t *count) {
    uint32_t i, cnt = 0; int best = -1; char tmp[BIG];
    out[0] = 0;
    for (i = 0; i < c->nf; i++) {
        const fr_t *f = &c->f[i];
        if (f->dead || !seq(f->id, id) || !f->desc) continue;
        cnt++;
        if (skipitun) { char l4[5]; uint32_t k; for (k = 0; k < 4 && f->desc[k]; k++) l4[k] = lc(f->desc[k]); l4[k] = 0; if (seq(l4, "itun")) continue; }
        if (!frame_text(f, dostrip, tmp, sizeof tmp)) continue;
        if (best >= 0) {
            const fr_t *g = &c->f[best]; int r = (f->desc[0] != 0) - (g->desc[0] != 0);
            if (!r) r = scmpi(f->desc, g->desc);
            if (!r) r = scmpi(f->lang, g->lang);
            if (r >= 0) continue;
        }
        best = i < 0x7fffffff ? (int)i : -1;
    }
    if (count) *count = cnt;
    if (best >= 0) frame_text(&c->f[best], dostrip, out, cap);
    return best;
}
static void note(mdb_tags_t *t, const char *name) {
    uint32_t n = slen(t->ignored), l = slen(name), k;
    if (n + l + 2 >= sizeof t->ignored) return;
    if (n) t->ignored[n++] = ',';
    for (k = 0; k <= l; k++) t->ignored[n + k] = name[k];
}

static void id3_interpret(cx_t *c) {
    mdb_tags_t *t = c->t; char *tmp, *lb; uint32_t v, i, cnt = 0; char date[64], tt[BIG];
    tmp = xalloc(c, LONGCAP); lb = xalloc(c, LONGCAP);
    if (!tmp || !lb) return;
    if (field(c, F_title, tt, 256)) put(t->title, tt);
    if (field(c, F_artist, tt, 256)) put(t->artist, tt);
    if (field(c, F_album, tt, 256)) put(t->album, tt);
    if (field(c, F_album_artist, tt, 256)) put(t->album_artist, tt);
    if (field(c, F_genre, tt, 256)) put(t->genre, tt);
    { int g = find_alias(c, "TCON"); char gg[256]; if (g >= 0 && genre_first(&c->f[g], gg)) put(t->genre, gg); }
    if (field(c, F_composer, tt, 256)) put(t->composer, tt);
    if (field(c, F_comment, tmp, LONGCAP)) t->comment = dupx(c, tmp);
    if (field(c, F_grouping, tmp, LONGCAP)) t->grouping = dupx(c, tmp);
    if (field(c, F_subtitle, tmp, LONGCAP)) t->subtitle = dupx(c, tmp);
    if (field(c, F_description, tmp, LONGCAP)) t->description = dupx(c, tmp);
    if (field(c, F_copyright, tmp, LONGCAP)) t->copyright = dupx(c, tmp);
    if (field(c, F_content_provider, tmp, LONGCAP)) t->content_provider = dupx(c, tmp);
    if (field(c, F_lyrics, tmp, LONGCAP)) t->lyrics = dupx(c, tmp);
    /* ID3: USLT and COMM (ordered by desc/lang) take priority over the aliases */
    best_cl(c, "USLT", 0, lb, LONGCAP, 0, &cnt);        /* cnt = number of USLT frames */
    if (lb[0]) t->lyrics = dupx(c, lb);
    t->lyrics_frames = cnt;
    if (best_cl(c, "COMM", 1, lb, LONGCAP, 1, 0) >= 0) t->comment = dupx(c, lb);
    /* date */
    date[0] = 0; if (field(c, F_date, tt, 63)) put(date, tt);
    if (date[0]) { char y4[5]; uint32_t y = 0; for (i = 0; i < 4 && date[i]; i++) y4[i] = date[i]; y4[i] = 0; if (numtxt(y4, 9999, &y)) t->year = y; }
    { char rd[64]; rd[0] = 0; if (field(c, F_release_date, tt, 63)) put(rd, tt); else put(rd, date); if (rd[0]) t->release_date = release_date(rd); }
    /* track / disc */
    {
        char tr[64], dc[64], trt[64], dct[64]; char *sl; uint32_t k;
        tr[0] = dc[0] = trt[0] = dct[0] = 0;
        if (field(c, F_track, tt, 63)) put(tr, tt);
        if (field(c, F_disc, tt, 63)) put(dc, tt);
        if (field(c, F_track_total, tt, 63)) put(trt, tt);
        if (field(c, F_disc_total, tt, 63)) put(dct, tt);
        for (sl = tr; *sl && *sl != '/'; sl++) ;
        if (*sl == '/') { char r[64]; *sl = 0; for (k = 0; sl[1 + k]; k++) r[k] = sl[1 + k]; r[k] = 0; strip(r); if (r[0]) put(trt, r); }
        for (sl = dc; *sl && *sl != '/'; sl++) ;
        if (*sl == '/') { char r[64]; *sl = 0; for (k = 0; sl[1 + k]; k++) r[k] = sl[1 + k]; r[k] = 0; strip(r); if (r[0]) put(dct, r); }
        strip(tr); strip(dc);
        if (numtxt(tr, 0xFFFFFFFFu, &v)) t->track = v;
        if (numtxt(dc, 0xFFFFFFFFu, &v)) t->disc = v;
        if (numtxt(trt, 0xFFFFFFFFu, &v)) t->total_tracks = v;
        if (numtxt(dct, 0xFFFFFFFFu, &v)) t->total_discs = v;
    }
    if (field(c, F_bpm, tt, 63)) {      /* "149.8" -> "149" (\d{1,5}\.\d+) */
        uint32_t l = slen(tt), k = 0; while (k < l && tt[k] >= '0' && tt[k] <= '9') k++;
        if (k >= 1 && k <= 5 && tt[k] == '.' && k + 1 < l && dig(tt + k + 1, (int)(l - k - 1))) tt[k] = 0;
        if (numtxt(tt, 65535, &v)) t->bpm = v;
    }
    if (field(c, F_compilation, tt, 63) && flagtxt(tt, &v)) t->compilation = v;
    if (field(c, F_gapless_album, tt, 63) && flagtxt(tt, &v)) t->gapless_album = v;
    if (field(c, F_content_advisory, tt, 63)) {
        char l2[16]; uint32_t k; for (k = 0; tt[k] && k < 15; k++) l2[k] = lc(tt[k]);
        l2[k] = 0;
        if (seq(l2, "1") || seq(l2, "4") || seq(l2, "explicit")) t->explicit_flag = 1;
        else if (seq(l2, "2") || seq(l2, "clean")) t->explicit_flag = 2;
    }
    {   /* POPM -> rating (ordered by email) */
        char beml[128]; int have = 0; uint32_t k, bv = 0;
        beml[0] = 0;
        for (k = 0; k < c->nf; k++) {
            const fr_t *f = &c->f[k]; uint32_t e = 0; char em[128];
            if (f->dead || !seq(f->id, "POPM")) continue;
            while (e < f->n && f->d[e] && e < 126) { em[e] = (char)f->d[e]; e++; }
            em[e] = 0;
            if (e + 1 >= f->n) continue;
            if (!have || scmpi(em, beml) < 0) { uint32_t m; for (m = 0; em[m]; m++) beml[m] = em[m]; beml[m] = 0; bv = f->d[e + 1]; have = 1; }
        }
        if (have) t->rating = bv == 0 ? 0 : bv <= 31 ? 20 : bv <= 95 ? 40 : bv <= 159 ? 60 : bv <= 223 ? 80 : 100;
    }
    {   /* fields a full tagger would use but this tool does NOT write: warn */
        static const int unsup[] = { F_sort_title, F_sort_artist, F_sort_album, F_sort_album_artist, F_sort_composer, F_normalization_gain_db, F_sound_check, F_media_kind,
                                     F_podcast, F_category, F_podcast_rss_url, F_podcast_enclosure_url, F_track_keywords, F_show, F_episode, F_season_number, F_episode_number,
                                     F_tv_network, F_sort_show, F_purchase_account, F_purchaser_name };
        static const char *const unn[] = { "sort_title", "sort_artist", "sort_album", "sort_album_artist", "sort_composer", "normalization_gain_db", "sound_check", "media_kind",
                                     "podcast", "category", "podcast_rss_url", "podcast_enclosure_url", "track_keywords", "show", "episode", "season_number", "episode_number",
                                     "tv_network", "sort_show", "purchase_account", "purchaser_name" };
        uint32_t k, m;
        for (k = 0; k < sizeof unsup / sizeof unsup[0]; k++) if (field(c, unsup[k], tt, 250)) note(t, unn[k]);
        for (m = 0; m < c->nf; m++) {
            const fr_t *f = &c->f[m];
            if (!f->dead && seq(f->id, "COMM") && f->desc && lc(f->desc[0]) == 'i' && lc(f->desc[1]) == 't' && lc(f->desc[2]) == 'u' && lc(f->desc[3]) == 'n' && lc(f->desc[4]) == 'n' && lc(f->desc[5]) == 'o' && lc(f->desc[6]) == 'r' && lc(f->desc[7]) == 'm' && !f->desc[8]) { note(t, "sound_check(iTunNORM)"); break; }
        }
    }
    /* lyrics: only when there is exactly one USLT and it is the text (otherwise the MP3 would have to be rewritten) */
    if (t->lyrics && t->lyrics[0] && !blank(t->lyrics)) {
        if (t->lyrics_frames == 1) t->lyrics_flag = 1;
        else { t->lyrics = 0; note(t, "lyrics(USLT count != 1: would need an MP3 rewrite)"); }
    } else t->lyrics = 0;
}

/* parse ID3v2/v1 and return the start of the audio */
static size_t id3_all(const uint8_t *b, size_t len, mdb_tags_t *t) {
    static cx_t sc; cx_t *c = &sc; size_t p = 0;
    c->nf = 0; c->t = t; c->xb = t->xbuf; c->xcap = t->xcap; c->xused = 0;
    if (!t->xbuf) { /* no work memory: header only */
        if (len >= 10 && b[0] == 'I' && b[1] == 'D' && b[2] == '3' && b[3] >= 2 && b[3] <= 4) { p = 10 + synchsafe(b + 6); if (b[5] & 0x10) p += 10; }
        return p;
    }
    if (len >= 10 && b[0] == 'I' && b[1] == 'D' && b[2] == '3' && b[3] >= 2 && b[3] <= 4) {
        uint32_t ts = synchsafe(b + 6); parse_id3v2(c, b, len, ts); p = 10 + ts;
        if (b[5] & 0x10) p += 10;
    }
    if (len >= 128 && b[len - 128] == 'T' && b[len - 127] == 'A' && b[len - 126] == 'G') parse_id3v1(c, b + len - 128);
    upd24(c);
    id3_interpret(c);
    return p;
}

static const uint16_t BR_L1[16] = {0,32,64,96,128,160,192,224,256,288,320,352,384,416,448,0};
static const uint16_t BR_L2[16] = {0,32,48,56,64,80,96,112,128,160,192,224,256,320,384,0};
static const uint16_t BR_L3[16] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
static const uint16_t BR_L3H[16] = {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0};   /* MPEG2/2.5 L2/L3 */
static const uint16_t BR_L1H[16] = {0,32,48,56,64,80,96,112,128,144,160,176,192,224,256,0};

#ifndef MDB_TAGS_DFLT_X
#define MDB_TAGS_DFLT_X (1u << 20)   /* the app (NanoSync) defines 16: it always passes xbuf */
#endif
/* total = real file size; len = bytes in b. If total > len, the last 128 B of b are the real tail of the file (ID3v1) and the
   prefix comes before it (used by NanoSync, which does not load the whole MP3). With total == len: normal behavior. */
static int tags_impl(const uint8_t *b, size_t len, size_t total, mdb_tags_t *t) {
    size_t p = 0, start; uint32_t i; char *xb = t->xbuf; uint32_t xc = t->xcap;
    static char dflt_x[MDB_TAGS_DFLT_X];
    { char *z = (char *)t; for (i = 0; i < sizeof *t; i++) z[i] = 0; }
    if (!xb) { xb = dflt_x; xc = sizeof dflt_x; }
    t->xbuf = xb; t->xcap = xc;
    p = id3_all(b, len, t);
    /* find the first valid frame (2 consecutive coherent frames) */
    for (start = p; start + 4 < len && start < p + 65536; start++) {
        uint8_t h1, h2, h3; uint32_t ver, layer, bri, sri, br, sr, spf, fsz, pad; const uint16_t *tab;
        if (b[start] != 0xFF || (b[start + 1] & 0xE0) != 0xE0) continue;
        h1 = b[start + 1]; h2 = b[start + 2]; h3 = b[start + 3]; (void)h3;
        ver = (h1 >> 3) & 3; layer = (h1 >> 1) & 3; bri = h2 >> 4; sri = (h2 >> 2) & 3; pad = (h2 >> 1) & 1;
        if (ver == 1 || layer == 0 || bri == 0 || bri == 15 || sri == 3) continue;
        {   /* ver: 3 = MPEG1, 2 = MPEG2, 0 = MPEG2.5 ; layer: 3=L1 2=L2 1=L3 */
            static const uint16_t SR[3][3] = {{44100,48000,32000},{22050,24000,16000},{11025,12000,8000}};
            int vi = ver == 3 ? 0 : ver == 2 ? 1 : 2;
            sr = SR[vi][sri];
            if (layer == 3) { tab = ver == 3 ? BR_L1 : BR_L1H; spf = 384; }
            else if (layer == 2) { tab = ver == 3 ? BR_L2 : BR_L3H; spf = 1152; }
            else { tab = ver == 3 ? BR_L3 : BR_L3H; spf = ver == 3 ? 1152 : 576; }
            br = tab[bri];
            fsz = layer == 3 ? (12000u * br / sr + pad) * 4 : (spf / 8 * 1000u * br / 1000 / sr) + pad;
            if (layer != 3) fsz = (spf / 8) * br * 1000 / sr + pad;
            if (fsz < 24) continue;
            if (start + fsz + 4 < len) { if (b[start + fsz] != 0xFF || (b[start + fsz + 1] & 0xE0) != 0xE0) continue; }
            t->sample_rate = sr; t->bitrate_kbps = br; t->layer = 4 - layer; t->mpeg = ver == 3 ? 1 : 2;
            t->channels = ((b[start + 3] >> 6) == 3) ? 1 : 2;
            {   /* Xing/Info/VBRI */
                uint32_t side = ver == 3 ? (t->channels == 1 ? 17 : 32) : (t->channels == 1 ? 9 : 17);
                const uint8_t *x = b + start + 4 + side; uint32_t frames = 0, bytes = 0;
                if (start + 4 + side + 16 < len && ((x[0] == 'X' && x[1] == 'i' && x[2] == 'n' && x[3] == 'g') || (x[0] == 'I' && x[1] == 'n' && x[2] == 'f' && x[3] == 'o'))) {
                    uint32_t fl = be32(x + 4); const uint8_t *q = x + 8;
                    if (fl & 1) { frames = be32(q); q += 4; }
                    if (fl & 2) { bytes = be32(q); q += 4; }
                    t->vbr = (x[0] == 'X');
                } else {
                    const uint8_t *v = b + start + 4 + 32;
                    if (start + 4 + 32 + 18 < len && v[0] == 'V' && v[1] == 'B' && v[2] == 'R' && v[3] == 'I') { bytes = be32(v + 10); frames = be32(v + 14); t->vbr = 1; }
                }
                if (frames) {
                    uint64_t ms = (uint64_t)frames * spf * 1000u / sr;
                    t->duration_ms = (uint32_t)ms;
                    if (t->vbr && bytes && ms) t->bitrate_kbps = (uint32_t)((uint64_t)bytes * 8 / ms);
                } else {
                    size_t audio = total - start; uint32_t mpl = 0;
                    if (len >= 128 && b[len - 128] == 'T' && b[len - 127] == 'A' && b[len - 126] == 'G') audio -= 128;
                    (void)mpl;
                    t->duration_ms = (uint32_t)((uint64_t)audio * 8 / br);     /* bytes*8 / (kbit/s) = ms */
                }
            }
            return 0;
        }
    }
    return -1;
}
int mdb_tags_read(const uint8_t *b, size_t len, mdb_tags_t *t) { return tags_impl(b, len, len, t); }
int mdb_tags_read_ex(const uint8_t *b, size_t len, size_t total, mdb_tags_t *t) { return tags_impl(b, len, total, t); }
