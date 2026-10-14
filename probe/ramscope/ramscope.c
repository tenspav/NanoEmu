/*
 * RAMScope — does this iPod nano 7G physically have more than the visible 64 MB
 * of DRAM?  Plan: workers/agy-ram.txt.  Raw-surface homebrew app (HBAppKind 1).
 *
 * Steps (one per UI frame, live "STEP n/5" progress, breadcrumb report.txt is
 * written BEFORE every risky access so a crash still leaves a trail):
 *   1 PREFLIGHT  CPSR mode, L1 page-table sanity, read DMC 0x3d700000/+04/+08/
 *                +0c/+10 (+0x100/+0x104 self-refresh state)
 *   2 MAP        L1[0x8c0] (0x08b2a300) invalid -> 0x0c084c12 (PA 0x0c000000,
 *                uncached section, AP=3), written through the uncached table
 *                alias 0x88b2a300; TLB invalidate only if running privileged
 *                (an invalid->valid change needs none: faults are not cached)
 *   3 ALIAS TEST for 4 offsets: save A=0x88000000+o, B=0x8c000000+o; IRQs OFF;
 *                write unique patterns to B; read A and B; restore B then A.
 *   4 ENABLE CS1 (OPT-IN, second button, RISKY): MEMCONFIG1 = MEMCONFIG0 with
 *                base 0x0c, DIRECTCMD refresh(rank1), repeat test, restore.
 *   5 RESTORE    L1 entry back to its original value, final report.
 *
 * LIMITATION (stated honestly): a user-mode app cannot install a data-abort
 * handler. If 0x0c000000 has no bus decode and the interconnect raises an
 * external abort, the OS handler reboots the device (no damage, nothing
 * persisted). report.txt then ends at the last "stage=" breadcrumb, which says
 * exactly which access did it — that outcome is itself the "no RAM" signal.
 *
 * Output: /Apps/Data/probe/ramscope/report.txt (own dir: does not clobber the
 * DumpRAM/DumpBoot reports).
 */
#include "hb_raw_surface.h"
#include "hb_sdk.h"

#define OUT_DIR     "/Apps/Data/probe/ramscope"
#define REPORT_PATH "/Apps/Data/probe/ramscope/report.txt"

#define DMC_BASE     0x3d700000u
#define TTB_BASE     0x08b28000u          /* L1 table, PA == cached VA          */
#define TTB_UC_ALIAS 0x88b28000u          /* same table through uncached alias  */
#define IDX_B        0x8c0u               /* VA 0x8c000000 >> 20                */
#define IDX_A        0x880u               /* VA 0x88000000 >> 20                */
#define SECT_B       0x0c084c12u          /* PA 0x0c000000, AP=3, uncached      */
#define VA_A         0x88000000u
#define VA_B         0x8c000000u
#define NOFF         4u
static const uint32_t OFFS[NOFF] = { 0x0u, 0x1000u, 0x100000u, 0x2000000u };

#define BG   0x0E1118
#define FG   0xE8ECF4
#define DIM  0x7C869A
#define OKC  0x2FBF71
#define BAD  0xE63946
#define ACC  0x6aa8ff
#define BTN  0x27423B

#define BTN_X 20
#define BTN_Y 130
#define BTN_W 200
#define BTN_H 70
#define EN_X  20
#define EN_Y  250
#define EN_W  200
#define EN_H  50

/* ------------------------------------------------------------- 3x5 font --- */
static const uint16_t FONT_AZ[26] = {
    0b010101111101101, 0b110101110101110, 0b011100100100011, 0b110101101101110,
    0b111100110100111, 0b111100110100100, 0b011100101101011, 0b101101111101101,
    0b111010010010111, 0b001001001101010, 0b101101110101101, 0b100100100100111,
    0b101111111101101, 0b110101101101101, 0b010101101101010, 0b110101110101100,
    0b010101101111011, 0b110101110101101, 0b011100110001110, 0b111010010010010,
    0b101101101101111, 0b101101101101010, 0b101101111111101, 0b101101010101101,
    0b101101010010010, 0b111001010100111,
};
static const uint16_t FONT_09[10] = {
    0b111101101101111, 0b010110010010111, 0b110001010100111, 0b110001110001110,
    0b101101111001001, 0b111100110001110, 0b011100111101111, 0b111001010010010,
    0b111101111101111, 0b111101111001110,
};

static uint16_t glyph_of(char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (c >= 'A' && c <= 'Z') return FONT_AZ[c - 'A'];
    if (c >= '0' && c <= '9') return FONT_09[c - '0'];
    switch (c) {
    case '-': return 0b000000111000000;
    case '/': return 0b001001010100100;
    case '.': return 0b000000000000010;
    case ':': return 0b000010000010000;
    case '%': return 0b101001010100101;
    default:  return 0;
    }
}

static void text(int x, int y, const char *s, int sc, uint32_t col)
{
    for (; *s; s++, x += 4 * sc) {
        uint16_t gl = glyph_of(*s);
        for (int r = 0; r < 5; r++)
            for (int c = 0; c < 3; c++)
                if (gl & (1u << (14 - (r * 3 + c))))
                    hb_raw_fill_rect(x + c * sc, y + r * sc, sc, sc, col);
    }
}

/* --------------------------------------------------------------- helpers --- */
static char *put_s(char *o, const char *s) { while (*s) *o++ = *s++; return o; }

static char *put_hex(char *o, uint32_t v)
{
    for (int i = 0; i < 8; i++) {
        uint32_t d = (v >> (28 - 4 * i)) & 0xFu;
        *o++ = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    }
    return o;
}

static char *put_dec(char *o, uint32_t v)
{
    char tmp[10];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + v % 10u); v /= 10u; }
    while (n) *o++ = tmp[--n];
    return o;
}

static char *put_kv(char *o, const char *k, uint32_t v)
{
    o = put_s(o, k); o = put_dec(o, v); *o++ = '\n';
    return o;
}

static char *put_kvx(char *o, const char *k, uint32_t v)
{
    o = put_s(o, k); o = put_s(o, "0x"); o = put_hex(o, v); *o++ = '\n';
    return o;
}


/* ----------------------------------------------------- privileged glue --- */
static inline uint32_t rd32(uint32_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uint32_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void dsb(void) { __asm__ volatile("dsb" ::: "memory"); }
static inline void isb(void) { __asm__ volatile("isb" ::: "memory"); }

static inline uint32_t cpsr_read(void)
{
    uint32_t r;
    __asm__ volatile("mrs %0, cpsr" : "=r"(r));
    return r;
}

/* OS irq-save/restore (svc 0x46 selector 0/1) — same as DumpRAM v6. */
static inline uint32_t irq_save(void)
{
    register uint32_t r12 __asm__("r12") = 0;
    register uint32_t r0 __asm__("r0");
    __asm__ volatile("svc #70" : "+r"(r12), "=r"(r0) :: "r1", "r2", "r3", "memory", "cc");
    return r0;
}
static inline void irq_restore(uint32_t m)
{
    register uint32_t r12 __asm__("r12") = 1;
    register uint32_t r0 __asm__("r0") = m;
    __asm__ volatile("svc #70" : "+r"(r12), "+r"(r0) :: "r1", "r2", "r3", "memory", "cc");
}

/* ------------------------------------------------------------- state --- */
enum { S_IDLE, S_RUN, S_RESULT, S_ENABLE, S_FAIL };
enum { V_NONE, V_ALIASED, V_NOSTORE, V_EXTRA, V_INCONCLUSIVE };
static int      g_state, g_step;
static uint32_t g_frame, g_mode, g_privileged;
static uint32_t g_dmc[7];                 /* +0,+4,+8,+c,+10,+100,+104 */
static uint32_t g_ttb_a, g_ttb_b0, g_ttb_tbl, g_ttb_b_now;
static uint32_t g_tlbi;                   /* 0 skipped(user), 1 done */
static uint32_t g_a0[NOFF], g_b0[NOFF], g_pat[NOFF], g_ra[NOFF], g_rb[NOFF];
static uint32_t g_verdict, g_verdict_en, g_enable_tried, g_cfg1_after;
static bool     g_mapped, g_cfg1_written;
static char     g_fail[24];
static char     g_rep[3072];

static const char *vtxt(uint32_t v)
{
    switch (v) {
    case V_ALIASED:      return "ALIASED (writes to 0x0c000000 land in 0x08000000)";
    case V_NOSTORE:      return "NO STORAGE at 0x0c000000 (writes do not stick)";
    case V_EXTRA:        return "INDEPENDENT MEMORY at 0x0c000000";
    case V_INCONCLUSIVE: return "INCONCLUSIVE";
    default:             return "not run";
    }
}

static void report(const char *stage, bool final)
{
    char *o = g_rep;
    o = put_s(o, "version=1\nstage="); o = put_s(o, stage); *o++ = '\n';
    o = put_kvx(o, "cpsr=", g_mode);
    o = put_kv(o, "privileged=", g_privileged);
    static const char *dn[7] = { "dmc_0x00=", "dmc_0x04=", "dmc_0x08_memconfig0=",
        "dmc_0x0c_memconfig1=", "dmc_0x10_directcmd=", "dmc_0x100=", "dmc_0x104=" };
    for (int i = 0; i < 7; i++) o = put_kvx(o, dn[i], g_dmc[i]);
    o = put_kvx(o, "ttb_l1_0x880_alias_a=", g_ttb_a);
    o = put_kvx(o, "ttb_l1_0x88b_table_page=", g_ttb_tbl);
    o = put_kvx(o, "ttb_l1_0x8c0_orig=", g_ttb_b0);
    o = put_kvx(o, "ttb_l1_0x8c0_now=", g_ttb_b_now);
    o = put_kv(o, "tlbi_done=", g_tlbi);
    for (uint32_t i = 0; i < NOFF; i++) {
        o = put_s(o, "off=0x"); o = put_hex(o, OFFS[i]);
        o = put_s(o, " a0=0x"); o = put_hex(o, g_a0[i]);
        o = put_s(o, " b0=0x"); o = put_hex(o, g_b0[i]);
        o = put_s(o, " pat=0x"); o = put_hex(o, g_pat[i]);
        o = put_s(o, " ra=0x"); o = put_hex(o, g_ra[i]);
        o = put_s(o, " rb=0x"); o = put_hex(o, g_rb[i]); *o++ = '\n';
    }
    o = put_s(o, "verdict="); o = put_s(o, vtxt(g_verdict)); *o++ = '\n';
    if (g_enable_tried) {
        o = put_s(o, "cs1_enable_tried=1\n");
        o = put_kvx(o, "memconfig1_written=", g_cfg1_after);
        o = put_s(o, "verdict_after_enable="); o = put_s(o, vtxt(g_verdict_en)); *o++ = '\n';
    }
    o = put_s(o, final ? "DONE\n" : "RUNNING\n");
    hb_fs_write(REPORT_PATH, g_rep, (uint32_t)(o - g_rep));
}

static bool ttb_unmap_and_verify(void)
{
    if (g_mapped) {
        wr32(TTB_UC_ALIAS + IDX_B * 4, g_ttb_b0);
        dsb(); isb();
        if (g_privileged) {
            __asm__ volatile("mcr p15, 0, %0, c8, c7, 0" :: "r"(0) : "memory"); /* TLBIALL */
            dsb(); isb();
            g_tlbi = 1;
        }
        g_mapped = false;
    }
    g_ttb_b_now = rd32(TTB_UC_ALIAS + IDX_B * 4);
    return g_ttb_b_now == g_ttb_b0;
}

static void fail(const char *why)
{
    char *o = put_s(g_fail, why); *o = 0;
    ttb_unmap_and_verify();
    report(why, true);
    g_state = S_FAIL;
}

/* One aliasing pass. Masks IRQs, restores everything it wrote. */
static uint32_t alias_pass(void)
{
    volatile uint32_t *A = (volatile uint32_t *)VA_A, *B = (volatile uint32_t *)VA_B;
    uint32_t m = irq_save();
    uint32_t saw_alias = 0, saw_ind = 0, saw_none = 0;
    for (uint32_t i = 0; i < NOFF; i++) {
        uint32_t w = OFFS[i] / 4;
        g_a0[i] = A[w];
        g_b0[i] = B[w];                       /* <- first touch of PA 0x0c000000+ */
        uint32_t p = 0x55aa55aau ^ (i * 0x01010101u);
        if (p == g_a0[i] || p == g_b0[i]) p = ~p;
        g_pat[i] = p;
    }
    for (uint32_t i = 0; i < NOFF; i++) { B[OFFS[i] / 4] = g_pat[i]; }
    dsb();
    for (uint32_t i = 0; i < NOFF; i++) {
        g_ra[i] = A[OFFS[i] / 4];
        g_rb[i] = B[OFFS[i] / 4];
    }
    for (int i = (int)NOFF - 1; i >= 0; i--) {   /* restore B first, then A */
        B[OFFS[i] / 4] = g_b0[i];
        A[OFFS[i] / 4] = g_a0[i];
    }
    dsb();
    irq_restore(m);
    for (uint32_t i = 0; i < NOFF; i++) {
        if (g_ra[i] == g_pat[i]) saw_alias++;
        else if (g_rb[i] == g_pat[i]) saw_ind++;
        else saw_none++;
    }
    if (saw_alias) return V_ALIASED;
    if (saw_ind == NOFF) return V_EXTRA;
    if (saw_none) return V_NOSTORE;
    return V_INCONCLUSIVE;
}

/* ------------------------------------------------------------- steps --- */
static void step_run(void)
{
    switch (g_step) {
    case 0:
        hb_fs_mkdir("/Apps/Data/probe");
        hb_fs_mkdir(OUT_DIR);
        g_mode = cpsr_read();
        g_privileged = (g_mode & 0x1fu) != 0x10u;
        g_ttb_a = rd32(TTB_BASE + IDX_A * 4);
        g_ttb_tbl = rd32(TTB_BASE + 0x88bu * 4);
        g_ttb_b0 = rd32(TTB_BASE + IDX_B * 4);
        /* the table must be where the plan says: A maps PA 0x08000000 and the
         * page holding the table is writable (AP=3) through the uncached alias */
        if ((g_ttb_a & 0xfff00003u) != 0x08000002u || ((g_ttb_a >> 10) & 3u) != 3u)
            { fail("TTBLAYOUT"); return; }
        if ((g_ttb_tbl & 3u) != 2u || ((g_ttb_tbl >> 10) & 3u) != 3u || (g_ttb_tbl & 0x8000u))
            { fail("TTBRO"); return; }
        if ((g_ttb_b0 & 3u) != 0u) { fail("ENTRYUSED"); return; }   /* not invalid: don't touch */
        static const uint32_t ro[7] = { 0x00, 0x04, 0x08, 0x0c, 0x10, 0x100, 0x104 };
        for (int i = 0; i < 7; i++) g_dmc[i] = rd32(DMC_BASE + ro[i]);
        report("preflight_done_next_map", false);
        break;
    case 1:
        report("mapping_L1_0x8c0", false);
        g_mapped = true;
        wr32(TTB_UC_ALIAS + IDX_B * 4, SECT_B);
        dsb(); isb();
        if (g_privileged) {
            __asm__ volatile("mcr p15, 0, %0, c8, c7, 0" :: "r"(0) : "memory");
            dsb(); isb();
            g_tlbi = 1;
        }
        g_ttb_b_now = rd32(TTB_UC_ALIAS + IDX_B * 4);
        if (g_ttb_b_now != SECT_B) { fail("MAPVERIFY"); return; }
        report("mapped_next_aliastest", false);   /* last breadcrumb before B is touched */
        break;
    case 2:
        g_verdict = alias_pass();
        report("aliastest_done", false);
        break;
    case 3:
        if (!ttb_unmap_and_verify()) { fail("RESTORE"); return; }
        report("done", true);
        g_state = S_RESULT;
        return;
    }
    g_step++;
}

/* Opt-in: try to bring rank 1 up. Everything touched is restored. */
static void do_enable(void)
{
    g_enable_tried = 1;
    uint32_t cfg0 = rd32(DMC_BASE + 0x08), cfg1 = rd32(DMC_BASE + 0x0c);
    if (cfg1 != 0 || cfg0 == 0 || cfg0 == 0xffffffffu) {
        g_cfg1_after = cfg1;               /* already configured / odd: leave alone */
        g_verdict_en = V_INCONCLUSIVE;
        report("enable_skipped", true);
        g_state = S_RESULT;
        return;
    }
    uint32_t new1 = (cfg0 & 0x00ffffffu) | 0x0c000000u;
    g_cfg1_after = new1;
    report("enable_cs1_writing", false);
    /* re-map for the second pass */
    g_mapped = true;
    wr32(TTB_UC_ALIAS + IDX_B * 4, SECT_B);
    dsb(); isb();
    if (g_privileged) {
        __asm__ volatile("mcr p15, 0, %0, c8, c7, 0" :: "r"(0) : "memory");
        dsb(); isb();
    }
    uint32_t m = irq_save();
    wr32(DMC_BASE + 0x0c, new1);
    g_cfg1_written = true;
    dsb();
    wr32(DMC_BASE + 0x10, 0x05100000u);    /* REFA, rank 1 (Exynos-style encoding) */
    dsb();
    irq_restore(m);
    report("enable_cs1_written_next_test", false);
    g_verdict_en = alias_pass();
    m = irq_save();
    wr32(DMC_BASE + 0x0c, 0);              /* put MEMCONFIG1 back */
    dsb();
    irq_restore(m);
    g_dmc[3] = rd32(DMC_BASE + 0x0c);
    ttb_unmap_and_verify();
    report("done_after_enable", true);
    g_state = S_RESULT;
}

/* ------------------------------------------------------------- view --- */
static void draw(void)
{
    char b[48], *o;
    hb_raw_fill(BG);
    text(8, 12, "RAMSCOPE", 3, ACC);
    text(8, 44, "DRAM > 64MB PROBE  READ-MOSTLY", 1, DIM);

    if (g_state == S_FAIL) {
        text(24, 100, "FAIL", 3, BAD);
        text(24, 140, g_fail, 2, BAD);
        text(24, 180, "STATE RESTORED", 2, FG);
        text(24, 210, "SEE REPORT.TXT", 2, FG);
        return;
    }
    if (g_state == S_IDLE) {
        hb_raw_fill_rect(BTN_X, BTN_Y, BTN_W, BTN_H, ACC);
        hb_raw_rect_outline(BTN_X, BTN_Y, BTN_W, BTN_H, 2, FG);
        text(BTN_X + 20, BTN_Y + 28, "SCAN RAM", 3, BG);
        text(12, 230, "TAP TO SCAN", 2, DIM);
        text(12, 256, "CRASH = NO RAM AT 0X0C", 1, DIM);
        return;
    }
    if (g_state == S_RUN || g_state == S_ENABLE) {
        uint32_t dots = (g_frame / 15u) % 4u;
        o = put_s(b, "BUSY");
        for (uint32_t i = 0; i < dots; i++) *o++ = '.';
        *o = 0;
        text(8, 70, b, 3, FG);
        o = put_s(b, g_state == S_ENABLE ? "CS1 ENABLE TRY" : "STEP ");
        if (g_state == S_RUN) { o = put_dec(o, (uint32_t)g_step + 1); o = put_s(o, "/4"); }
        *o = 0;
        text(8, 110, b, 2, FG);
        hb_raw_rect_outline(20, 150, 200, 20, 2, FG);
        hb_raw_fill_rect(22, 152, g_state == S_ENABLE ? 98 : 49 * g_step, 16, ACC);
        text(8, 190, g_state == S_ENABLE ? "RISKY - KEEP STILL" : "DO NOT TOUCH", 2, DIM);
        return;
    }
    /* S_RESULT */
    uint32_t v = g_enable_tried ? g_verdict_en : g_verdict;
    bool extra = g_verdict == V_EXTRA || g_verdict_en == V_EXTRA;
    if (extra) {
        text(8, 70, "EXTRA RAM FOUND!", 2, OKC);
    } else {
        text(8, 70, "RAM: 64MB ONLY", 2, g_verdict == V_INCONCLUSIVE ? BAD : FG);
    }
    text(8, 100, v == V_ALIASED ? "ALIASED" : v == V_NOSTORE ? "NO STORAGE" :
                 v == V_EXTRA ? "INDEPENDENT" : "INCONCLUSIVE", 2, DIM);
    o = put_s(b, "CS1 CFG 0X"); o = put_hex(o, g_dmc[3]); *o = 0;
    text(8, 130, b, 1, DIM);
    o = put_s(b, "CS0 CFG 0X"); o = put_hex(o, g_dmc[2]); *o = 0;
    text(8, 140, b, 1, DIM);
    text(8, 160, g_privileged ? "MODE PRIVILEGED" : "MODE USER", 1, DIM);
    text(8, 176, "OK - REBOOT ME", 2, OKC);
    if (!g_enable_tried && !extra && g_dmc[3] == 0) {
        hb_raw_rect_outline(EN_X, EN_Y, EN_W, EN_H, 2, BAD);
        text(EN_X + 10, EN_Y + 10, "TRY ENABLE CS1", 2, BAD);
        text(EN_X + 10, EN_Y + 32, "RISKY - OPTIONAL", 1, DIM);
    }
}

static int g_prev_down;

void hb_raw_init(int w, int h)
{
    (void)w; (void)h;
    g_state = S_IDLE;
    g_step = 0;
    g_prev_down = 0;
    g_frame = 0;
    g_fail[0] = 0;
    draw();
}

void hb_raw_frame(const hb_spoint_t *t)
{
    int down = t->down ? 1 : 0;
    int tapped = down && !g_prev_down;
    g_prev_down = down;
    g_frame++;

    if (g_state == S_IDLE) {
        if (tapped && t->x >= BTN_X && t->x < BTN_X + BTN_W &&
            t->y >= BTN_Y && t->y < BTN_Y + BTN_H) {
            g_state = S_RUN;           /* paint BUSY now, work from the next frame */
            g_step = 0;
            draw();
        }
        return;
    }
    if (g_state == S_RUN) {
        static int painted;
        if (!painted) { painted = 1; draw(); return; }  /* BUSY visible before step 1 */
        step_run();
        draw();
        return;
    }
    if (g_state == S_ENABLE) {
        do_enable();
        draw();
        return;
    }
    if (g_state == S_RESULT && tapped && !g_enable_tried && g_verdict != V_EXTRA &&
        g_dmc[3] == 0 && t->x >= EN_X && t->x < EN_X + EN_W &&
        t->y >= EN_Y && t->y < EN_Y + EN_H) {
        g_state = S_ENABLE;
        draw();                         /* BUSY first; the work runs next frame */
    }
}
