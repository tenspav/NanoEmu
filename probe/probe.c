/*
 * probe, one-shot hardware probe for the iPod nano 7G, installed as
 * /Apps/Startup/ab.hbapp so the jailbreak runs it at Home screen render.
 *
 * STAGED writes so partial progress survives a fault partway through:
 *   1. /Apps/Data/probe/report.txt, CPSR + regs 0x3C500000, 0x3C500048,
 *      0x38900000, 0x22000000, written BEFORE touching the clock gate.
 *   2. write 0 to 0x3C500048 (ungate VROM clock), settle, re-read, then
 *      /Apps/Data/probe/gate.txt with before/after values.
 *   3. dump 256 KB from 0x20000000 as 4 x 64 KB files
 *      /Apps/Data/probe/vrom0.bin .. vrom3.bin, each fully written and
 *      closed before the next chunk is read, a fault in chunk N leaves
 *      chunks 0..N-1 on disk.
 *   4. rewrite report.txt with an ALL_DONE line appended.
 *   5. spin forever, the watchdog reboots the iPod back to disk mode.
 *
 * hb_fs_write is create/truncate + write + sync + close in one call (no
 * append mode in the SDK), so "append ALL_DONE" = rewrite the whole report
 * from the static buffer it was built in.
 *
 * RELOC app (HRL1 blob via tools/mkrelocapp.py), so do NOT use HB_APP_ENTRY:
 * its fixed-VA linker symbols (__bss_start__ etc. from hb_app.ld) don't exist
 * in a base-zero relocatable image. The loader zeroes .bss and publishes the
 * span via SVC 70 before jumping to payload_entry (same path as
 * system/resident_selector).
 */
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

static uint8_t g_chunk[CHUNK_BYTES];
static char    g_rep[512];
static char   *g_rep_end;

static inline uint32_t rd32(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

static inline uint32_t read_cpsr(void)
{
    uint32_t v;
    __asm__ volatile("mrs %0, cpsr" : "=r"(v));
    return v;
}

/* ---- tiny report formatter (no libc) ---- */

static char *put_s(char *o, const char *s)
{
    while (*s) *o++ = *s++;
    return o;
}

static char *put_hex(char *o, uint32_t v)
{
    for (int i = 0; i < 8; i++) {
        uint32_t d = (v >> (28 - 4 * i)) & 0xFu;
        *o++ = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    }
    return o;
}

__attribute__((section(".text.entry"), used, noinline))
void payload_entry(void)
{
    hb_trace_init();
    hb_trace_log("PROBE   ", 0, 0);

    /* CPSR first, before anything can change it. */
    uint32_t cpsr = read_cpsr();

    /* ---- stage 1: report.txt with CPSR + registers, gate untouched ---- */
    uint32_t r_clk0  = rd32(REG_PROBE_0);
    uint32_t r_gate0 = rd32(REG_CLKGATE);
    uint32_t r_disp  = rd32(REG_DISP);
    uint32_t r_iram  = rd32(IRAM_BASE);

    char *o = g_rep;
    o = put_s(o, "cpsr=0x");         o = put_hex(o, cpsr);   *o++ = '\n';
    o = put_s(o, "reg_3c500000=0x"); o = put_hex(o, r_clk0); *o++ = '\n';
    o = put_s(o, "reg_3c500048=0x"); o = put_hex(o, r_gate0); *o++ = '\n';
    o = put_s(o, "reg_38900000=0x"); o = put_hex(o, r_disp); *o++ = '\n';
    o = put_s(o, "reg_22000000=0x"); o = put_hex(o, r_iram); *o++ = '\n';
    g_rep_end = o;

    hb_fs_mkdir(OUT_DIR);
    bool rep_ok = hb_fs_write(REPORT_PATH, g_rep, (uint32_t)(g_rep_end - g_rep));
    hb_trace_log("PROBEREP", rep_ok ? 1 : 0, (uint32_t)(g_rep_end - g_rep));

    /* ---- stage 2: ungate the VROM clock, record before/after in gate.txt ---- */
    *(volatile uint32_t *)REG_CLKGATE = 0;
    __asm__ volatile("dsb sy" : : : "memory");
    uint32_t t0 = hb_time_uptime_us();
    while (hb_time_uptime_us() - t0 < 1000u) { }  /* ~1 ms settle */
    uint32_t r_gate1 = rd32(REG_CLKGATE);

    static char gate[96];
    char *g = gate;
    g = put_s(g, "gate_before=0x"); g = put_hex(g, r_gate0); *g++ = '\n';
    g = put_s(g, "gate_after=0x");  g = put_hex(g, r_gate1); *g++ = '\n';
    bool gate_ok = hb_fs_write(GATE_PATH, gate, (uint32_t)(g - gate));
    hb_trace_log("PROBEGAT", gate_ok ? 1 : 0, r_gate1);

    /* ---- stage 3: VROM dump, one 64 KB file per chunk ---- */
    for (uint32_t i = 0; i < VROM_BYTES / CHUNK_BYTES; i++) {
        hb_trace_log("PROBECHK", i, VROM_BASE + i * CHUNK_BYTES);
        const uint8_t *src = (const uint8_t *)(VROM_BASE + i * CHUNK_BYTES);
        for (uint32_t j = 0; j < CHUNK_BYTES; j++) g_chunk[j] = src[j];

        char path[] = VROM_PATH_TMPL;
        path[sizeof path - 6] = (char)('0' + i);    /* vromN.bin */
        bool ok = hb_fs_write(path, g_chunk, CHUNK_BYTES);
        hb_trace_log("PROBEWR ", i, ok ? 1 : 0);
    }

    /* ---- stage 4: report the full run survived ---- */
    g_rep_end = put_s(g_rep_end, "ALL_DONE\n");
    rep_ok = hb_fs_write(REPORT_PATH, g_rep, (uint32_t)(g_rep_end - g_rep));
    hb_trace_log("PROBEDON", rep_ok ? 1 : 0, cpsr);

    /* ---- stage 5: spin forever; the watchdog reboots to disk mode ---- */
    for (;;) { __asm__ volatile("" : : : "memory"); }
}
