/* Generates Library.itdb / Locations.itdb / Dynamic.itdb from the logical iTunesCDB. */
#include "mdb_sqlgen.h"
#include "mdb_text.h"
#include "sha1.h"

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
static int tagis(const uint8_t *p, const char *t) { return p[0] == t[0] && p[1] == t[1] && p[2] == t[2] && p[3] == t[3]; }
static int scmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (int)(uint8_t)*a - (int)(uint8_t)*b; }
static size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static void mcpy(void *d, const void *s, size_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; }

enum { S_TITLE, S_ARTIST, S_ALBUM, S_GENRE, S_COMP, S_AA, S_SHOW, S_EPISODE, S_COMMENT, S_CATEGORY, S_GROUPING, S_DESC, S_COPYRIGHT,
       S_STITLE, S_SARTIST, S_SALBUM, S_SAA, S_SCOMP, S_EQ, S_LOC, S_LYRICS, S_N };
typedef struct {
    const char *s[S_N];
    uint64_t pid; uint32_t track_id, media_kind, year, content_rating, compilation, checked_flag, remember, skip_shuffle, gapless_album, artwork_id;
    uint32_t start, stop, length, tn, total_tracks, disc, total_discs, bpm, play_count, rating, app_rating, skip_count, bookmark;
    int32_t volume; uint32_t sound_check;
    int64_t last_modified, last_played, last_skipped, date_added, release, added_itunes;   /* unix */
    uint32_t size, bitrate, rate, pregap, postgap, gapless, gapless_payload, filetype, lyrics_flag, album_id, artist_id, composer_id, season, episode;
    uint64_t sample_count;
    int64_t pid_album, pid_artist, pid_tartist, pid_comp;
    uint32_t genre_id, category_id, album_order, ord;
} qt_t;

typedef struct { const char *album, *aa, *show; int64_t native; uint32_t first, nmem; int64_t pid; } agrp_t;
typedef struct { const char *key; int64_t native; int64_t pid; } kgrp_t;

typedef struct {
    mdb_arena_t *ar; const mdb_schema_t *sc; mtab_t *tab[3]; int32_t tz;
    qt_t *t; uint32_t n; uint64_t dbid; const char *devname;
} G;

/* ---------------------------------------------------------- values/rows */
static mval_t V_I(int64_t x) { mval_t v; v.t = MV_INT; v.i = x; v.s = 0; v.n = 0; return v; }
static mval_t V_N(void) { mval_t v; v.t = MV_NULL; v.i = 0; v.s = 0; v.n = 0; return v; }
static mval_t V_R(uint32_t x) { mval_t v; v.t = MV_REAL; v.i = (int64_t)mdb_f64_from_u32(x); v.s = 0; v.n = 0; return v; }
static mval_t V_T(const char *s) { mval_t v; v.t = MV_TEXT; v.i = 0; v.s = s; v.n = (uint32_t)slen(s); return v; }
static mval_t V_TN(const char *s) { return (s && s[0]) ? V_T(s) : V_N(); }   /* text, or NULL if empty */

static int tabidx(const mdb_schema_t *sc, const char *name) {
    int i; for (i = 0; i < sc->ntab; i++) if (!scmp(sc->tab[i].name, name)) return i;
    return -1;
}
static int colidx(const mdb_tab_t *t, const char *name) {
    int i; for (i = 0; i < t->ncols; i++) if (!scmp(t->cols[i].name, name)) return i;
    return -1;
}
/* apply column affinity */
static mval_t aff(const mdb_col_t *c, mval_t v) {
    if (c->aff == 'R' && v.t == MV_INT) return V_R((uint32_t)v.i);
    return v;
}
static mrow_t *newrow(G *g, int db, int ti, int64_t rowid_auto) {
    const mdb_tab_t *t = &g->sc[db].tab[ti]; mtab_t *mt = &g->tab[db][ti]; mrow_t *r; int c;
    if (mt->n == mt->cap) {
        uint32_t nc = mt->cap ? mt->cap * 2 : 64; mrow_t *nr = mdb_aalloc(g->ar, sizeof(mrow_t) * nc);
        if (!nr) return 0;
        if (mt->n) mcpy(nr, mt->rows, sizeof(mrow_t) * mt->n);
        mt->rows = nr; mt->cap = nc;
    }
    r = &mt->rows[mt->n++];
    r->v = mdb_aalloc(g->ar, sizeof(mval_t) * t->ncols);
    if (!r->v) return 0;
    for (c = 0; c < t->ncols; c++) {
        const mdb_col_t *cl = &t->cols[c];
        r->v[c] = cl->dtype == 0 ? V_N() : cl->dtype == 1 ? aff(cl, V_I(cl->di)) : cl->dtype == 2 ? V_R(0) : V_T(cl->ds);
    }
    r->rowid = rowid_auto;
    return r;
}
/* positional row: vals[0..n) in the first columns */
static int addrow(G *g, int db, const char *tname, int64_t auto_rowid, const mval_t *vals, int n) {
    int ti = tabidx(&g->sc[db], tname), c; const mdb_tab_t *t = &g->sc[db].tab[ti]; mrow_t *r;
    r = newrow(g, db, ti, auto_rowid); if (!r) return MDB_SQL_E_MEM;
    for (c = 0; c < n; c++) r->v[c] = aff(&t->cols[c], vals[c]);
    if (t->rowid_col >= 0) { if (vals[t->rowid_col].t != MV_INT) return MDB_SQL_E_FORMAT; r->rowid = vals[t->rowid_col].i; }
    return 0;
}
#define ROW(db, tn, auto_id, ...) do { mval_t _v[] = {__VA_ARGS__}; int _r = addrow(g, db, tn, auto_id, _v, (int)(sizeof _v / sizeof _v[0])); if (_r) return _r; } while (0)

/* ----------------------------------------------------------------- parse */
static const char *mstr(mdb_arena_t *ar, const uint8_t *mh) {
    uint32_t tl = rd32(mh + 8), enc = rd32(mh + 24), len = rd32(mh + 28); char *o;
    if (tl < 40 || 40 + len > tl) return "";
    o = mdb_aalloc(ar, len * 2 + 2); if (!o) return 0;
    if (enc == 1) mdb_u16le_to_u8(mh + 40, len, o); else { mcpy(o, mh + 40, len); o[len] = 0; }
    return o;
}
static int64_t mac2unix(const G *g, uint32_t m, int utc) { return m ? (int64_t)m - 2082844800LL - (utc ? 0 : g->tz) : 0; }
static int64_t core(int64_t unix_s) { return unix_s ? unix_s - 978307200LL : 0; }

static int parse_cdb(G *g, const uint8_t *cdb, size_t len, uint32_t *ntr) {
    uint32_t hs = rd32(cdb + 4), cnt = 0; size_t off;
    if (len < 244 || !tagis(cdb, "mhbd")) return MDB_SQL_E_FORMAT;
    g->tz = (int32_t)rd32(cdb + 0x6C); g->dbid = rd64(cdb + 0x18); g->devname = "iPod";
    /* count and read */
    for (off = hs; off < len;) {
        const uint8_t *s = cdb + off; uint32_t type, hl, tl;
        if (!tagis(s, "mhsd")) return MDB_SQL_E_FORMAT;
        hl = rd32(s + 4); tl = rd32(s + 8); type = rd32(s + 12);
        if (type == 1) {
            uint32_t i, c = rd32(s + hl + 8); size_t p = off + hl + rd32(s + hl + 4);
            g->t = mdb_aalloc(g->ar, sizeof(qt_t) * (c + 1)); if (!g->t) return MDB_SQL_E_MEM;
            for (i = 0; i < c; i++) {
                const uint8_t *m = cdb + p; qt_t *q = &g->t[i]; uint32_t hl2 = rd32(m + 4), nm = rd32(m + 12), j, o2 = hl2, k;
                if (!tagis(m, "mhit") || hl2 < 0x1F8) return MDB_SQL_E_FORMAT;
                for (k = 0; k < sizeof *q; k++) ((uint8_t *)q)[k] = 0;
                for (k = 0; k < S_N; k++) q->s[k] = "";
                q->track_id = rd32(m + 0x10); q->media_kind = rd32(m + 0xD0); q->year = rd32(m + 0x34); q->content_rating = m[0x92];
                q->compilation = m[0x1E]; q->checked_flag = m[0x78]; q->remember = m[0xA6]; q->skip_shuffle = m[0xA5]; q->gapless_album = rd16(m + 0x102);
                q->artwork_id = rd32(m + 0x160); q->start = rd32(m + 0x44); q->stop = rd32(m + 0x48); q->length = rd32(m + 0x28); q->tn = rd32(m + 0x2C);
                q->total_tracks = rd32(m + 0x30); q->disc = rd32(m + 0x5C); q->total_discs = rd32(m + 0x60); q->bpm = rd16(m + 0x7A);
                q->volume = (int32_t)rd32(m + 0x40); q->sound_check = rd32(m + 0x4C); q->play_count = rd32(m + 0x50); q->rating = m[0x1F]; q->app_rating = m[0x79];
                q->skip_count = rd32(m + 0x9C); q->bookmark = rd32(m + 0x6C);
                q->last_modified = mac2unix(g, rd32(m + 0x20), 0); q->last_played = mac2unix(g, rd32(m + 0x58), 0); q->last_skipped = mac2unix(g, rd32(m + 0xA0), 0);
                q->date_added = mac2unix(g, rd32(m + 0x68), 0); q->release = mac2unix(g, rd32(m + 0x8C), 1); q->added_itunes = mac2unix(g, rd32(m + 0xDC), 0);
                q->size = rd32(m + 0x24); q->bitrate = rd32(m + 0x38); q->rate = rd32(m + 0x3C) >> 16; q->sample_count = rd64(m + 0xBC);
                q->pregap = rd32(m + 0xB8); q->postgap = rd32(m + 0xC8); q->gapless = rd16(m + 0x100); q->gapless_payload = rd32(m + 0xF8);
                q->filetype = rd32(m + 0x18); q->lyrics_flag = m[0xB0]; q->album_id = rd32(m + 0x120); q->artist_id = rd32(m + 0x1E0); q->composer_id = rd32(m + 0x1F4);
                q->season = rd32(m + 0xD4); q->episode = rd32(m + 0xD8);
                q->pid = rd64(m + 0x70) ? rd64(m + 0x70) : q->track_id;
                for (j = 0; j < nm; j++) {
                    const uint8_t *h = m + o2; uint32_t ty = rd32(h + 12), ml = rd32(h + 8); int idx = -1; const char *str;
                    switch (ty) {
                    case 1: idx = S_TITLE; break; case 4: idx = S_ARTIST; break; case 3: idx = S_ALBUM; break; case 5: idx = S_GENRE; break;
                    case 12: idx = S_COMP; break; case 22: idx = S_AA; break; case 19: idx = S_SHOW; break; case 20: idx = S_EPISODE; break;
                    case 8: idx = S_COMMENT; break; case 9: idx = S_CATEGORY; break; case 13: idx = S_GROUPING; break; case 14: idx = S_DESC; break;
                    case 39: idx = S_COPYRIGHT; break; case 27: idx = S_STITLE; break; case 23: idx = S_SARTIST; break; case 28: idx = S_SALBUM; break;
                    case 29: idx = S_SAA; break; case 30: idx = S_SCOMP; break; case 7: idx = S_EQ; break; case 2: idx = S_LOC; break; case 10: idx = S_LYRICS; break;
                    }
                    if (idx >= 0) { str = mstr(g->ar, h); if (!str) return MDB_SQL_E_MEM; q->s[idx] = str; }
                    o2 += ml;
                }
                p += rd32(m + 8);
            }
            cnt = c;
        } else if (type == 2 || type == 3) {
            uint32_t c = rd32(s + hl + 8), i; size_t p = off + hl + rd32(s + hl + 4);
            for (i = 0; i < c; i++) {
                const uint8_t *y = cdb + p;
                if (!(y[0x14] & 1) || rd16(y + 0x50)) return MDB_SQL_E_UNSUPPORTED;        /* user/podcast playlists: out of scope */
                if (type == 3 && rd32(y + 12) > 0) {
                    const uint8_t *h = y + rd32(y + 4);
                    if (rd32(h + 12) == 1) { const char *d = mstr(g->ar, h); if (!d) return MDB_SQL_E_MEM; g->devname = d; }
                }
                p += rd32(y + 8);
            }
        }
        off += tl;
    }
    *ntr = cnt;
    return 0;
}

/* ------------------------------------------------------------- utilities */
static const char *casefold_s(G *g, const char *s) {
    char *o = mdb_aalloc(g->ar, mdb_casefold(s, 0) + 1); if (!o) return 0;
    mdb_casefold(s, o); return o;
}
static const char *strip_article(G *g, const char *s) {
    /* album name for sorting: remove "the "/"an "/"a " (casefold comparison, cut by length) */
    static const char *art[3] = {"the ", "an ", "a "}; int i;
    for (i = 0; i < 3; i++) {
        size_t n = slen(art[i]); size_t k;
        for (k = 0; k < n; k++) { char c = s[k]; if (!c) break; if (c >= 'A' && c <= 'Z') c += 32; if (c != art[i][k]) break; }
        if (k == n) return s + n;
    }
    (void)g; return s;
}
static int64_t stable_pid(const char *kind, const char *p1, const char *p2, const char *p3, int salt) {
    mdb_sha1_t c; uint8_t d[20]; uint64_t v = 0; int i; char tmp[12]; static const uint8_t us = 0x1F;
    mdb_sha1_init(&c); mdb_sha1_update(&c, kind, slen(kind));
    if (p1) { mdb_sha1_update(&c, &us, 1); mdb_sha1_update(&c, p1, slen(p1)); }
    if (p2) { mdb_sha1_update(&c, &us, 1); mdb_sha1_update(&c, p2, slen(p2)); }
    if (p3) { mdb_sha1_update(&c, &us, 1); mdb_sha1_update(&c, p3, slen(p3)); }
    if (salt) { int n = 0, x = salt; char r[12]; while (x) { r[n++] = (char)('0' + x % 10); x /= 10; } for (i = 0; i < n; i++) tmp[i] = r[n - 1 - i]; tmp[n] = 0;
                mdb_sha1_update(&c, &us, 1); mdb_sha1_update(&c, tmp, (size_t)n); }
    mdb_sha1_final(&c, d);
    for (i = 7; i >= 0; i--) v = (v << 8) | d[i];
    return (int64_t)(v ? v : 1);
}
/* pid allocation */
static int alloc_pids(G *g, const char *kind, uint32_t n, const char *const *p1, const char *const *p2, const char *const *p3, const int64_t *native, int64_t *out) {
    uint32_t i, j; (void)g;
    for (i = 0; i < n; i++) {
        uint32_t cntn = 0; int64_t cand; int salt = 0, clash;
        if (native[i]) for (j = 0; j < n; j++) if (native[j] == native[i]) cntn++;
        cand = (native[i] && cntn == 1) ? native[i] : stable_pid(kind, p1[i], p2 ? p2[i] : 0, p3 ? p3[i] : 0, 0);
        do {
            clash = 0;
            for (j = 0; j < i; j++) if (out[j] == cand) clash = 1;
            if (clash) { salt++; cand = stable_pid(kind, p1[i], p2 ? p2[i] : 0, p3 ? p3[i] : 0, salt); }
        } while (clash);
        out[i] = cand;
    }
    return 0;
}
/* ranks: rank of each name in a set of names (by casefold) */
static int name_rank(G *g, const char *const *names, uint32_t n, uint32_t *rank) {
    uint32_t i, j, m = 0; const char **keys = mdb_aalloc(g->ar, sizeof(char *) * (n + 1));
    if (!keys) return MDB_SQL_E_MEM;
    for (i = 0; i < n; i++) {
        const char *k = casefold_s(g, names[i]); int dup = 0;
        if (!k) return MDB_SQL_E_MEM;
        for (j = 0; j < m; j++) if (!scmp(keys[j], k)) { dup = 1; break; }
        if (!dup) keys[m++] = k;
    }
#define GRP(s) ((s)[0] == 0 ? 2 : ((s)[0] >= '0' && (s)[0] <= '9') ? 1 : 0)
    for (i = 0; i < n; i++) {
        const char *k = casefold_s(g, names[i]); uint32_t below = 0;
        for (j = 0; j < m; j++) { int gj = GRP(keys[j]), gk = GRP(k); if (gj < gk || (gj == gk && scmp(keys[j], k) < 0)) below++; }
        rank[i] = (below + 1) * 100;
    }
    return 0;
}
/* stable text ids (genre/category): rank by (casefold, value) among distinct values */
static int text_ids(G *g, int field, uint32_t *idout, uint32_t n) {
    uint32_t i, j;
    for (i = 0; i < n; i++) {
        const char *v = g->t[i].s[field]; uint32_t rank = 0; const char *cv;
        if (!v[0]) { idout[i] = 0; continue; }
        cv = casefold_s(g, v); if (!cv) return MDB_SQL_E_MEM;
        for (j = 0; j < n; j++) {
            const char *w = g->t[j].s[field], *cw; int c, dup = 0; uint32_t k;
            if (!w[0]) continue;
            for (k = 0; k < j; k++) if (!scmp(g->t[k].s[field], w)) { dup = 1; break; }
            if (dup) continue;
            cw = casefold_s(g, w); if (!cw) return MDB_SQL_E_MEM;
            c = scmp(cw, cv); if (!c) c = scmp(w, v);
            if (c < 0) rank++;
        }
        idout[i] = rank + 1;
    }
    return 0;
}
static const char *ext_of(const char *path) {
    const char *dot = 0, *p = path; while (*p) { if (*p == '.') dot = p; if (*p == '/' || *p == ':' || *p == '\\') dot = 0; p++; }
    return dot ? dot + 1 : "";
}
static int file_location(G *g, const char *loc, const char **out) {
    /* file location: normalize separators; strip "iPod_Control/Music" */
    size_t n = slen(loc), i, st = 0; char *o = mdb_aalloc(g->ar, n + 2); const char *parts[16]; int np = 0; char *w;
    if (!o) return MDB_SQL_E_MEM;
    for (i = 0; i < n; i++) o[i] = (loc[i] == ':' || loc[i] == '\\') ? '/' : loc[i];
    o[n] = 0;
    while (st < n && o[st] == '/') st++;
    { size_t e = n; while (e > st && o[e - 1] == '/') e--; o[e] = 0; n = e; }
    w = o + st; parts[np++] = w;
    for (i = 0; w[i]; i++) if (w[i] == '/' && np < 16) { w[i] = 0; parts[np++] = w + i + 1; }
    if (np >= 4 && !scmp(parts[0], "iPod_Control") && !scmp(parts[1], "Music")) {
        char *r = mdb_aalloc(g->ar, n + 2), *q = r; int k;
        if (!r) return MDB_SQL_E_MEM;
        for (k = 2; k < np; k++) { size_t l = slen(parts[k]); mcpy(q, parts[k], l); q += l; if (k + 1 < np) *q++ = '/'; }
        *q = 0; *out = r; return 0;
    }
    if (np >= 2) { char *r = mdb_aalloc(g->ar, n + 2), *q = r; size_t l1 = slen(parts[np - 2]), l2 = slen(parts[np - 1]);
        if (!r) return MDB_SQL_E_MEM;
        mcpy(q, parts[np - 2], l1); q += l1; *q++ = '/'; mcpy(q, parts[np - 1], l2); q += l2; *q = 0; *out = r; return 0; }
    *out = parts[0]; return 0;
}

/* ----------------------------------------------------------------- build */
int mdb_sqlgen_build(const uint8_t *cdb, size_t len, mdb_sqlgen_out_t *o, void *scratch, size_t scratch_size) {
    G gg, *g = &gg; mdb_arena_t ar; uint32_t n = 0, i; int r, db;
    agrp_t *ag; uint32_t nag = 0; kgrp_t *ar_k, *ta_k, *co_k; uint32_t nar = 0, nta = 0, nco = 0;
    uint32_t *gid, *cid, *aorder, *arank, *srank; const char **names, **snames; int64_t *tmp_n, *tmp_o;
    int64_t sum_audio = 0; uint32_t *agi;
    ar.base = scratch; ar.cap = scratch_size; ar.used = 0;
    gg.ar = &ar; gg.t = 0; gg.n = 0;
    for (db = 0; db < 3; db++) {
        uint32_t k;
        gg.sc = MDB_SCHEMAS; gg.tab[db] = mdb_aalloc(&ar, sizeof(mtab_t) * MDB_SCHEMAS[db].ntab);
        if (!gg.tab[db]) return MDB_SQL_E_MEM;
        for (k = 0; k < MDB_SCHEMAS[db].ntab; k++) { gg.tab[db][k].rows = 0; gg.tab[db][k].n = 0; gg.tab[db][k].cap = 0; gg.tab[db][k].next_rowid = 1; }
    }
    r = parse_cdb(g, cdb, len, &n); if (r) return r;
    g->n = n;
    for (i = 0; i < n; i++) {
        qt_t *q = &g->t[i];
        if (q->media_kind & ~(uint32_t)1) return MDB_SQL_E_UNSUPPORTED;     /* music audio only */
        { const char *ex = ext_of(q->s[S_LOC]); if (scmp(ex, "mp3") && scmp(ex, "MP3")) return MDB_SQL_E_UNSUPPORTED; }
    }
    /* album groups: key (album, album_artist||artist, show) */
    ag = mdb_aalloc(&ar, sizeof(agrp_t) * (n + 1)); agi = mdb_aalloc(&ar, 4 * (n + 1));
    ar_k = mdb_aalloc(&ar, sizeof(kgrp_t) * (n + 1)); ta_k = mdb_aalloc(&ar, sizeof(kgrp_t) * (n + 1)); co_k = mdb_aalloc(&ar, sizeof(kgrp_t) * (n + 1));
    if (n && (!ag || !agi || !ar_k || !ta_k || !co_k)) return MDB_SQL_E_MEM;
    for (i = 0; i < n; i++) {
        qt_t *q = &g->t[i]; const char *aa = q->s[S_AA][0] ? q->s[S_AA] : q->s[S_ARTIST]; uint32_t k;
        for (k = 0; k < nag; k++) if (!scmp(ag[k].album, q->s[S_ALBUM]) && !scmp(ag[k].aa, aa) && !scmp(ag[k].show, q->s[S_SHOW])) break;
        if (k == nag) { ag[k].album = q->s[S_ALBUM]; ag[k].aa = aa; ag[k].show = q->s[S_SHOW]; ag[k].native = q->album_id; ag[k].first = i; ag[k].nmem = 0; nag++; }
        ag[k].nmem++; agi[i] = k;
        for (k = 0; k < nar; k++) if (!scmp(ar_k[k].key, aa)) break;
        if (k == nar) { ar_k[k].key = aa; ar_k[k].native = q->artist_id; nar++; }
        for (k = 0; k < nta; k++) if (!scmp(ta_k[k].key, q->s[S_ARTIST])) break;
        if (k == nta) { ta_k[k].key = q->s[S_ARTIST]; ta_k[k].native = 0; nta++; }
        for (k = 0; k < nco; k++) if (!scmp(co_k[k].key, q->s[S_COMP])) break;
        if (k == nco) { co_k[k].key = q->s[S_COMP]; co_k[k].native = q->composer_id; nco++; }
    }
    /* pids */
    names = mdb_aalloc(&ar, sizeof(char *) * (n + 1) * 3); tmp_n = mdb_aalloc(&ar, 8 * (n + 1)); tmp_o = mdb_aalloc(&ar, 8 * (n + 1));
    if (n && (!names || !tmp_n || !tmp_o)) return MDB_SQL_E_MEM;
    { const char **p1 = names, **p2 = names + n, **p3 = names + 2 * n;
      for (i = 0; i < nag; i++) { p1[i] = ag[i].album; p2[i] = ag[i].aa; p3[i] = ag[i].show; tmp_n[i] = ag[i].native; }
      alloc_pids(g, "album", nag, p1, p2, p3, tmp_n, tmp_o); for (i = 0; i < nag; i++) ag[i].pid = tmp_o[i];
      for (i = 0; i < nar; i++) { p1[i] = ar_k[i].key; tmp_n[i] = ar_k[i].native; }
      alloc_pids(g, "artist", nar, p1, 0, 0, tmp_n, tmp_o); for (i = 0; i < nar; i++) ar_k[i].pid = tmp_o[i];
      for (i = 0; i < nta; i++) { p1[i] = ta_k[i].key; tmp_n[i] = 0; }
      alloc_pids(g, "track_artist", nta, p1, 0, 0, tmp_n, tmp_o); for (i = 0; i < nta; i++) ta_k[i].pid = tmp_o[i];
      for (i = 0; i < nco; i++) { p1[i] = co_k[i].key; tmp_n[i] = co_k[i].native; }
      alloc_pids(g, "composer", nco, p1, 0, 0, tmp_n, tmp_o); for (i = 0; i < nco; i++) co_k[i].pid = tmp_o[i]; }
    /* album ranks (name without article) and sort_album ranks per track */
    snames = mdb_aalloc(&ar, sizeof(char *) * (n + 1)); arank = mdb_aalloc(&ar, 4 * (n + 1)); srank = mdb_aalloc(&ar, 4 * (n + 1)); aorder = mdb_aalloc(&ar, 4 * (n + 1));
    gid = mdb_aalloc(&ar, 4 * (n + 1)); cid = mdb_aalloc(&ar, 4 * (n + 1));
    if (n && (!snames || !arank || !srank || !aorder || !gid || !cid)) return MDB_SQL_E_MEM;
    for (i = 0; i < nag; i++) snames[i] = strip_article(g, ag[i].album);
    r = name_rank(g, snames, nag, arank); if (r) return r;
    for (i = 0; i < n; i++) snames[i] = g->t[i].s[S_SALBUM][0] ? g->t[i].s[S_SALBUM] : strip_article(g, g->t[i].s[S_ALBUM]);
    r = name_rank(g, snames, n, srank); if (r) return r;
    r = text_ids(g, S_GENRE, gid, n); if (r) return r;
    r = text_ids(g, S_CATEGORY, cid, n); if (r) return r;
    (void)aorder;

    /* ---- Library ---- */
    ROW(0, "version_info", 0, V_I(1), V_I(1), V_I(111), V_I(0), V_I(0), V_I(1109), V_I(2));
    ROW(0, "db_info", 0, V_I((int64_t)g->dbid), V_I((int64_t)(g->dbid ? g->dbid : 1)), V_N(), V_I(-1), V_I(-1), V_N(), V_N(), V_N());
    ROW(0, "location_kind_map", 0, V_I(1), V_T("MPEG audio file"));
    ROW(0, "location_kind_map", 0, V_I(2), V_T("Purchased AAC audio file"));
    ROW(0, "location_kind_map", 0, V_I(3), V_T("AAC audio file"));
    ROW(1, "base_location", 0, V_I(1), V_T("iPod_Control/Music"));
    /* genre_map / category_map */
    for (i = 0; i < n; i++) if (gid[i]) {
        uint32_t k, dup = 0; for (k = 0; k < i; k++) if (gid[k] == gid[i]) dup = 1;
        if (!dup) ROW(0, "genre_map", 0, V_I(gid[i]), V_T(g->t[i].s[S_GENRE]), V_I(gid[i]), V_I(0), V_I(1), V_I(0), V_I(0), V_I(0));
    }
    for (i = 0; i < n; i++) if (cid[i]) {
        uint32_t k, dup = 0; for (k = 0; k < i; k++) if (cid[k] == cid[i]) dup = 1;
        if (!dup) ROW(0, "category_map", 0, V_I(cid[i]), V_T(g->t[i].s[S_CATEGORY]));
    }
    /* album */
    for (i = 0; i < nag; i++) {
        uint32_t k, artw = 0, allc = 1, anyc = 0, season = 0, sr = 0; int64_t artw_pid = 0; int64_t artist_pid = 0; const char *sname = 0;
        for (k = 0; k < n; k++) if (agi[k] == i) {
            if (g->t[k].artwork_id && !artw) { artw = 1; artw_pid = (int64_t)g->t[k].pid; }
            if (!g->t[k].compilation) allc = 0; else anyc = 1;
            if (!season && g->t[k].s[S_SHOW][0]) season = g->t[k].season;
        }
        { uint32_t f = ag[i].first; sname = g->t[f].s[S_SALBUM][0] ? g->t[f].s[S_SALBUM] : strip_article(g, g->t[f].s[S_ALBUM]); sr = srank[f]; }
        for (k = 0; k < nar; k++) if (!scmp(ar_k[k].key, ag[i].aa)) artist_pid = ar_k[k].pid;
        ROW(0, "album", 0, V_I(ag[i].pid), V_I(2), V_I(artw), V_I(artw_pid), V_I(artist_pid), V_I(0), V_TN(ag[i].album), V_I(arank[i]), V_I(allc),
            V_N(), V_I(season), V_I(ag[i].album[0] ? 0 : 1), V_I(1), V_I(0), V_I(sr), V_I(100), V_I(anyc), V_TN(sname), V_I(0), V_I(0), V_I(ag[i].nmem));
    }
    for (i = 0; i < nar; i++) {                     /* schema 1109: + has_non_compilation_tracks, album_count */
        uint32_t k, nalb = 0, noncomp = 0;
        for (k = 0; k < nag; k++) if (!scmp(ag[k].aa, ar_k[i].key)) nalb++;
        for (k = 0; k < n; k++) if (!g->t[k].compilation && !scmp(ag[agi[k]].aa, ar_k[i].key)) noncomp = 1;
        ROW(0, "artist", 0, V_I(ar_k[i].pid), V_I(2), V_I(0), V_I(0), V_TN(ar_k[i].key), V_I(100), V_TN(ar_k[i].key), V_I(ar_k[i].key[0] ? 0 : 1), V_I(1), V_I(0),
            V_I(noncomp), V_I(nalb));
    }
    for (i = 0; i < nta; i++) ROW(0, "track_artist", 0, V_I(ta_k[i].pid), V_TN(ta_k[i].key), V_I(100), V_TN(ta_k[i].key), V_I(1), V_I(0), V_I(1), V_I(ta_k[i].key[0] ? 0 : 1), V_I(0));
    for (i = 0; i < nco; i++) ROW(0, "composer", 0, V_I(co_k[i].pid), V_TN(co_k[i].key), V_I(100), V_TN(co_k[i].key), V_I(co_k[i].key[0] ? 0 : 1), V_I(1));

    for (i = 0; i < n; i++) {
        qt_t *q = &g->t[i]; const mdb_tab_t *it = &MDB_SCHEMAS[0].tab[tabidx(&MDB_SCHEMAS[0], "item")]; mrow_t *rw; int ti = tabidx(&MDB_SCHEMAS[0], "item"); uint32_t k;
        const char *aa = q->s[S_AA][0] ? q->s[S_AA] : q->s[S_ARTIST]; const char *loc; const char *ex;
        int64_t apid = ag[agi[i]].pid, arpid = 0, tapid = 0, copid = 0; mval_t *v;
#define SETI(col, val) do { int _c = colidx(it, col); v[_c] = aff(&it->cols[_c], V_I(val)); } while (0)
#define SETV(col, val) do { int _c = colidx(it, col); v[_c] = aff(&it->cols[_c], val); } while (0)
        for (k = 0; k < nar; k++) if (!scmp(ar_k[k].key, aa)) arpid = ar_k[k].pid;
        for (k = 0; k < nta; k++) if (!scmp(ta_k[k].key, q->s[S_ARTIST])) tapid = ta_k[k].pid;
        for (k = 0; k < nco; k++) if (!scmp(co_k[k].key, q->s[S_COMP])) copid = co_k[k].pid;
        q->pid_album = apid; q->pid_artist = arpid; q->pid_tartist = tapid; q->pid_comp = copid;
        rw = newrow(g, 0, ti, (int64_t)q->pid); if (!rw) return MDB_SQL_E_MEM;
        v = rw->v;
        SETI("pid", (int64_t)q->pid); SETI("media_kind", q->media_kind);
        SETI("is_song", (q->media_kind & 1) ? 1 : 0);                      /* MEDIA_TYPE_AUDIO = 1 */
        SETI("date_modified", core(q->last_modified)); SETI("year", q->year); SETI("content_rating", q->content_rating); SETI("is_compilation", q->compilation);
        SETI("is_user_disabled", q->checked_flag); SETI("remember_bookmark", q->remember); SETI("exclude_from_shuffle", q->skip_shuffle);
        SETI("part_of_gapless_album", q->gapless_album); SETI("artwork_status", q->artwork_id ? 1 : 0); SETI("artwork_cache_id", q->artwork_id);
        SETI("start_time_ms", q->start); SETI("stop_time_ms", q->stop); SETI("total_time_ms", q->length); SETI("track_number", q->tn); SETI("track_count", q->total_tracks);
        SETI("disc_number", q->disc); SETI("disc_count", q->total_discs); SETI("bpm", q->bpm);
        SETI("relative_volume", q->volume);
        SETV("eq_preset", V_TN(q->s[S_EQ]));
        SETI("genre_id", gid[i]); SETI("category_id", cid[i]); SETI("album_pid", apid); SETI("artist_pid", arpid); SETI("composer_pid", copid);
        SETV("title", V_T(q->s[S_TITLE])); SETV("artist", V_T(q->s[S_ARTIST])); SETV("album", V_T(q->s[S_ALBUM])); SETV("album_artist", V_T(q->s[S_AA])); SETV("composer", V_T(q->s[S_COMP]));
        SETV("sort_title", V_TN(q->s[S_STITLE])); SETV("sort_artist", V_TN(q->s[S_SARTIST]));
        SETV("sort_album", V_TN(q->s[S_SALBUM][0] ? q->s[S_SALBUM] : strip_article(g, q->s[S_ALBUM])));
        SETV("sort_album_artist", V_TN(q->s[S_SAA])); SETV("sort_composer", V_TN(q->s[S_SCOMP]));
        SETI("album_order", srank[i]);
        SETV("comment", V_TN(q->s[S_COMMENT])); SETV("grouping", V_TN(q->s[S_GROUPING])); SETV("description", V_TN(q->s[S_DESC])); SETV("copyright", V_TN(q->s[S_COPYRIGHT]));
        SETI("track_artist_pid", tapid); SETI("physical_order", i); SETI("has_lyrics", q->lyrics_flag ? 1 : 0); SETI("date_released", core(q->release));
        ex = ext_of(q->s[S_LOC]); (void)ex;
        sum_audio += q->size;
        ROW(0, "avformat_info", (int64_t)i + 1, V_I((int64_t)q->pid), V_I(0), V_I(301), V_I(q->bitrate), V_I(0), V_R(q->rate),
            V_I((int64_t)q->sample_count), V_I(q->gapless ? 1 : 0), V_I(q->pregap), V_I(q->postgap), V_I(q->gapless_payload), V_I(0), V_I(0), V_I(q->sound_check));
        if (q->added_itunes || q->release) {
            mrow_t *sr2 = newrow(g, 0, tabidx(&MDB_SCHEMAS[0], "store_info"), (int64_t)q->pid); const mdb_tab_t *st = &MDB_SCHEMAS[0].tab[tabidx(&MDB_SCHEMAS[0], "store_info")];
            if (!sr2) return MDB_SQL_E_MEM;
            sr2->v[0] = V_I((int64_t)q->pid); sr2->v[colidx(st, "date_purchased")] = V_I(core(q->added_itunes)); sr2->v[colidx(st, "date_released")] = V_I(core(q->release));
        }
        if (file_location(g, q->s[S_LOC], &loc)) return MDB_SQL_E_MEM;
        ROW(1, "location", (int64_t)i + 1, V_I((int64_t)q->pid), V_I(0), V_I(1), V_I(0x46494C45), V_T(loc), V_I(0x4D503320), V_I(1), V_I(core(q->date_added)), V_I(q->size), V_N(), V_N(), V_N(), V_N());
        ROW(2, "item_stats", 0, V_I((int64_t)q->pid), V_I(q->play_count > 0), V_I(core(q->last_played)), V_I(q->play_count), V_I(0), V_I(core(q->last_skipped)), V_I(q->skip_count), V_I(0),
            V_R(q->bookmark), V_R(q->bookmark), V_I(q->rating), V_I(q->app_rating), V_I(0), V_I(q->play_count), V_I(q->skip_count), V_I(0));
    }
    ROW(0, "track_size_calc", 0, V_I(1), V_T("audio"), V_I(sum_audio));
    ROW(0, "track_size_calc", 0, V_I(2), V_T("video"), V_I(0));
    ROW(0, "track_size_calc", 0, V_I(3), V_T("music_video"), V_I(0));
    {
        int64_t now = 0, mpid = (int64_t)(g->dbid ? g->dbid : 1); int64_t mx = 0;
        for (i = 0; i < n; i++) if (g->t[i].date_added > mx) mx = g->t[i].date_added;
        now = core(mx);
        ROW(0, "container", 0, V_I(mpid), V_I(0), V_I(now), V_I(now), V_T(g->devname), V_I(100), V_I(0), V_I(1), V_I(0), V_I(1), V_I(0), V_N(), V_N(), V_I(0), V_N(), V_N(), V_N(), V_N(), V_N(), V_N(), V_N());
        /* container: smart_* columns per the container insert rule (smart_is_folder=0, dynamic/filtered/genius/enabled/limited=NULL, ... evaluation_order=0) */
        for (i = 0; i < n; i++) ROW(0, "item_to_container", (int64_t)i + 1, V_I((int64_t)g->t[i].pid), V_I(mpid), V_I(i), V_N());
        ROW(2, "container_ui", 0, V_I(mpid), V_I(0), V_I(0), V_I(1), V_I(0), V_I(0), V_I(0));
    }
    /* write the three files */
    {
        size_t cap;
        cap = o->cap[0]; r = mdb_sqlite_write(&MDB_SCHEMAS[0], g->tab[0], o->buf[0], cap, &o->len[0], &ar); if (r) return r;
        cap = o->cap[1]; r = mdb_sqlite_write(&MDB_SCHEMAS[1], g->tab[1], o->buf[1], cap, &o->len[1], &ar); if (r) return r;
        cap = o->cap[2]; r = mdb_sqlite_write(&MDB_SCHEMAS[2], g->tab[2], o->buf[2], cap, &o->len[2], &ar); if (r) return r;
    }
    o->scratch_used = ar.used;
    return 0;
}
