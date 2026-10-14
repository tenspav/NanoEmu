/*
 * DumpBoot — extract the reserved NAND region (logical 4 KB blocks 0..0xc03f:
 * boot chain + diagnostics partition + the rest of the "Part 2" firmware area)
 * to files on the data partition.
 *
 * READ-ONLY on the reserved region: the only flash writes are the output
 * files themselves (the data partition's FAT, via the normal SDK file API),
 * never an LBA below 0xc040. See docs/research/dumpboot-design.md.
 *
 * Raw-surface homebrew app (HBAppKind 1, no LVGL). UNLIKE DumpRAM this is a
 * long operation (192 MB), so it is a per-frame state machine: every frame
 * does at most one 1 MB chunk, then returns so the resident can blit the live
 * progress (BUSY / percent / MB / errors / elapsed) to the screen.
 *
 * Read path: the OS's own logical block reader (Thumb 0x081d85bb), called as
 *     rc = reader(obj, lba4k, buf, count, 0)          rc == 0 on success
 * with dev = *(u32*)(0x08971660+4) and obj = **(u32**)(dev+4). It supplies
 * corrected data (kernel ECC + cache maintenance), unshifted logical LBAs
 * when [backend+0x20] == 0 (gated below), and takes the OS storage lock.
 *
 * Output, all under /Apps/Data/probe/ :
 *   reserved.bin   49216 x 4096 B at fixed offsets (lba * 4096); unreadable
 *                  sectors are zero-filled (see status.bin)
 *   status.bin     49216 bytes, one per LBA: 0 unread, 1 ok, 2 ok after
 *                  individual retry, 3 read error, 4 reader said ok but left
 *                  the sentinel untouched (treated as a failure)
 *   manifest.json  identifiers, geometry, ranges, reader pointers, results
 *   report.txt     bytes, ms, error counts, first-failure detail
 * report.txt/manifest.json are written first as "running" breadcrumbs, then
 * rewritten at the end, so a crash still leaves a trail.
 */
#include "hb_raw_surface.h"
#include "hb_sdk.h"
#include "hb_heap.h"

#define OUT_DIR       "/Apps/Data/probe"
#define RESERVED_PATH "/Apps/Data/probe/reserved.bin"
#define STATUS_PATH   "/Apps/Data/probe/status.bin"
#define MANIFEST_PATH "/Apps/Data/probe/manifest.json"
#define REPORT_PATH   "/Apps/Data/probe/report.txt"

#define SECTOR_BYTES  4096u
#define SECTOR_WORDS  (SECTOR_BYTES / 4u)
#define LBA_COUNT     0xc040u                    /* 0..0xc03f = 49216 sectors */
#define TOTAL_BYTES   (LBA_COUNT * SECTOR_BYTES) /* 201,588,736               */
#define TOTAL_MB      (TOTAL_BYTES / (1024u * 1024u))   /* 192 (+0.25)        */
#define GROUP_SECT    64u                        /* sectors per reader call   */
#define CHUNK_BIG     (1024u * 1024u)
#define HEAP_HEADROOM (2u * 1024u * 1024u)       /* leave the OS this much    */
#define SENT          0xa5a5a5a5u                /* buffer sentinel           */
#define RETRIES       3u                         /* individual retries/sector */
#define FAST_FAIL     64u                        /* consecutive fails -> fast */

/* OS logical block reader + the pointer chain to its object */
typedef int (*os_reader_fn)(uint32_t obj, uint32_t lba, void *buf,
                            uint32_t count, uint32_t flags);
#define OS_READER     ((os_reader_fn)(0x081d85bbu))   /* Thumb (bit 0 set)   */
#define DEV_GLOBAL    0x08971660u
#define GATE_LBA_A    0xc040u    /* first data-volume sector (disk-image MBR) */
#define GATE_LBA_B    0xc07fu    /* FAT32 boot sector                         */
#define DRAM_LO       0x08000000u
#define DRAM_HI       0x0c000000u
#define WMR_SIG_ADDR  0x08d09340u                /* Whimory signature (docs)  */
#define WMR_MAGIC     0x776d7278u                /* 'wmrx'                    */
/* SDK hb_fs.c fixed scratch: never let our buffer sit on it */
#define SDK_SCRATCH_LO 0x09118000u
#define SDK_SCRATCH_HI 0x0911b210u

#define BG   0x0E1118
#define FG   0xE8ECF4
#define DIM  0x7C869A
#define OKC  0x2FBF71
#define BAD  0xE63946
#define ACC  0xF0A030
#define BTN  0x27423B

#define BTN_X 20
#define BTN_Y 150
#define BTN_W 200
#define BTN_H 90
#define CAN_X 20
#define CAN_Y 268
#define CAN_W 200
#define CAN_H 40

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

static char *put_hex2(char *o, uint32_t v)
{
    for (int i = 0; i < 2; i++) {
        uint32_t d = (v >> (4 - 4 * i)) & 0xFu;
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

/* ------------------------------------------------------------ dump state --- */
enum { S_IDLE, S_BUSY0, S_PREP, S_READ, S_DONE, S_OK, S_FAIL };
static int      g_state;
static char     g_fail_stage[16];
static bool     g_cancel;

static uint8_t  g_status[LBA_COUNT];        /* per-LBA validity (status.bin)  */
static uint8_t *g_raw;                      /* heap block (for free)          */
static uint8_t *g_buf;                      /* 32-byte aligned chunk buffer   */
static uint32_t g_chunk_bytes;
static uint32_t g_next_lba;                 /* next LBA to read               */
static uint32_t g_obj, g_dev, g_backend, g_backend_off;
static uint32_t g_gate_rc_a, g_gate_rc_b, g_gate_sig_a, g_gate_sig_b;
static uint8_t  g_gate_head[16];            /* first 16 B of LBA 0xc040       */

static uint32_t g_ok, g_retried_ok, g_failed, g_untouched;
static uint32_t g_calls, g_bulk_fail, g_indiv_reads, g_consec, g_chunk_err;
static uint32_t g_ff_lba = 0xffffffffu, g_ff_rc, g_ff_kind;   /* first failure */
static uint32_t g_ff_bulk_lba = 0xffffffffu, g_ff_bulk_rc;
static uint32_t g_hash = 0x811c9dc5u;       /* FNV-1a over 32-bit words       */
static uint32_t g_t_start, g_t_read0, g_t_end, g_bytes_written;
static bool     g_wr_ok, g_stream_open, g_cancelled;
static uint32_t g_wmr_magic, g_wmr_sig[16];

#define MAX_RUNS 24
static uint32_t g_run_lba[MAX_RUNS], g_run_len[MAX_RUNS];
static uint32_t g_nruns, g_runs_dropped;

static char g_rep[3072];
static char g_man[4096];

/* ----------------------------------------------------------- reader glue --- */
static bool dram_ptr(uint32_t p)
{
    return p >= DRAM_LO && p < DRAM_HI && (p & 3u) == 0;
}

static bool resolve_reader(void)
{
    uint32_t dev = *(volatile uint32_t *)(DEV_GLOBAL + 4);
    if (!dram_ptr(dev)) return false;
    g_dev = dev;
    uint32_t p = *(volatile uint32_t *)(dev + 4);
    if (!dram_ptr(p)) return false;
    uint32_t obj = *(volatile uint32_t *)p;
    if (!dram_ptr(obj)) return false;
    g_obj = obj;
    uint32_t be = *(volatile uint32_t *)(obj + 4);
    if (!dram_ptr(be)) return false;
    g_backend = be;
    g_backend_off = *(volatile uint32_t *)(be + 0x20);
    return true;
}

static void sent_fill(uint32_t *p, uint32_t words)
{
    for (uint32_t i = 0; i < words; i++) p[i] = SENT;
}

/* true if the whole sector is still the sentinel (reader wrote nothing) */
static bool sector_untouched(const uint32_t *p)
{
    for (uint32_t i = 0; i < SECTOR_WORDS; i++)
        if (p[i] != SENT) return false;
    return true;
}

static int os_read(uint32_t lba, uint32_t n, uint8_t *buf)
{
    sent_fill((uint32_t *)buf, n * SECTOR_WORDS);
    g_calls++;
    return OS_READER(g_obj, lba, buf, n, 0);
}

static void note_run(uint32_t lba)
{
    if (g_nruns && g_run_lba[g_nruns - 1] + g_run_len[g_nruns - 1] == lba) {
        g_run_len[g_nruns - 1]++;
    } else if (g_nruns < MAX_RUNS) {
        g_run_lba[g_nruns] = lba;
        g_run_len[g_nruns++] = 1;
    } else {
        g_runs_dropped++;
    }
}

static void sector_fail(uint32_t lba, int rc, uint8_t code, uint8_t *dst)
{
    for (uint32_t i = 0; i < SECTOR_BYTES; i++) dst[i] = 0;
    g_status[lba] = code;
    g_failed++;
    g_chunk_err++;
    g_consec++;
    if (code == 4) g_untouched++;
    if (g_ff_lba == 0xffffffffu) { g_ff_lba = lba; g_ff_rc = (uint32_t)rc; g_ff_kind = code; }
    note_run(lba);
}

static void sector_ok(uint32_t lba, uint8_t code)
{
    g_status[lba] = code;
    if (code == 2) g_retried_ok++; else g_ok++;
    g_consec = 0;
}

/* Read `n` (<= GROUP_SECT) sectors at `lba` into `dst`; never aborts: every
 * sector ends up with a status byte, failures are zero-filled. */
static void read_group(uint32_t lba, uint32_t n, uint8_t *dst)
{
    int rc = os_read(lba, n, dst);
    if (rc == 0) {
        for (uint32_t i = 0; i < n; i++) {
            if (sector_untouched((const uint32_t *)(dst + i * SECTOR_BYTES)))
                sector_fail(lba + i, 0, 4, dst + i * SECTOR_BYTES);
            else
                sector_ok(lba + i, 1);
        }
        return;
    }
    g_bulk_fail++;
    if (g_ff_bulk_lba == 0xffffffffu) { g_ff_bulk_lba = lba; g_ff_bulk_rc = (uint32_t)rc; }

    bool skip = false;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *sb = dst + i * SECTOR_BYTES;
        if (skip) { sector_fail(lba + i, rc, 3, sb); continue; }
        uint32_t tries = g_consec >= FAST_FAIL ? 1u : RETRIES;
        bool ok = false;
        int r = rc;
        for (uint32_t t = 0; t < tries && !ok; t++) {
            g_indiv_reads++;
            r = os_read(lba + i, 1, sb);
            ok = r == 0 && !sector_untouched((const uint32_t *)sb);
        }
        if (ok) { sector_ok(lba + i, 2); continue; }
        sector_fail(lba + i, r, 3, sb);
        /* long dead stretch: one probe per group instead of 64 x retries */
        if (g_consec >= FAST_FAIL && i == 0) skip = true;
    }
}

static uint32_t fnv_words(uint32_t h, const uint32_t *p, uint32_t words)
{
    for (uint32_t i = 0; i < words; i++) h = (h ^ p[i]) * 0x01000193u;
    return h;
}

/* ------------------------------------------------------- report/manifest --- */
static uint32_t elapsed_ms(void)
{
    uint32_t end = g_t_end ? g_t_end : hb_time_uptime_ms();
    return end - g_t_start;
}

static void write_report(const char *stage, bool final)
{
    char *o = g_rep;
    o = put_s(o, "version=1\n");
    o = put_s(o, "stage="); o = put_s(o, stage); *o++ = '\n';
    o = put_kv(o, "lba_first=", 0);
    o = put_kv(o, "lba_count=", LBA_COUNT);
    o = put_kv(o, "sector_bytes=", SECTOR_BYTES);
    o = put_kv(o, "next_lba=", g_next_lba);
    o = put_kv(o, "reserved_bytes=", g_bytes_written);
    o = put_kv(o, "ms_total=", elapsed_ms());
    o = put_kv(o, "ms_read_loop=", g_t_end ? g_t_end - g_t_read0 : 0u);
    o = put_kv(o, "chunk_bytes=", g_chunk_bytes);
    o = put_kv(o, "sectors_ok=", g_ok);
    o = put_kv(o, "sectors_ok_after_retry=", g_retried_ok);
    o = put_kv(o, "sectors_failed=", g_failed);
    o = put_kv(o, "sectors_sentinel_untouched=", g_untouched);
    o = put_kv(o, "reader_calls=", g_calls);
    o = put_kv(o, "bulk_failures=", g_bulk_fail);
    o = put_kv(o, "individual_reads=", g_indiv_reads);
    o = put_kvx(o, "reader_obj=", g_obj);
    o = put_kvx(o, "reader_dev=", g_dev);
    o = put_kvx(o, "reader_backend=", g_backend);
    o = put_kvx(o, "backend_off20=", g_backend_off);
    o = put_kv(o, "gate_0xc040_rc=", g_gate_rc_a);
    o = put_kv(o, "gate_0xc040_sig55aa=", g_gate_sig_a);
    o = put_kv(o, "gate_0xc07f_rc=", g_gate_rc_b);
    o = put_kv(o, "gate_0xc07f_sig55aa=", g_gate_sig_b);
    if (g_ff_lba == 0xffffffffu) {
        o = put_s(o, "first_failure=none\n");
    } else {
        o = put_kv(o, "first_failure_lba=", g_ff_lba);
        o = put_kvx(o, "first_failure_rc=", g_ff_rc);
        o = put_kv(o, "first_failure_kind=", g_ff_kind);   /* 3 err, 4 untouched */
        if (g_ff_bulk_lba != 0xffffffffu) {
            o = put_kv(o, "first_bulk_failure_lba=", g_ff_bulk_lba);
            o = put_kvx(o, "first_bulk_failure_rc=", g_ff_bulk_rc);
        }
        for (uint32_t i = 0; i < g_nruns; i++) {
            o = put_s(o, "fail_run="); o = put_dec(o, g_run_lba[i]);
            *o++ = '+'; o = put_dec(o, g_run_len[i]); *o++ = '\n';
        }
        if (g_runs_dropped) o = put_kv(o, "fail_runs_not_listed=", g_runs_dropped);
    }
    o = put_kvx(o, "checksum_fnv1a32_words=", g_hash);
    o = put_s(o, "errors=");
    if (!g_wr_ok) o = put_s(o, "write ");
    if (g_failed) o = put_s(o, "sector-read ");
    if (g_cancelled) o = put_s(o, "cancelled ");
    if (g_wr_ok && !g_failed && !g_cancelled) o = put_s(o, "none");
    *o++ = '\n';
    o = put_s(o, final ? ((g_wr_ok && !g_cancelled && g_next_lba >= LBA_COUNT)
                          ? "ALL_DONE\n" : "INCOMPLETE\n")
                       : "RUNNING\n");
    hb_fs_write(REPORT_PATH, g_rep, (uint32_t)(o - g_rep));
}

static void write_manifest(const char *status)
{
    char *o = g_man;
    o = put_s(o, "{\n  \"tool\": \"DumpBoot\",\n  \"format\": 1,\n");
    o = put_s(o, "  \"status\": \""); o = put_s(o, status); o = put_s(o, "\",\n");
    o = put_s(o, "  \"read_only_on_reserved\": true,\n");
    o = put_s(o, "  \"firmware_id\": {\n");
    o = put_s(o, "    \"whimory_sig_addr\": \"0x08d09340\",\n");
    o = put_s(o, "    \"whimory_magic_ok\": "); o = put_s(o, g_wmr_magic == WMR_MAGIC ? "true" : "false");
    o = put_s(o, ",\n    \"whimory_sig_hex\": \"");
    for (int i = 0; i < 64; i++) o = put_hex2(o, ((const uint8_t *)g_wmr_sig)[i]);
    o = put_s(o, "\",\n    \"note\": \"signature bytes read from live DRAM; serial/model "
                 "identifiers not located yet\"\n  },\n");
    o = put_s(o, "  \"geometry\": {\n");
    o = put_s(o, "    \"sector_bytes\": 4096,\n    \"first_lba\": 0,\n");
    o = put_s(o, "    \"lba_count\": "); o = put_dec(o, LBA_COUNT);
    o = put_s(o, ",\n    \"total_bytes\": "); o = put_dec(o, TOTAL_BYTES);
    o = put_s(o, ",\n    \"nand_page_bytes\": 16384,\n    \"nand_pages_per_block\": 128,\n"
                 "    \"nand_ce\": 2,\n"
                 "    \"nand_source\": \"docs/research/diag-partition-map.md "
                 "(PPN params, not re-read by this app)\"\n  },\n");
    o = put_s(o, "  \"ranges\": [\n"
                 "    {\"name\": \"firmware_part2\", \"lba_first\": 64, \"lba_last\": 49215, "
                 "\"note\": \"Part 2; LBAs 0..63 precede it\"},\n"
                 "    {\"name\": \"mse_partition_table\", \"lba_first\": 69, \"lba_last\": 69, "
                 "\"note\": \"part2 offset 0x5000 (docs)\"},\n"
                 "    {\"name\": \"diag\", \"lba_first\": 240, \"lba_last\": 753, "
                 "\"byte_offset\": 983040, \"byte_length\": 2105344, "
                 "\"payload_bytes\": 2101347, \"note\": \"NAND:diag, load 0x08000000\"}\n  ],\n");
    o = put_s(o, "  \"reader\": {\n    \"fn\": \"0x081d85bb\",\n");
    o = put_s(o, "    \"dev\": \"0x"); o = put_hex(o, g_dev);
    o = put_s(o, "\",\n    \"obj\": \"0x"); o = put_hex(o, g_obj);
    o = put_s(o, "\",\n    \"backend\": \"0x"); o = put_hex(o, g_backend);
    o = put_s(o, "\",\n    \"backend_off20\": "); o = put_dec(o, g_backend_off);
    o = put_s(o, ",\n    \"gate_head_0xc040\": \"");
    for (int i = 0; i < 16; i++) o = put_hex2(o, g_gate_head[i]);
    o = put_s(o, "\",\n    \"gate_sig55aa_0xc040\": "); o = put_dec(o, g_gate_sig_a);
    o = put_s(o, ",\n    \"gate_sig55aa_0xc07f\": "); o = put_dec(o, g_gate_sig_b);
    o = put_s(o, "\n  },\n  \"result\": {\n");
    o = put_s(o, "    \"next_lba\": "); o = put_dec(o, g_next_lba);
    o = put_s(o, ",\n    \"sectors_ok\": "); o = put_dec(o, g_ok);
    o = put_s(o, ",\n    \"sectors_ok_after_retry\": "); o = put_dec(o, g_retried_ok);
    o = put_s(o, ",\n    \"sectors_failed\": "); o = put_dec(o, g_failed);
    o = put_s(o, ",\n    \"first_failure_lba\": ");
    if (g_ff_lba == 0xffffffffu) o = put_s(o, "null"); else o = put_dec(o, g_ff_lba);
    o = put_s(o, ",\n    \"reserved_bytes\": "); o = put_dec(o, g_bytes_written);
    o = put_s(o, ",\n    \"ms\": "); o = put_dec(o, elapsed_ms());
    o = put_s(o, ",\n    \"checksum_fnv1a32_words\": \"0x"); o = put_hex(o, g_hash);
    o = put_s(o, "\"\n  },\n  \"files\": {\"reserved\": \"reserved.bin\", "
                 "\"status\": \"status.bin\", \"report\": \"report.txt\"},\n"
                 "  \"status_codes\": {\"0\": \"unread\", \"1\": \"ok\", "
                 "\"2\": \"ok after retry\", \"3\": \"read error (zero-filled)\", "
                 "\"4\": \"reader ok but sentinel untouched (zero-filled)\"}\n}\n");
    hb_fs_write(MANIFEST_PATH, g_man, (uint32_t)(o - g_man));
}

/* ----------------------------------------------------------- dump stages --- */
static void fail(const char *stage)
{
    char *o = g_fail_stage;
    o = put_s(o, stage);
    *o = 0;
    g_t_end = hb_time_uptime_ms();
    if (g_stream_open) { hb_fs_stream_close(); g_stream_open = false; }
    write_report(stage, true);
    write_manifest("failed");
    if (g_raw) { hb_os_free(g_raw); g_raw = 0; }
    g_state = S_FAIL;
}

static bool gate_sig(const uint8_t *b) { return b[510] == 0x55 && b[511] == 0xaa; }

/* Everything before the first reserved read: pointers, gates, buffer, files. */
static void stage_prep(void)
{
    g_t_start = hb_time_uptime_ms();
    hb_fs_mkdir(OUT_DIR);

    for (int i = 0; i < 16; i++)
        g_wmr_sig[i] = *(volatile uint32_t *)(WMR_SIG_ADDR + 4u * i);
    g_wmr_magic = g_wmr_sig[0];

    if (!resolve_reader()) { fail("PTRS"); return; }
    if (g_backend_off != 0) { fail("OFFSET"); return; }   /* shifted LBAs: refuse */

    /* chunk buffer: 1 MB if the heap allows it with headroom, else smaller */
    static const uint32_t sizes[] = { CHUNK_BIG, CHUNK_BIG / 4, CHUNK_BIG / 16 };
    uint8_t *bad = 0;
    for (uint32_t s = 0; s < 3 && !g_raw; s++) {
        uint32_t want = sizes[s] + 32u;
        if (hb_os_heap_largest() < want || hb_os_heap_free() < want + HEAP_HEADROOM) continue;
        uint8_t *p = (uint8_t *)hb_os_alloc(want);
        if (!p) continue;
        if ((uint32_t)p < SDK_SCRATCH_HI && (uint32_t)p + want > SDK_SCRATCH_LO) {
            bad = p;                              /* keep it so it isn't re-handed */
            p = (uint8_t *)hb_os_alloc(want);
            if (!p) continue;
        }
        g_raw = p;
        g_chunk_bytes = sizes[s];
    }
    if (bad) hb_os_free(bad);
    if (!g_raw) { fail("ALLOC"); return; }
    g_buf = (uint8_t *)(((uint32_t)g_raw + 31u) & ~31u);

    /* gate: the reader must return the data-volume MBR at global LBA 0xc040 */
    g_gate_rc_a = (uint32_t)os_read(GATE_LBA_A, 1, g_buf);
    if (g_gate_rc_a != 0 || sector_untouched((const uint32_t *)g_buf)) { fail("GATE"); return; }
    g_gate_sig_a = gate_sig(g_buf);
    for (int i = 0; i < 16; i++) g_gate_head[i] = g_buf[i];
    g_gate_rc_b = (uint32_t)os_read(GATE_LBA_B, 1, g_buf);
    g_gate_sig_b = g_gate_rc_b == 0 && gate_sig(g_buf);

    write_report("running", false);
    write_manifest("incomplete");
    if (!hb_fs_stream_open(RESERVED_PATH)) { fail("OPEN"); return; }
    g_stream_open = true;
    g_wr_ok = true;
    g_next_lba = 0;
    g_t_read0 = hb_time_uptime_ms();
    g_state = S_READ;
}

/* One chunk per frame so the screen can show live progress between chunks. */
static void stage_read_chunk(void)
{
    uint32_t lba = g_next_lba;
    uint32_t n = g_chunk_bytes / SECTOR_BYTES;
    if (n > LBA_COUNT - lba) n = LBA_COUNT - lba;
    g_chunk_err = 0;

    for (uint32_t g = 0; g < n; g += GROUP_SECT) {
        uint32_t m = n - g < GROUP_SECT ? n - g : GROUP_SECT;
        read_group(lba + g, m, g_buf + g * SECTOR_BYTES);
    }
    g_hash = fnv_words(g_hash, (const uint32_t *)g_buf, n * SECTOR_WORDS);
    if (!hb_fs_stream_write(g_buf, n * SECTOR_BYTES)) {
        g_wr_ok = false;
        fail("WRITE");
        return;
    }
    g_bytes_written += n * SECTOR_BYTES;
    g_next_lba += n;
    hb_trace_log("DUMPBOOT", g_next_lba, g_failed);

    if (g_next_lba >= LBA_COUNT || g_cancel) {
        g_cancelled = g_next_lba < LBA_COUNT;
        g_state = S_DONE;
    }
}

static void stage_finish(void)
{
    bool ok = true;
    if (!hb_fs_stream_close()) { ok = false; g_wr_ok = false; }
    g_stream_open = false;
    g_t_end = hb_time_uptime_ms();
    if (g_raw) { hb_os_free(g_raw); g_raw = 0; }
    if (!hb_fs_write(STATUS_PATH, g_status, LBA_COUNT)) { ok = false; g_wr_ok = false; }
    write_report(g_cancelled ? "cancelled" : "done", true);
    write_manifest(g_cancelled ? "cancelled" : (g_failed ? "complete_with_errors" : "complete"));
    if (!ok) { put_s(g_fail_stage, "FINAL"); g_state = S_FAIL; }
    else g_state = S_OK;
}

/* ------------------------------------------------------------------- view --- */
static uint32_t g_frame;

static void draw(void)
{
    char b[40], *o;
    hb_raw_fill(BG);
    text(8, 12, "DUMPBOOT", 3, ACC);
    text(8, 44, "RESERVED NAND LBA 0-0XC03F", 1, DIM);
    text(8, 52, "READ-ONLY -> /APPS/DATA/PROBE", 1, DIM);

    if (g_state == S_OK) {
        text(24, 110, g_cancelled ? "CANCELLED" : "OK - REBOOT ME", 2, g_cancelled ? BAD : OKC);
        text(24, 140, "FILES SAVED", 2, FG);
        o = put_s(b, "BAD SECTORS: "); o = put_dec(o, g_failed); *o = 0;
        text(24, 170, b, 2, g_failed ? BAD : FG);
        o = put_dec(b, g_bytes_written / (1024u * 1024u)); o = put_s(o, " MB"); *o = 0;
        text(24, 200, b, 2, FG);
        o = put_dec(b, elapsed_ms() / 1000u); o = put_s(o, " S"); *o = 0;
        text(24, 230, b, 2, DIM);
        if (g_cancelled) text(24, 260, "PARTIAL - REBOOT ME", 2, FG);
        return;
    }
    if (g_state == S_FAIL) {
        text(24, 110, "FAIL", 3, BAD);
        text(24, 150, g_fail_stage, 2, BAD);
        text(24, 190, "SEE REPORT.TXT", 2, FG);
        text(24, 220, "REBOOT ME", 2, FG);
        return;
    }
    if (g_state == S_IDLE) {
        hb_raw_fill_rect(BTN_X, BTN_Y, BTN_W, BTN_H, ACC);
        hb_raw_rect_outline(BTN_X, BTN_Y, BTN_W, BTN_H, 2, FG);
        text(BTN_X + (BTN_W - 96) / 2, BTN_Y + (BTN_H - 15) / 2, "DUMPBOOT", 3, BG);
        text(12, 280, "TAP TO DUMP", 2, DIM);
        return;
    }

    /* busy: BUSY + animated dots (proves it is alive), bar, %, MB, errors, time */
    uint32_t done = g_next_lba;
    uint32_t pct = done * 100u / LBA_COUNT;
    uint32_t dots = (g_frame / 20u) % 4u;
    o = put_s(b, "BUSY");
    for (uint32_t i = 0; i < dots; i++) *o++ = '.';
    *o = 0;
    text(8, 72, b, 3, FG);
    hb_raw_rect_outline(20, 120, 200, 24, 2, FG);
    hb_raw_fill_rect(22, 122, (int)(196u * pct / 100u), 20, ACC);
    o = put_dec(b, pct); o = put_s(o, "%"); *o = 0;
    text(20, 156, b, 4, FG);
    o = put_dec(b, done / (1024u * 1024u / SECTOR_BYTES));
    o = put_s(o, " / "); o = put_dec(o, TOTAL_MB); o = put_s(o, " MB"); *o = 0;
    text(20, 192, b, 2, FG);
    o = put_s(b, "ERR "); o = put_dec(o, g_failed); *o = 0;
    text(20, 216, b, 2, g_failed ? BAD : DIM);
    o = put_s(b, "T "); o = put_dec(o, (hb_time_uptime_ms() - g_t_start) / 1000u);
    o = put_s(o, " S"); *o = 0;
    text(20, 240, b, 2, DIM);
    if (g_state == S_READ) {
        hb_raw_rect_outline(CAN_X, CAN_Y, CAN_W, CAN_H, 2, DIM);
        text(CAN_X + 14, CAN_Y + 13, g_cancel ? "STOPPING..." : "HOLD TO CANCEL", 1, DIM);
    }
}

/* ------------------------------------------------------------------ entry --- */
static int g_prev_down;

void hb_raw_init(int w, int h)
{
    (void)w; (void)h;
    g_state = S_IDLE;
    g_fail_stage[0] = 0;
    g_prev_down = 0;
    g_frame = 0;
    draw();
}

void hb_raw_frame(const hb_spoint_t *t)
{
    int down = t->down ? 1 : 0;
    int tapped = down && !g_prev_down;
    g_prev_down = down;
    g_frame++;

    switch (g_state) {
    case S_IDLE:
        if (tapped && t->x >= BTN_X && t->x < BTN_X + BTN_W &&
            t->y >= BTN_Y && t->y < BTN_Y + BTN_H) {
            g_state = S_BUSY0;      /* BUSY is painted THIS frame, work next */
            g_t_start = hb_time_uptime_ms();
            draw();
        }
        return;
    case S_BUSY0:
        g_state = S_PREP;           /* one more repaint, then do the prep   */
        draw();
        return;
    case S_PREP:
        stage_prep();
        break;
    case S_READ:
        if (down && t->x >= CAN_X && t->x < CAN_X + CAN_W &&
            t->y >= CAN_Y && t->y < CAN_Y + CAN_H)
            g_cancel = true;
        stage_read_chunk();
        break;
    case S_DONE:
        stage_finish();
        break;
    default:
        return;                     /* S_OK / S_FAIL: user reboots manually */
    }
    draw();
}
