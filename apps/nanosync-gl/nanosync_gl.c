/* NanoSync, GL surface front end: the same import engine as nanosync.c (ns_engine, one memory block,
 * ~12 ms work slices) with a small UI drawn through the GL surface instead of LVGL.
 *
 * Why: an LV_SURFACE app only advances while the resident's LVGL heartbeat calls its frame callback.
 * Residents older than that heartbeat (for example the one in the NanoEmu snap6 snapshot) call it twice and
 * stop, so the engine never leaves "Starting...". GL surface apps get gl_app_frame() every frame on every
 * resident, so this build steps the engine from there.
 *
 * UI: the whole 240x432 screen is text and boxes rendered with the 8x8 font into one RGBA texture, rebuilt
 * when its content changes (at most every 200 ms); three buttons at the bottom (Import, Reload, Free RAM),
 * pressed by tapping. */
#include <stdint.h>
#include "gl.h"
#include "hb_gl_surface.h"
#include "hb_sdk.h"
#include "hb_heap.h"
#include "hb_surface_input.h"
#include "ns_engine.h"
#include "ns_hb.h"

#define RESERVE_BYTES   (1536u * 1024u)       /* heap left free for the OS */
#define BLOCK_MAX       (2048u * 1024u)

#define SW      240
#define SH      432
#define TW      256                           /* texture: power of two */
#define TH      512
#define COLS    29                            /* 8 px glyphs in 232 px */

#define C_BG    0x101826u
#define C_BOX   0x1c2740u
#define C_TEXT  0xffffffu
#define C_INFO  0x7fd3ffu
#define C_WARN  0xffd479u
#define C_DIM   0x8a95a8u
#define C_BTN   0x2f6fd6u
#define C_BTNOFF 0x3a4458u
#define C_BTNDN 0x1d4a96u
#define C_BAR   0x7fd3ffu

enum { B_IMPORT, B_RELOAD, B_FREE, B_COUNT };
static const struct { int x, y, w, h; const char *label; } btn[B_COUNT] = {
    { 6, 384, 70, 40, "Import" }, { 80, 384, 78, 40, "Reload" }, { 162, 384, 72, 40, "Free RAM" },
};

static ns_t *g_ns;
static uint8_t *g_blk;
static int g_freed, g_nomem;
static uint8_t g_px[TW * TH * 4];
static GLuint g_tex;
static int g_ready, g_dirty = 1, g_pressed = -1, g_was_down;
static uint32_t g_last_ms;
static uint32_t g_sig;                        /* hash of what the texture shows */

/* ------------------------------------------------------------------ memory (as nanosync.c) */
static void block_free(void) {
    ns_hb_close_all();
    if (g_blk) { if (HB_APP_AUX_BLOCK == g_blk) HB_APP_AUX_BLOCK = 0; hb_os_free(g_blk); }
    g_blk = 0; g_ns = 0;
}
static int block_alloc(void) {
    uint32_t f = hb_os_heap_free(), lg = hb_os_heap_largest(), cap, tries;
    if (HB_APP_AUX_BLOCK) { hb_os_free(HB_APP_AUX_BLOCK); HB_APP_AUX_BLOCK = 0; }   /* leftover of a previous launch */
    cap = f > RESERVE_BYTES ? f - RESERVE_BYTES : 0;
    if (cap > lg) cap = lg;
    if (cap > BLOCK_MAX) cap = BLOCK_MAX;
    for (tries = 0; tries < 4 && cap >= ns_min_block(); tries++) {
        uint8_t *b = (uint8_t *)hb_os_alloc(cap);
        if (b) {
            if (hb_os_heap_free() >= RESERVE_BYTES - 256u * 1024u) {
                g_blk = b; HB_APP_AUX_BLOCK = b;
                g_ns = ns_create(&NS_HB_OS, b, cap);
                if (g_ns) { hb_trace_log("NSBLK   ", cap, hb_os_heap_free()); return 0; }
            }
            hb_os_free(b); HB_APP_AUX_BLOCK = 0; g_blk = 0;
        }
        cap = cap > 320u * 1024u ? cap - 256u * 1024u : 0;
    }
    return -1;
}

/* ------------------------------------------------------------------ drawing into the texture */
static void fill(int x, int y, int w, int h, uint32_t rgb) {
    int i, j;
    for (j = y < 0 ? 0 : y; j < y + h && j < TH; j++)
        for (i = x < 0 ? 0 : x; i < x + w && i < TW; i++) {
            uint8_t *p = &g_px[(j * TW + i) * 4];
            p[0] = (uint8_t)(rgb >> 16); p[1] = (uint8_t)(rgb >> 8); p[2] = (uint8_t)rgb; p[3] = 255;
        }
}
/* one line of text; returns the width drawn */
static int text(int x, int y, const char *s, uint32_t rgb, int scale) {
    int x0 = x, gx, gy;
    for (; *s && x + 8 * scale <= TW; s++, x += 8 * scale) {
        const uint8_t *g = hb_font8x8_glyph(*s);
        for (gy = 0; gy < 8 * scale; gy++)
            for (gx = 0; gx < 8 * scale; gx++)
                if (g[gy / scale] & (0x80 >> (gx / scale))) {
                    uint8_t *p = &g_px[((y + gy) * TW + x + gx) * 4];
                    p[0] = (uint8_t)(rgb >> 16); p[1] = (uint8_t)(rgb >> 8); p[2] = (uint8_t)rgb; p[3] = 255;
                }
    }
    return x - x0;
}
static int slen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
/* word-wrapped text, at most `maxl` lines of COLS characters; returns the lines used */
static int wrap(int x, int y, const char *s, uint32_t rgb, int maxl) {
    char line[COLS + 1];
    int used = 0;
    while (s && *s && used < maxl) {
        int n = slen(s), cut = n <= COLS ? n : COLS, k;
        for (k = 0; k < cut && s[k] != '\n'; k++) ;
        if (k < cut) cut = k;                                  /* explicit line break */
        else if (n > COLS) { for (k = cut; k > 0 && s[k] != ' '; k--) ; if (k > 8) cut = k; }
        for (k = 0; k < cut; k++) line[k] = s[k];
        line[cut] = 0;
        text(x, y + used * 11, line, rgb, 1);
        used++;
        s += cut;
        while (*s == ' ' || *s == '\n') s++;
    }
    return used;
}
static void put(char **p, const char *s) { while (*s) *(*p)++ = *s++; **p = 0; }
static void put_u(char **p, uint32_t v) {
    char t[11]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *(*p)++ = t[--n];
    **p = 0;
}
static void put_i(char **p, int v) { if (v < 0) { *(*p)++ = '-'; v = -v; } put_u(p, (uint32_t)v); }

static const char *stage_name(ns_stage s) {
    switch (s) { case NS_ST_COPY: return "Copying"; case NS_ST_GEN: return "Generating"; case NS_ST_WRITE: return "Writing"; case NS_ST_VERIFY: return "Verifying"; default: return ""; }
}
static int import_enabled(void) { return g_ns && ns_get_phase(g_ns) == NS_P_READY && ns_nfiles(g_ns) > 0; }

static void render(void) {
    char buf[96], *p;
    int i, y;
    fill(0, 0, TW, TH, C_BG);
    text((SW - 8 * 2 * 8) / 2, 8, "NanoSync", C_TEXT, 2);
    if (!g_ns) {
        wrap(6, 40, g_nomem ? "Not enough memory" : g_freed == 2 ? "Import complete. Your songs are in Music." : g_freed ? "Memory released, you can exit" : "No memory", C_INFO, 3);
        wrap(6, 86, g_nomem ? "Close other apps or restart the iPod, then tap Reload." : g_freed == 2 ? "Press Home and open Music. Tap Reload to import more." : "Tap Reload to continue.", C_WARN, 4);
        p = buf; put(&p, "free heap "); put_u(&p, hb_os_heap_free() / 1024u); put(&p, " KB");
        text(6, 360, buf, C_DIM, 1);
    } else {
        ns_phase ph = ns_get_phase(g_ns);
        uint32_t n = ns_nfiles(g_ns), prog = ns_get_progress(g_ns);
        wrap(6, 40, ns_get_msg(g_ns), C_INFO, 4);
        wrap(6, 86, ns_get_msg2(g_ns), C_WARN, 4);
        p = buf;
        if (ns_lib_before(g_ns) >= 0) {
            put(&p, "Library: "); put_i(&p, ns_lib_before(g_ns));
            if (ns_lib_after(g_ns) >= 0) { put(&p, " -> "); put_i(&p, ns_lib_after(g_ns)); }
            put(&p, " tracks"); if (ns_dryrun(g_ns)) put(&p, " (dry run)");
        } else put(&p, "Library: ?");
        text(6, 136, buf, C_TEXT, 1);
        fill(6, 152, 228, 156, C_BOX);
        y = 156;
        if (ph == NS_P_FAIL || ph == NS_P_DONE) y += 11 * wrap(10, y, ns_get_diag(g_ns), C_WARN, 4);
        for (i = 0; i < (int)n && y < 296; i++, y += 11) {
            char a[28], b[18]; const ns_file *f = ns_file_at(g_ns, (uint32_t)i);
            ns_label(g_ns, (uint32_t)i, a, sizeof a, b, sizeof b);
            p = buf; put_u(&p, (uint32_t)i + 1); put(&p, ". ");
            if (f->status < 0) put(&p, "X ");
            put(&p, a); if (b[0]) { put(&p, " - "); put(&p, b); }
            if (p - buf > COLS - 1) buf[COLS - 1] = 0;
            text(10, y, buf, f->status < 0 ? C_WARN : C_TEXT, 1);
        }
        p = buf; put(&p, stage_name(ns_get_stage(g_ns))); put(&p, " "); put_u(&p, prog); put(&p, "%");
        text(6, 318, buf, C_TEXT, 1);
        fill(6, 334, 228, 10, C_BOX);
        fill(6, 334, (int)(228u * (prog > 100 ? 100 : prog) / 100u), 10, C_BAR);
        p = buf; put(&p, "RAM "); put_u(&p, ns_block_bytes(g_ns) / 1024u); put(&p, "K peak ");
        put_u(&p, ns_mem_peak(g_ns) / 1024u); put(&p, "K bat "); put_i(&p, NS_HB_OS.battery_pct()); put(&p, "%");
        text(6, 356, buf, C_DIM, 1);
    }
    for (i = 0; i < B_COUNT; i++) {
        int on = i == B_IMPORT ? import_enabled() : 1;
        uint32_t c = !on ? C_BTNOFF : g_pressed == i ? C_BTNDN : C_BTN;
        int lw = 8 * slen(btn[i].label);
        fill(btn[i].x, btn[i].y, btn[i].w, btn[i].h, c);
        text(btn[i].x + (btn[i].w - lw) / 2, btn[i].y + (btn[i].h - 8) / 2, btn[i].label, on ? C_TEXT : C_DIM, 1);
    }
}

/* a cheap fingerprint of everything render() shows, to rebuild the texture only on change */
static uint32_t mix(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }
static uint32_t mix_s(uint32_t h, const char *s) { while (s && *s) h = mix(h, (uint8_t)*s++); return mix(h, 0); }
static uint32_t signature(void) {
    uint32_t h = 2166136261u, i;
    h = mix(h, (uint32_t)g_pressed); h = mix(h, (uint32_t)g_freed); h = mix(h, (uint32_t)g_nomem);
    if (!g_ns) return mix(h, hb_os_heap_free() >> 10);
    h = mix(h, ns_get_phase(g_ns)); h = mix(h, ns_get_stage(g_ns)); h = mix(h, ns_get_progress(g_ns));
    h = mix_s(h, ns_get_msg(g_ns)); h = mix_s(h, ns_get_msg2(g_ns)); h = mix_s(h, ns_get_diag(g_ns));
    h = mix(h, (uint32_t)ns_lib_before(g_ns)); h = mix(h, (uint32_t)ns_lib_after(g_ns)); h = mix(h, ns_nfiles(g_ns));
    h = mix(h, ns_mem_peak(g_ns) >> 10);
    for (i = 0; i < ns_nfiles(g_ns); i++) h = mix(h, (uint32_t)ns_file_at(g_ns, i)->status);
    return h;
}

/* ------------------------------------------------------------------ music library reload */
/* RetailOS 37A40005 (iPod nano 7G 1.1.2) keeps the library open from boot. After an import, Music is made to
 * read the new databases without a restart:
 *  1. the music model object (class TMusicModel, vtable 0x08799df0, instance pointer in the global 0x08812dfc)
 *     drops its data: UnloadData, vtable +0x90 (0x081b0ca0);
 *  2. the SQLite (3.7.5) connection to Library.itdb (connection object in the global 0x08811220, class vtable
 *     0x087b06e8, sqlite3* at +4) holds file handles opened at boot, on the clusters the files had then. For
 *     every database of the connection (main, Locations, Dynamic: db->aDb +8, nDb +4; Btree->BtShared +4 ->
 *     Pager +0) only the file is reopened: the pager's sqlite3_file (+0x3c) is closed with its own xClose and
 *     opened again in place with the VFS's xOpen (SQLite_iPod_VFS, pager->pVfs +0) on pager->zFilename (+0x90)
 *     with the same flags (file +0xc). The pager then sees the new change counter and drops its page cache;
 *  3. the DBAccess library layer is rebuilt (Init 0x0848f058, state byte 0x088111f8 back to its boot value 3);
 *  4. the model loads again: ReloadData, vtable +0x80 (0x081b03e0).
 * The databases must have the schema this firmware writes (version 1109, core/mdb_schema.h): the OS's
 * statements use its columns and the in-place reload does not migrate older schemas the way a boot does.
 * Every step checks the firmware's code or data first and is skipped on a different firmware. */
#define FW_MUSIC_MODEL_PTR   0x08812dfcu
#define FW_MUSIC_MODEL_VT    0x08799df0u
#define FW_RELOAD_DATA       0x081b03e1u
#define FW_UNLOAD_DATA       0x081b0ca1u
static int fw_is(uint32_t a, uint32_t w0, uint32_t w1) {
    const volatile uint32_t *p = (const volatile uint32_t *)a;
    return p[0] == w0 && p[1] == w1;
}
static void reopen_db_files(void) {
    void **conn = *(void ***)0x08811220u;
    uint8_t *db, *aDb; int nDb, k;
    if (!conn || (uint32_t)conn[0] != 0x087b06e8u || !conn[1] ||
        *(uint32_t *)((uint8_t *)conn[1] + 0x38) != 0xa029a697u) return;          /* SQLITE_MAGIC_OPEN */
    db = (uint8_t *)conn[1]; nDb = *(int *)(db + 4); aDb = *(uint8_t **)(db + 8);
    for (k = 0; k < nDb && k < 8; k++) {
        uint8_t *bt = *(uint8_t **)(aDb + 16 * k + 4), *shared, *pager, *vfs;
        void **fd; const char *name; int flags, out = 0;
        if (!bt || !(shared = *(uint8_t **)(bt + 4)) || !(pager = *(uint8_t **)shared)) continue;
        vfs = *(uint8_t **)pager; fd = *(void ***)(pager + 0x3c); name = *(const char **)(pager + 0x90);
        if (!vfs || !fd || !name || *(uint32_t *)(vfs + 0x18) != 0x0844b745u ||
            (uint32_t)fd[0] != 0x0875d9d8u || !((uint8_t *)fd)[8]) continue;
        flags = *(int *)((uint8_t *)fd + 0xc);
        ((int (*)(void *))((uint32_t *)fd[0])[1])(fd);                                  /* xClose */
        ((int (*)(void *, const char *, void *, int, int *))*(uint32_t *)(vfs + 0x18))(vfs, name, fd, flags, &out);
    }
}
static int g_reloaded;
static int library_reload(void) {
    void **obj = *(void ***)FW_MUSIC_MODEL_PTR, **vt;
    if (!obj) return -1;
    vt = (void **)obj[0];
    if ((uint32_t)vt != FW_MUSIC_MODEL_VT || (uint32_t)vt[0x80 / 4] != FW_RELOAD_DATA ||
        (uint32_t)vt[0x90 / 4] != FW_UNLOAD_DATA) return -1;
    ((int (*)(void *))vt[0x90 / 4])(obj);                      /* UnloadData */
    reopen_db_files();
    if (fw_is(0x0848f058u, 0x4c06b510u, 0x1c406860u)) {
        ((void (*)(void))0x0848f059u)();                      /* DBAccess library Init */
        *(volatile uint8_t *)0x088111f8u = 3;
    }
    ((void (*)(void *, int))vt[0x80 / 4])(obj, 0);            /* ReloadData */
    return 0;
}

/* ------------------------------------------------------------------ input */
static int hit(int x, int y) {
    int i;
    for (i = 0; i < B_COUNT; i++)
        if (x >= btn[i].x && x < btn[i].x + btn[i].w && y >= btn[i].y && y < btn[i].y + btn[i].h) return i;
    return -1;
}
static void press(int b) {
    if (b == B_IMPORT) { if (g_ns) ns_request_import(g_ns); }
    else if (b == B_RELOAD) {
        if (!g_ns) { g_freed = 0; g_nomem = 0; if (block_alloc()) g_nomem = 1; }   /* reload after "Free RAM" */
        else { ns_request_rescan(g_ns); library_reload(); }    /* also refresh Music from the files on disk */
    } else if (b == B_FREE && g_ns) {
        ns_phase p = ns_get_phase(g_ns);
        if (p == NS_P_READY || p == NS_P_DONE || p == NS_P_FAIL) { block_free(); g_freed = 1; }   /* never mid-import */
    }
}
static void touch(void) {
    hb_spoint_t t;
    hb_surface_touch_read(&t);
    if (t.down && !g_was_down) g_pressed = hit(t.x, t.y);
    if (!t.down && g_was_down) { if (g_pressed >= 0 && hit(t.x, t.y) == g_pressed) press(g_pressed); g_pressed = -1; }
    else if (t.down && g_pressed >= 0 && hit(t.x, t.y) != g_pressed) g_pressed = -1;
    g_was_down = t.down;
}

/* ------------------------------------------------------------------ GL surface contract */
void gl_app_init(void) {
    g_ready = 0; g_dirty = 1; g_pressed = -1; g_was_down = 0;
    if (block_alloc()) g_nomem = 1;
}

void gl_app_frame(int w, int h, uint32_t frame) {
    static const GLfloat uv[8] = { 0.f, 0.f, (GLfloat)SW / TW, 0.f, 0.f, (GLfloat)SH / TH, (GLfloat)SW / TW, (GLfloat)SH / TH };
    GLfloat v[8];
    uint32_t now = hb_time_uptime_ms(), sig;
    (void)frame;
    if (!g_ready) {
        glGenTextures(1, &g_tex);
        glBindTexture(GL_TEXTURE_2D, g_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        g_ready = 1;
    }
    touch();
    if (g_ns) {
        ns_step(g_ns, 12000);
        if (!g_reloaded && ns_get_result(g_ns) == NS_R_OK) {
            /* done: give the 2 MB back to the OS first (older residents never free the app's block, and
             * Music needs the heap to load the library and play), then reload the library */
            g_reloaded = 1; block_free(); g_freed = 2; library_reload();
        }
        if (g_ns && ns_get_result(g_ns) != NS_R_OK) g_reloaded = 0;
    }
    sig = signature();
    if (sig != g_sig) { g_sig = sig; g_dirty = 1; }
    if (g_dirty && (now - g_last_ms >= 200u || g_pressed >= 0 || !g_last_ms)) {
        render();
        glBindTexture(GL_TEXTURE_2D, g_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TW, TH, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_px);
        g_dirty = 0; g_last_ms = now ? now : 1;
    }
    glViewport(0, 0, w, h);
    glClearColor(0.063f, 0.094f, 0.149f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(0.f, (GLfloat)w, (GLfloat)h, 0.f, -1.f, 1.f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    v[0] = 0; v[1] = 0; v[2] = SW; v[3] = 0; v[4] = 0; v[5] = SH; v[6] = SW; v[7] = SH;
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, v);
    glTexCoordPointer(2, GL_FLOAT, 0, uv);
    glColor4f(1.f, 1.f, 1.f, 1.f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_TEXTURE_2D);
}
