/* NanoSync - engine (see ns_engine.h). Integers only, no malloc, no libc. Safety order:
   verify -> plan -> generate everything in memory (dry run only) -> .bak BACKUP (hash) -> JOURNAL (hash) -> copy MP3 (SHA-1)
   -> generate database -> write (Library, Locations, Dynamic, cbk, iTunesCDB last) -> re-read/verify -> done.txt -> delete
   the journal (commit) -> remove sources. Crash/error at any point after the journal: restore from the .bak files and delete the copies. */
#include "ns_engine.h"
#include "ns_art.h"
#include "mdb_cdb.h"
#include "mdb_sqlgen.h"
#include "mdb_sqlite.h"
#include "mdb_sig.h"
#include "mdb_zlib.h"
#include "mdb_deflate.h"
#include "sha1.h"

#define NDB 10                      /* database files, in write order: Library, Locations, Dynamic, cbk, iTunesCDB */
#define IDX_CDB 4
#define IOBUF NS_PIECE
#define POOL_BYTES 32768u
#define XBUF_BYTES (400u * 1024u)       /* work memory of the tag reader (2 x LONGCAP + texts); lives in the work area (hi), only while reading */
#define JBUF 12288u
#define LOGBUF 12288u
#define PCAP_MAX (640u * 1024u)
#define MAXPATH 160

/* The library signature (HashAB) is keyed with the device's FireWire GUID: 16 hex characters, identical to the USB serial number the
   iPod reports. It is read at start from /iPod_Control/Device/SysInfo ("FirewireGuid: 0x...") or SysInfoExtended (<key>FireWireGUID</key>
   <string>...</string>); a build may still fix it with -DNS_GUID_HEX=\"0123456789ABCDEF\" (tools/build.sh --guid). */
static uint8_t GUID[8];
static int g_guid_ok;
static int hexv(int a) { return a >= '0' && a <= '9' ? a - '0' : (a | 32) >= 'a' && (a | 32) <= 'f' ? (a | 32) - 'a' + 10 : -1; }
static int guid_hex(const char *h) {
    uint8_t g[8]; int i;
    for (i = 0; i < 8; i++) {
        int a = hexv(h[2 * i]), b = a < 0 ? -1 : hexv(h[2 * i + 1]);
        if (a < 0 || b < 0) return 0;
        g[i] = (uint8_t)(a << 4 | b);
    }
    for (i = 0; i < 8; i++) GUID[i] = g[i];
    return 1;
}
static const char *find_s(const char *hay, uint32_t n, const char *needle) {
    uint32_t i, k, m = 0; while (needle[m]) m++;
    for (i = 0; i + m <= n; i++) { for (k = 0; k < m && hay[i + k] == needle[k]; k++) {} if (k == m) return hay + i + m; }
    return 0;
}
static void guid_init(const ns_os *os, char *buf, uint32_t cap) {
    uint32_t n; const char *p;
    g_guid_ok = 0;
#ifdef NS_GUID_HEX
    if (guid_hex(NS_GUID_HEX)) { g_guid_ok = 1; return; }
#endif
    n = os->exists("/iPod_Control/Device/SysInfo") ? os->read_all("/iPod_Control/Device/SysInfo", buf, cap - 1) : 0;
    if (n < cap && (p = find_s(buf, n, "FirewireGuid: 0x")) && (uint32_t)(p - buf) + 16 <= n && guid_hex(p)) { g_guid_ok = 1; return; }
    n = os->exists("/iPod_Control/Device/SysInfoExtended") ? os->read_all("/iPod_Control/Device/SysInfoExtended", buf, cap - 1) : 0;
    if (n < cap && (p = find_s(buf, n, "<key>FireWireGUID</key>"))) {
        const char *q = find_s(p, n - (uint32_t)(p - buf), "<string>");
        if (q && (uint32_t)(q - buf) + 16 <= n && guid_hex(q)) g_guid_ok = 1;
    }
}

static const struct { const char *path, *bak; } DBF[NDB] = {
    { NS_DIR_ITL "/Library.itdb",        NS_DIR_ITUNES "/Library.itdb.bak" },
    { NS_DIR_ITL "/Locations.itdb",      NS_DIR_ITUNES "/Locations.itdb.bak" },
    { NS_DIR_ITL "/Dynamic.itdb",        NS_DIR_ITUNES "/Dynamic.itdb.bak" },
    { NS_DIR_ITL "/Locations.itdb.cbk",  NS_DIR_ITUNES "/Locations.itdb.cbk.bak" },
    { NS_DIR_ITUNES "/iTunesCDB",        NS_DIR_ITUNES "/iTunesCDB.bak" },
    { NS_DIR_ART "/ArtworkDB",           NS_DIR_ART "/ArtworkDB.bak" },                 /* 5: only when there is new artwork */
    { NS_DIR_ART "/F1010_1.ithmb",       NS_DIR_ART "/F1010_1.ithmb.bak" },             /* 6..9: .ithmb per format (they grow by appending) */
    { NS_DIR_ART "/F1013_1.ithmb",       NS_DIR_ART "/F1013_1.ithmb.bak" },
    { NS_DIR_ART "/F1015_1.ithmb",       NS_DIR_ART "/F1015_1.ithmb.bak" },
    { NS_DIR_ART "/F1016_1.ithmb",       NS_DIR_ART "/F1016_1.ithmb.bak" },
};
#define NS_MAXCV 8u
#define CV_POOL 16384u
#define NS_F_DONE "/Apps/Data/Importar/done.txt"

typedef struct { uint32_t size; uint8_t sha[20]; } dbinfo;

typedef struct {                       /* piecewise transfer / hash (reader [+ writer]) */
    int active, rd, wr, hashing, cap, err, tailzero, wsha; uint32_t padto;  /* cap: capture prefix/tail for the tags */
    mdb_sha1_t sh; uint32_t done, total; uint8_t sha[20];
    const uint8_t *app; uint32_t applen, gap; int wsh_on; mdb_sha1_t wsh; uint8_t wshf[20];   /* appendix (.ithmb): zeros up to the alignment + new data; SHA-1 of what was written */
} xfer_t;

struct ns {
    const ns_os *os;
    uint8_t *base; uint32_t bytes;
    ns_phase ph; uint32_t sub, sub2; ns_stage stage; uint32_t prog;
    ns_result result; char msg[NS_MSG], msg2[NS_MSG];
    int dry, first_run, lib_ok, want_import, journaled, rollback, rec_mem, rec_crash, committed;
    uint32_t nf; ns_file files[NS_MAX_FILES];
    mdb_tags_t *tg;                     /* [NS_MAX_FILES] */
    uint8_t *iobuf; char *pool; uint32_t pool_used; char *xbuf; char *jbuf; uint32_t jlen; char *logb; uint32_t loglen; int log_dirty;
    int lib_before, lib_after; uint32_t lin, n_other, n_pending_clean;
    /* work area: lo grows up, hi grows down */
    uint8_t *wbase, *wend, *lo, *hi; uint32_t peak, need;
    uint8_t *P; uint32_t Pcap, Pn, Ptail; uint8_t tail[128]; int have_tail, Pover;
    /* database */
    dbinfo orig[NDB]; dbinfo neww[NDB]; uint8_t *cdb_in; uint32_t cdb_in_len;
    uint8_t *out_buf[NDB]; uint32_t out_len[NDB];      /* generated files in DBF order */
    uint8_t *log_out; uint32_t log_out_len, log_out_cap; uint8_t *hi_mark;
    mdb_track_t trk[NS_MAX_FILES]; mdb_assigned_t asg[NS_MAX_FILES];
    uint32_t xtra, now;
    xfer_t xf;
    uint32_t rng;
    uint32_t bytes_total, bytes_done;
    /* in-memory journal (restore) */
    dbinfo jd[NDB]; char jm[NS_MAX_FILES][40]; uint32_t njm; int jd_ok[NDB];
    ns_stats st;
    uint32_t t_unit0;
    char reason[96]; char diag[300]; uint32_t rdmax; int vsz_bad; uint32_t dlen[NDB], rec_cur[NDB], rec_tail, rec_len; uint8_t rec_h[20];
    /* artwork */
    int art_cand, art_bak, artw, noart; uint32_t ncv, cvcap, cvsc_n, cv_slot, art_maxref, art_next, cvsz[NS_MAXCV], cvid[NS_MAXCV], cvfirst[NS_MAXCV], cvbase[NS_FMT_N];
    uint8_t *cvbuf[NS_FMT_N], *cvsc, *cvpool; uint8_t cvkey[NS_MAXCV][20], cvkey_cur[20]; ns_cv cv;
};

/* database files taking part in this run: 0..4 always; 5..9 (artwork) only if there is JPEG artwork (backup/journal) / if the covers were written (write) */
static int act_j(const ns_t *s, uint32_t i) { return i < 5 || s->art_bak; }
/* --------------------------------------------------------------------------- util */
static uint32_t slen(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }
static void scopy(char *d, const char *s, uint32_t cap) { uint32_t i = 0; if (!cap) return; while (s[i] && i + 1 < cap) { d[i] = s[i]; i++; } d[i] = 0; }
static void scat(char *d, const char *s, uint32_t cap) { uint32_t n = slen(d); scopy(d + n, s, cap - n); }
static int seq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void mcopy(void *d, const void *s, size_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; }
static void mzero(void *d, size_t n) { uint8_t *a = d; while (n--) *a++ = 0; }
static int mdiff(const void *a, const void *b, size_t n) { const uint8_t *x = a, *y = b; while (n--) { if (*x++ != *y++) return 1; } return 0; }
static void u2s(char *d, uint32_t v, uint32_t cap) { char t[12]; int n = 0; if (!v) t[n++] = '0'; while (v) { t[n++] = (char)('0' + v % 10); v /= 10; } { uint32_t i = 0; while (n && i + 1 < cap) d[i++] = t[--n]; d[i] = 0; } }
static void cat_u(char *d, uint32_t v, uint32_t cap) { char t[12]; u2s(t, v, sizeof t); scat(d, t, cap); }
static void hex20(char *o, const uint8_t *h) { static const char X[] = "0123456789abcdef"; int i; for (i = 0; i < 20; i++) { o[2 * i] = X[h[i] >> 4]; o[2 * i + 1] = X[h[i] & 15]; } o[40] = 0; }
static int unhex20(uint8_t *h, const char *s) { int i; for (i = 0; i < 40; i++) { char c = s[i]; int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; if (v < 0) return -1; if (i & 1) h[i >> 1] |= (uint8_t)v; else h[i >> 1] = (uint8_t)(v << 4); } return 0; }
static uint32_t parse_u(const char **pp) { const char *p = *pp; uint32_t v = 0; while (*p >= '0' && *p <= '9') v = v * 10 + (uint32_t)(*p++ - '0'); *pp = p; return v; }
static uint32_t now_us(ns_t *s) { return s->os->now_us(); }

static void set_msg(ns_t *s, const char *a, const char *b) { scopy(s->msg, a, NS_MSG); scopy(s->msg2, b ? b : "", NS_MSG); }
static void trc(ns_t *s, const char *tag, uint32_t a, uint32_t b) { if (s->os->trace) s->os->trace(tag, a, b); }

/* in-memory log (rewritten whole at milestones; the SDK has no append) */
static void lg(ns_t *s, const char *t, const char *a, uint32_t v) {
    char line[400]; uint32_t n;
    line[0] = 0; scat(line, "t=", sizeof line); cat_u(line, s->os->unix_time(), sizeof line); scat(line, " ", sizeof line); scat(line, t, sizeof line);
    if (a) { scat(line, " ", sizeof line); scat(line, a, sizeof line); }
    if (v != 0xFFFFFFFFu) { scat(line, " ", sizeof line); cat_u(line, v, sizeof line); }
    scat(line, "\n", sizeof line); n = slen(line);
    if (s->loglen + n >= LOGBUF) {                                 /* drop the oldest half (at a line break) */
        uint32_t cut = LOGBUF / 2; while (cut < s->loglen && s->logb[cut - 1] != '\n') cut++;
        mcopy(s->logb, s->logb + cut, s->loglen - cut); s->loglen -= cut;
    }
    mcopy(s->logb + s->loglen, line, n); s->loglen += n; s->logb[s->loglen] = 0; s->log_dirty = 1;
}
/* read one piece with fallback: if the FIRST read of a file (position 0) returns 0 bytes for a large request, try 4096 and 512
   on the same handle and remember the size that worked (s->rdmax), logging "DIAG READSIZE" (real data about the device OS). */
static int32_t rd_piece(ns_t *s, void *buf, uint32_t n, int first) {
    static const uint32_t T[2] = {4096u, 512u}; int32_t g; uint32_t i;
    if (n > s->rdmax) n = s->rdmax;
    g = s->os->r_read(buf, n);
    if (g > 0 || !first) return g;
    for (i = 0; i < 2; i++) {
        ns_iodiag dg; if (n <= T[i]) continue;
        dg.rc = 0; dg.req = 0; dg.got = 0; dg.calls = 0; if (s->os->io_diag) s->os->io_diag(&dg);
        lg(s, "DIAG READSIZE failed", 0, n); lg(s, "DIAG READSIZE rc", 0, (uint32_t)dg.rc);
        g = s->os->r_read(buf, T[i]);
        if (g > 0) { s->rdmax = T[i]; lg(s, "DIAG READSIZE worked with", 0, T[i]); return g; }
    }
    return g;
}
static int write_whole(ns_t *s, const char *path, const void *d, uint32_t n) {
    if (s->os->w_open(path)) return -1;
    if (n && s->os->w_write(d, n)) { s->os->w_close(); return -1; }
    return s->os->w_close();
}
static void log_flush(ns_t *s) { if (s->log_dirty) { write_whole(s, NS_F_LOG, s->logb, s->loglen); s->log_dirty = 0; } }

/* work area */
static void wtrack(ns_t *s) { uint32_t u = (uint32_t)((s->lo - s->wbase) + (s->wend - s->hi)); if (u > s->peak) s->peak = u; }
static uint32_t wfree(const ns_t *s) { return (uint32_t)(s->hi - s->lo); }
static uint8_t *lo_alloc(ns_t *s, uint32_t n) { uint8_t *p; n = (n + 15u) & ~15u; if (n > wfree(s)) return 0; p = s->lo; s->lo += n; wtrack(s); return p; }
static uint8_t *hi_alloc(ns_t *s, uint32_t n) { n = (n + 15u) & ~15u; if (n > wfree(s)) return 0; s->hi -= n; wtrack(s); return s->hi; }
static void hi_reset(ns_t *s) { s->hi = s->wend; }

/* --------------------------------------------------------------------------- piecewise transfer */
static void xf_close(ns_t *s) { if (s->xf.rd) { s->os->r_close(); s->xf.rd = 0; } if (s->xf.wr) { s->os->w_close(); s->xf.wr = 0; } s->xf.active = 0; }
/* src: file to read; dst: file to create (NULL = hash only); total = expected bytes of src (fsize) */
static int xf_begin(ns_t *s, const char *src, const char *dst, uint32_t total, int cap) {
    xfer_t *x = &s->xf; mzero(x, sizeof *x);
    if (s->os->r_open(src)) { x->err = -1; return -1; }
    x->rd = 1; x->active = 1; x->total = total; x->cap = cap; mdb_sha1_init(&x->sh);
    if (dst) { if (s->os->w_open(dst)) { xf_close(s); x->err = -2; return -2; } x->wr = 1; }
    if (cap) { s->Pn = 0; s->Ptail = 0; s->have_tail = 0; s->Pover = 0; }
    return 0;
}
static uint32_t need_prefix(const uint8_t *b, uint32_t have, uint32_t size) {
    uint32_t need = 70000u + 4096u;
    if (have >= 10 && b[0] == 'I' && b[1] == 'D' && b[2] == '3' && b[3] >= 2 && b[3] <= 4)
        need += 10u + (((uint32_t)b[6] << 21) | ((uint32_t)b[7] << 14) | ((uint32_t)b[8] << 7) | b[9]) + 10u;
    if (need > size) need = size;
    return need;
}
/* 0 = continue, 1 = finished ok (sha in x->sha), <0 error */
static int xf_piece(ns_t *s) {
    xfer_t *x = &s->xf; int32_t got; uint32_t n = x->total - x->done; uint8_t *buf = s->iobuf;
    if (n > IOBUF) n = IOBUF;
    if (n > s->rdmax) n = s->rdmax;
    if (n == 0) { got = 0; } else { got = rd_piece(s, buf, n, x->done == 0); if (x->done == 0 && got > 0 && (uint32_t)got < n) n = (uint32_t)got; if (got < 0 || (uint32_t)got > n) { xf_close(s); x->err = -3; return -3; } }
    if (n && (uint32_t)got != n) { xf_close(s); x->err = -4; return -4; }                   /* file smaller than expected */
    if (got) {
        mdb_sha1_update(&x->sh, buf, (size_t)got);
        if (x->cap) {                                                          /* keep the prefix (tag + 70 KB) and the 128 B tail */
            uint32_t pos = x->done;
            if (pos == 0) s->Ptail = need_prefix(buf, (uint32_t)got, x->total);            /* prefix target */
            if (pos < s->Ptail) {
                uint32_t c = s->Ptail - pos; if (c > (uint32_t)got) c = (uint32_t)got;
                if (s->Pn + c <= s->Pcap) { mcopy(s->P + s->Pn, buf, c); s->Pn += c; } else s->Pover = 1;
            }
            if ((uint32_t)got >= 128) mcopy(s->tail, buf + got - 128, 128);
            else { mcopy(s->tail, s->tail + got, 128 - (uint32_t)got); mcopy(s->tail + 128 - (uint32_t)got, buf, (uint32_t)got); }
        }
        if (x->wr && s->os->w_write(buf, (uint32_t)got)) { xf_close(s); x->err = -5; return -5; }
        if (x->wr && x->wsh_on) mdb_sha1_update(&x->wsh, buf, (size_t)got);
        x->done += (uint32_t)got; s->bytes_done += (uint32_t)got;
    }
    if (x->done >= x->total) {
        uint8_t probe; int32_t extra = s->os->r_read(&probe, 1);                 /* the file cannot be larger than expected */
        mdb_sha1_final(&x->sh, x->sha); x->hashing = 1;
        if (x->tailzero) {                                                     /* restore over a file that does not shrink: only zeros may follow the data */
            uint32_t tl = 0; int bad = 0;
            if (extra > 0) {
                tl = 1; bad = probe != 0;
                for (;;) { uint32_t m = IOBUF < s->rdmax ? IOBUF : s->rdmax, k; int32_t g = s->os->r_read(s->iobuf, m); if (g <= 0) break; for (k = 0; k < (uint32_t)g; k++) if (s->iobuf[k]) bad = 1; tl += (uint32_t)g; }
            }
            s->rec_tail = tl;
            if (bad) { xf_close(s); x->err = -6; return -6; }
        } else if (extra > 0) { xf_close(s); x->err = -6; return -6; }
        s->os->r_close(); x->rd = 0;
        if (x->wr) {
            x->wr = 0;
            if (x->gap || x->applen) {                                         /* .ithmb: zeros up to the image alignment + new images */
                uint32_t rem = x->gap; const uint8_t *ap = x->app;
                mzero(s->iobuf, IOBUF);
                while (rem) { uint32_t m = rem > IOBUF ? IOBUF : rem; if (s->os->w_write(s->iobuf, m)) { s->os->w_close(); x->active = 0; x->err = -5; return -5; } if (x->wsh_on) mdb_sha1_update(&x->wsh, s->iobuf, m); rem -= m; }
                rem = x->applen;
                while (rem) { uint32_t m = rem > IOBUF ? IOBUF : rem; if (s->os->w_write(ap, m)) { s->os->w_close(); x->active = 0; x->err = -5; return -5; } if (x->wsh_on) mdb_sha1_update(&x->wsh, ap, m); ap += m; rem -= m; }
            }
            if (x->wsh_on) mdb_sha1_final(&x->wsh, x->wshf);
            if (x->padto > x->total) {                                         /* current file larger than the restored one (the OS does not shrink): pad with zeros */
                uint32_t rem = x->padto - x->total;
                mzero(s->iobuf, IOBUF);
                while (rem) { uint32_t m = rem > IOBUF ? IOBUF : rem; if (s->os->w_write(s->iobuf, m)) { s->os->w_close(); x->active = 0; x->err = -5; return -5; } rem -= m; }
            }
            if (s->os->w_close()) { x->active = 0; x->err = -7; return -7; }
        }
        x->active = 0; return 1;
    }
    return 0;
}

/* --------------------------------------------------------------------------- journal */
static void jline(ns_t *s, const char *a) { uint32_t n = slen(a); if (s->jlen + n < JBUF - 64) { mcopy(s->jbuf + s->jlen, a, n); s->jlen += n; } }
static void build_journal(ns_t *s) {
    uint32_t i; char t[80]; uint8_t sum[20]; char hx[48];
    s->jlen = 0; jline(s, "NSJ1\n");
    for (i = 0; i < NDB; i++) { if (!act_j(s, i)) continue; t[0] = 0; scat(t, "D ", sizeof t); cat_u(t, i, sizeof t); scat(t, " ", sizeof t); cat_u(t, s->orig[i].size, sizeof t); scat(t, " ", sizeof t); hex20(hx, s->orig[i].sha); scat(t, hx, sizeof t); scat(t, "\n", sizeof t); jline(s, t); }
    for (i = 0; i < s->nf; i++) if (s->files[i].status == 0) { t[0] = 0; scat(t, "M ", sizeof t); scat(t, s->files[i].dest, sizeof t); scat(t, "\n", sizeof t); jline(s, t); }
    mdb_sha1(s->jbuf, s->jlen, sum); hex20(hx, sum);
    t[0] = 0; scat(t, "E ", sizeof t); scat(t, hx, sizeof t); scat(t, "\n", sizeof t); jline(s, t);
}
/* 0 = valid; -1 = invalid/incomplete */
static int parse_journal(ns_t *s, const char *b, uint32_t n) {
    uint32_t p = 0, line_start; int e_seen = 0, hdr = 0; uint32_t i;
    s->njm = 0; for (i = 0; i < NDB; i++) s->jd_ok[i] = 0;
    while (p < n) {
        const char *l; line_start = p; l = b + p;
        while (p < n && b[p] != '\n') p++;
        if (p >= n) return -1;                                                  /* last line without \n = incomplete */
        p++;
        if (l[0] == 'N' && l[1] == 'S' && l[2] == 'J' && l[3] == '1' && line_start == 0) hdr = 1;
        else if (l[0] == 'D' && l[1] == ' ') {
            const char *q = l + 2; uint32_t idx = parse_u(&q), sz; if (*q++ != ' ' || idx >= NDB) return -1; sz = parse_u(&q); if (*q++ != ' ') return -1;
            if (unhex20(s->jd[idx].sha, q) || q[40] != '\n') return -1;
            s->jd[idx].size = sz; s->jd_ok[idx] = 1;
        } else if (l[0] == 'M' && l[1] == ' ') {
            uint32_t k = 0; const char *q = l + 2; if (s->njm >= NS_MAX_FILES) return -1;
            while (q[k] != '\n' && k < 39) { s->jm[s->njm][k] = q[k]; k++; } if (q[k] != '\n') return -1; s->jm[s->njm][k] = 0; s->njm++;
        } else if (l[0] == 'E' && l[1] == ' ') {
            uint8_t want[20], got[20]; if (unhex20(want, l + 2) || l[42] != '\n' || p != n) return -1;
            mdb_sha1(b, line_start, got); if (mdiff(want, got, 20)) return -1; e_seen = 1;
        } else return -1;
    }
    if (!hdr || !e_seen) return -1;
    for (i = 0; i < 5; i++) if (!s->jd_ok[i]) return -1;                       /* 5..9 (artwork) are in the journal only when there was artwork */
    return 0;
}

/* --------------------------------------------------------------------------- failures */
static void to_recover(ns_t *s, int from_mem) { s->rec_crash = from_mem ? 0 : 1; s->rollback = 1; s->rec_mem = from_mem; s->ph = NS_P_RECOVER; s->sub = 0; s->sub2 = 0; s->stage = NS_ST_NONE; xf_close(s); }
static const char *const PHN[] = {"INIT","RECOVER","SCAN","TAGS","LIB","READY","CHECK","PLAN","BAK","JRN","COPY","COPYVER","DRY","ART","GEN","WRITE","VERIFY","RMSRC","JDEL","DONE","FAIL"};
static void ascii_copy(char *d, const char *s, uint32_t cap);
static void cat_i(char *d, int32_t v, uint32_t cap) { if (v < 0) { scat(d, "-", cap); cat_u(d, (uint32_t)(-(int64_t)v), cap); } else cat_u(d, (uint32_t)v, cap); }
/* automatic code: E_<PHASE>[VER]_<error>  (e.g. E_DRY_SHORT = dry run, short read; E_COPY_WRITE; E_COPYVER_SRC_OPEN) */
static void auto_code(const ns_t *s, char *o, uint32_t cap) {
    static const char *const EN[] = {"", "SRC_OPEN", "DST_OPEN", "READ", "SHORT", "WRITE", "EOFX", "WCLOSE"};
    uint32_t ph = (uint32_t)s->ph; int k = -s->xf.err;
    o[0] = 0; scat(o, "E_", cap); scat(o, ph < sizeof PHN / sizeof *PHN ? PHN[ph] : "?", cap);
    if (s->ph == NS_P_COPY && s->sub2 >= 2) scat(o, "VER", cap);
    if (s->ph == NS_P_BAK && s->sub2 >= 2) scat(o, "VER", cap);
    if (k >= 1 && k <= 7) { scat(o, "_", cap); scat(o, EN[k], cap); } else { scat(o, "_", cap); cat_u(o, s->sub2, cap); }
}
/* diagnostic in short lines (screen ~28 columns): code / raw return / requested-obtained / position / ASCII file name */
static void diag_build(ns_t *s, const char *code, int32_t ret, const char *file) {
    ns_iodiag dg; char *d = s->diag; const uint32_t cap = sizeof s->diag; char t[40];
    dg.rc = 0; dg.req = 0; dg.got = 0; dg.calls = 0; if (s->os->io_diag) s->os->io_diag(&dg);
    d[0] = 0; scat(d, code, cap); scat(d, "\n", cap);
    scat(d, "ret=", cap); cat_i(d, ret, cap); scat(d, " rc=", cap); cat_i(d, dg.rc, cap); scat(d, "\n", cap);
    scat(d, "req=", cap); cat_u(d, dg.req, cap); scat(d, " got=", cap); cat_u(d, dg.got, cap); scat(d, " n=", cap); cat_u(d, dg.calls, cap); scat(d, "\n", cap);
    scat(d, "pos=", cap); cat_u(d, s->xf.done, cap); scat(d, "/", cap); cat_u(d, s->xf.total, cap); scat(d, "\n", cap);
    if (file && file[0]) { ascii_copy(t, file, 24); scat(d, t, cap); scat(d, " (", cap); cat_u(d, slen(file), cap); scat(d, "B)", cap); }
}
static void diag_log(ns_t *s) {              /* same information, on a single line, in log.txt */
    char t[300]; uint32_t i, k = 0;
    for (i = 0; s->diag[i] && k + 1 < sizeof t; i++) t[k++] = s->diag[i] == '\n' ? ' ' : s->diag[i];
    t[k] = 0; lg(s, "DIAG", t, 0xFFFFFFFFu);
}
static void fail_c(ns_t *s, const char *code, const char *what, int32_t ret) {
    const char *file = 0; char ac[40];
    if ((s->ph == NS_P_COPY || s->ph == NS_P_TAIL || s->ph == NS_P_TAGS) && s->sub < s->nf) file = s->files[s->sub].name;
    if ((s->ph == NS_P_WRITE || s->ph == NS_P_VERIFY) && s->sub < NDB) file = DBF[s->sub].path;
    if (!code) { auto_code(s, ac, sizeof ac); code = ac; if (ret == 0) ret = s->xf.err; }
    diag_build(s, code, ret, file);
    xf_close(s);
    lg(s, "ERROR", code, 0xFFFFFFFFu); lg(s, "ERROR", what, 0xFFFFFFFFu); diag_log(s);
    scopy(s->reason, what, sizeof s->reason);
    if (s->journaled && !s->committed) {
        set_msg(s, "Error: restoring the library...", what);
        to_recover(s, 1);
    } else {
        s->ph = NS_P_FAIL; s->stage = NS_ST_NONE; s->result = s->committed ? NS_R_OK : NS_R_REFUSED;
        set_msg(s, s->committed ? "Imported, but with a warning" : "Refused: nothing was changed", what);
        log_flush(s);
    }
}
static void fail(ns_t *s, const char *what) { fail_c(s, 0, what, 0); }
static void refuse(ns_t *s, const char *what) { fail(s, what); }

/* --------------------------------------------------------------------------- tags */
static void ascii_copy(char *d, const char *s, uint32_t cap) {
    uint32_t i = 0; const unsigned char *p = (const unsigned char *)s;
    while (*p && i + 1 < cap) {
        if (*p < 0x80) { d[i++] = (char)((*p >= 32) ? *p : ' '); p++; }
        else { /* UTF-8: one character becomes '?' (the screen font is ASCII only) */
            d[i++] = '?'; p++; while ((*p & 0xC0) == 0x80) p++;
        }
    }
    d[i] = 0;
}
static const char *pool_dup(ns_t *s, const char *src) {
    uint32_t n; char *o;
    if (!src || !src[0]) return 0;
    n = slen(src) + 1; if (s->pool_used + n > POOL_BYTES) return 0;
    o = s->pool + s->pool_used; mcopy(o, src, n); s->pool_used += n; return o;
}
/* read the tags from the prefix in s->P (with tail) and store them in tg[i]; compact long texts into the pool */
static int parse_tags(ns_t *s, uint32_t i, uint32_t size, int with_tail) {
    mdb_tags_t *t = &s->tg[i]; uint32_t len; int r; uint32_t t0 = now_us(s);
    mzero(t, sizeof *t); t->xbuf = s->xbuf; t->xcap = XBUF_BYTES;
    if (s->Pn >= size) { len = size; r = mdb_tags_read_ex(s->P, len, size, t); }
    else {
        if (with_tail) mcopy(s->P + s->Pn, s->tail, 128); else mzero(s->P + s->Pn, 128);
        len = s->Pn + 128; r = mdb_tags_read_ex(s->P, len, size, t);
    }
    s->st.t_tags_us += now_us(s) - t0;
    if (r || !t->sample_rate || !t->duration_ms) return -1;
    t->comment = pool_dup(s, t->comment); t->grouping = pool_dup(s, t->grouping); t->subtitle = pool_dup(s, t->subtitle);
    t->description = pool_dup(s, t->description); t->copyright = pool_dup(s, t->copyright); t->content_provider = pool_dup(s, t->content_provider);
    t->lyrics = 0; t->xbuf = 0; t->xcap = 0;
    return 0;
}
static int is_mp3_name(const char *n) { uint32_t l = slen(n); return l > 4 && n[l - 4] == '.' && (n[l - 3] | 32) == 'm' && (n[l - 2] | 32) == 'p' && n[l - 1] == '3'; }
static void path_imp(char *o, const char *name) { scopy(o, NS_DIR_IMP "/", MAXPATH); scat(o, name, MAXPATH); }
static void base_noext(const char *name, char *buf, uint32_t n) {
    const char *d = 0, *p; uint32_t l;
    for (p = name; *p; p++) if (*p == '.') d = p;
    l = d && d > name ? (uint32_t)(d - name) : slen(name); if (l >= n) l = n - 1; mcopy(buf, name, l); buf[l] = 0;
}

/* --------------------------------------------------------------------------- creation */
uint32_t ns_state_bytes(void) {
    return 4096u + (uint32_t)sizeof(struct ns) + IOBUF + NS_MAX_FILES * (uint32_t)sizeof(mdb_tags_t) + POOL_BYTES + JBUF + LOGBUF + 256u;
}
uint32_t ns_min_block(void) { return ns_state_bytes() + XBUF_BYTES + 320u * 1024u; }
ns_t *ns_create(const ns_os *os, void *block, uint32_t bytes) {
    uint8_t *b = (uint8_t *)(((uintptr_t)block + 15u) & ~(uintptr_t)15u); ns_t *s; uint32_t pad = (uint32_t)(b - (uint8_t *)block), off;
    if (bytes < pad || bytes - pad < ns_min_block()) return 0;
    bytes -= pad; s = (ns_t *)b; mzero(s, sizeof *s);
    s->os = os; s->base = b; s->bytes = bytes; off = (uint32_t)((sizeof *s + 15u) & ~15u);
    s->iobuf = b + off; off += IOBUF;
    s->tg = (mdb_tags_t *)(void *)(b + off); off += (NS_MAX_FILES * (uint32_t)sizeof(mdb_tags_t) + 15u) & ~15u;
    s->pool = (char *)b + off; off += POOL_BYTES;
    s->jbuf = (char *)b + off; off += JBUF;
    guid_init(os, s->jbuf, JBUF);
    s->logb = (char *)b + off; off += LOGBUF + 16u;
    s->wbase = b + ((off + 15u) & ~15u); s->wend = b + (bytes & ~15u);
    s->lo = s->wbase; s->hi = s->wend;
    s->rng = 0x9E3779B9u ^ os->now_us() ^ os->unix_time() * 2654435761u; if (!s->rng) s->rng = 1;
    s->ph = NS_P_INIT; s->lib_before = s->lib_after = -1; s->rdmax = NS_PIECE;
    set_msg(s, "Starting...", 0);
    return s;
}

/* --------------------------------------------------------------------------- UI access */
ns_phase ns_get_phase(const ns_t *s) { return s->ph; }
ns_stage ns_get_stage(const ns_t *s) { return s->stage; }
uint32_t ns_get_progress(const ns_t *s) { return s->prog; }
const char *ns_get_msg(const ns_t *s) { return s->msg; }
const char *ns_get_msg2(const ns_t *s) { return s->msg2; }
const char *ns_get_diag(const ns_t *s) { return s->diag; }
ns_result ns_get_result(const ns_t *s) { return s->result; }
uint32_t ns_nfiles(const ns_t *s) { return s->nf; }
const ns_file *ns_file_at(const ns_t *s, uint32_t i) { return i < s->nf ? &s->files[i] : 0; }
const mdb_tags_t *ns_tags_at(const ns_t *s, uint32_t i) { return i < s->nf && s->files[i].have_tags ? &s->tg[i] : 0; }
int ns_dryrun(const ns_t *s) { return s->dry; }
int ns_lib_before(const ns_t *s) { return s->lib_before; }
int ns_lib_after(const ns_t *s) { return s->lib_after; }
uint32_t ns_mem_need(const ns_t *s) { return s->need; }
uint32_t ns_mem_have(const ns_t *s) { return (uint32_t)(s->wend - s->wbase); }
uint32_t ns_mem_peak(const ns_t *s) { return s->peak; }
uint32_t ns_block_bytes(const ns_t *s) { return s->bytes; }
const char *ns_log_text(const ns_t *s, uint32_t *len) { if (len) *len = s->loglen; return s->logb; }
const ns_stats *ns_get_stats(const ns_t *s) { return &s->st; }
int ns_can_cancel(const ns_t *s) { return s->ph == NS_P_READY || s->ph == NS_P_CHECK || s->ph == NS_P_PLAN; }
void ns_label(const ns_t *s, uint32_t i, char *o1, uint32_t c1, char *o2, uint32_t c2) {
    char base[NS_NAME];
    if (i >= s->nf) { if (c1) o1[0] = 0; if (c2) o2[0] = 0; return; }
    base_noext(s->files[i].name, base, sizeof base);
    if (s->files[i].have_tags) { ascii_copy(o1, s->tg[i].title[0] ? s->tg[i].title : base, c1); ascii_copy(o2, s->tg[i].artist, c2); }
    else { ascii_copy(o1, base, c1); scopy(o2, s->files[i].reason, c2); }
}
void ns_request_import(ns_t *s) { if (s->ph == NS_P_READY) { s->want_import = 1; s->diag[0] = 0; } }
void ns_request_rescan(ns_t *s) { if (s->ph == NS_P_READY || s->ph == NS_P_DONE || s->ph == NS_P_FAIL) { s->ph = NS_P_SCAN; s->sub = 0; s->sub2 = 0; s->result = NS_R_NONE; s->want_import = 0; s->diag[0] = 0; } }

/* --------------------------------------------------------------------------- CDB: track count */
static uint32_t rd32(const uint8_t *p) { return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
/* highest artwork_id_ref (mhit 0x160) of the existing tracks */
static uint32_t cdb_max_artref(const uint8_t *lg, uint32_t len) {
    uint32_t off, best = 0;
    if (len < 16 || lg[0] != 'm' || lg[1] != 'h' || lg[2] != 'b' || lg[3] != 'd') return 0;
    off = rd32(lg + 4);
    while (off + 16 <= len && lg[off] == 'm' && lg[off + 1] == 'h' && lg[off + 2] == 's' && lg[off + 3] == 'd') {
        uint32_t hl = rd32(lg + off + 4), tl = rd32(lg + off + 8), type = rd32(lg + off + 12);
        if (type == 1 && off + hl + 12 <= len) {
            uint32_t cnt = rd32(lg + off + hl + 8), p = off + hl + rd32(lg + off + hl + 4), i;
            for (i = 0; i < cnt && p + 0x164 <= len; i++) { uint32_t v = rd32(lg + p + 0x160); if (v > best) best = v; if (rd32(lg + p + 8) < 16) break; p += rd32(lg + p + 8); }
            return best;
        }
        if (tl < 16) return 0;
        off += tl;
    }
    return best;
}
static int cdb_ntracks(const uint8_t *lg, uint32_t len) {
    uint32_t off;
    if (len < 16 || lg[0] != 'm' || lg[1] != 'h' || lg[2] != 'b' || lg[3] != 'd') return -1;
    off = rd32(lg + 4);
    while (off + 16 <= len && lg[off] == 'm' && lg[off + 1] == 'h' && lg[off + 2] == 's' && lg[off + 3] == 'd') {
        uint32_t hl = rd32(lg + off + 4), tl = rd32(lg + off + 8), type = rd32(lg + off + 12);
        if (type == 1 && off + hl + 12 <= len && lg[off + hl] == 'm' && lg[off + hl + 1] == 'h' && lg[off + hl + 2] == 'l' && lg[off + hl + 3] == 't') return (int)rd32(lg + off + hl + 8);
        if (tl < 16) return -1;
        off += tl;
    }
    return -1;
}

/* --------------------------------------------------------------------------- recovery */
static void rec_done(ns_t *s) {
    s->journaled = 0; s->ph = NS_P_DONE; s->stage = NS_ST_NONE; s->prog = 100;
    if (s->rec_crash) { s->result = NS_R_RESTORED; set_msg(s, "Restored: the previous import was interrupted", "The library is back to its previous state. The MP3 files are still in the folder."); }
    else { s->result = NS_R_ROLLED_BACK; set_msg(s, "Failed: library restored", s->reason); }
    lg(s, "RESTORED", s->rec_crash ? "after crash" : "after error", 0xFFFFFFFFu);
    log_flush(s);
}
static void rec_critical(ns_t *s, const char *why) {
    diag_build(s, "E_RECOVER", s->xf.err, 0); xf_close(s); diag_log(s); s->ph = NS_P_FAIL; s->result = NS_R_CRITICAL; s->stage = NS_ST_NONE;
    set_msg(s, "CRITICAL ERROR: could not restore", why); lg(s, "CRITICAL", why, 0xFFFFFFFFu); log_flush(s);
}
/* sub: 0 read journal; 1 remove copies; 2.. per database file (sub2: 0 current hash, 1 .bak hash, 2 copy, 3 verify); 99 remove journal */
static int ph_recover(ns_t *s) {
    char p[MAXPATH];
    if (s->sub == 0) {
        if (!s->rec_mem) {
            uint32_t n = s->os->read_all(NS_F_JOURNAL, s->jbuf, JBUF - 1); s->rec_crash = 1;
            if (!n || parse_journal(s, s->jbuf, n)) {
                /* incomplete journal: the crash happened BEFORE any change to the final files */
                s->os->remove(NS_F_JOURNAL); lg(s, "JOURNAL", "incomplete: nothing to restore", 0xFFFFFFFFu);
                s->journaled = 0; s->ph = NS_P_SCAN; s->sub = 0; s->rollback = 0; set_msg(s, "Incomplete journal removed (nothing was changed)", 0); log_flush(s); return 1;
            }
            lg(s, "RECOVERING", "valid journal", s->njm);
        } else {
            uint32_t i, k = 0; for (i = 0; i < NDB; i++) { s->jd[i] = s->orig[i]; s->jd_ok[i] = act_j(s, i) ? 1 : 0; }
            for (i = 0; i < s->nf; i++) if (s->files[i].status == 0) { scopy(s->jm[k++], s->files[i].dest, 40); }
            s->njm = k;
        }
        set_msg(s, "Restoring the library...", 0); s->stage = NS_ST_NONE; s->sub = 1; s->sub2 = 0; return 1;
    }
    if (s->sub == 1) {                                                         /* remove the MP3 copies of this run */
        if (s->sub2 < s->njm) {
            p[0] = '/'; p[1] = 0; scat(p, s->jm[s->sub2], sizeof p);
            if (s->os->exists(p)) s->os->remove(p);
            s->sub2++; return 1;
        }
        s->sub = 2; s->sub2 = 0; return 1;
    }
    if (s->sub >= 2 && s->sub < 2 + NDB) {
        uint32_t idx = s->sub - 2; int r; int32_t sz;
        s->prog = 100u * idx / NDB;
        if (!s->jd_ok[idx]) { s->sub++; s->sub2 = 0; return 1; }
        if (idx >= 5 && s->jd[idx].size == 0) {                                 /* did not exist before: remove it if the OS allows (otherwise it stays unreferenced) */
            if (s->os->exists(DBF[idx].path) && s->os->remove(DBF[idx].path)) lg(s, "WARNING could not remove (unreferenced)", DBF[idx].path, 0xFFFFFFFFu);
            s->sub++; s->sub2 = 0; return 1;
        }
        switch (s->sub2) {
        case 0: {                                                              /* hash of the current final file */
            sz = s->os->fsize(DBF[idx].path); s->rec_cur[idx] = sz < 0 ? 0 : (uint32_t)sz;
            if (idx == 3 && s->rec_cur[1] > s->jd[1].size) { s->sub2 = 21; return 1; }   /* Locations has a zero tail: the cbk must cover the physical file */
            if (sz < 0 || (uint32_t)sz != s->jd[idx].size) { s->sub2 = 1 + 1; return 1; }   /* different size => rewrite */
            if (xf_begin(s, DBF[idx].path, 0, (uint32_t)sz, 0)) { s->sub2 = 2; return 1; }
            s->sub2 = 10; return 1; }
        case 10: r = xf_piece(s);
            if (r < 0) { s->sub2 = 2; return 1; }
            if (r == 1) { if (!mdiff(s->xf.sha, s->jd[idx].sha, 20)) { s->sub = s->sub + 1; s->sub2 = 0; return 1; } s->sub2 = 2; }
            return 1;
        case 1: s->sub2 = 2; return 1;
        case 2: {                                                              /* verify the .bak */
            sz = s->os->fsize(DBF[idx].bak);
            if (sz < 0 || (uint32_t)sz != s->jd[idx].size) { rec_critical(s, "backup .bak missing or wrong size"); return 0; }
            if (xf_begin(s, DBF[idx].bak, 0, (uint32_t)sz, 0)) { rec_critical(s, "could not open the .bak backup"); return 0; }
            s->sub2 = 11; return 1; }
        case 11: r = xf_piece(s);
            if (r < 0) { rec_critical(s, "failed to read the .bak backup"); return 0; }
            if (r == 1) { if (mdiff(s->xf.sha, s->jd[idx].sha, 20)) { rec_critical(s, "backup .bak does not match (hash)"); return 0; } s->sub2 = 3; }
            return 1;
        case 3:                                                                /* copy .bak -> final (remove the final first: no leftover of the old size) */
            if (s->os->exists(DBF[idx].path) && !s->os->remove(DBF[idx].path)) s->rec_cur[idx] = 0;   /* if the OS allows removal there is no tail; otherwise write over it and pad with zeros */
            if (xf_begin(s, DBF[idx].bak, DBF[idx].path, s->jd[idx].size, 0)) { rec_critical(s, "could not rewrite the original file"); return 0; }
            s->xf.padto = s->rec_cur[idx] > s->jd[idx].size ? s->rec_cur[idx] : 0;
            s->sub2 = 12; return 1;
        case 12: r = xf_piece(s);
            if (r < 0) { rec_critical(s, "failed to rewrite the original"); return 0; }
            if (r == 1) s->sub2 = 4;
            return 1;
        case 4:                                                                /* verify the result */
            if (xf_begin(s, DBF[idx].path, 0, s->jd[idx].size, 0)) { rec_critical(s, "could not reopen the restored original"); return 0; }
            s->xf.tailzero = 1; s->rec_tail = 0; s->sub2 = 13; return 1;
        case 13: r = xf_piece(s);
            if (r < 0) { rec_critical(s, "failed to re-read the restored original"); return 0; }
            if (r == 1) {
                if (mdiff(s->xf.sha, s->jd[idx].sha, 20)) { rec_critical(s, "restored original does not match"); return 0; }
                if (s->rec_tail) { char t[NS_MSG]; t[0] = 0; scat(t, "real ", sizeof t); cat_u(t, s->jd[idx].size, sizeof t); scat(t, " physical ", sizeof t); cat_u(t, s->jd[idx].size + s->rec_tail, sizeof t); lg(s, "DIAG PAD restored", t, 0xFFFFFFFFu); }
                if (idx == 4 && s->rec_tail) { s->sub2 = 20; return 1; }                 /* restored CDB with a zero tail: re-sign the physical file */
                s->sub = s->sub + 1; s->sub2 = 0; }
            return 1;
        case 20: case 21: {                                                    /* 20: original iTunesCDB + zeros, re-signed | 21: cbk over the physical Locations */
            uint32_t pl = s->sub2 == 20 ? s->rec_cur[4] : s->rec_cur[1], need = s->sub2 == 20 ? s->jd[4].size : s->jd[1].size; uint8_t *buf, *cb;
            s->lo = s->wbase; hi_reset(s); buf = lo_alloc(s, pl + 16);
            if (!buf) { rec_critical(s, "not enough memory to complete the restored file"); return 0; }
            if (s->sub2 == 20) {
                if (s->os->read_all(DBF[4].bak, buf, need) != need) { rec_critical(s, "could not read the iTunesCDB .bak"); return 0; }
                mzero(buf + need, pl - need);
                if (mdb_cdb_sign(buf, pl, GUID)) { rec_critical(s, "failed to sign the restored iTunesCDB"); return 0; }
                cb = buf; s->rec_len = pl;
            } else {
                if (s->os->read_all(DBF[1].path, buf, pl) != pl) { rec_critical(s, "could not read the restored Locations"); return 0; }
                cb = lo_alloc(s, (uint32_t)mdb_cbk_size(pl) + 16);
                if (!cb) { rec_critical(s, "not enough memory (restored cbk)"); return 0; }
                mdb_cbk_build(cb, buf, pl, GUID); s->rec_len = (uint32_t)mdb_cbk_size(pl);
            }
            mdb_sha1(cb, s->rec_len, s->rec_h);
            if (write_whole(s, DBF[idx].path, cb, s->rec_len)) { rec_critical(s, "failed to write the padded file"); return 0; }
            lg(s, "DIAG PAD file padded", DBF[idx].path, s->rec_len);
            s->sub2 = 22; return 1; }
        case 22:
            if (xf_begin(s, DBF[idx].path, 0, s->rec_len, 0)) { rec_critical(s, "could not reopen the padded file"); return 0; }
            s->sub2 = 23; return 1;
        case 23: r = xf_piece(s);
            if (r < 0) { rec_critical(s, "failed to re-read the padded file"); return 0; }
            if (r == 1) { if (mdiff(s->xf.sha, s->rec_h, 20)) { rec_critical(s, "padded file does not match"); return 0; } s->sub = s->sub + 1; s->sub2 = 0; }
            return 1;
        }
        return 1;
    }
    /* end: delete done.txt (if the failure was while writing it) and the journal */
    if (s->os->exists(NS_F_DONE)) s->os->remove(NS_F_DONE);
    if (s->os->remove(NS_F_JOURNAL)) { rec_critical(s, "could not delete the journal"); return 0; }
    rec_done(s); return 0;
}

/* --------------------------------------------------------------------------- scan */
static void load_log(ns_t *s) {
    if (s->loglen == 0) { uint32_t n = s->os->read_all(NS_F_LOG, s->logb, LOGBUF / 2); if (n > LOGBUF / 2) n = 0; s->loglen = n; s->logb[n] = 0; }
}
static int ncmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (int)(uint8_t)*a - (int)(uint8_t)*b; }
static int done_match(ns_t *s, const char *name, uint32_t size) {      /* done.txt: lines "name\nsize\n" */
    const char *p = s->jbuf; uint32_t n = s->jlen, pos = 0;
    while (pos < n) {
        uint32_t k = 0, sz, i = 0; const char *q; int same;
        while (p[pos + k] && p[pos + k] != '\n') k++;
        if (pos + k >= n) return 0;
        q = p + pos + k + 1; sz = parse_u(&q);
        same = (slen(name) == k); while (same && i < k) { if (p[pos + i] != name[i]) same = 0; i++; }
        if (same && sz == size) return 1;
        while (*q == '\n') q++;
        pos = (uint32_t)(q - p);
    }
    return 0;
}
static int ph_scan(ns_t *s) {
    char p[MAXPATH], nm[NS_NAME + 8]; int isd;
    switch (s->sub) {
    case 0: {                                                                  /* folder, markers, journal */
        s->art_cand = s->art_bak = s->artw = 0; s->ncv = s->cvcap = 0; s->cvbuf[0] = 0; s->nf = 0; s->n_other = 0; s->pool_used = 0; s->lib_ok = 0; s->lib_before = -1; s->lib_after = -1; s->journaled = 0; s->rollback = 0; s->committed = 0;
        s->lo = s->wbase; hi_reset(s); s->cdb_in = 0; s->P = 0; s->rec_crash = 0;
        if (!s->os->is_dir("/Apps/Data")) s->os->mkdir("/Apps/Data");
        if (!s->os->is_dir(NS_DIR_IMP) && s->os->mkdir(NS_DIR_IMP)) { s->ph = NS_P_FAIL; s->result = NS_R_REFUSED; set_msg(s, "Could not create /Apps/Data/Importar", 0); return 0; }
        if (!g_guid_ok) { s->ph = NS_P_FAIL; s->result = NS_R_REFUSED; set_msg(s, "Could not read the iPod serial (SysInfo)", 0); return 0; }
        load_log(s);
        if (!s->os->exists(NS_F_INIT)) {                                       /* first run: dry run on by default */
            static const char tx[] = "NanoSync. Dry run is on: delete dryrun.txt to import for real.\n";
            if (!s->os->exists(NS_F_DRY)) write_whole(s, NS_F_DRY, tx, sizeof tx - 1);
            write_whole(s, NS_F_INIT, "1\n", 2); s->first_run = 1; lg(s, "FIRST RUN", "dry run on", 0xFFFFFFFFu);
        }
        s->dry = s->os->exists(NS_F_DRY); s->noart = s->os->exists(NS_F_NOART);      /* noart.txt = do not write covers */
        if (s->os->exists(NS_F_JOURNAL)) { s->ph = NS_P_RECOVER; s->sub = 0; s->rec_mem = 0; set_msg(s, "Journal found: restoring...", 0); return 1; }
        s->sub = 1; return 1; }
    case 1: {                                                                  /* done.txt: sources already imported by an interrupted run */
        s->jlen = s->os->exists(NS_F_DONE) ? s->os->read_all(NS_F_DONE, s->jbuf, JBUF - 2) : 0; if (s->jlen > JBUF - 2) s->jlen = 0; s->jbuf[s->jlen] = 0;
        s->sub = 2; return 1; }
    case 2:
        if (s->os->d_open(NS_DIR_IMP)) { s->ph = NS_P_FAIL; s->result = NS_R_REFUSED; set_msg(s, "Could not list the import folder", 0); return 0; }
        s->sub = 3; return 1;
    case 3: {
        uint32_t k;
        for (k = 0; k < 8; k++) {
            if (!s->os->d_next(nm, sizeof nm, &isd)) { s->os->d_close(); s->sub = 4; return 1; }
            if (isd || nm[0] == '.') continue;
            if (slen(nm) >= sizeof nm - 1) { s->n_other++; lg(s, "IGNORED name too long (bytes, truncated)", 0, slen(nm)); continue; }   /* the platform truncated the name */
            if (!is_mp3_name(nm)) { s->n_other++; continue; }
            if (s->nf >= NS_MAX_FILES) { s->n_other++; continue; }
            if (slen(nm) >= NS_NAME) { s->n_other++; lg(s, "IGNORED name too long (bytes)", 0, slen(nm)); continue; }
            if (s->jlen) { char pp[MAXPATH]; int32_t z; path_imp(pp, nm); z = s->os->fsize(pp); if (z > 0 && done_match(s, nm, (uint32_t)z)) { if (!s->os->remove(pp)) lg(s, "already imported source removed", nm, 0xFFFFFFFFu); continue; } }
            { ns_file *f = &s->files[s->nf]; mzero(f, sizeof *f); scopy(f->name, nm, NS_NAME); s->nf++; }
        }
        return 1; }
    case 4: {                                                                  /* sort by name (insertion) and read sizes */
        uint32_t i, j;
        for (i = 1; i < s->nf; i++) { ns_file t = s->files[i]; for (j = i; j > 0 && ncmp(s->files[j - 1].name, t.name) > 0; j--) s->files[j] = s->files[j - 1]; s->files[j] = t; }
        for (i = 0; i < s->nf; i++) { int32_t z; path_imp(p, s->files[i].name); z = s->os->fsize(p); s->files[i].size = z < 0 ? 0 : (uint32_t)z; if (z <= 0) { s->files[i].status = -1; scopy(s->files[i].reason, "empty", sizeof s->files[i].reason); } }
        if (s->jlen) { s->os->remove(NS_F_DONE); s->jlen = 0; }
        s->sub = 0; s->ph = NS_P_LIB; s->sub2 = 0; set_msg(s, "Reading the library...", 0); return 1; }
    }
    return 0;
}

/* library: HashAB + count */
static int ph_lib(ns_t *s) {
    int32_t sz; uint32_t i;
    if (s->sub == 0) {
        for (i = 5; i < NDB; i++) { sz = s->os->fsize(DBF[i].path); s->orig[i].size = sz > 0 ? (uint32_t)sz : 0; mzero(s->orig[i].sha, 20); }   /* artwork: may not exist */
        for (i = 0; i < 5; i++) { sz = s->os->fsize(DBF[i].path); if (sz <= 0) { s->lib_ok = 0; set_msg(s, "Library incomplete: file missing", DBF[i].path); s->ph = NS_P_TAGS; s->sub = 0; s->sub2 = 0; lg(s, "LIBRARY", "file missing", 0xFFFFFFFFu); return 1; } s->orig[i].size = (uint32_t)sz; }
        s->cdb_in_len = s->orig[IDX_CDB].size;
        s->lo = s->wbase; s->cdb_in = lo_alloc(s, s->cdb_in_len + 16);
        if (!s->cdb_in || s->os->read_all(DBF[IDX_CDB].path, s->cdb_in, s->cdb_in_len) != s->cdb_in_len) { s->lib_ok = 0; set_msg(s, "Could not read the iTunesCDB (memory?)", 0); s->ph = NS_P_TAGS; s->sub = 0; s->sub2 = 0; return 1; }
        mdb_sha1(s->cdb_in, s->cdb_in_len, s->orig[IDX_CDB].sha);
        if (!mdb_cdb_verify(s->cdb_in, s->cdb_in_len, GUID)) {
            s->lib_ok = 0; set_msg(s, "iTunesCDB: invalid HashAB signature. Import blocked", "Nothing will be written."); lg(s, "HASHAB", "invalid: refused", 0xFFFFFFFFu);
            s->ph = NS_P_TAGS; s->sub = 0; s->sub2 = 0; return 1;
        }
        s->sub = 1; return 1;
    }
    {   /* decompress to count (and measure the logical CDB) */
        uint32_t cap = s->cdb_in_len * 8u + 65536u, avail = wfree(s) / 2u; uint8_t *lgb; size_t ll = 0; int r = -1; uint32_t t0 = now_us(s);
        for (;;) {
            if (cap > avail) cap = avail;
            hi_reset(s); lgb = hi_alloc(s, cap + 16);
            if (!lgb) break;
            r = mdb_cdb_unpack(s->cdb_in, s->cdb_in_len, lgb, cap, &ll);
            if (!r) break;
            if (cap >= avail) break;
            cap *= 2;
        }
        s->st.t_cdb_us += now_us(s) - t0;
        if (r) { s->lib_ok = 0; set_msg(s, "iTunesCDB too large for the memory (or invalid)", 0); lg(s, "CDB", "unpack failed", 0xFFFFFFFFu); }
        else { s->lin = (uint32_t)ll; s->lib_before = cdb_ntracks(lgb, (uint32_t)ll); s->art_maxref = cdb_max_artref(lgb, (uint32_t)ll); s->lib_ok = 1; s->st.cdb_phys_in = s->cdb_in_len; s->st.cdb_logical_in = (uint32_t)ll; set_msg(s, "Reading the tags...", 0); lg(s, "LIBRARY tracks", "", (uint32_t)s->lib_before); }
        hi_reset(s);
    }
    s->ph = NS_P_TAGS; s->sub = 0; s->sub2 = 0; return 1;
}

static int alloc_tagmem(ns_t *s) {      /* P (prefix+tail) and xbuf in the hi area */
    uint32_t avail; hi_reset(s); avail = wfree(s);
    if (avail < XBUF_BYTES + 200u * 1024u) return -1;
    avail -= XBUF_BYTES + 4096u; s->Pcap = avail < PCAP_MAX ? (avail - 160u) & ~15u : PCAP_MAX;
    s->P = hi_alloc(s, s->Pcap + 160); s->xbuf = (char *)hi_alloc(s, XBUF_BYTES);
    return (s->P && s->xbuf) ? 0 : -1;
}
/* tags from each file's prefix (for the list) */
static void tag_skip(ns_t *s, ns_file *f, int st, const char *code, const char *why) {
    f->status = (int8_t)st; scopy(f->reason, code, sizeof f->reason);          /* the list shows the code (E_TAG...) */
    s->xf.done = s->Pn; s->xf.total = f->size; diag_build(s, code, st, f->name); lg(s, "TAGSKIP", why, 0xFFFFFFFFu); diag_log(s);
    s->sub++; s->sub2 = 0; }
static int ph_tags(ns_t *s) {
    char p[MAXPATH]; ns_file *f; int32_t got; uint32_t n;
    if (s->sub >= s->nf) {
        s->ph = NS_P_READY; s->prog = 0; s->stage = NS_ST_NONE;
        if (s->nf == 0) set_msg(s, "No MP3 in /Apps/Data/Importar", "Copy MP3 files into this folder.");
        else if (!s->lib_ok) { /* keep the library message */ }
        else set_msg(s, s->dry ? "Ready (DRY RUN: nothing will be written)" : "Ready to import", 0);
        lg(s, "LIST mp3", "", s->nf); log_flush(s); return 0;
    }
    f = &s->files[s->sub];
    if (f->status < 0) { s->sub++; s->sub2 = 0; return 1; }
    if (s->sub2 == 0) {
        if (!s->P && alloc_tagmem(s)) { tag_skip(s, f, -2, "E_TAGMEM", "no memory"); return 1; }
        if (f->size < 256) { tag_skip(s, f, -1, "E_TAGSHORT", "MP3 too short/empty"); return 1; }
        path_imp(p, f->name);
        if (s->os->r_open(p)) { tag_skip(s, f, -1, "E_TAGOPEN", "could not open"); return 1; }
        s->Pn = 0; s->Ptail = 0; s->sub2 = 1; return 1;
    }
    if (s->Pn == 0) {                                                          /* first piece: find the tag size */
        n = f->size < NS_PIECE ? f->size : NS_PIECE;
        got = rd_piece(s, s->P, n, 1);
        if (got <= 0) { s->os->r_close(); tag_skip(s, f, -1, "E_TAGPREFIX", "prefix read error"); return 1; }
        s->Pn = (uint32_t)got; s->Ptail = need_prefix(s->P, s->Pn, f->size);
        if (s->Ptail + 128 > s->Pcap) { s->os->r_close(); tag_skip(s, f, -2, "E_TAGBIG", "large ID3 tag (cover)"); return 1; }
        return 1;
    }
    if (s->Pn < s->Ptail) {
        n = s->Ptail - s->Pn; if (n > NS_PIECE) n = NS_PIECE;
        got = rd_piece(s, s->P + s->Pn, n, 0);
        if (got <= 0) { s->os->r_close(); tag_skip(s, f, -1, "E_TAGREAD", "tag read error"); return 1; }
        s->Pn += (uint32_t)got; return 1;
    }
    s->os->r_close();
    if (parse_tags(s, s->sub, f->size, 0)) tag_skip(s, f, -1, "E_TAGPARSE", "invalid MP3");
    else {
        uint32_t co = 0, cl = 0; int cr = s->noart ? 0 : ns_id3_cover(s->P, s->Pn, &co, &cl);
        f->have_tags = 1; f->cv = cr == 1 ? 1 : (cr < 0 ? 2 : 0); if (cr == 1) s->art_cand = 1;
        if (cr < 0) lg(s, "WARNING unsupported cover (PNG/unsynchronized or compressed ID3): track enters without cover", f->name, 0xFFFFFFFFu);
        s->sub++; s->sub2 = 0;
    }
    return 1;
}

/* --------------------------------------------------------------------------- checks / plan */
static uint32_t rnd(ns_t *s) { uint32_t x = s->rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; s->rng = x; return x; }
static uint32_t est_need(ns_t *s, uint32_t n) {
    uint32_t lout = s->lin + n * 4096u + 8192u;
    /* worst phase: logical CDB input + output + scratch (measured: ~2x the input) ; then SQLite (3 outputs + scratch) */
    uint32_t g1 = s->cdb_in_len + lout + s->lin + 2u * s->lin + n * 4096u + 65536u;
    uint32_t g2 = s->cdb_in_len + lout + lout / 2u + 65536u + MDB_DEFLATE_WORK + lout + 65536u;
    uint32_t ng = (uint32_t)(s->lib_before > 0 ? s->lib_before : 0) + n;     /* sqlgen: measured scratch ~368 KB + ~12 KB per track of the final library */
    uint32_t sq = s->orig[0].size + s->orig[1].size + s->orig[2].size + 3u * (n * 1536u + 20480u) + 376832u + 12544u * ng;
    uint32_t g3 = s->cdb_in_len + lout / 2u + sq;
    uint32_t m = g1 > g2 ? g1 : g2; if (g3 > m) m = g3;
    { uint32_t g0 = s->cdb_in_len + XBUF_BYTES + 262144u; if (g0 > m) m = g0; }
    return m + 65536u;                                       /* measured margin: the estimate was ~40 KB below the real minimum */
}
static int ph_check(ns_t *s) {
    uint32_t i, ok = 0; int bp;
    s->bytes_total = 0; s->bytes_done = 0;
    for (i = 0; i < s->nf; i++) if (s->files[i].status == 0) { ok++; s->bytes_total += s->files[i].size; }
    if (!ok) { s->ph = NS_P_READY; set_msg(s, "No valid MP3 to import", 0); return 0; }
    if (!s->lib_ok) { s->ph = NS_P_READY; return 0; }                           /* the message already explains */
    bp = s->os->battery_pct();
    if (bp < NS_MIN_BATT) { char t[NS_MSG]; t[0] = 0; scat(t, "Battery ", sizeof t); cat_u(t, (uint32_t)(bp < 0 ? 0 : bp), sizeof t); scat(t, "%: minimum ", sizeof t); cat_u(t, NS_MIN_BATT, sizeof t); scat(t, "%. Charge the iPod.", sizeof t);
        s->ph = NS_P_READY; s->result = NS_R_REFUSED; set_msg(s, "Low battery: import refused", t); lg(s, "REFUSED battery", "", (uint32_t)bp); log_flush(s); return 0; }
    s->need = est_need(s, ok);
#ifdef NS_SIM_NOEST
    s->need = 0;
#endif
    if (s->need > (uint32_t)(s->wend - s->wbase)) {
        char t[NS_MSG]; t[0] = 0; scat(t, "Needs ~", sizeof t); cat_u(t, s->need / 1024u, sizeof t); scat(t, " KB, has ", sizeof t); cat_u(t, (uint32_t)(s->wend - s->wbase) / 1024u, sizeof t); scat(t, " KB. Import fewer tracks or free RAM.", sizeof t);
        s->ph = NS_P_READY; s->result = NS_R_REFUSED; set_msg(s, "Not enough memory: import refused", t); lg(s, "REFUSED memory", "", s->need); log_flush(s); return 0;
    }
    lg(s, s->dry ? "START DRY RUN" : "START IMPORT", "tracks", ok); trc(s, "NSSTART ", ok, s->dry);
    s->peak = 0; s->result = NS_R_NONE; s->ph = NS_P_PLAN; s->sub = 0; s->sub2 = 0; set_msg(s, "Planning file names...", 0);
    return 1;
}
static const char L4[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
static int ph_plan(ns_t *s) {
    char p[MAXPATH], fx[8], dir[40]; uint32_t i, tries, cnt; ns_file *f; int isd; char nm[40];
    while (s->sub < s->nf && s->files[s->sub].status != 0) s->sub++;
    if (s->sub >= s->nf) {
        s->sub = 0; s->sub2 = 0;
        s->art_bak = (!s->dry && s->art_cand) ? 1 : 0;
        if (s->dry) { s->ph = NS_P_TAIL; s->stage = NS_ST_COPY; set_msg(s, "Dry run: reading the MP3 files...", 0); }
        else { s->ph = NS_P_BAK; set_msg(s, "Making a backup (.bak)...", 0); }
        return 1;
    }
    f = &s->files[s->sub];
    for (tries = 0; tries < 400; tries++) {
        uint32_t r = rnd(s), a = (r >> 3) % 50u, k; int dup = 0;
        fx[0] = 'F'; fx[1] = (char)('0' + a / 10); fx[2] = (char)('0' + a % 10); fx[3] = 0;
        dir[0] = 0; scat(dir, "/iPod_Control/Music/", sizeof dir); scat(dir, fx, sizeof dir);
        if (s->os->is_dir(dir)) {                                              /* up to 99 files per folder */
            cnt = 0; if (!s->os->d_open(dir)) { while (cnt < 120 && s->os->d_next(nm, sizeof nm, &isd)) cnt++; s->os->d_close(); }
            if (cnt >= 99) continue;
        }
        { uint32_t q = rnd(s); f->dest[0] = 0; scat(f->dest, "iPod_Control/Music/", sizeof f->dest); scat(f->dest, fx, sizeof f->dest); scat(f->dest, "/", sizeof f->dest);
          k = slen(f->dest); for (i = 0; i < 4; i++) { f->dest[k++] = L4[(q >> (i * 8)) % 26u]; } f->dest[k] = 0; scat(f->dest, ".mp3", sizeof f->dest); }
        p[0] = '/'; p[1] = 0; scat(p, f->dest, sizeof p);
        if (s->os->exists(p)) continue;
        for (i = 0; i < s->sub; i++) if (seq(s->files[i].dest, f->dest)) dup = 1;
        if (dup) continue;
        if (!s->dry && !s->os->is_dir(dir) && s->os->mkdir(dir)) { refuse(s, "could not create the Music/Fxx folder"); return 0; }
        s->sub++; return 1;
    }
    refuse(s, "could not find a free name in Music/Fxx"); return 0;
}

/* --------------------------------------------------------------------------- backup */
/* sub = file index; sub2: 0 open copy, 1 pieces, 2 open .bak re-read, 3 pieces */
static int ph_bak(ns_t *s) {
    uint32_t idx = s->sub; int r;
    if (idx >= NDB) { s->ph = NS_P_JRN; s->sub = 0; s->sub2 = 0; set_msg(s, "Writing the journal...", 0); return 1; }
    if (!act_j(s, idx) || (idx >= 5 && s->orig[idx].size == 0)) { s->sub++; s->sub2 = 0; return 1; }   /* artwork absent: nothing to save */
    s->stage = NS_ST_NONE; s->prog = 100u * idx / NDB;
    switch (s->sub2) {
    case 0: if (xf_begin(s, DBF[idx].path, DBF[idx].bak, s->orig[idx].size, 0)) { refuse(s, "could not create the .bak"); return 0; } s->sub2 = 1; return 1;
    case 1: r = xf_piece(s); if (r < 0) { refuse(s, "failed to copy to the .bak"); return 0; }
        if (r == 1) {
            if (idx == IDX_CDB && mdiff(s->xf.sha, s->orig[idx].sha, 20)) { fail_c(s, "E_SHA_BAK", "iTunesCDB changed during the backup", 0); return 0; }
            mcopy(s->orig[idx].sha, s->xf.sha, 20); s->sub2 = 2;
        }
        return 1;
    case 2: if (xf_begin(s, DBF[idx].bak, 0, s->orig[idx].size, 0)) { refuse(s, "could not reopen the .bak"); return 0; } s->sub2 = 3; return 1;
    case 3: r = xf_piece(s); if (r < 0) { refuse(s, "failed to re-read the .bak"); return 0; }
        if (r == 1) { if (mdiff(s->xf.sha, s->orig[idx].sha, 20)) { fail_c(s, "E_SHA_BAK", ".bak does not match (hash)", 0); return 0; } s->sub++; s->sub2 = 0; }
        return 1;
    }
    return 0;
}
static int ph_jrn(ns_t *s) {
    uint32_t n;
    if (s->sub == 0) {
        build_journal(s);
        if (write_whole(s, NS_F_JOURNAL, s->jbuf, s->jlen)) { s->os->remove(NS_F_JOURNAL); refuse(s, "could not write the journal"); return 0; }
        s->sub = 1; return 1;
    }
    {   /* journal re-read */
        static uint8_t chk[JBUF]; (void)chk;
        n = s->os->read_all(NS_F_JOURNAL, s->iobuf, JBUF);
        if (n != s->jlen || mdiff(s->iobuf, s->jbuf, n)) { s->os->remove(NS_F_JOURNAL); refuse(s, "journal does not match on re-read"); return 0; }
        s->journaled = 1; lg(s, "JOURNAL written", "", s->jlen); log_flush(s);
        s->ph = NS_P_COPY; s->sub = 0; s->sub2 = 0; s->stage = NS_ST_COPY; s->prog = 0; set_msg(s, "Copying MP3 files...", 0); return 1;
    }
}

/* --------------------------------------------------------------------------- copy / read (dry run) of the tracks */
static void progress_bytes(ns_t *s) { s->prog = s->bytes_total ? (uint32_t)(((uint64_t)s->bytes_done * 100u) / s->bytes_total) : 100u; if (s->prog > 100) s->prog = 100; }
/* next step after the cover (or when skipping the cover) */
static int cv_next(ns_t *s, ns_file *f, int dry) {
    if (dry) { lg(s, "DRY RUN read", f->name, f->size); s->sub++; s->sub2 = 0; } else s->sub2 = 2;
    return 1;
}
/* cover memory: in the free areas (lo), once; released when the database generation restarts the lo region */
static int art_alloc(ns_t *s) {
    uint32_t k, psum = 0, scr = ns_cv_scratch_need() + 2048u, avail;
    if (s->cvbuf[0]) return 0;
    for (k = 0; k < NS_FMT_N; k++) psum += NS_FMT[k].psize;
    avail = wfree(s); if (avail < scr + CV_POOL + 65536u + psum) return -1;
    avail -= scr + CV_POOL + 65536u; s->cvcap = avail / psum; if (s->cvcap > NS_MAXCV) s->cvcap = NS_MAXCV;
    if (!s->cvcap) return -1;
    for (k = 0; k < NS_FMT_N; k++) { s->cvbuf[k] = lo_alloc(s, s->cvcap * NS_FMT[k].psize); if (!s->cvbuf[k]) { s->cvbuf[0] = 0; return -1; } }
    lg(s, "COVER memory: distinct covers per import", 0, s->cvcap);
    s->cvsc_n = scr; s->cvsc = lo_alloc(s, scr); s->cvpool = lo_alloc(s, CV_POOL);
    if (!s->cvsc || !s->cvpool) { s->cvbuf[0] = 0; return -1; }
    return 0;
}
/* image ids (from the current ArtworkDB) and validation of the existing ArtworkDB; failure => covers ignored (the track enters without cover) */
static void art_prep(ns_t *s) {
    uint32_t j, nid = 0, mx = 0, base; uint8_t *mark = s->lo, *buf = 0; int r = 0;
    if (!s->ncv) return;
    if (s->orig[5].size) {
        buf = lo_alloc(s, s->orig[5].size + 16);
        if (!buf || s->os->read_all(DBF[5].path, buf, s->orig[5].size) != s->orig[5].size) r = -3;
        else r = ns_adb_info(buf, s->orig[5].size, &nid, &mx);
    }
    s->lo = mark;
    if (r) {
        uint32_t i; lg(s, r == -2 ? "WARNING existing ArtworkDB has an MHIF of different size: covers ignored" : "WARNING existing ArtworkDB unexpected/unreadable: covers ignored", "", 0xFFFFFFFFu);
        for (i = 0; i < s->nf; i++) s->files[i].cs = 0;
        s->ncv = 0; s->art_bak = 0; return;
    }
    base = nid > mx + 1 ? nid : mx + 1; if (s->art_maxref + 1 > base) base = s->art_maxref + 1; if (base < 100) base = 100;
    for (j = 0; j < s->ncv; j++) s->cvid[j] = base + j;
    s->art_next = base + s->ncv;
}
/* ph = COPY: sub = file; sub2: 0 open, 1 pieces (copy), 2 open destination re-read, 3 pieces.  TAIL (dry run): read only. */
static int ph_copy(ns_t *s, int dry) {
    char sp[MAXPATH], dp[MAXPATH]; ns_file *f; int r;
    while (s->sub < s->nf && s->files[s->sub].status != 0) s->sub++;
    if (s->sub >= s->nf) {
        art_prep(s);
        if (s->ncv && !s->dry) { s->ph = NS_P_ART; s->sub = 0; s->sub2 = 0; s->stage = NS_ST_COPY; s->prog = 0; set_msg(s, "Writing the covers...", 0); return 1; }
        hi_reset(s); s->P = 0; s->ph = NS_P_GEN; s->sub = 0; s->sub2 = 0; s->stage = NS_ST_GEN; s->prog = 0; set_msg(s, "Generating the database...", 0); return 1;
    }
    f = &s->files[s->sub]; path_imp(sp, f->name); dp[0] = '/'; dp[1] = 0; scat(dp, f->dest, sizeof dp);
    switch (s->sub2) {
    case 0:
        if (!s->P && alloc_tagmem(s)) { fail(s, "not enough memory for the tags"); return 0; }
        if (xf_begin(s, sp, dry ? 0 : dp, f->size, 1)) { fail(s, "could not open source/destination"); return 0; }
        s->sub2 = 1; return 1;
    case 1: r = xf_piece(s); progress_bytes(s);
        if (r < 0) { fail(s, dry ? "failed to read the MP3" : "failed to copy the MP3"); return 0; }
        if (r == 0 && s->Pover) { fail_c(s, "E_TAGBIG", "ID3 tag too large", 0); return 0; }
        if (r == 1) {
            mcopy(f->sha, s->xf.sha, 20);
            if (s->Pover) { fail_c(s, "E_TAGBIG", "ID3 tag too large", 0); return 0; }
            if (parse_tags(s, s->sub, f->size, 1)) { fail_c(s, "E_TAGPARSE", "MP3 tags unreadable in the copy", 0); return 0; }
            f->have_tags = 1;
            if (f->cv == 1) { s->sub2 = 20; return 1; }                          /* decode the cover (P still holds this file's prefix) */
            if (dry) { lg(s, "DRY RUN read", f->name, f->size); s->sub++; s->sub2 = 0; return 1; }
            s->sub2 = 2;
        }
        return 1;
    case 20: {                                                                 /* cover start: find, deduplicate, allocate, prepare the decoder */
        uint32_t co = 0, cl = 0, j; int cr = ns_id3_cover(s->P, s->Pn, &co, &cl), k;
        if (cr != 1 || co + cl > s->Pn) { lg(s, "WARNING cover outside the prefix read: track enters without cover", f->name, 0xFFFFFFFFu); f->cv = 2; return cv_next(s, f, dry); }
        mdb_sha1(s->P + co, cl, s->cvkey_cur);
        for (j = 0; j < s->ncv; j++) if (!mdiff(s->cvkey_cur, s->cvkey[j], 20)) { f->cs = (uint8_t)(j + 1); return cv_next(s, f, dry); }   /* same cover in the batch = same image */
        if (art_alloc(s)) { lg(s, "WARNING not enough memory for the cover: track enters without cover", f->name, 0xFFFFFFFFu); f->cv = 2; return cv_next(s, f, dry); }
        if (s->ncv >= s->cvcap) { lg(s, "WARNING limit of distinct covers per import (memory): track enters without cover", f->name, s->cvcap); f->cv = 2; return cv_next(s, f, dry); }
        { uint8_t *out[NS_FMT_N], *img = s->P + co, *a1 = (uint8_t *)(((uintptr_t)s->xbuf + 3u) & ~(uintptr_t)3u), *a2 = 0; uint32_t n1 = XBUF_BYTES - 8u, n2 = 0;
          for (k = 0; k < NS_FMT_N; k++) out[k] = s->cvbuf[k] + s->ncv * NS_FMT[k].psize;
          /* memory for the progressive JPEG coefficients: xbuf sits right below P; the image is moved to the end of P and EVERYTHING left is used (xbuf + P up to the image) */
          if ((uint8_t *)s->xbuf + XBUF_BYTES == s->P && s->Pcap >= cl + 16u) {
              uint8_t *dst = s->P + ((s->Pcap - cl) & ~15u); uint32_t q;
              if (dst > img) { for (q = cl; q > 0; q--) dst[q - 1] = img[q - 1]; } else if (dst < img) { for (q = 0; q < cl; q++) dst[q] = img[q]; }
              img = dst; n1 = (uint32_t)(dst - a1) & ~15u;
          } else if (s->Pcap > s->Pn + 64u) { a2 = s->P + ((s->Pn + 31u) & ~15u); n2 = (s->Pcap - s->Pn - 64u) & ~15u; }
          k = ns_cv_begin(&s->cv, img, cl, s->cvpool, CV_POOL, s->cvsc, s->cvsc_n, out, a1, n1, a2, n2); }
        if (k) { lg(s, k == -1 ? "WARNING unsupported JPEG cover (format): track enters without cover" : "WARNING cover too large/small or not enough memory: track enters without cover", f->name, 0xFFFFFFFFu); f->cv = 2; return cv_next(s, f, dry); }
        s->cv_slot = s->ncv; s->sub2 = 21; return 1; }
    case 21: {                                                                 /* decode in slices of a few MCUs */
        uint32_t t0 = now_us(s); int k = ns_cv_step(&s->cv, 8);
        s->st.t_cover_us += now_us(s) - t0;
        if (k < 0) { lg(s, "WARNING failed to decode the cover (corrupt JPEG): track enters without cover", f->name, 0xFFFFFFFFu); f->cv = 2; return cv_next(s, f, dry); }
        if (k == 1) {
            uint32_t j = s->cv_slot; mcopy(s->cvkey[j], s->cvkey_cur, 20); s->cvsz[j] = s->cv.W0 * s->cv.H0 * 3u; s->cvfirst[j] = 0xFFFFFFFFu; s->ncv++; f->cs = (uint8_t)(j + 1);
            lg(s, "COVER decoded (width)", f->name, s->cv.W0); lg(s, "COVER height", 0, s->cv.H0); lg(s, "COVER scale", 0, s->cv.sc); lg(s, "COVER time us", 0, s->st.t_cover_us);
            return cv_next(s, f, dry);
        }
        return 1; }
    case 2: if (xf_begin(s, dp, 0, f->size, 0)) { fail(s, "could not reopen the copy"); return 0; } s->sub2 = 3; return 1;
    case 3: r = xf_piece(s);
        if (r < 0) { fail(s, "failed to re-read the copy"); return 0; }
        if (r == 1) { if (mdiff(s->xf.sha, f->sha, 20)) { fail_c(s, "E_SHA", "MP3 copy does not match (SHA-1)", 0); return 0; } lg(s, "COPIED", f->dest, f->size); s->sub++; s->sub2 = 0; }
        return 1;
    }
    return 0;
}

/* --------------------------------------------------------------------------- database generation */
static uint32_t ssz(const char *s) { return s ? slen(s) : 0; }
static int pack_cdb(ns_t *s, const uint8_t *lgc, size_t llen, uint8_t *ph, size_t cap, size_t *plen, uint8_t *work) {
    uint32_t hs = rd32(lgc + 4); size_t z;
    if (llen < 0xAB + 57 || lgc[0] != 'm' || hs < 0xAB + 57 || hs > llen) return -1;
    if (cap < hs + 64) return -1;
    mcopy(ph, lgc, hs);
    z = mdb_zdeflate_w(lgc + hs, llen - hs, ph + hs, cap - hs, work, MDB_DEFLATE_WORK);
    if (!z) return -2;
    { uint32_t v = (uint32_t)(hs + z); ph[8] = (uint8_t)v; ph[9] = (uint8_t)(v >> 8); ph[10] = (uint8_t)(v >> 16); ph[11] = (uint8_t)(v >> 24); }
    ph[0x0C] = 2; ph[0x0D] = ph[0x0E] = ph[0x0F] = 0; ph[0xA8] = 1; ph[0xA9] = 0;
    *plen = hs + z;
    (void)s;
    return mdb_cdb_sign(ph, *plen, GUID);
}
static int art_build(ns_t *s); static int ph_gen(ns_t *s) {
    uint32_t i, n = 0, t0; int r; uint8_t *scr; uint32_t scr_n;
    switch (s->sub) {
    case 0: {                                                                  /* mdb_track_t from the tags */
        char base[NS_NAME];
        s->now = s->os->unix_time(); s->xtra = 0;
        mdb_sqlite_change_counter = (s->now & 0x7fffffffu) | 2u;      /* never 1: differs from the previous file */
        for (i = 0; i < s->nf; i++) {
            ns_file *f = &s->files[i]; mdb_tags_t *t; mdb_track_t *tr; if (f->status) continue; t = &s->tg[i]; tr = &s->trk[n]; mzero(tr, sizeof *tr);
            if (!t->title[0]) { base_noext(f->name, base, sizeof base); mcopy(t->title, base, slen(base) + 1); }
            tr->title = t->title; tr->artist = t->artist; tr->album = t->album; tr->genre = t->genre; tr->composer = t->composer; tr->album_artist = t->album_artist;
            if (f->cs && s->ncv) { uint32_t sl = (uint32_t)f->cs - 1u; tr->artwork_id = s->cvid[sl]; tr->artwork_count = NS_FMT_N; tr->artwork_size = s->cvsz[sl]; if (s->cvfirst[sl] == 0xFFFFFFFFu) s->cvfirst[sl] = n; }
            tr->location = f->dest; tr->year = t->year; tr->track_no = t->track; tr->disc_no = t->disc; tr->length_ms = t->duration_ms;
            tr->size = f->size; tr->bitrate_kbps = t->bitrate_kbps; tr->sample_rate = t->sample_rate; tr->vbr = t->vbr;
            tr->date_added = s->now; tr->last_modified = s->now;
            tr->comment = t->comment; tr->grouping = t->grouping; tr->subtitle = t->subtitle; tr->description = t->description; tr->copyright = t->copyright;
            tr->content_provider = t->content_provider; tr->lyrics = 0; tr->lyrics_flag = 0; tr->total_tracks = t->total_tracks; tr->total_discs = t->total_discs;
            tr->bpm = t->bpm; tr->compilation = 0; tr->gapless_album = t->gapless_album; tr->explicit_flag = t->explicit_flag; tr->rating = t->rating; tr->release_date = t->release_date;
            if (t->compilation) lg(s, "WARNING compilation flag ignored", f->name, 0xFFFFFFFFu);
            if (t->lyrics_frames) lg(s, "WARNING lyrics ignored (Extras.itdb not supported)", f->name, 0xFFFFFFFFu);
            if (t->ignored[0]) lg(s, "WARNING fields not written", t->ignored, 0xFFFFFFFFu);
            s->xtra += 8u * (ssz(tr->comment) + ssz(tr->grouping) + ssz(tr->description) + ssz(tr->subtitle) + ssz(tr->copyright) + ssz(tr->content_provider));
            n++;
        }
        s->sub2 = n; s->sub = 1; s->prog = 5; return 1; }
    case 1: {                                                                  /* logical CDB: input, output, scratch */
        uint32_t lcap = s->lin + 64u, ocap; size_t ll = 0, olen = 0; uint8_t *lin, *lout; n = s->sub2;
        s->lo = s->cdb_in + ((s->cdb_in_len + 16 + 15u) & ~15u); hi_reset(s);
        ocap = (uint32_t)mdb_cdb_out_bound(s->lin, n) + s->xtra;
        lout = hi_alloc(s, ocap); lin = hi_alloc(s, lcap);
        if (!lout || !lin) { fail(s, "not enough memory (CDB)"); return 0; }
        t0 = now_us(s);
        r = mdb_cdb_unpack(s->cdb_in, s->cdb_in_len, lin, lcap, &ll);
        if (r || ll != s->lin) { fail(s, "failed to reopen the current CDB"); return 0; }
        scr_n = wfree(s) & ~15u; scr = hi_alloc(s, scr_n);
        if (!scr) { fail(s, "not enough memory (CDB scratch)"); return 0; }
        r = mdb_cdb_add_tracks(lin, ll, s->trk, n, lout, ocap, &olen, s->asg, scr, scr_n);
        s->st.t_cdb_us += now_us(s) - t0;
        if (r) { fail(s, r == MDB_E_SCRATCH ? "not enough memory (CDB scratch)" : "the CDB generator rejected the data"); return 0; }
        s->log_out = lout; s->log_out_len = (uint32_t)olen; s->log_out_cap = ocap; s->st.cdb_logical_out = (uint32_t)olen;
        s->hi = lout;                                                          /* release input and scratch (keep only the output) */
        s->lib_after = s->lib_before >= 0 ? s->lib_before + (int)n : -1;
        s->sub = 2; s->prog = 25; return 1; }
    case 2: {                                                                  /* compress (deflate) + sign + self-check */
        uint32_t pcap = s->log_out_len / 2u + 65536u; size_t plen = 0; uint8_t *ph, *work, *chk; size_t cl = 0; uint32_t ll = s->log_out_len;
        for (;;) {
            s->lo = s->cdb_in + ((s->cdb_in_len + 16 + 15u) & ~15u);           /* restart the persistent region after the current CDB */
            s->hi = s->log_out;
            ph = lo_alloc(s, pcap); work = hi_alloc(s, MDB_DEFLATE_WORK);
            if (!ph || !work) { fail(s, "not enough memory (deflate)"); return 0; }
            t0 = now_us(s);
            r = pack_cdb(s, s->log_out, ll, ph, pcap, &plen, (uint8_t *)work);
            s->st.t_defl_us += now_us(s) - t0;
            if (r == -2 && pcap < mdb_zdeflate_bound(ll) + 4096u) { pcap = (uint32_t)mdb_zdeflate_bound(ll) + 4096u; continue; }
            break;
        }
        if (r) { fail(s, "failed to compress/sign the new iTunesCDB"); return 0; }
        t0 = now_us(s);
        if (!mdb_cdb_verify(ph, plen, GUID)) { fail(s, "self-check: new CDB HashAB invalid"); return 0; }
        s->st.t_sign_us += now_us(s) - t0;
        chk = hi_alloc(s, ll + 64u + 16u);
        if (!chk) { fail(s, "not enough memory (CDB self-check)"); return 0; }
        r = mdb_cdb_unpack(ph, plen, chk, ll + 64u, &cl);
        if (r || cl != ll || mdiff(chk + 0xAB + 57, s->log_out + 0xAB + 57, ll - 0xAB - 57)) { fail(s, "self-check: CDB round trip failed"); return 0; }
        s->lo = ph + ((plen + 16 + 15u) & ~(size_t)15u);                        /* give back the excess of the CDB buffer */
        s->out_buf[IDX_CDB] = ph; s->out_len[IDX_CDB] = (uint32_t)plen; s->st.cdb_phys_out = (uint32_t)plen;
        s->hi = s->log_out; s->sub = 3; s->prog = 50; return 1; }
    case 3: {                                                                  /* SQLite + cbk */
        mdb_sqlgen_out_t so; uint32_t k, ncap[3], attempt; uint8_t *cbk, *lo0 = s->lo; n = s->sub2;
        for (attempt = 0; attempt < 2; attempt++) {
            uint32_t grow = attempt == 0 ? n * 1536u + 16384u : n * 8192u + 65536u;
            s->lo = lo0; s->hi = s->log_out;
            for (k = 0; k < 3; k++) { ncap[k] = (s->orig[k].size + grow + 4095u) & ~4095u; s->out_buf[k] = lo_alloc(s, ncap[k]); if (!s->out_buf[k]) break; so.buf[k] = s->out_buf[k]; so.cap[k] = ncap[k]; so.len[k] = 0; }
            if (k < 3) { fail(s, "not enough memory (SQLite)"); return 0; }
            scr_n = wfree(s) & ~15u; scr = hi_alloc(s, scr_n);
            if (!scr || scr_n < 65536u) { fail(s, "not enough memory (SQLite scratch)"); return 0; }
            t0 = now_us(s);
            r = mdb_sqlgen_build(s->log_out, s->log_out_len, &so, scr, scr_n);
            s->st.t_sql_us += now_us(s) - t0;
            if (r != MDB_SQL_E_SPACE) break;                                   /* retry only if the output did not fit */
        }
        if (r) { fail(s, r == MDB_SQL_E_MEM ? "not enough memory (SQLite)" : r == MDB_SQL_E_SPACE ? "SQLite output larger than expected" : "the SQLite generator rejected the data"); return 0; }
        for (k = 0; k < 3; k++) { s->out_len[k] = (uint32_t)so.len[k]; s->st.sql_len[k] = (uint32_t)so.len[k]; }
        s->st.scratch_sql_used = (uint32_t)so.scratch_used;
        s->hi = s->log_out;                                                    /* release the SQLite scratch */
        cbk = lo_alloc(s, (uint32_t)mdb_cbk_size(so.len[1]) + 16);
        if (!cbk) { fail(s, "not enough memory (cbk)"); return 0; }
        mdb_cbk_build(cbk, so.buf[1], so.len[1], GUID); s->out_buf[3] = cbk; s->out_len[3] = (uint32_t)mdb_cbk_size(so.len[1]); s->st.cbk_len = s->out_len[3];
        s->hi = s->wend; s->sub = 4; s->prog = 90; return 1; }
    case 4: {
        if (s->ncv && !s->dry && art_build(s)) { fail_c(s, "E_ARTDB", "failed to build the ArtworkDB", 0); return 0; }
        for (i = 0; i < NDB; i++) { if (!s->out_buf[i]) continue; mdb_sha1(s->out_buf[i], s->out_len[i], s->neww[i].sha); s->neww[i].size = s->out_len[i]; s->dlen[i] = s->out_len[i]; }
        s->st.t_gen_us = s->st.t_cdb_us + s->st.t_sql_us + s->st.t_defl_us + s->st.t_sign_us;
        lg(s, "GENERATED new database", "tracks", s->sub2); trc(s, "NSGEN   ", s->peak, s->st.t_gen_us);
        if (s->dry) {
            char t[NS_MSG]; s->ph = NS_P_DONE; s->stage = NS_ST_NONE; s->prog = 100; s->result = NS_R_DRYRUN_OK;
            t[0] = 0; scat(t, "Library: ", sizeof t); cat_u(t, (uint32_t)s->lib_before, sizeof t); scat(t, " -> ", sizeof t); cat_u(t, (uint32_t)s->lib_after, sizeof t); scat(t, " tracks. Peak memory ", sizeof t); cat_u(t, s->peak / 1024u, sizeof t); scat(t, " KB", sizeof t);
            set_msg(s, "DRY RUN complete: nothing was written", t); lg(s, "DRY RUN ok", t, 0xFFFFFFFFu); log_flush(s); return 0;
        }
        s->ph = NS_P_WRITE; s->sub = 0; s->sub2 = 0; s->stage = NS_ST_WRITE; s->prog = 0; set_msg(s, "Writing the database...", 0); return 1; }
    }
    return 0;
}

/* --------------------------------------------------------------------------- write, verify, commit */
/* new ArtworkDB = current + new images (owner = dbid of the first track using the image), offsets in the .ithmb = base + slot * psize */
static int art_build(ns_t *s) {
    uint32_t j, k, ids[NS_MAXCV], srcsz[NS_MAXCV], offs[NS_FMT_N][8], olen = s->orig[5].size, cap, len; uint64_t owner[NS_MAXCV]; uint8_t *old = 0, *out;
    for (j = 0; j < s->ncv; j++) {
        if (s->cvfirst[j] == 0xFFFFFFFFu) return -1;
        ids[j] = s->cvid[j]; srcsz[j] = s->cvsz[j]; owner[j] = s->asg[s->cvfirst[j]].db_track_id;
        for (k = 0; k < NS_FMT_N; k++) offs[k][j] = s->cvbase[k] + j * NS_FMT[k].psize;
    }
    if (olen) { old = lo_alloc(s, olen + 16); if (!old || s->os->read_all(DBF[5].path, old, olen) != olen) return -2; }
    cap = olen + s->ncv * 1024u + 4u * 124u + 1536u; out = lo_alloc(s, cap); if (!out) return -3;
    len = ns_adb_build(old, olen, s->ncv, ids, owner, srcsz, offs, s->art_next, out, cap); if (!len) return -4;
    s->out_buf[5] = out; s->out_len[5] = len; s->dlen[5] = len;
    return 0;
}
/* ART phase: write the .ithmb files (old from the .bak + zeros up to the alignment + new images), one format at a time. The OS has no "append": the
   whole file is rewritten from the .bak (which was written and verified before the journal). The .ithmb files only grow. */
static int ph_art(ns_t *s) {
    uint32_t k = s->sub, idx, psz, S, al, r;
    if (k >= NS_FMT_N) {
        s->artw = 1; hi_reset(s); s->P = 0;
        s->ph = NS_P_GEN; s->sub = 0; s->sub2 = 0; s->stage = NS_ST_GEN; s->prog = 0; set_msg(s, "Generating the database...", 0); return 1;
    }
    idx = 6 + k; psz = NS_FMT[k].psize; S = s->orig[idx].size; al = ((S + psz - 1u) / psz) * psz;
    s->prog = 100u * k / NS_FMT_N;
    switch (s->sub2) {
    case 0:
        if (k == 0 && !s->os->is_dir(NS_DIR_ART) && s->os->mkdir(NS_DIR_ART)) { fail_c(s, "E_ARTDIR", "could not create the Artwork folder", 0); return 0; }
        s->cvbase[k] = al;
        if (S) {
            if (xf_begin(s, DBF[idx].bak, DBF[idx].path, S, 0)) { fail_c(s, "E_ARTOPEN", "could not open the .ithmb to rewrite", 0); return 0; }
            s->xf.app = s->cvbuf[k]; s->xf.applen = s->ncv * psz; s->xf.gap = al - S; s->xf.wsh_on = 1; mdb_sha1_init(&s->xf.wsh); s->sub2 = 1;
        } else {
            mzero(&s->xf, sizeof s->xf);
            if (s->os->w_open(DBF[idx].path)) { fail_c(s, "E_ARTOPEN", "could not create the .ithmb", 0); return 0; }
            s->xf.total = s->ncv * psz; mdb_sha1_init(&s->xf.wsh); s->sub2 = 2;
        }
        return 1;
    case 1:
        r = (uint32_t)xf_piece(s); if ((int32_t)r < 0) { fail(s, "failed to rewrite the .ithmb"); return 0; }
        if (r == 1) { mcopy(s->neww[idx].sha, s->xf.wshf, 20); s->out_len[idx] = al + s->ncv * psz; s->neww[idx].size = s->out_len[idx]; s->dlen[idx] = s->out_len[idx]; lg(s, "WRITTEN", DBF[idx].path, s->out_len[idx]); s->sub++; s->sub2 = 0; }
        return 1;
    case 2: {
        uint32_t off = s->xf.done, n = s->xf.total - off; if (n > NS_PIECE) n = NS_PIECE;
        if (n && s->os->w_write(s->cvbuf[k] + off, n)) { s->os->w_close(); fail_c(s, "E_ARTWRITE", "failed to write the .ithmb", -5); return 0; }
        if (n) mdb_sha1_update(&s->xf.wsh, s->cvbuf[k] + off, n);
        s->xf.done += n;
        if (s->xf.done >= s->xf.total) {
            if (s->os->w_close()) { fail_c(s, "E_ARTWRITE", "failed to close the .ithmb", -7); return 0; }
            mdb_sha1_final(&s->xf.wsh, s->neww[idx].sha); s->out_len[idx] = s->xf.total; s->neww[idx].size = s->xf.total; s->dlen[idx] = s->xf.total;
            lg(s, "WRITTEN", DBF[idx].path, s->out_len[idx]); s->sub++; s->sub2 = 0;
        }
        return 1; }
    }
    return 0;
}
/* write up to n bytes of database file idx from off: data from out_buf (up to dlen) and ZEROS beyond that (padding) */
static int db_chunk(ns_t *s, uint32_t idx, uint32_t off, uint32_t n) {
    uint32_t dl = s->dlen[idx]; const uint8_t *p;
    if (off + n <= dl) p = s->out_buf[idx] + off;
    else { mzero(s->iobuf, n); if (off < dl) mcopy(s->iobuf, s->out_buf[idx] + off, dl - off); p = s->iobuf; }
    if (s->xf.wsha) mdb_sha1_update(&s->xf.sh, p, n);
    return s->os->w_write(p, n);
}
/* cbk over the PHYSICAL Locations.itdb (with the zero padding), computed over the file as it is on disk */
static int rebuild_cbk(ns_t *s) {
    uint32_t pl = s->out_len[1]; uint8_t *lb = lo_alloc(s, pl), *cb;
    if (!lb) return -1;
    mcopy(lb, s->out_buf[1], s->dlen[1]); mzero(lb + s->dlen[1], pl - s->dlen[1]);
    cb = lo_alloc(s, (uint32_t)mdb_cbk_size(pl) + 16); if (!cb) return -1;
    mdb_cbk_build(cb, lb, pl, GUID);
    s->out_buf[3] = cb; s->dlen[3] = s->out_len[3] = (uint32_t)mdb_cbk_size(pl); s->st.cbk_len = s->out_len[3];
    mdb_sha1(cb, s->out_len[3], s->neww[3].sha); s->neww[3].size = s->out_len[3];
    return 0;
}
/* physical iTunesCDB larger than the new one: pad with zeros and RE-SIGN (HashAB is over the physical file) */
static int pad_cdb(ns_t *s, uint32_t sz) {
    uint8_t *nb = lo_alloc(s, sz + 16);
    if (!nb) return -1;
    mcopy(nb, s->out_buf[4], s->dlen[4]); mzero(nb + s->dlen[4], sz - s->dlen[4]);
    if (mdb_cdb_sign(nb, sz, GUID) || !mdb_cdb_verify(nb, sz, GUID)) return -2;
    s->out_buf[4] = nb; s->dlen[4] = s->out_len[4] = sz; s->st.cdb_phys_out = sz;
    return 0;
}
/* sub = file; sub2: 0 open | 1 write | 5 check the PHYSICAL size | 6 reopen to pad | 7 rewrite with zeros.
   The OS keeps Library.itdb open: remove fails (rc=5) and overwriting a larger file does NOT truncate. So a database file is never shrunk:
   if the fsize after writing is larger than the new content, the new content + zeros up to the current size is rewritten (whole pages of
   zeros in the .itdb files: the SQLite header keeps the real size in pages and version-valid-for == change counter, so SQLite
   ignores the extra pages). cbk and iTunesCDB follow the physical file. */
static int ph_write(ns_t *s) {
    uint32_t idx = s->sub; uint32_t off, n; int32_t sz;
    if (idx >= NDB) { s->ph = NS_P_VERIFY; s->sub = 0; s->sub2 = 0; s->stage = NS_ST_VERIFY; s->prog = 0; set_msg(s, "Verifying what was written...", 0); return 1; }
    if (idx >= 5 && (!s->artw || idx >= 6)) { s->sub++; s->sub2 = 0; return 1; }     /* artwork: ArtworkDB here; the .ithmb files were already written in the ART phase */
    s->prog = 100u * idx / NDB;
    switch (s->sub2) {
    case 0: case 6:
#ifdef NS_SIM_OLD_RM      /* simulator only: reproduces the PREVIOUS engine (fatal remove-before-write) to prove before/after */
        if (s->sub2 == 0 && s->os->exists(DBF[idx].path)) { if (s->os->remove(DBF[idx].path)) { fail_c(s, "E_DBREMOVE", "could not remove the database file before rewriting", 0); return 0; } lg(s, "REMOVED", DBF[idx].path, 0xFFFFFFFFu); }
#endif
        if (s->sub2 == 0 && idx == 3 && s->out_len[1] > s->dlen[1] && rebuild_cbk(s)) { fail_c(s, "E_PADCBK", "not enough memory (padded Locations cbk)", 0); return 0; }
        if (s->os->w_open(DBF[idx].path)) { fail_c(s, "E_DBOPEN", "could not open the database file for writing", 0); return 0; }
        s->xf.done = 0; s->xf.total = s->out_len[idx];
        if (s->sub2 == 6) { mdb_sha1_init(&s->xf.sh); s->xf.wsha = 1; s->sub2 = 7; } else s->sub2 = 1;
        return 1;
    case 1: case 7:
        off = s->xf.done; n = s->out_len[idx] - off; if (n > NS_PIECE) n = NS_PIECE;
        if (n && db_chunk(s, idx, off, n)) { s->os->w_close(); s->xf.wsha = 0; fail_c(s, "E_DBWRITE", "failed to write the database file", -5); return 0; }
        s->xf.done += n;
        if (s->xf.done >= s->out_len[idx]) {
            if (s->os->w_close()) { s->xf.wsha = 0; fail_c(s, "E_DBCLOSE", "failed to close the database file", -7); return 0; }
            lg(s, "WRITTEN", DBF[idx].path, s->out_len[idx]);
            if (s->sub2 == 7) { mdb_sha1_final(&s->xf.sh, s->neww[idx].sha); s->neww[idx].size = s->out_len[idx]; s->xf.wsha = 0; s->sub++; s->sub2 = 0; }
            else s->sub2 = 5;
        }
        return 1;
    case 5:
        sz = s->os->fsize(DBF[idx].path);
        if (sz <= (int32_t)s->out_len[idx]) { s->sub++; s->sub2 = 0; return 1; }       /* the OS truncated: nothing to do */
        { char t[NS_MSG]; t[0] = 0; scat(t, "real ", sizeof t); cat_u(t, s->out_len[idx], sizeof t); scat(t, " physical ", sizeof t); cat_u(t, (uint32_t)sz, sizeof t); lg(s, "DIAG PAD", t, 0xFFFFFFFFu); lg(s, "DIAG PAD file", DBF[idx].path, 0xFFFFFFFFu); }
        if (idx == 3) { fail_c(s, "E_PADCBK", "the new cbk is smaller than the current one (cannot shrink)", 0); return 0; }
        if (idx == 4) { int pr = pad_cdb(s, (uint32_t)sz); if (pr) { fail_c(s, "E_PADCDB", pr == -1 ? "not enough memory (padded iTunesCDB)" : "failed to sign the padded iTunesCDB", pr); return 0; } }
        else s->out_len[idx] = (uint32_t)sz;
        s->sub2 = 6; return 1;
    }
    return 0;
}
static int ph_verify(ns_t *s) {
    uint32_t idx = s->sub; int r; int32_t sz;
    if (idx >= NDB) {
        /* write done.txt (imported sources) before the commit */
        char *b = s->jbuf; uint32_t i, k = 0; b[0] = 0;
        for (i = 0; i < s->nf; i++) if (s->files[i].status == 0) { scat(b, s->files[i].name, JBUF - 32); scat(b, "\n", JBUF - 32); cat_u(b, s->files[i].size, JBUF - 32); scat(b, "\n", JBUF - 32); k++; }
        if (write_whole(s, NS_F_DONE, b, slen(b))) { fail(s, "could not write done.txt"); return 0; }
        s->ph = NS_P_JDEL; s->sub = 0; set_msg(s, "Finishing...", 0); s->prog = 100; return 1;
    }
    if (idx >= 5 && !s->artw) { s->sub++; s->sub2 = 0; return 1; }
    s->prog = 100u * idx / NDB;
    if (s->sub2 == 0) {
        sz = s->os->fsize(DBF[idx].path);
        /* the directory size (FS_GETSIZE) right after rewriting an EXISTING file may be stale on the device: it is logged, but the
           decision belongs to the RE-READ (exactly out_len bytes, nothing past the end, SHA-1 equal to the expected one). */
        s->vsz_bad = (sz < 0 || (uint32_t)sz != s->out_len[idx]);
        if (s->vsz_bad) { lg(s, "DIAG VERIFYSIZE file", DBF[idx].path, 0xFFFFFFFFu); lg(s, "DIAG VERIFYSIZE fsize", 0, (uint32_t)sz); lg(s, "DIAG VERIFYSIZE expected", 0, s->out_len[idx]); }
        if (xf_begin(s, DBF[idx].path, 0, s->out_len[idx], 0)) { fail(s, "could not re-read the written file"); return 0; }
        s->sub2 = 1; return 1;
    }
    r = xf_piece(s);
    if (r < 0) { fail(s, "failed to re-read the written file"); return 0; }
    if (r == 1) { if (mdiff(s->xf.sha, s->neww[idx].sha, 20)) { fail_c(s, "E_SHA_DB", "written file does not match (hash)", 0); return 0; }
        if (s->vsz_bad) { lg(s, "WARNING directory size stale, content matches (re-read + SHA-1)", DBF[idx].path, 0xFFFFFFFFu); s->vsz_bad = 0; }
        s->sub++; s->sub2 = 0; }
    return 1;
}
static int ph_jdel(ns_t *s) {
    if (s->os->remove(NS_F_JOURNAL)) { fail(s, "could not delete the journal"); return 0; }
    s->committed = 1; s->journaled = 0; lg(s, "COMMIT journal deleted", "", 0xFFFFFFFFu);
    s->ph = NS_P_RMSRC; s->sub = 0; set_msg(s, "Removing imported sources...", 0); return 1;
}
static int ph_rmsrc(ns_t *s) {
    char p[MAXPATH];
    while (s->sub < s->nf && s->files[s->sub].status != 0) s->sub++;
    if (s->sub >= s->nf) {
        s->os->remove(NS_F_DONE);
        { char t[NS_MSG]; t[0] = 0; scat(t, "Library: ", sizeof t); cat_u(t, (uint32_t)s->lib_before, sizeof t); scat(t, " -> ", sizeof t); cat_u(t, (uint32_t)s->lib_after, sizeof t); scat(t, " tracks. Restart the iPod.", sizeof t);
          s->ph = NS_P_DONE; s->result = NS_R_OK; s->stage = NS_ST_NONE; s->prog = 100; set_msg(s, "Import complete", t); lg(s, "DONE", t, 0xFFFFFFFFu); trc(s, "NSDONE  ", s->nf, s->peak); }
        log_flush(s); return 0;
    }
    path_imp(p, s->files[s->sub].name);
    if (s->os->remove(p)) lg(s, "WARNING could not remove the source", s->files[s->sub].name, 0xFFFFFFFFu);
    else lg(s, "source removed", s->files[s->sub].name, 0xFFFFFFFFu);
    s->sub++; return 1;
}

/* --------------------------------------------------------------------------- main loop */
static int dispatch(ns_t *s) {
    switch (s->ph) {
    case NS_P_INIT: s->ph = NS_P_SCAN; s->sub = 0; set_msg(s, "Looking for MP3 files in /Apps/Data/Importar", 0); return 1;
    case NS_P_RECOVER: return ph_recover(s);
    case NS_P_SCAN: return ph_scan(s);
    case NS_P_LIB: return ph_lib(s);
    case NS_P_TAGS: return ph_tags(s);
    case NS_P_READY: if (s->want_import) { s->want_import = 0; s->ph = NS_P_CHECK; s->P = 0; s->result = NS_R_NONE; return 1; } return 0;
    case NS_P_CHECK: return ph_check(s);
    case NS_P_PLAN: return ph_plan(s);
    case NS_P_BAK: return ph_bak(s);
    case NS_P_JRN: return ph_jrn(s);
    case NS_P_COPY: return ph_copy(s, 0);
    case NS_P_TAIL: return ph_copy(s, 1);
    case NS_P_ART: return ph_art(s);
    case NS_P_GEN: return ph_gen(s);
    case NS_P_WRITE: return ph_write(s);
    case NS_P_VERIFY: return ph_verify(s);
    case NS_P_JDEL: return ph_jdel(s);
    case NS_P_RMSRC: return ph_rmsrc(s);
    default: return 0;
    }
}
int ns_step(ns_t *s, uint32_t budget_us) {
    uint32_t t0 = now_us(s), t1;
    for (;;) {
        uint32_t u0 = now_us(s); int more = dispatch(s); uint32_t du = now_us(s) - u0;
        if (du > s->st.max_unit_us) s->st.max_unit_us = du;
        if (!more) break;
        t1 = now_us(s); if (t1 - t0 >= budget_us) break;
    }
    return !(s->ph == NS_P_READY || s->ph == NS_P_DONE || s->ph == NS_P_FAIL) ? 1 : 0;
}
