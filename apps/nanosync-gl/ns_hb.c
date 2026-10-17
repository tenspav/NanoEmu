/* NanoSync - device platform: SDK hb_fs_* + FOBJ streaming reads.
   SDK limits that shape this file: hb_fs_read reads ONE block from the start (no offset); hb_fs_stream_* is ONE sequential
   writer (creates/truncates, EOF = bytes written, sync on close); there is NO rename and no positional write/read -> the engine uses
   .bak + journal. Sequential reads of large files: open with FOBJ_OPEN and call FOBJ_READ repeatedly on the same
   handle. */
#include "hb_sdk.h"
#include "hb_heap.h"
#include "ns_hb.h"
#include "ns_rd.h"

#define FOBJ_CACHE_BYTES 0x1010u
typedef void (*fobj_open_fn) (void *o, const char *path, int a2, int a3, uint32_t a4, uint32_t a5, void *a6);
typedef void (*fobj_close_fn)(void *o);
typedef int  (*fobj_ready_fn)(void *o);
typedef int  (*fobj_read_fn) (void *o, uint32_t n, void *buf, uint32_t *got);
#define FOBJ_OPEN  ((fobj_open_fn) (0x084137a8u | 1u))
#define FOBJ_CLOSE ((fobj_close_fn)(0x08423be0u | 1u))
#define FOBJ_READY ((fobj_ready_fn)(0x08417e18u | 1u))
#define FOBJ_READ  ((fobj_read_fn) (0x0841cddcu | 1u))

/* reader object + cache: static (like sdk/hb_fs.c; small stack); app .bss */
static uint8_t s_obj[0x400] __attribute__((aligned(8)));
static uint8_t s_cache[FOBJ_CACHE_BYTES] __attribute__((aligned(32)));
static int s_ropen, s_wopen, s_dopen;
static ns_iodiag s_dg;            /* last raw I/O return (error diagnostics) */
static hb_dir_t s_dir;

static int p_exists(const char *p) { return hb_fs_exists(p) ? 1 : 0; }
static int32_t p_fsize(const char *p) { if (!hb_fs_exists(p) || hb_fs_is_dir(p)) return -1; return (int32_t)hb_fs_size(p); }
static int p_is_dir(const char *p) { return hb_fs_is_dir(p) ? 1 : 0; }
static int p_mkdir(const char *p) { return hb_fs_mkdir(p) ? 0 : -1; }
static int p_remove(const char *p) { return hb_fs_remove(p) ? 0 : -1; }
static uint32_t p_read_all(const char *p, void *b, uint32_t max) { return hb_fs_read(p, b, max); }

static int p_r_open(const char *path) {
    uint32_t i; int ready;
    if (s_ropen) return -1;
    for (i = 0; i < FOBJ_CACHE_BYTES; i++) s_cache[i] = 0;
    FOBJ_OPEN(s_obj, path, 1 /* read */, 0 /* main volume */, 0x1000, 1, s_cache);
    ready = FOBJ_READY(s_obj);
    s_dg.rc = ready; s_dg.req = 0; s_dg.got = 0; s_dg.calls = 0;     /* rc = raw FOBJ_READY return */
    if (ready) { s_ropen = 1; return 0; }
    FOBJ_CLOSE(s_obj);
    return -1;
}
/* up to n bytes; fewer than n at end of file (the OS EOF rc is undocumented: see ns_rd.h); 0 = end/error; <0 = inconsistent OS */
static int32_t p_r_read(void *buf, uint32_t n) {
    if (!s_ropen) return -1;
    return ns_seq_read(FOBJ_READ, s_obj, buf, n, &s_dg);
}
static void p_io_diag(ns_iodiag *o) { *o = s_dg; }
static void p_r_close(void) { if (s_ropen) { FOBJ_CLOSE(s_obj); s_ropen = 0; } }

/* write diagnostic: rc = 0 ok / -1 failed (the SDK only returns bool), req = bytes requested, got = total written to this file, calls = calls */
static int p_w_open(const char *path) { s_dg.rc = 0; s_dg.req = 0; s_dg.got = 0; s_dg.calls = 0; if (s_wopen) return -1; if (!hb_fs_stream_open(path)) { s_dg.rc = -1; return -1; } s_wopen = 1; return 0; }
static int p_w_write(const void *b, uint32_t n) {
    int ok = hb_fs_stream_write(b, n) ? 1 : 0;
    s_dg.req = n; s_dg.calls++; s_dg.rc = ok ? 0 : -1; if (ok) s_dg.got += n;
    return ok ? 0 : -1;
}
static int p_w_close(void) { if (!s_wopen) return -1; s_wopen = 0; return hb_fs_stream_close() ? 0 : -1; }

static int p_d_open(const char *path) { if (s_dopen) return -1; if (!hb_fs_dir_open(&s_dir, path, false)) return -1; s_dopen = 1; return 0; }
static int p_d_next(char *name, int cap, int *isd) {
    bool d = false;
    while (hb_fs_dir_next(&s_dir, name, cap, &d)) {
        if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) continue;
        *isd = d ? 1 : 0; return 1;
    }
    return 0;
}
static void p_d_close(void) { if (s_dopen) { hb_fs_dir_close(&s_dir); s_dopen = 0; } }

static void *p_alloc(uint32_t n) { return hb_os_alloc(n); }
static void p_free(void *p) { hb_os_free(p); }
static uint32_t p_hfree(void) { return hb_os_heap_free(); }
static uint32_t p_hlarge(void) { return hb_os_heap_largest(); }
static uint32_t p_now(void) { return hb_time_uptime_us(); }
static int p_batt(void) { uint32_t l = hb_battery_level_0_to_15(); if (l > 15) l = 15; return (int)(l * 100u / 15u); }
static void p_trace(const char *t, uint32_t a, uint32_t b) { hb_trace_log(t, a, b); }

/* civil date -> Unix seconds (the iPod clock time is treated as UTC; no time zone) */
static uint32_t p_unix(void) {
    hb_rtc_time_t t; int32_t y, m, d; int64_t days; uint32_t yoe, doy, doe;
    t.seconds = t.minutes = t.hours = 0; t.day_of_month = 1; t.month = 1; t.year = 0;
    hb_rtc_read(&t);
    y = t.year; m = t.month; d = t.day_of_month;
    if (y < 2020 || y > 2099 || m < 1 || m > 12 || d < 1 || d > 31) return 1767225600u;   /* invalid clock: 2026-01-01 */
    y -= m <= 2;
    yoe = (uint32_t)(y - (y >= 0 ? y : y - 399) / 400 * 400);
    doy = (uint32_t)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    days = (int64_t)((y >= 0 ? y : y - 399) / 400) * 146097 + (int64_t)doe - 719468;
    return (uint32_t)(days * 86400 + (int64_t)t.hours * 3600 + (int64_t)t.minutes * 60 + t.seconds);
}

const ns_os NS_HB_OS = { p_exists, p_fsize, p_is_dir, p_mkdir, p_remove, p_read_all, p_r_open, p_r_read, p_r_close, p_w_open, p_w_write, p_w_close,
    p_d_open, p_d_next, p_d_close, p_alloc, p_free, p_hfree, p_hlarge, p_now, p_batt, p_unix, p_trace, p_io_diag };

/* close any open handle (when freeing memory / restarting) */
void ns_hb_close_all(void) { p_r_close(); p_d_close(); if (s_wopen) { s_wopen = 0; hb_fs_stream_close(); } }
