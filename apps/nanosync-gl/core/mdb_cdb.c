#include "mdb_cdb.h"
#include "mdb_text.h"
#include "mdb_zlib.h"
#include "mdb_sig.h"
#include "mdb_tmpl.h"

/* ------------------------------------------------------------------ util */
static uint32_t rd32(const uint8_t *p) { return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr64(uint8_t *p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32)); }
static int tagis(const uint8_t *p, const char *t) { return p[0] == t[0] && p[1] == t[1] && p[2] == t[2] && p[3] == t[3]; }
static void mcpy(void *d, const void *s, size_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; }
static void mset(void *d, int v, size_t n) { uint8_t *a = d; while (n--) *a++ = (uint8_t)v; }
static int scmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (int)(uint8_t)*a - (int)(uint8_t)*b; }

static const char *nz(const char *s) { return s ? s : ""; }

typedef struct { uint8_t *base; size_t cap, used; } arena_t;
static void *aalloc(arena_t *a, size_t n) {
    void *p; n = (n + 7) & ~(size_t)7;
    if (a->used + n > a->cap) return 0;
    p = a->base + a->used; a->used += n; return p;
}

/* bounds-checked writer */
typedef struct { uint8_t *b; size_t cap, pos; int err; } W;
static uint8_t *wres(W *w, size_t n) {
    uint8_t *p;
    if (w->err || w->pos + n > w->cap) { w->err = 1; return 0; }
    p = w->b + w->pos; w->pos += n; return p;
}
static void wput(W *w, const void *s, size_t n) { uint8_t *p = wres(w, n); if (p) mcpy(p, s, n); }

/* ------------------------------------------------------------- zlib frame */
int mdb_cdb_unpack(const uint8_t *phys, size_t plen, uint8_t *logical, size_t cap, size_t *llen) {
    uint32_t hs, decl; size_t olen = 0; int r;
    if (plen < 16 || !tagis(phys, "mhbd")) return MDB_E_FORMAT;
    hs = rd32(phys + 4); decl = rd32(phys + 8);
    if (hs < 0xAA || hs >= plen || decl <= hs || decl > plen || cap < hs) return MDB_E_FORMAT;
    mcpy(logical, phys, hs);
    r = mdb_inflate(phys + hs, decl - hs, logical + hs, cap - hs, &olen, 0);
    if (r) return MDB_E_FORMAT;
    wr32(logical + 8, (uint32_t)(hs + olen));
    wr16(logical + 0xA8, 0);
    *llen = hs + olen;
    return MDB_OK;
}
size_t mdb_cdb_pack_bound(size_t llen) { return llen + 2 + 5 * (llen / 65535 + 2) + 4 + 16; }
int mdb_cdb_pack(const uint8_t *logical, size_t llen, uint8_t *phys, size_t cap, size_t *plen, const uint8_t guid[8]) {
    uint32_t hs; size_t z;
    if (llen < 16 || !tagis(logical, "mhbd")) return MDB_E_FORMAT;
    hs = rd32(logical + 4);
    if (hs < 0xAB + 57 || hs > llen || cap < hs + mdb_zstore_bound(llen - hs)) return MDB_E_SPACE;
    mcpy(phys, logical, hs);
    z = mdb_zstore(logical + hs, llen - hs, phys + hs);
    wr32(phys + 8, (uint32_t)(hs + z));
    wr32(phys + 0x0C, 2);
    wr16(phys + 0xA8, 1);
    *plen = hs + z;
    return mdb_cdb_sign(phys, *plen, guid) ? MDB_E_FORMAT : MDB_OK;
}

/* ------------------------------------------------------------------ model */
enum { S_TITLE, S_ARTIST, S_ALBUM, S_GENRE, S_COMP, S_AA, S_SHOW, S_N };
enum { O_TITLE, O_ALBUM, O_ARTIST, O_AA, O_COMP, O_SHOW, O_N };
typedef struct {
    const char *s[S_N], *so[O_N], *nk[S_N + 1];
    uint32_t track_id, album_id, artist_id, composer_id, disc, tn, season, episode;
    uint64_t dbid, dbid2;
    uint8_t compilation;
} tinfo_t;

static const char *getstr(arena_t *a, const uint8_t *mh) {
    uint32_t tl = rd32(mh + 8), enc = rd32(mh + 24), len = rd32(mh + 28); char *o;
    if (tl < 40 || 40 + len > tl) return "";
    o = aalloc(a, len * 2 + 2);
    if (!o) return 0;
    if (enc == 1) mdb_u16le_to_u8(mh + 40, len, o);
    else { mcpy(o, mh + 40, len); o[len] = 0; }
    return o;
}

typedef struct {
    const uint8_t *in; size_t in_len; uint32_t hs; int32_t tz;
    arena_t ar; tinfo_t *ti; uint32_t E, N, T;
    uint32_t max_tid, max_browse, max_mhip; uint64_t max_db, max_sql;
    const mdb_track_t *tr; mdb_assigned_t *asg;
    uint32_t new_albums, new_artists;
    uint32_t *order[16]; uint32_t order_kind[16]; int norder;
} C;

static const uint8_t *mhsd_at(C *c, size_t off, size_t *next) {
    const uint8_t *p = c->in + off;
    if (off + 96 > c->in_len || !tagis(p, "mhsd")) return 0;
    *next = off + rd32(p + 8);
    if (*next > c->in_len || *next < off + 96) return 0;
    return p;
}

static int parse_mhit(C *c, const uint8_t *m, tinfo_t *t) {
    uint32_t hl = rd32(m + 4), tl = rd32(m + 8), nm = rd32(m + 12), off, i;
    mset(t, 0, sizeof *t);
    for (i = 0; i < S_N; i++) t->s[i] = "";
    for (i = 0; i < O_N; i++) t->so[i] = "";
    if (hl < 0x1F8 || tl < hl) return MDB_E_FORMAT;
    t->track_id = rd32(m + 0x10); t->compilation = m[0x1E];
    t->tn = rd32(m + 0x2C); t->disc = rd32(m + 0x5C); t->dbid = rd64(m + 0x70); t->dbid2 = rd64(m + 0xA8);
    t->album_id = rd32(m + 0x120); t->artist_id = rd32(m + 0x1E0); t->composer_id = rd32(m + 0x1F4);
    t->season = rd32(m + 0xD4); t->episode = rd32(m + 0xD8);
    off = hl;
    for (i = 0; i < nm; i++) {
        const uint8_t *h; uint32_t ty, mt;
        if (off + 24 > tl) return MDB_E_FORMAT;
        h = m + off; mt = rd32(h + 8);
        if (!tagis(h, "mhod") || mt < 24 || off + mt > tl) return MDB_E_FORMAT;
        ty = rd32(h + 12);
        if (ty == 1 || ty == 4 || ty == 3 || ty == 5 || ty == 12 || ty == 22 || ty == 19 || ty == 23 || (ty >= 27 && ty <= 31)) {
            const char *s = getstr(&c->ar, h); int idx = -1, oi = -1;
            if (!s) return MDB_E_SCRATCH;
            switch (ty) {
            case 1: idx = S_TITLE; break; case 4: idx = S_ARTIST; break; case 3: idx = S_ALBUM; break;
            case 5: idx = S_GENRE; break; case 12: idx = S_COMP; break; case 22: idx = S_AA; break; case 19: idx = S_SHOW; break;
            case 27: oi = O_TITLE; break; case 28: oi = O_ALBUM; break; case 23: oi = O_ARTIST; break;
            case 29: oi = O_AA; break; case 30: oi = O_COMP; break; case 31: oi = O_SHOW; break;
            }
            if (idx >= 0) t->s[idx] = s;
            if (oi >= 0) t->so[oi] = s;
        }
        off += mt;
    }
    return MDB_OK;
}

static void track_from_new(tinfo_t *t, const mdb_track_t *n) {
    uint32_t i;
    mset(t, 0, sizeof *t);
    for (i = 0; i < O_N; i++) t->so[i] = "";
    t->s[S_TITLE] = nz(n->title); t->s[S_ARTIST] = nz(n->artist); t->s[S_ALBUM] = nz(n->album);
    t->s[S_GENRE] = nz(n->genre); t->s[S_COMP] = nz(n->composer); t->s[S_AA] = nz(n->album_artist); t->s[S_SHOW] = "";
    t->tn = n->track_no; t->disc = n->disc_no;
}

/* step 1: scan the document */
static int analyse(C *c) {
    size_t off = c->hs, next; uint32_t ei = 0; int r;
    const uint8_t *s;
    /* track count */
    c->E = 0;
    for (off = c->hs; off < c->in_len; off = next) {
        s = mhsd_at(c, off, &next);
        if (!s) return MDB_E_FORMAT;
        if (rd32(s + 12) == 1) c->E = rd32(s + rd32(s + 4) + 8);
    }
    c->T = c->E + c->N;
    c->ti = aalloc(&c->ar, sizeof(tinfo_t) * (c->T + 1));
    if (!c->ti) return MDB_E_SCRATCH;
    for (off = c->hs; off < c->in_len; off = next) {
        uint32_t type, hl, lh, cnt, i; size_t p;
        s = mhsd_at(c, off, &next); type = rd32(s + 12); hl = rd32(s + 4); lh = hl;
        if (type == 1) {
            p = off + hl + rd32(s + hl + 4); cnt = rd32(s + hl + 8);
            for (i = 0; i < cnt; i++) {
                const uint8_t *m = c->in + p; tinfo_t *t = &c->ti[ei];
                if (p + 24 > next || !tagis(m, "mhit")) return MDB_E_FORMAT;
                r = parse_mhit(c, m, t); if (r) return r;
                if (t->track_id > c->max_tid) c->max_tid = t->track_id;
                if (t->dbid > c->max_db) c->max_db = t->dbid;
                if (t->dbid2 > c->max_db) c->max_db = t->dbid2;
                if (t->album_id > c->max_browse) c->max_browse = t->album_id;
                if (t->artist_id > c->max_browse) c->max_browse = t->artist_id;
                if (t->composer_id > c->max_browse) c->max_browse = t->composer_id;
                p += rd32(m + 8); ei++;
            }
        } else if (type == 4 || type == 8) {
            p = off + hl + rd32(s + hl + 4); cnt = rd32(s + hl + 8);
            for (i = 0; i < cnt; i++) {
                const uint8_t *m = c->in + p; uint32_t id; uint64_t sq;
                if (p + 24 > next) return MDB_E_FORMAT;
                id = rd32(m + 0x10); sq = rd64(m + 0x14);
                if (id > c->max_browse) c->max_browse = id;
                if (sq > c->max_sql) c->max_sql = sq;
                p += rd32(m + 8);
            }
        } else if (type == 2 || type == 3) {
            p = off + hl + rd32(s + hl + 4); cnt = rd32(s + hl + 8);
            for (i = 0; i < cnt; i++) {
                const uint8_t *y = c->in + p; uint32_t nmod, nip, j, q;
                if (p + 184 > next || !tagis(y, "mhyp")) return MDB_E_FORMAT;
                nmod = rd32(y + 12); nip = rd32(y + 16); q = rd32(y + 4);
                for (j = 0; j < nmod; j++) q += rd32(y + q + 8);
                for (j = 0; j < nip; j++) {
                    uint64_t pid = rd64(y + q + 0x3C);
                    if (pid > c->max_mhip) c->max_mhip = (uint32_t)pid;
                    q += rd32(y + q + 8);
                }
                p += rd32(y + 8);
            }
        }
        (void)lh;
    }
    if (ei != c->E) return MDB_E_FORMAT;
    return MDB_OK;
}

/* step 2: ids of the groups and new tracks */
static int same_album(const tinfo_t *a, const tinfo_t *b) {
    const char *aa = a->s[S_AA][0] ? a->s[S_AA] : a->s[S_ARTIST], *ba = b->s[S_AA][0] ? b->s[S_AA] : b->s[S_ARTIST];
    return a->compilation == b->compilation && a->season == b->season && !scmp(a->s[S_ALBUM], b->s[S_ALBUM]) && !scmp(aa, ba) && !scmp(a->s[S_SHOW], b->s[S_SHOW]);
}
static int assign(C *c) {
    uint32_t i, j; uint64_t mx_db = c->max_db; uint32_t browse = c->max_browse; uint64_t sql = c->max_sql; uint32_t tid = c->max_tid;
    for (i = 0; i < c->N; i++) {
        tinfo_t *t = &c->ti[c->E + i]; track_from_new(t, &c->tr[i]);
        t->track_id = ++tid; t->dbid = t->dbid2 = ++mx_db;
        c->asg[i].track_id = t->track_id; c->asg[i].db_track_id = t->dbid; c->asg[i].position = 0;
        c->asg[i].album_id = c->asg[i].artist_id = c->asg[i].composer_id = 0; c->asg[i].album_sql_id = c->asg[i].artist_sql_id = 0;
        c->asg[i].mhip_pid[0] = c->asg[i].mhip_pid[1] = 0;
    }
    /* albums */
    for (i = 0; i < c->N; i++) {
        tinfo_t *t = &c->ti[c->E + i]; uint32_t id = 0;
        for (j = 0; j < c->E + i && !id; j++) if (c->ti[j].album_id && same_album(&c->ti[j], t)) id = c->ti[j].album_id;
        for (j = c->E; j < c->E + i && !id; j++) if (c->asg[j - c->E].album_id && same_album(&c->ti[j], t)) id = c->asg[j - c->E].album_id;
        if (!id) { id = ++browse; c->asg[i].album_sql_id = ++sql; c->new_albums++; }
        c->asg[i].album_id = id; t->album_id = id;
    }
    for (i = 0; i < c->N; i++) {
        tinfo_t *t = &c->ti[c->E + i]; uint32_t id = 0;
        for (j = 0; j < c->E + i && !id; j++) if (c->ti[j].artist_id && !scmp(c->ti[j].s[S_ARTIST], t->s[S_ARTIST])) id = c->ti[j].artist_id;
        if (!id) { id = ++browse; c->asg[i].artist_sql_id = ++sql; c->new_artists++; }
        c->asg[i].artist_id = id; t->artist_id = id;
    }
    for (i = 0; i < c->N; i++) {
        tinfo_t *t = &c->ti[c->E + i]; uint32_t id = 0; char *k1, *k2;
        if (!t->s[S_COMP][0]) continue;
        k1 = aalloc(&c->ar, mdb_casefold(t->s[S_COMP], 0) + 1); if (!k1) return MDB_E_SCRATCH;
        mdb_casefold(t->s[S_COMP], k1);
        for (j = 0; j < c->E + i && !id; j++) {
            if (!c->ti[j].composer_id || !c->ti[j].s[S_COMP][0]) continue;
            k2 = aalloc(&c->ar, mdb_casefold(c->ti[j].s[S_COMP], 0) + 1); if (!k2) return MDB_E_SCRATCH;
            mdb_casefold(c->ti[j].s[S_COMP], k2);
            if (!scmp(k1, k2)) id = c->ti[j].composer_id;
        }
        if (!id) id = ++browse;
        c->asg[i].composer_id = id; t->composer_id = id;
    }
    return MDB_OK;
}

/* --------------------------------------------------------------- indexes */
static const char *nk(C *c, tinfo_t *t, int f) {
    /* f: 0 title 1 album 2 artist 3 genre 4 composer 5 album-artist(0x23) 6 show */
    if (!t->nk[f]) {
        const char *src; char *o;
        switch (f) {
        case 0: src = t->so[O_TITLE][0] ? t->so[O_TITLE] : t->s[S_TITLE]; break;
        case 1: src = t->so[O_ALBUM][0] ? t->so[O_ALBUM] : t->s[S_ALBUM]; break;
        case 2: src = t->so[O_ARTIST][0] ? t->so[O_ARTIST] : t->s[S_ARTIST]; break;
        case 3: src = t->s[S_GENRE]; break;
        case 4: src = t->so[O_COMP][0] ? t->so[O_COMP] : t->s[S_COMP]; break;
        case 5: src = t->so[O_AA][0] ? t->so[O_AA] : t->s[S_AA][0] ? t->s[S_AA] : t->so[O_ARTIST][0] ? t->so[O_ARTIST] : t->s[S_ARTIST]; break;
        default: src = t->so[O_SHOW][0] ? t->so[O_SHOW] : t->s[S_SHOW]; break;
        }
        o = aalloc(&c->ar, mdb_norm_key(src, 0) + 1);
        if (!o) return 0;
        mdb_norm_key(src, o); t->nk[f] = o;
    }
    return t->nk[f];
}
static int cmpn(uint32_t a, uint32_t b) { return a < b ? -1 : a > b ? 1 : 0; }
/* comparison order of the reference sort key */
static int cmp_kind(C *c, int kind, uint32_t ia, uint32_t ib, int *oom) {
    tinfo_t *a = &c->ti[ia], *b = &c->ti[ib]; int r, i, n = 0; int seq[6];
    static const int8_t dummy = 0; (void)dummy;
    switch (kind) {
    case 3: seq[n++] = 0; break;
    case 4: seq[n++] = 1; seq[n++] = -1; seq[n++] = -2; seq[n++] = 0; break;
    case 5: seq[n++] = 2; seq[n++] = 1; seq[n++] = -1; seq[n++] = -2; seq[n++] = 0; break;
    case 7: seq[n++] = 3; seq[n++] = 2; seq[n++] = 1; seq[n++] = -1; seq[n++] = -2; seq[n++] = 0; break;
    case 0x12: seq[n++] = 4; seq[n++] = 1; seq[n++] = -1; seq[n++] = -2; seq[n++] = 0; break;
    case 0x23: seq[n++] = 5; seq[n++] = 1; seq[n++] = -1; seq[n++] = -2; seq[n++] = 0; break;
    case 0x1D: seq[n++] = 6; seq[n++] = -3; seq[n++] = -4; seq[n++] = 0; break;
    case 0x1E: seq[n++] = -3; seq[n++] = -4; seq[n++] = 6; seq[n++] = 0; break;
    default: seq[n++] = -4; seq[n++] = -3; seq[n++] = 6; seq[n++] = 0; break; /* 0x1F */
    }
    for (i = 0; i < n; i++) {
        int f = seq[i];
        if (f >= 0) {
            const char *x = nk(c, a, f), *y = nk(c, b, f);
            if (!x || !y) { *oom = 1; return 0; }
            r = scmp(x, y);
        } else if (f == -1) r = cmpn(a->disc, b->disc);
        else if (f == -2) r = cmpn(a->tn, b->tn);
        else if (f == -3) r = cmpn(a->season, b->season);
        else r = cmpn(a->episode, b->episode);
        if (r) return r;
    }
    return 0;
}
static int kind_ok(int k) { return k == 3 || k == 4 || k == 5 || k == 7 || k == 0x12 || k == 0x1D || k == 0x1E || k == 0x1F || k == 0x23; }

static uint32_t *get_order(C *c, int kind, int *err) {
    int i; uint32_t n = c->T, w, lo, mid, hi, *a, *b, *tmp; int oom = 0;
    for (i = 0; i < c->norder; i++) if ((int)c->order_kind[i] == kind) return c->order[i];
    if (c->norder >= 16) { *err = MDB_E_UNSUPPORTED; return 0; }
    a = aalloc(&c->ar, 4 * (n + 1)); tmp = aalloc(&c->ar, 4 * (n + 1));
    if (!a || !tmp) { *err = MDB_E_SCRATCH; return 0; }
    for (lo = 0; lo < n; lo++) a[lo] = lo;
    /* stable bottom-up merge sort */
    for (w = 1; w < n; w <<= 1) {
        for (lo = 0; lo < n; lo += 2 * w) {
            uint32_t i1, i2, k;
            mid = lo + w < n ? lo + w : n; hi = lo + 2 * w < n ? lo + 2 * w : n;
            i1 = lo; i2 = mid; k = lo;
            while (i1 < mid && i2 < hi) {
                int r = cmp_kind(c, kind, a[i2], a[i1], &oom);   /* take from the right only if strictly smaller */
                if (oom) { *err = MDB_E_SCRATCH; return 0; }
                tmp[k++] = r < 0 ? a[i2++] : a[i1++];
            }
            while (i1 < mid) tmp[k++] = a[i1++];
            while (i2 < hi) tmp[k++] = a[i2++];
        }
        b = a; a = tmp; tmp = b;
    }
    c->order[c->norder] = a; c->order_kind[c->norder] = (uint32_t)kind; c->norder++;
    return a;
}

/* ----------------------------------------------------------- mhod writer */
static void put_mhod_hdr(W *w, uint32_t type, uint32_t tl) {
    uint8_t *p = wres(w, 24);
    if (!p) return;
    mset(p, 0, 24); mcpy(p, "mhod", 4); wr32(p + 4, 24); wr32(p + 8, tl); wr32(p + 12, type);
}
static void put_mhod_str(W *w, uint32_t type, const char *s) {
    size_t n = mdb_u8_to_u16le(s, 0); uint8_t *p;
    put_mhod_hdr(w, type, (uint32_t)(40 + n));
    p = wres(w, 16 + n);
    if (!p) return;
    wr32(p, 1); wr32(p + 4, (uint32_t)n); wr32(p + 8, 1); wr32(p + 12, 0);
    mdb_u8_to_u16le(s, p + 16);
}
static uint32_t to_mac(const C *c, uint32_t unix_s) { return (uint32_t)((int64_t)unix_s + 2082844800LL + c->tz); }
static uint32_t float_bits(uint32_t v) {
    uint32_t m = 0, e;
    if (!v) return 0;
    while ((v >> (m + 1)) != 0) m++;
    e = 127 + m;
    return (e << 23) | ((m <= 23 ? (v << (23 - m)) : (v >> (m - 23))) & 0x7FFFFF);
}
static void put_mhit(C *c, W *w, uint32_t i) {
    const mdb_track_t *t = &c->tr[i]; const mdb_assigned_t *a = &c->asg[i];
    size_t st = w->pos; uint8_t *h; uint32_t nm = 0; char loc[512]; size_t k, j;
    h = wres(w, 624);
    if (!h) return;
    mcpy(h, MDB_MHIT_TMPL, 624);
    wr32(h + 0x10, a->track_id); h[0x1C] = t->vbr ? 1 : 0;
    wr32(h + 0x20, to_mac(c, t->last_modified)); wr32(h + 0x24, t->size); wr32(h + 0x28, t->length_ms);
    wr32(h + 0x2C, t->track_no); wr32(h + 0x34, t->year); wr32(h + 0x38, t->bitrate_kbps);
    wr32(h + 0x3C, t->sample_rate << 16); wr32(h + 0x5C, t->disc_no); wr32(h + 0x68, to_mac(c, t->date_added));
    wr64(h + 0x70, a->db_track_id); wr32(h + 0x88, float_bits(t->sample_rate)); wr64(h + 0xA8, a->db_track_id);
    wr32(h + 0x30, t->total_tracks); wr32(h + 0x60, t->total_discs); h[0x7A] = (uint8_t)t->bpm; h[0x7B] = (uint8_t)(t->bpm >> 8);
    if (t->compilation) h[0x1E] = 1;
    if (t->rating) h[0x1F] = (uint8_t)t->rating;
    if (t->release_date) wr32(h + 0x8C, (uint32_t)(t->release_date + 2082844800LL));
    if (t->lyrics_flag) h[0xB0] = 1;
    if (t->artwork_id) { h[0x7C] = (uint8_t)t->artwork_count; h[0x7D] = (uint8_t)(t->artwork_count >> 8); wr32(h + 0x80, t->artwork_size); h[0xA4] = 1; wr32(h + 0x160, t->artwork_id); }
    if (t->explicit_flag) h[0x92] = (uint8_t)t->explicit_flag;
    if (t->gapless_album) { h[0x102] = 1; h[0x103] = 0; }
    wr32(h + 0x120, a->album_id); wr32(h + 0x12C, t->size); wr32(h + 0x1E0, a->artist_id); wr32(h + 0x1F4, a->composer_id);
    put_mhod_str(w, 1, nz(t->title)); put_mhod_str(w, 4, nz(t->artist)); put_mhod_str(w, 3, nz(t->album));
    put_mhod_str(w, 5, nz(t->genre)); put_mhod_str(w, 19, ""); put_mhod_str(w, 20, ""); put_mhod_str(w, 22, nz(t->album_artist));
    nm = 7;
    k = 0; loc[k++] = ':';
    for (j = 0; nz(t->location)[j] && k < sizeof loc - 1; j++) loc[k++] = t->location[j] == '/' ? ':' : t->location[j];
    loc[k] = 0;
    put_mhod_str(w, 2, loc); nm++;
    if (nz(t->comment)[0]) { put_mhod_str(w, 8, t->comment); nm++; }
    if (nz(t->lyrics)[0]) { put_mhod_str(w, 10, t->lyrics); nm++; }
    if (nz(t->composer)[0]) { put_mhod_str(w, 12, t->composer); nm++; }
    if (nz(t->grouping)[0]) { put_mhod_str(w, 13, t->grouping); nm++; }
    if (nz(t->description)[0]) { put_mhod_str(w, 14, t->description); nm++; }
    if (nz(t->subtitle)[0]) { put_mhod_str(w, 18, t->subtitle); nm++; }
    if (nz(t->content_provider)[0]) { put_mhod_str(w, 37, t->content_provider); nm++; }
    if (nz(t->copyright)[0]) { put_mhod_str(w, 39, t->copyright); nm++; }
    put_mhod_str(w, 6, "MPEG audio file"); nm++;
    if (w->err) return;
    wr32(w->b + st + 8, (uint32_t)(w->pos - st)); wr32(w->b + st + 0xC, nm);
}
static void put_mhip(W *w, const mdb_assigned_t *a, int slot, uint32_t pos) {
    size_t st = w->pos; uint8_t *p = wres(w, 76);
    if (!p) return;
    mcpy(p, MDB_MHIP_TMPL, 76);
    wr32(p + 0x18, a->track_id); wr64(p + 0x2C, a->db_track_id); wr64(p + 0x3C, a->mhip_pid[slot]);
    put_mhod_hdr(w, 100, 44);
    p = wres(w, 20); if (!p) return;
    mset(p, 0, 20); wr32(p, pos);
    wr32(w->b + st + 8, (uint32_t)(w->pos - st)); wr32(w->b + st + 0xC, 1);
}
static void put_mhia(C *c, W *w, uint32_t i) {
    const mdb_track_t *t = &c->tr[i]; const mdb_assigned_t *a = &c->asg[i];
    size_t st = w->pos; uint8_t *p = wres(w, 88); uint32_t nm = 0, k; uint64_t mindb = a->db_track_id;
    if (!p) return;
    mcpy(p, MDB_MHIA_TMPL, 88);
    for (k = 0; k < c->N; k++) if (c->asg[k].album_id == a->album_id && c->asg[k].db_track_id < mindb) mindb = c->asg[k].db_track_id;
    wr32(p + 0x10, a->album_id); wr64(p + 0x14, a->album_sql_id); wr64(p + 0x20, mindb);
    if (nz(t->album)[0]) { put_mhod_str(w, 200, t->album); nm++; }
    { const char *aa = nz(t->album_artist)[0] ? t->album_artist : nz(t->artist);
      if (aa[0]) { put_mhod_str(w, 201, aa); nm++; } }
    if (w->err) return;
    wr32(w->b + st + 8, (uint32_t)(w->pos - st)); wr32(w->b + st + 0xC, nm);
}
static void put_mhii(C *c, W *w, uint32_t i) {
    const mdb_track_t *t = &c->tr[i]; const mdb_assigned_t *a = &c->asg[i];
    size_t st = w->pos; uint8_t *p = wres(w, 80);
    (void)c;
    if (!p) return;
    mcpy(p, MDB_MHII_TMPL, 80);
    wr32(p + 0x10, a->artist_id); wr64(p + 0x14, a->artist_sql_id);
    put_mhod_str(w, 300, nz(t->artist));
    if (w->err) return;
    wr32(w->b + st + 8, (uint32_t)(w->pos - st)); wr32(w->b + st + 0xC, 1);
}

static int put_index(C *c, W *w, int kind) {
    int err = 0; uint32_t *o = get_order(c, kind, &err), i, n = c->T, ne = 0, tl; uint8_t *p;
    if (!o) return err;
    put_mhod_hdr(w, 52, 72 + 4 * n);
    p = wres(w, 48 + 4 * (size_t)n); if (!p) return MDB_E_SPACE;
    mset(p, 0, 48 + 4 * (size_t)n); wr32(p, (uint32_t)kind); wr32(p + 4, n);
    for (i = 0; i < n; i++) wr32(p + 48 + 4 * i, o[i]);
    /* jump table */
    {
        uint32_t last = 0xFFFFFFFFu; size_t save;
        for (i = 0; i < n; i++) {
            const char *k = 0; tinfo_t *t = &c->ti[o[i]]; uint32_t letter;
            switch (kind) {
            case 3: k = nk(c, t, 0); break; case 4: k = nk(c, t, 1); break; case 5: k = nk(c, t, 2); break;
            case 7: k = nk(c, t, 3); break; case 0x12: k = nk(c, t, 4); break; case 0x23: k = nk(c, t, 5); break;
            case 0x1D: k = nk(c, t, 6); break; default: k = ""; break;
            }
            if (!k) return MDB_E_SCRATCH;
            letter = (kind == 0x1E || kind == 0x1F) ? '0' : mdb_jump_letter(k);
            if (letter != last) { ne++; last = letter; }
        }
        tl = 40 + 12 * ne;
        put_mhod_hdr(w, 53, tl);
        p = wres(w, 16 + 12 * (size_t)ne); if (!p) return MDB_E_SPACE;
        mset(p, 0, 16 + 12 * (size_t)ne); wr32(p, (uint32_t)kind); wr32(p + 4, ne);
        save = 16 - 12; last = 0xFFFFFFFFu;
        for (i = 0; i < n; i++) {
            const char *k = 0; tinfo_t *t = &c->ti[o[i]]; uint32_t letter;
            switch (kind) {
            case 3: k = nk(c, t, 0); break; case 4: k = nk(c, t, 1); break; case 5: k = nk(c, t, 2); break;
            case 7: k = nk(c, t, 3); break; case 0x12: k = nk(c, t, 4); break; case 0x23: k = nk(c, t, 5); break;
            case 0x1D: k = nk(c, t, 6); break; default: k = ""; break;
            }
            letter = (kind == 0x1E || kind == 0x1F) ? '0' : mdb_jump_letter(k);
            if (letter != last) { save += 12; wr32(p + save, letter); wr32(p + save + 4, i); wr32(p + save + 8, 1); last = letter; }
            else wr32(p + save + 8, rd32(p + save + 8) + 1);
        }
    }
    return MDB_OK;
}

/* ------------------------------------------------------------- orchestration */
size_t mdb_cdb_out_bound(size_t in_len, uint32_t n) { return in_len + (size_t)n * 4096 + 8192; }
size_t mdb_cdb_scratch_need(size_t in_len, uint32_t n) { return in_len * 3 + (size_t)n * 4096 + 65536; }

int mdb_cdb_add_tracks(const uint8_t *in, size_t in_len, const mdb_track_t *tr, uint32_t n,
                       uint8_t *out, size_t out_cap, size_t *out_len, mdb_assigned_t *asg,
                       void *scratch, size_t scratch_size) {
    C c; W w; size_t off, next; int r; uint32_t i, mhip_seq, slot = 0, hsz;
    if (!in || !tr || !out || !asg || !scratch || n == 0 || in_len < 244 || !tagis(in, "mhbd")) return MDB_E_ARG;
    mset(&c, 0, sizeof c);
    c.in = in; c.in_len = in_len; c.hs = rd32(in + 4); c.tz = (int32_t)rd32(in + 0x6C); c.tr = tr; c.asg = asg; c.N = n;
    c.ar.base = scratch; c.ar.cap = scratch_size;
    if (c.hs < 0xAB + 57 || c.hs > in_len || rd32(in + 8) != in_len) return MDB_E_FORMAT;
    r = analyse(&c); if (r) return r;
    r = assign(&c); if (r) return r;
    mhip_seq = c.max_mhip;
    w.b = out; w.cap = out_cap; w.pos = 0; w.err = 0;
    wput(&w, in, c.hs);
    for (off = c.hs; off < in_len; off = next) {
        const uint8_t *s = mhsd_at(&c, off, &next); uint32_t type = rd32(s + 12), hl = rd32(s + 4), lc;
        size_t st = w.pos; const uint8_t *L; size_t cstart;
        if (type != 1 && type != 2 && type != 3 && type != 4 && type != 8) { wput(&w, s, next - off); continue; }
        L = s + hl; lc = rd32(L + 8); hsz = rd32(L + 4);
        wput(&w, s, hl);                         /* mhsd header */
        { size_t lst = w.pos; wput(&w, L, hsz);   /* list: copy, then adjust the count */
          cstart = off + hl + hsz;
          if (type == 1) {
              wput(&w, in + cstart, next - cstart);
              for (i = 0; i < n; i++) put_mhit(&c, &w, i);
              if (!w.err) wr32(w.b + lst + 8, lc + n);
          } else if (type == 4 || type == 8) {
              uint32_t add = 0;
              wput(&w, in + cstart, next - cstart);
              for (i = 0; i < n; i++) {
                  if (type == 4 && asg[i].album_sql_id) { put_mhia(&c, &w, i); add++; }
                  if (type == 8 && asg[i].artist_sql_id) { put_mhii(&c, &w, i); add++; }
              }
              if (!w.err) wr32(w.b + lst + 8, lc + add);
          } else {
              size_t p = cstart; uint32_t y;
              for (y = 0; y < lc; y++) {
                  const uint8_t *m = in + p; uint32_t mt = rd32(m + 8);
                  if (p + mt > next || !tagis(m, "mhyp")) return MDB_E_FORMAT;
                  if (!(m[0x14] & 1) || rd16(m + 0x50)) { wput(&w, m, mt); p += mt; continue; }
                  /* master playlist */
                  {
                      size_t ys = w.pos; uint32_t nmod = rd32(m + 12), nip = rd32(m + 16), q = rd32(m + 4), j; int has_idx = 0;
                      for (j = 0, q = rd32(m + 4); j < nmod; j++) { if (rd32(m + q + 12) == 52) has_idx = 1; q += rd32(m + q + 8); }
                      q = rd32(m + 4);
                      wput(&w, m, q);
                      for (j = 0; j < nmod; j++) {
                          const uint8_t *h = m + q; uint32_t ml = rd32(h + 8), ty = rd32(h + 12);
                          if (ty == 52) {
                              int kind = (int)rd32(h + 24);
                              if (!kind_ok(kind)) return MDB_E_UNSUPPORTED;
                              r = put_index(&c, &w, kind); if (r) return r;
                              q += ml; j++;                      /* consumes the following type-53 entry */
                              if (j >= nmod || rd32(m + q + 12) != 53) return MDB_E_FORMAT;
                              q += rd32(m + q + 8);
                              continue;
                          }
                          if (ty == 53) return MDB_E_FORMAT;
                          wput(&w, h, ml); q += ml;
                      }
                      if (!has_idx) {                          /* empty library: create the 5 default indexes */
                          static const int dk[5] = {3, 5, 4, 7, 0x12}; int di;
                          for (di = 0; di < 5; di++) { r = put_index(&c, &w, dk[di]); if (r) return r; }
                          if (!w.err) wr32(w.b + ys + 12, nmod + 10);
                      }
                      wput(&w, m + q, rd32(m + 8) - q);        /* existing mhips */
                      for (i = 0; i < n; i++) {
                          asg[i].mhip_pid[slot] = ++mhip_seq;
                          put_mhip(&w, &asg[i], (int)slot, nip + i);
                      }
                      if (!w.err) { wr32(w.b + ys + 8, (uint32_t)(w.pos - ys)); wr32(w.b + ys + 16, nip + n); }
                  }
                  p += mt;
              }
              slot += 0;   /* slot advances per mhsd (below) */
          }
        }
        if (type == 2 || type == 3) slot = slot < 1 ? slot + 1 : slot;
        if (w.err) return MDB_E_SPACE;
        wr32(w.b + st + 8, (uint32_t)(w.pos - st));
    }
    if (w.err) return MDB_E_SPACE;
    wr32(out + 8, (uint32_t)w.pos);
    *out_len = w.pos;
    return MDB_OK;
}
