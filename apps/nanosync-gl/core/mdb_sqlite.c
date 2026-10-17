#include "mdb_sqlite.h"

#define PG MDB_SQL_PAGE
static void mcpy(void *d, const void *s, size_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; }
static void mset(void *d, int v, size_t n) { uint8_t *a = d; while (n--) *a++ = (uint8_t)v; }
void *mdb_aalloc(mdb_arena_t *a, size_t n) {
    void *p; n = (n + 7) & ~(size_t)7;
    if (a->used + n > a->cap) return 0;
    p = a->base + a->used; a->used += n; return p;
}
uint64_t mdb_f64_from_u32(uint32_t v) {
    uint32_t m = 0; uint64_t e, frac;
    if (!v) return 0;
    while ((v >> (m + 1)) != 0) m++;
    e = 1023 + m; frac = ((uint64_t)v << (52 - m)) & 0xFFFFFFFFFFFFFULL;
    return (e << 52) | frac;
}

/* ---- varints and records ---- */
static int varint_len(uint64_t v) { int n = 1; if (v > 0x00FFFFFFFFFFFFFFULL) return 9; while (v >= 0x80) { v >>= 7; n++; } return n; }
static int put_varint(uint8_t *p, uint64_t v) {
    int i, n;
    if (v > 0x00FFFFFFFFFFFFFFULL) {
        p[8] = (uint8_t)v; v >>= 8;
        for (i = 7; i >= 0; i--) { p[i] = (uint8_t)((v & 0x7F) | 0x80); v >>= 7; }
        return 9;
    }
    n = varint_len(v);
    for (i = n - 1; i >= 0; i--) { p[i] = (uint8_t)((v & 0x7F) | (i == n - 1 ? 0 : 0x80)); v >>= 7; }
    return n;
}
static uint64_t serial(const mval_t *v, int *blen) {
    int64_t x;
    switch (v->t) {
    case MV_NULL: *blen = 0; return 0;
    case MV_REAL: *blen = 8; return 7;
    case MV_TEXT: *blen = (int)v->n; return 13 + 2 * (uint64_t)v->n;
    default:
        x = v->i;
        if (x == 0) { *blen = 0; return 8; }
        if (x == 1) { *blen = 0; return 9; }
        if (x >= -128 && x <= 127) { *blen = 1; return 1; }
        if (x >= -32768 && x <= 32767) { *blen = 2; return 2; }
        if (x >= -8388608 && x <= 8388607) { *blen = 3; return 3; }
        if (x >= -2147483648LL && x <= 2147483647LL) { *blen = 4; return 4; }
        if (x >= -140737488355328LL && x <= 140737488355327LL) { *blen = 6; return 5; }
        *blen = 8; return 6;
    }
}
static size_t hdr_total(size_t h2) { int l = varint_len(h2 + 1); if (varint_len(h2 + (size_t)l) != l) l = varint_len(h2 + (size_t)l); return h2 + (size_t)l; }
static size_t rec_size(const mval_t *v, int n) {
    size_t h2 = 0, bs = 0; int i, b;
    for (i = 0; i < n; i++) { uint64_t st = serial(&v[i], &b); h2 += varint_len(st); bs += (size_t)b; }
    return hdr_total(h2) + bs;
}
static size_t rec_put(const mval_t *v, int n, uint8_t *o) {
    size_t h2 = 0, pos; int i, b;
    for (i = 0; i < n; i++) { uint64_t st = serial(&v[i], &b); h2 += varint_len(st); }
    pos = (size_t)put_varint(o, hdr_total(h2));
    for (i = 0; i < n; i++) pos += (size_t)put_varint(o + pos, serial(&v[i], &b));
    for (i = 0; i < n; i++) {
        const mval_t *x = &v[i]; uint64_t st = serial(x, &b); int k;
        if (x->t == MV_TEXT) { mcpy(o + pos, x->s, x->n); pos += x->n; }
        else if (x->t == MV_REAL) { for (k = 7; k >= 0; k--) o[pos++] = (uint8_t)((uint64_t)x->i >> (8 * k)); }
        else if (x->t == MV_INT && st >= 1 && st <= 6) { for (k = b - 1; k >= 0; k--) o[pos++] = (uint8_t)((uint64_t)x->i >> (8 * k)); }
    }
    return pos;
}
static int vcmp(const mval_t *a, const mval_t *b) {
    int ca = a->t == MV_NULL ? 0 : a->t == MV_TEXT ? 3 : 1, cb = b->t == MV_NULL ? 0 : b->t == MV_TEXT ? 3 : 1;
    if (ca != cb) return ca < cb ? -1 : 1;
    if (ca == 3) {
        uint32_t k, m = a->n < b->n ? a->n : b->n;
        for (k = 0; k < m; k++) if (a->s[k] != b->s[k]) return (uint8_t)a->s[k] < (uint8_t)b->s[k] ? -1 : 1;
        return a->n < b->n ? -1 : a->n > b->n ? 1 : 0;
    }
    if (ca == 1) return a->i < b->i ? -1 : a->i > b->i ? 1 : 0;
    return 0;
}

/* ---- B-tree builder ---- */
typedef struct { uint8_t *out; uint32_t cap_pages, next; mdb_arena_t *ar; int err; } S;
typedef struct { const uint8_t *key; uint32_t klen; int64_t rowid; const mval_t *vals; int nvals; } ent_t;
typedef struct { uint32_t pg; int64_t maxrow; const ent_t *div; } node_t;

static uint8_t *page(S *s, uint32_t pgno) {
    if (pgno == 0 || pgno > s->cap_pages) { s->err = -2; return 0; }
    return s->out + (size_t)(pgno - 1) * PG;
}
static uint32_t newpage(S *s) { uint32_t p = s->next++; if (p > s->cap_pages) s->err = -2; return p; }
static void w16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void w32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static void write_page(S *s, uint32_t pgno, uint8_t type, uint32_t right, uint8_t **cells, const uint32_t *clen, uint32_t nc) {
    uint8_t *p = page(s, pgno); uint32_t hdr = (type == 0x0d || type == 0x0a) ? 8 : 12, base = (pgno == 1) ? 100 : 0, i, top = PG;
    if (!p) return;
    mset(p + base, 0, PG - base);
    p[base] = type; w16(p + base + 3, nc);
    for (i = 0; i < nc; i++) { top -= clen[i]; mcpy(p + top, cells[i], clen[i]); w16(p + base + hdr + 2 * i, top); }
    w16(p + base + 5, top);
    if (hdr == 12) w32(p + base + 8, right);
}

static void build_tree(S *s, int is_index, const ent_t *e, uint32_t n, uint32_t root) {
    uint32_t a = 0, nodes = 0, i;
    node_t *lvl, *nxt; uint8_t *cb, **cp; uint32_t *cl;
    uint32_t avail = PG - (root == 1 ? 100 : 0);
    if (s->err) return;
    lvl = mdb_aalloc(s->ar, sizeof(node_t) * (n + 2)); nxt = mdb_aalloc(s->ar, sizeof(node_t) * (n + 2));
    cb = mdb_aalloc(s->ar, PG * 2); cl = mdb_aalloc(s->ar, 4 * (n + 2)); cp = mdb_aalloc(s->ar, sizeof(uint8_t *) * (n + 2));
    if (!lvl || !nxt || !cb || !cl || !cp) { s->err = -3; return; }
    for (;;) {
        uint32_t used = 0, nc = 0, j = a, pos = 0, k, pgno; int last;
        uint32_t lim = avail - 8;
        while (j < n) {
            uint32_t sz = (uint32_t)varint_len(e[j].klen) + e[j].klen + (is_index ? 0 : (uint32_t)varint_len((uint64_t)e[j].rowid));
            if ((is_index && e[j].klen > 1002) || (!is_index && e[j].klen > 4061)) { s->err = -4; return; }
            if (used + sz + 2 > lim) break;
            used += sz + 2; nc++; j++;
        }
        if (nc == 0 && n > 0) { s->err = -4; return; }
        last = (j >= n);
        if (!last && is_index && j == n - 1 && nc > 1) { nc--; j--; }   /* avoid an empty leaf after promotion */
        pgno = (a == 0 && last) ? root : newpage(s);
        if (s->err) return;
        for (k = 0; k < nc; k++) {
            const ent_t *x = &e[a + k]; uint32_t o = pos;
            pos += (uint32_t)put_varint(cb + pos, x->klen);
            if (!is_index) pos += (uint32_t)put_varint(cb + pos, (uint64_t)x->rowid);
            mcpy(cb + pos, x->key, x->klen); pos += x->klen;
            cp[k] = cb + o; cl[k] = pos - o;
        }
        write_page(s, pgno, is_index ? 0x0a : 0x0d, 0, cp, cl, nc);
        if (a == 0 && last) return;
        lvl[nodes].pg = pgno; lvl[nodes].maxrow = nc ? e[a + nc - 1].rowid : 0; lvl[nodes].div = 0; nodes++;
        if (is_index && !last) { lvl[nodes - 1].div = &e[j]; j++; }
        a = j;
        if (last) break;
    }
    while (nodes > 1) {
        uint32_t i0 = 0, nn = 0;
        uint32_t lim = avail - 12;
        while (i0 < nodes) {
            uint32_t used = 0, nc = 0, j = i0, pgno, k, pos = 0; int whole;
            while (j + 1 < nodes) {
                uint32_t sz = is_index ? 4 + (uint32_t)varint_len(lvl[j].div->klen) + lvl[j].div->klen : 4 + (uint32_t)varint_len((uint64_t)lvl[j].maxrow);
                if (used + sz + 2 > lim) break;
                used += sz + 2; nc++; j++;
            }
            if (nc == 0 && j + 1 < nodes) { s->err = -4; return; }
            whole = (j + 1 >= nodes) && i0 == 0;
            pgno = whole ? root : newpage(s);
            if (s->err) return;
            for (k = 0; k < nc; k++) {
                const node_t *x = &lvl[i0 + k]; uint32_t o = pos;
                w32(cb + pos, x->pg); pos += 4;
                if (is_index) { pos += (uint32_t)put_varint(cb + pos, x->div->klen); mcpy(cb + pos, x->div->key, x->div->klen); pos += x->div->klen; }
                else pos += (uint32_t)put_varint(cb + pos, (uint64_t)x->maxrow);
                cp[k] = cb + o; cl[k] = pos - o;
            }
            write_page(s, pgno, is_index ? 0x02 : 0x05, lvl[j].pg, cp, cl, nc);
            if (whole) return;
            nxt[nn].pg = pgno; nxt[nn].maxrow = lvl[j].maxrow; nxt[nn].div = is_index ? lvl[j].div : 0; nn++;
            i0 = j + 1;
        }
        for (i = 0; i < nn; i++) lvl[i] = nxt[i];
        nodes = nn;
    }
}

/* stable merge sort */
typedef int (*cmpf)(const ent_t *, const ent_t *);
static int cmp_rowid(const ent_t *a, const ent_t *b) { return a->rowid < b->rowid ? -1 : a->rowid > b->rowid ? 1 : 0; }
static int cmp_key(const ent_t *a, const ent_t *b) {
    int i, n = a->nvals < b->nvals ? a->nvals : b->nvals, r;
    for (i = 0; i < n; i++) { r = vcmp(&a->vals[i], &b->vals[i]); if (r) return r; }
    return 0;
}
static int sort_ents(S *s, ent_t *e, uint32_t n, cmpf f) {
    ent_t *tmp = mdb_aalloc(s->ar, sizeof(ent_t) * (n + 1)), *a = e, *b = tmp, *t; uint32_t w, lo, hi, mid, k, i1, i2;
    if (!tmp) return -3;
    for (w = 1; w < n; w <<= 1) {
        for (lo = 0; lo < n; lo += 2 * w) {
            mid = lo + w < n ? lo + w : n; hi = lo + 2 * w < n ? lo + 2 * w : n; i1 = lo; i2 = mid; k = lo;
            while (i1 < mid && i2 < hi) b[k++] = f(&a[i2], &a[i1]) < 0 ? a[i2++] : a[i1++];
            while (i1 < mid) b[k++] = a[i1++];
            while (i2 < hi) b[k++] = a[i2++];
        }
        t = a; a = b; b = t;
    }
    if (a != e) for (k = 0; k < n; k++) e[k] = a[k];
    return 0;
}

static mval_t vtext(const char *s) { mval_t v; uint32_t n = 0; while (s && s[n]) n++; v.t = MV_TEXT; v.i = 0; v.s = s; v.n = n; return v; }
static mval_t vint(int64_t x) { mval_t v; v.t = MV_INT; v.i = x; v.s = 0; v.n = 0; return v; }
static mval_t vnull(void) { mval_t v; v.t = MV_NULL; v.i = 0; v.s = 0; v.n = 0; return v; }

/* file change counter (header +24, version-valid-for +92). A reader that keeps the database open (the iPod's own
 * SQLite connection) drops its page cache only when this value changes, so each import uses a new one. */
uint32_t mdb_sqlite_change_counter = 1;

int mdb_sqlite_write(const mdb_schema_t *sc, mtab_t *tabs, uint8_t *out, size_t cap, size_t *outlen, mdb_arena_t *ar) {
    S s; uint32_t k, i, nobj = sc->nobj, roots = nobj + 1; int r;
    ent_t *master; uint8_t *hdr;
    s.out = out; s.cap_pages = (uint32_t)(cap / PG); s.next = roots + 1; s.ar = ar; s.err = 0;
    master = mdb_aalloc(ar, sizeof(ent_t) * (nobj + 1));
    if (!master) return -3;
    /* tables and indexes */
    for (k = 0; k < nobj; k++) {
        const mdb_obj_t *ob = &sc->obj[k]; uint32_t root = k + 2; ent_t *ents; uint32_t n;
        mval_t mv[5]; uint8_t *rec; size_t rs;
        if (!ob->is_index) {
            const mdb_tab_t *t = &sc->tab[ob->ref]; mtab_t *mt = &tabs[ob->ref];
            n = mt->n; ents = mdb_aalloc(ar, sizeof(ent_t) * (n + 1));
            if (!ents) return -3;
            for (i = 0; i < n; i++) {
                mval_t *v = mt->rows[i].v, *tmp = mdb_aalloc(ar, sizeof(mval_t) * t->ncols); uint8_t *rb; size_t sz; uint32_t c;
                if (!tmp) return -3;
                for (c = 0; c < t->ncols; c++) tmp[c] = v[c];
                if (t->rowid_col >= 0) tmp[t->rowid_col] = vnull();
                sz = rec_size(tmp, t->ncols); rb = mdb_aalloc(ar, sz);
                if (!rb) return -3;
                rec_put(tmp, t->ncols, rb);
                ents[i].key = rb; ents[i].klen = (uint32_t)sz; ents[i].rowid = mt->rows[i].rowid; ents[i].vals = 0; ents[i].nvals = 0;
            }
            r = sort_ents(&s, ents, n, cmp_rowid); if (r) return r;
            build_tree(&s, 0, ents, n, root);
        } else {
            const mdb_idx_t *ix = &sc->idx[ob->ref]; const mdb_tab_t *t = &sc->tab[ix->tab]; mtab_t *mt = &tabs[ix->tab];
            n = mt->n; ents = mdb_aalloc(ar, sizeof(ent_t) * (n + 1));
            if (!ents) return -3;
            for (i = 0; i < n; i++) {
                mval_t *kv = mdb_aalloc(ar, sizeof(mval_t) * (ix->ncols + 1)); uint8_t *rb; size_t sz; uint32_t c;
                if (!kv) return -3;
                for (c = 0; c < ix->ncols; c++) kv[c] = (int)ix->cols[c] == t->rowid_col ? vint(mt->rows[i].rowid) : mt->rows[i].v[ix->cols[c]];
                kv[ix->ncols] = vint(mt->rows[i].rowid);
                sz = rec_size(kv, ix->ncols + 1); rb = mdb_aalloc(ar, sz);
                if (!rb) return -3;
                rec_put(kv, ix->ncols + 1, rb);
                ents[i].key = rb; ents[i].klen = (uint32_t)sz; ents[i].rowid = mt->rows[i].rowid; ents[i].vals = kv; ents[i].nvals = (int)ix->ncols + 1;
            }
            r = sort_ents(&s, ents, n, cmp_key); if (r) return r;
            build_tree(&s, 1, ents, n, root);
        }
        if (s.err) return s.err;
        /* sqlite_master row */
        mv[0] = vtext(ob->is_index ? "index" : "table"); mv[1] = vtext(ob->name); mv[2] = vtext(ob->tbl); mv[3] = vint(root);
        mv[4] = ob->sql ? vtext(ob->sql) : vnull();
        rs = rec_size(mv, 5); rec = mdb_aalloc(ar, rs);
        if (!rec) return -3;
        rec_put(mv, 5, rec);
        master[k].key = rec; master[k].klen = (uint32_t)rs; master[k].rowid = k + 1; master[k].vals = 0; master[k].nvals = 0;
    }
    build_tree(&s, 0, master, nobj, 1);
    if (s.err) return s.err;
    /* file header (100 bytes) */
    hdr = out;
    mcpy(hdr, "SQLite format 3", 16);
    w16(hdr + 16, PG); hdr[18] = 1; hdr[19] = 1; hdr[20] = 0; hdr[21] = 64; hdr[22] = 32; hdr[23] = 32;
    w32(hdr + 24, mdb_sqlite_change_counter); w32(hdr + 28, s.next - 1); w32(hdr + 32, 0); w32(hdr + 36, 0);
    w32(hdr + 40, nobj); w32(hdr + 44, 4); w32(hdr + 48, 0); w32(hdr + 52, 0); w32(hdr + 56, 1); w32(hdr + 60, 0);
    w32(hdr + 64, 0); w32(hdr + 68, 0); mset(hdr + 72, 0, 20); w32(hdr + 92, mdb_sqlite_change_counter); w32(hdr + 96, 3045000);
    *outlen = (size_t)(s.next - 1) * PG;
    return 0;
}
