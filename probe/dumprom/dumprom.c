/*
 * DumpROM — visible version of the headless VROM probe.
 *
 * Raw-surface homebrew app (HBAppKind 1, no LVGL): one big "DUMP ROM" button.
 * On tap it runs the staged dump from the headless probe, one stage per frame
 * (each frame stays short; progress is visible):
 *   1. /Apps/Data/probe/report.txt — CPSR + regs 0x3C500000, 0x3C500048,
 *      0x38900000, 0x22000000 (gate untouched)
 *   2. write 0 to 0x3C500048 (ungate VROM clock), settle, re-read ->
 *      /Apps/Data/probe/gate.txt (before/after)
 *   3. /Apps/Data/probe/vrom0.bin .. vrom3.bin — 256 KB from 0x20000000
 *      as 4 x 64 KB files, each closed before the next chunk is read
 *   4. report.txt rewritten with ALL_DONE appended
 * Then the app just sits showing "OK - REBOOT ME" (or "FAIL <stage>") — no
 * spin; the user reboots manually.
 *
 * Text is rendered straight into the surface framebuffer with a 3x5 font
 * (same approach as apps/memprobe — hb_draw_str draws via MIPI and would be
 * overwritten by the composited surface).
 */
#include "hb_raw_surface.h"
#include "hb_sdk.h"

#define VROM_BASE       0x20000000u
#define VROM_BYTES      (256u * 1024u)
#define CHUNK_BYTES     (64u * 1024u)

#define REG_CLKGATE     0x3C500048u     /* write 0: ungate VROM clock */
#define REG_PROBE_0     0x3C500000u
#define REG_DISP        0x38900000u     /* display regs (known readable) */
#define IRAM_BASE       0x22000000u

#define OUT_DIR         "/Apps/Data/probe"
#define REPORT_PATH     "/Apps/Data/probe/report.txt"
#define GATE_PATH       "/Apps/Data/probe/gate.txt"
#define VROM_PATH_TMPL  "/Apps/Data/probe/vrom0.bin"

#define BG   0x0E1118
#define FG   0xE8ECF4
#define DIM  0x7C869A
#define OKC  0x2FBF71
#define BAD  0xE63946
#define ACC  0x4AD9C8
#define BTN  0x27423B

/* button geometry */
#define BTN_X 20
#define BTN_Y 150
#define BTN_W 200
#define BTN_H 90

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
static inline uint32_t rd32(uint32_t addr) { return *(volatile uint32_t *)addr; }

static inline uint32_t read_cpsr(void)
{
    uint32_t v;
    __asm__ volatile("mrs %0, cpsr" : "=r"(v));
    return v;
}

static char *put_s(char *o, const char *s) { while (*s) *o++ = *s++; return o; }

static char *put_hex(char *o, uint32_t v)
{
    for (int i = 0; i < 8; i++) {
        uint32_t d = (v >> (28 - 4 * i)) & 0xFu;
        *o++ = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    }
    return o;
}

/* ------------------------------------------------------------- dump state --- */
static uint8_t g_chunk[CHUNK_BYTES];
static char    g_rep[512];
static char   *g_rep_end;

enum { S_IDLE, S_REPORT, S_GATE, S_V0, S_V1, S_V2, S_V3, S_FINAL, S_OK, S_FAIL };
static int  g_state;
static char g_status[24];           /* stage line shown under the button */
static char g_fail_stage[12];       /* which stage failed, for the FAIL screen */

static void set_status(const char *s)
{
    int i = 0;
    for (; s[i] && i < (int)sizeof g_status - 1; i++) g_status[i] = s[i];
    g_status[i] = 0;
}

/* One dump step for the current RUN state. Returns true on success. */
static bool run_step(void)
{
    switch (g_state) {
    case S_REPORT: {
        uint32_t cpsr   = read_cpsr();
        uint32_t r_clk0 = rd32(REG_PROBE_0);
        uint32_t r_gate = rd32(REG_CLKGATE);
        uint32_t r_disp = rd32(REG_DISP);
        uint32_t r_iram = rd32(IRAM_BASE);
        char *o = g_rep;
        o = put_s(o, "cpsr=0x");         o = put_hex(o, cpsr);   *o++ = '\n';
        o = put_s(o, "reg_3c500000=0x"); o = put_hex(o, r_clk0); *o++ = '\n';
        o = put_s(o, "reg_3c500048=0x"); o = put_hex(o, r_gate); *o++ = '\n';
        o = put_s(o, "reg_38900000=0x"); o = put_hex(o, r_disp); *o++ = '\n';
        o = put_s(o, "reg_22000000=0x"); o = put_hex(o, r_iram); *o++ = '\n';
        g_rep_end = o;
        hb_fs_mkdir(OUT_DIR);
        bool ok = hb_fs_write(REPORT_PATH, g_rep, (uint32_t)(g_rep_end - g_rep));
        hb_trace_log("DRREPORT", ok ? 1 : 0, cpsr);
        if (!ok) { put_s(g_fail_stage, "REPORT"); return false; }
        set_status("REPORT OK");
        return true;
    }
    case S_GATE: {
        uint32_t before = rd32(REG_CLKGATE);
        *(volatile uint32_t *)REG_CLKGATE = 0;
        __asm__ volatile("dsb sy" : : : "memory");
        uint32_t t0 = hb_time_uptime_us();
        while (hb_time_uptime_us() - t0 < 1000u) { }   /* ~1 ms settle */
        uint32_t after = rd32(REG_CLKGATE);
        static char gate[96];
        char *g = gate;
        g = put_s(g, "gate_before=0x"); g = put_hex(g, before); *g++ = '\n';
        g = put_s(g, "gate_after=0x");  g = put_hex(g, after);  *g++ = '\n';
        bool ok = hb_fs_write(GATE_PATH, gate, (uint32_t)(g - gate));
        hb_trace_log("DRGATE  ", ok ? 1 : 0, after);
        if (!ok) { put_s(g_fail_stage, "GATE"); return false; }
        set_status("GATE OK");
        return true;
    }
    case S_V0: case S_V1: case S_V2: case S_V3: {
        uint32_t i = (uint32_t)(g_state - S_V0);
        hb_trace_log("DRCHUNK ", i, VROM_BASE + i * CHUNK_BYTES);
        const uint8_t *src = (const uint8_t *)(VROM_BASE + i * CHUNK_BYTES);
        for (uint32_t j = 0; j < CHUNK_BYTES; j++) g_chunk[j] = src[j];
        char path[] = VROM_PATH_TMPL;
        path[sizeof path - 6] = (char)('0' + i);    /* vromN.bin */
        bool ok = hb_fs_write(path, g_chunk, CHUNK_BYTES);
        hb_trace_log("DRWR   ", i, ok ? 1 : 0);
        if (!ok) {
            char *f = put_s(g_fail_stage, "VROM");
            *f = (char)('0' + i); f[1] = 0;
            return false;
        }
        char st[12];
        char *p = put_s(st, "VROM");
        *p++ = (char)('0' + i);
        p = put_s(p, "/3 OK");
        *p = 0;
        set_status(st);
        return true;
    }
    case S_FINAL: {
        g_rep_end = put_s(g_rep_end, "ALL_DONE\n");
        bool ok = hb_fs_write(REPORT_PATH, g_rep, (uint32_t)(g_rep_end - g_rep));
        hb_trace_log("DRDONE  ", ok ? 1 : 0, 0);
        if (!ok) { put_s(g_fail_stage, "FINAL"); return false; }
        return true;
    }
    default:
        return true;
    }
}

/* ------------------------------------------------------------------- view --- */
static void draw(void)
{
    hb_raw_fill(BG);
    text(8, 12, "DUMPROM", 3, ACC);
    text(8, 44, "256K BOOT ROM AT", 1, DIM);
    text(8, 52, "0X20000000 -> /APPS/DATA/PROBE", 1, DIM);

    if (g_state == S_OK) {
        text(24, 170, "OK - REBOOT ME", 2, OKC);
        text(24, 200, "FILES SAVED", 2, FG);
        return;
    }
    if (g_state == S_FAIL) {
        text(24, 170, "FAIL", 3, BAD);
        text(24, 200, g_fail_stage, 2, BAD);
        return;
    }

    bool running = g_state != S_IDLE;
    hb_raw_fill_rect(BTN_X, BTN_Y, BTN_W, BTN_H, running ? BTN : ACC);
    hb_raw_rect_outline(BTN_X, BTN_Y, BTN_W, BTN_H, 2, FG);
    /* "DUMP ROM" is 8 chars -> 8*4*3=96 px at scale 3 */
    text(BTN_X + (BTN_W - 96) / 2, BTN_Y + (BTN_H - 15) / 2, "DUMP ROM", 3,
         running ? DIM : BG);

    text(12, 280, running ? "WORKING" : "TAP TO DUMP", 2, running ? FG : DIM);
    if (g_status[0]) text(12, 304, g_status, 2, OKC);
}

/* ------------------------------------------------------------------ entry --- */
static int g_prev_down;

void hb_raw_init(int w, int h)
{
    (void)w; (void)h;
    g_state = S_IDLE;
    g_status[0] = 0;
    g_prev_down = 0;
    draw();
}

void hb_raw_frame(const hb_spoint_t *t)
{
    int down = t->down ? 1 : 0;
    int tapped = down && !g_prev_down;
    g_prev_down = down;

    if (g_state == S_IDLE) {
        if (tapped &&
            t->x >= BTN_X && t->x < BTN_X + BTN_W &&
            t->y >= BTN_Y && t->y < BTN_Y + BTN_H) {
            g_state = S_REPORT;
            set_status("");
            draw();
        }
        return;
    }

    if (g_state >= S_REPORT && g_state <= S_FINAL) {
        if (!run_step()) {
            g_state = S_FAIL;
        } else if (g_state == S_FINAL) {
            g_state = S_OK;
        } else {
            g_state++;
        }
        draw();
        return;
    }

    /* S_OK / S_FAIL: sit there; the user reboots manually. */
}
