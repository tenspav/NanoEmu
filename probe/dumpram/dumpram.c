/*
 * DumpRAM, full-RAM snapshot companion to DumpROM.
 *
 * Raw-surface homebrew app (HBAppKind 1, no LVGL): one big "DUMPRAM" button.
 * On tap it runs the WHOLE dump inside a SINGLE frame, no per-stage UI
 * redraws, so the time skew between the register capture and the RAM read is
 * as small as the device allows (the previous 300 s staged dump came out
 * internally inconsistent). Order, tightest-skew-first:
 *
 *   0. mkdir /Apps/Data/probe           (FS setup, before the capture)
 *   1. capture r0-r15 + CPSR (Thumb bit) of THIS thread, setjmp-style
 *   2. ram-full.bin  <- 0x08000000..0x0BFFFFFF  (64 MB, 1 MB streamed chunks)
 *   3. iram.bin      <- 0x22000000..0x2203FFFF  (256 KB)
 *   4. ctx.bin       <- the 18-word snapshot from step 1 (72 bytes)
 *   5. report.txt    <- bytes per file, RAM-dump elapsed ms, errors
 *
 * NOTHING happens between the register capture (1) and the RAM dump (2): no
 * draw(), no status update. The "OK - REBOOT ME" confirmation is drawn only
 * AFTER every file is written and closed. On failure it shows "FAIL <stage>".
 *
 * ctx.bin layout (little-endian uint32, matches the prior 72-byte snapshot):
 *   [0..12] r0-r12   [13] sp(r13)   [14] lr(r14)   [15] pc(r15)
 *   [16] CPSR (Thumb bit = bit 5)   [17] marker 'DRAM' (0x4d415244)
 *
 * v2 (torn-map): the dump is still self-written through the filesystem, so it
 * can't be atomic, but it now MEASURES where it tore. Order:
 *   0. mkdir, allocate the IRAM copy buffer, OPEN ram-full.bin   (pre-capture)
 *   1. capture ctx; 2. copy IRAM (256 KB memcpy, ~1 ms)
 *   3. h0 pass: hash every 64 KB block of DRAM (and the IRAM copy source)
 *   4. stream DRAM in 1 MB writes; per block hash hb just before and ha just
 *      after its write
 *   5. iram.bin (from the copy), ctx.bin, torn-map.bin, report.txt
 * A block is "torn" if h0 != hb (changed between capture and its write) or
 * hb != ha (changed while being written). torn-map.bin layout (LE u32):
 *   'TORN', version 2, block_bytes, n_blocks, then n_blocks x {h0, hb, ha}
 *   for DRAM blocks 0x08000000 + i*block_bytes.
 *
 * v3 (NAND-idle capture): right before the register capture, wait for the FMC
 * (0x38a00000) to go idle so the snapshot doesn't catch the NAND driver
 * mid-operation from our own file writes. Idle = no pending completion bits
 * in the W1C status at +0x48 (bits 1/2/3 cmd/addr/data phase done, 20 PIO
 * data, 23 polled-status ready, 24; all set on completion and consumed via
 * W1C, so any still set means a recent op whose ISR hasn't run) AND no
 * pending bits in the FMSS sequencer CS_IRQ status at +0xc0c (bit0 done,
 * 2/3 errors; nonzero means a kicked sequence not yet acknowledged).
 * Bounded wait (5 s) + 200 ms settle. report.txt: version=3 +
 * nand_idle_wait_ms, fmc_status_at_capture.
 *
 * v4 (IRQ-off overlay): v3 made T0 NAND-idle, but the 64 MB stream itself is
 * written through the NAND thread + FTL, so the kernel, TCBs, the NAND
 * driver's request/stack and the FTL cursor are each read at a different
 * moment of the dump's own writes (snap4: 54 torn blocks, FTL assert on
 * resume). v4 snapshots the volatile set ATOMICALLY at T0:
 *   pre-T0: pick the blocks = static list (union of snap3+snap4 torn maps)
 *           + every block torn in the previous run's torn-map.bin, allocate
 *           one 64 KB heap buffer per block (keeps >= 2 MB heap free)
 *   T0:     svc 0x46 ip=0 (OS irq-save: masks I+F in SPSR, returns old mask)
 *           -> re-check FMC idle -> capture ctx (CPSR |= I|F, as it really
 *           is) -> copy IRAM + every overlay block -> svc 0x46 ip=1 restore
 *   post:   stream ram-full.bin as before, then overlay.bin.
 * No IRQ = no scheduler tick and the thread cannot block, so the copies are
 * one consistent T0 image (FMC idle => no flash DMA either). The emulator
 * lays overlay.bin over ram-full.bin (nano7 overlay=, run.sh --overlay).
 * overlay.bin (LE u32): 'OVLY', 1, block_bytes, n, copy_us, irq_mask,
 *   n_wanted, n_missing, then n DRAM base addrs, then n x block_bytes data.
 *
 * v5 (verified overlay, IRQs ON): v4 crashed the device on tap (0-byte
 * ram-full.bin, no report). svc 0x46 ip=0/1 does decode and run correctly from
 * user mode in the emulator, but v5 drops it anyway, and also fixes a likelier
 * culprit: the SDK keeps its file objects at FIXED, unallocated DRAM
 * (scratch cache 0x09118000, stream obj 0x0911a000 + cache 0x0911a200), and
 * v4's ~60 heap buffers could cover them, so the T0 copy clobbered the
 * already-open stream. v5:
 *   - rejects any overlay buffer overlapping the SDK scratch range;
 *   - keeps the heap's largest free block >= 1 MB (frees buffers until so);
 *   - writes report.txt "stage=prepared" + heap stats BEFORE the capture, so
 *     a crash still leaves a breadcrumb;
 *   - at T0 (IRQs on) copies each block with verification: hash src, copy,
 *     hash src again + hash the copy; accepted only if all three match
 *     (<= 8 tries). Only verified blocks go into overlay.bin (format v2).
 * Each block is internally consistent; blocks are mutually close in time
 * (one ~10-100 ms window instead of the 7 s stream), not strictly atomic.
 * overlay.bin v2 (LE u32): 'OVLY', 2, block_bytes, n, window_us, retries,
 *   n_wanted, n_dropped (alloc/scratch/heap/unverified), n addrs, n blocks.
 *
 * v6 (masked core + verified rest): snap5 showed the two blocks that matter
 * most never verify with IRQs on: kernel 0x08900000 (tick counter changes
 * every 2 ms) and stacks 0x0b400000 (our own stack is in it). v6 restores a
 * SHORT masked section for exactly those (svc 0x46 ip=0/1, emulator-verified
 * from user mode): IRQs off -> FMC idle re-check -> ctx capture (CPSR |= I|F)
 * -> IRAM + masked blocks (~400 KB, ~1-2 ms) -> IRQs on. All other blocks
 * keep the v5 verified copy; one that still fails verification gets a
 * masked copy instead of being dropped. Scratch guard / heap guard / the
 * prepared breadcrumb are kept. overlay.bin stays format v2.
 *
 * 3x5 text is rendered straight into the surface framebuffer, same as DumpROM.
 */
#include "hb_raw_surface.h"
#include "hb_sdk.h"
#include "hb_heap.h"

#define RAM_BASE    0x08000000u
#define RAM_BYTES   (64u * 1024u * 1024u)       /* 0x08000000..0x0BFFFFFF */
#define IRAM_BASE   0x22000000u
#define IRAM_BYTES  (256u * 1024u)              /* 0x22000000..0x2203FFFF */
#define CHUNK_BYTES (1u * 1024u * 1024u)        /* 1 MB streamed buffer    */
#define RAM_CHUNKS  (RAM_BYTES / CHUNK_BYTES)    /* 64                      */

#define OUT_DIR     "/Apps/Data/probe"
#define RAM_PATH    "/Apps/Data/probe/ram-full.bin"
#define IRAM_PATH   "/Apps/Data/probe/iram.bin"
#define CTX_PATH    "/Apps/Data/probe/ctx.bin"
#define REPORT_PATH "/Apps/Data/probe/report.txt"
#define TORN_PATH   "/Apps/Data/probe/torn-map.bin"
#define OVL_PATH    "/Apps/Data/probe/overlay.bin"

#define BLK_BYTES   (64u * 1024u)
#define RAM_BLKS    (RAM_BYTES / BLK_BYTES)        /* 1024 */
#define BLKS_PER_CHUNK (CHUNK_BYTES / BLK_BYTES)   /* 16   */

/* NAND flash controller (see emulator/machine/hw/arm/nano7_fmc.c). */
#define FMC_BASE        0x38a00000u
#define FMC_STATUS      (*(volatile uint32_t *)(FMC_BASE + 0x48))   /* W1C  */
#define FMSS_CS_STATUS  (*(volatile uint32_t *)(FMC_BASE + 0xc0c))  /* W1C  */
/* +0x48 pending-completion bits: 1 cmd, 2 addr, 3 data phase done,
 * 20 PIO data-in, 23 polled-status ready, 24 post-poll. */
#define FMC_PEND_MASK   0x181000eu
#define FMC_IDLE_WAIT_MAX_MS 5000u
#define FMC_SETTLE_MS        200u

/* v4 overlay: DRAM blocks copied with IRQs off at T0 */
#define OVL_MAGIC     0x594c564fu                 /* 'OVLY' */
#define OVL_MAX_BLKS  96u                         /* 6 MB cap          */
#define OVL_HEADROOM  (2u * 1024u * 1024u)        /* leave the OS this */
#define OVL_LARGEST   (1024u * 1024u)             /* keep a 1 MB block free */
#define OVL_TRIES     8u                          /* verified-copy tries */
#define FMC_IDLE_RETRIES 3u
/* v6: blocks copied inside the masked section (DRAM addr >> 16) */
static const uint16_t OVL_MASKED[] = { 0x0890, 0x0b40 };
/* SDK hb_fs.c fixed scratch: FOBJ_CACHE_ADDR .. STREAM_CACHE_ADDR+0x1010 */
#define SDK_SCRATCH_LO 0x09118000u
#define SDK_SCRATCH_HI 0x0911b210u

#define BG   0x0E1118
#define FG   0xE8ECF4
#define DIM  0x7C869A
#define OKC  0x2FBF71
#define BAD  0xE63946
#define ACC  0x4AD9C8
#define BTN  0x27423B

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

/* Fast 32-bit block hash (word sum + rotate-xor): change detection only. */
static uint32_t blk_hash(const volatile uint32_t *p, uint32_t bytes)
{
    uint32_t a = 0x9e3779b9u, b = 0;
    for (uint32_t i = 0; i < bytes / 4; i += 4) {
        a += p[i] ^ (p[i + 1] << 1);
        b ^= p[i + 2] + (p[i + 3] >> 1);
        a = (a << 5) | (a >> 27);
    }
    return a ^ (b * 0x85ebca6bu);
}

/* torn-map: header + {h0, hb, ha} per DRAM block */
static uint32_t g_torn[4 + 3 * RAM_BLKS];

/* Static overlay set, DRAM address >> 16: union of the snap3 + snap4 torn
 * maps (kernel 0x0890, sync/TCBs 0x08a6-0x08b2, PPN/FTL 0x08c0-0x08d1, heap
 * objects, thread stacks 0x0b28-0x0b45). */
static const uint16_t OVL_STATIC[] = {
    0x0881, 0x0890, 0x0891, 0x0897, 0x0899, 0x089a, 0x08a6, 0x08a7, 0x08a8,
    0x08a9, 0x08aa, 0x08ab, 0x08ae, 0x08af, 0x08b0, 0x08b1, 0x08b2, 0x08b8,
    0x08b9, 0x08c0, 0x08c1, 0x08c2, 0x08c3, 0x08c4, 0x08c5, 0x08c6, 0x08c7,
    0x08c8, 0x08c9, 0x08ca, 0x08cd, 0x08d0, 0x08d1, 0x0911, 0x0937, 0x0949,
    0x094a, 0x094b, 0x094c, 0x094d, 0x094f, 0x0950, 0x0957, 0x095c, 0x0965,
    0x0987, 0x0988, 0x098c, 0x098f, 0x0998, 0x0b28, 0x0b33, 0x0b3a, 0x0b3b,
    0x0b3c, 0x0b40, 0x0b41, 0x0b43, 0x0b44, 0x0b45,
};
static uint32_t  g_ovl_sel[RAM_BLKS / 32];        /* wanted-block bitmap   */
static uint32_t  g_ovl_addr[OVL_MAX_BLKS];
static uint32_t *g_ovl_buf[OVL_MAX_BLKS];
static uint32_t  g_ovl_hdr[8];
static uint32_t  g_ovl_n, g_ovl_want, g_ovl_missing, g_ovl_prev, g_ovl_us;
static uint32_t  g_ovl_retries, g_ovl_unverified, g_ovl_scratch;
static uint8_t   g_ovl_masked[OVL_MAX_BLKS];      /* v6: copied with IRQs off */
static uint32_t  g_ovl_nmasked, g_ovl_fallback, g_irq_mask, g_idle_retries, g_mask_us;
static uint32_t  g_heap_free0, g_heap_largest0, g_heap_free1, g_heap_largest1;

/* ------------------------------------------------------------- dump state --- */
static uint32_t g_ctx[18];                 /* r0-r15 + CPSR + marker          */
static char     g_rep[4096];              /* v5: breadcrumb lists buffers */
static uint32_t g_idle_ms;                 /* v3: NAND idle wait, ms          */
static uint32_t g_fmc_status;              /* v3: +0x48 at capture            */

/* Wait until the FMC reports no pending completions, bounded; then settle.
 * The SDK has no fsync: hb_fs_write flushes before returning, and nothing
 * has been written to the stream file yet at this point, so the only
 * in-flight NAND work can be the driver's own tail, hence the poll. */
static void nand_wait_idle(void)
{
    uint32_t start = hb_time_uptime_ms();
    while ((FMC_STATUS & FMC_PEND_MASK) != 0 || FMSS_CS_STATUS != 0) {
        if (hb_time_uptime_ms() - start >= FMC_IDLE_WAIT_MAX_MS) break;
    }
    uint32_t settle = hb_time_uptime_ms();
    while (hb_time_uptime_ms() - settle < FMC_SETTLE_MS) { }
    g_idle_ms = hb_time_uptime_ms() - start;
    g_fmc_status = FMC_STATUS;
}

/* OS irq-save / irq-restore (svc 0x46 ip=0 -> 0x080e04c6: SPSR |= I|F,
 * returns the old I|F bits; ip=1 -> 0x080e04dc puts them back). Checked in
 * the emulator from user mode. The handler runs C code: r1-r3 clobbered. */
static inline uint32_t irq_save(void)
{
    register uint32_t r12 __asm__("r12") = 0;
    register uint32_t r0 __asm__("r0");
    __asm__ volatile("svc #70" : "+r"(r12), "=r"(r0) :: "r1", "r2", "r3", "memory", "cc");
    return r0;
}

static inline void irq_restore(uint32_t mask)
{
    register uint32_t r12 __asm__("r12") = 1;
    register uint32_t r0 __asm__("r0") = mask;
    __asm__ volatile("svc #70" : "+r"(r12), "+r"(r0) :: "r1", "r2", "r3", "memory", "cc");
}

static bool fmc_idle(void)
{
    return (FMC_STATUS & FMC_PEND_MASK) == 0 && FMSS_CS_STATUS == 0;
}

static void copy_blk(uint32_t *dst, const volatile uint32_t *src)
{
    for (uint32_t i = 0; i < BLK_BYTES / 4; i += 4) {
        uint32_t a = src[i], b = src[i + 1], c = src[i + 2], d = src[i + 3];
        dst[i] = a; dst[i + 1] = b; dst[i + 2] = c; dst[i + 3] = d;
    }
}

static bool is_masked_blk(uint32_t i)
{
    for (uint32_t m = 0; m < sizeof OVL_MASKED / sizeof OVL_MASKED[0]; m++)
        if (((uint32_t)OVL_MASKED[m] << 16) == RAM_BASE + i * BLK_BYTES) return true;
    return false;
}

/* Pre-T0: choose the overlay blocks and allocate one buffer per block (64 KB
 * pieces fit a fragmented heap far better than one 4 MB block). */
static void ovl_prepare(void)
{
    for (uint32_t i = 0; i < sizeof OVL_STATIC / sizeof OVL_STATIC[0]; i++) {
        uint32_t b = ((uint32_t)OVL_STATIC[i] << 16) - RAM_BASE;
        b /= BLK_BYTES;
        g_ovl_sel[b / 32] |= 1u << (b % 32);
    }
    /* + whatever tore in the previous run (g_torn is reused as the buffer) */
    if (hb_fs_read(TORN_PATH, g_torn, sizeof g_torn) == sizeof g_torn &&
        g_torn[0] == 0x4e524f54u && g_torn[2] == BLK_BYTES && g_torn[3] == RAM_BLKS) {
        const uint32_t *h = g_torn + 4;
        for (uint32_t i = 0; i < RAM_BLKS; i++) {
            if (h[3 * i] != h[3 * i + 1] || h[3 * i + 1] != h[3 * i + 2]) {
                g_ovl_prev++;
                g_ovl_sel[i / 32] |= 1u << (i % 32);
            }
        }
    }
    for (uint32_t m = 0; m < sizeof OVL_MASKED / sizeof OVL_MASKED[0]; m++) {
        uint32_t b = (((uint32_t)OVL_MASKED[m] << 16) - RAM_BASE) / BLK_BYTES;
        g_ovl_sel[b / 32] |= 1u << (b % 32);
    }
    /* pass 0: the masked blocks (allocated first: they matter most);
     * pass 1: everything else */
    for (uint32_t pass = 0; pass < 2; pass++)
    for (uint32_t i = 0; i < RAM_BLKS; i++) {
        if (!(g_ovl_sel[i / 32] & (1u << (i % 32)))) continue;
        if (is_masked_blk(i) != (pass == 0)) continue;
        g_ovl_want++;
        void *p = 0;
        if (g_ovl_n < OVL_MAX_BLKS && hb_os_heap_free() >= OVL_HEADROOM + BLK_BYTES)
            p = hb_os_alloc(BLK_BYTES);
        if (!p) { g_ovl_missing++; continue; }
        uint32_t lo = (uint32_t)p, hi = lo + BLK_BYTES;
        if (lo < SDK_SCRATCH_HI && hi > SDK_SCRATCH_LO) {
            /* would overwrite the SDK's live file objects: keep it allocated
             * (so the heap can't hand it out again) until ovl_release */
            g_ovl_scratch++;
            g_ovl_missing++;
            g_ovl_addr[OVL_MAX_BLKS - g_ovl_scratch] = 0;
            g_ovl_buf[OVL_MAX_BLKS - g_ovl_scratch] = (uint32_t *)p;
            continue;
        }
        g_ovl_addr[g_ovl_n] = RAM_BASE + i * BLK_BYTES;
        g_ovl_masked[g_ovl_n] = pass == 0;
        g_ovl_nmasked += pass == 0;
        g_ovl_buf[g_ovl_n++] = (uint32_t *)p;
        if (g_ovl_n + g_ovl_scratch >= OVL_MAX_BLKS) { pass = 2; break; }
    }
    /* the FS/FTL allocate too: never leave the heap without a big block */
    while (g_ovl_n > g_ovl_nmasked && hb_os_heap_largest() < OVL_LARGEST) {
        hb_os_free(g_ovl_buf[--g_ovl_n]);
        g_ovl_missing++;
    }
    g_heap_free1 = hb_os_heap_free();
    g_heap_largest1 = hb_os_heap_largest();
}

/* T0 copy, IRQs ON: a block is accepted only if it did not change across the
 * copy (src hash before == src hash after == hash of the copy). */
static void ovl_copy_verified(void)
{
    for (uint32_t k = 0; k < g_ovl_n; k++) {
        if (g_ovl_masked[k]) continue;   /* already copied at T0 */
        const volatile uint32_t *src = (const volatile uint32_t *)g_ovl_addr[k];
        uint32_t *dst = g_ovl_buf[k];
        bool ok = false;
        for (uint32_t t = 0; t < OVL_TRIES && !ok; t++) {
            if (t) g_ovl_retries++;
            uint32_t h1 = blk_hash(src, BLK_BYTES);
            copy_blk(dst, src);
            uint32_t h2 = blk_hash(src, BLK_BYTES);
            ok = h1 == h2 && blk_hash(dst, BLK_BYTES) == h1;
        }
        if (!ok) {
            /* v6: never drop it, one short masked copy instead */
            g_ovl_unverified++;
            g_ovl_fallback++;
            uint32_t m = irq_save();
            copy_blk(dst, src);
            irq_restore(m);
        }
    }
}

static void ovl_release(void)
{
    for (uint32_t k = 0; k < g_ovl_n; k++) hb_os_free(g_ovl_buf[k]);
    for (uint32_t k = 0; k < g_ovl_scratch; k++) hb_os_free(g_ovl_buf[OVL_MAX_BLKS - 1 - k]);
}

/* Breadcrumb written before the capture: survives a crash later on. */
static void report_prepared(void)
{
    char *o = g_rep;
    o = put_s(o, "version=6\nstage=prepared\n");
    o = put_s(o, "heap_free_before=");    o = put_dec(o, g_heap_free0);    *o++ = '\n';
    o = put_s(o, "heap_largest_before="); o = put_dec(o, g_heap_largest0); *o++ = '\n';
    o = put_s(o, "heap_free_after=");     o = put_dec(o, g_heap_free1);    *o++ = '\n';
    o = put_s(o, "heap_largest_after=");  o = put_dec(o, g_heap_largest1); *o++ = '\n';
    o = put_s(o, "overlay_alloc=");       o = put_dec(o, g_ovl_n);         *o++ = '\n';
    o = put_s(o, "overlay_wanted=");      o = put_dec(o, g_ovl_want);      *o++ = '\n';
    o = put_s(o, "overlay_scratch_rejected="); o = put_dec(o, g_ovl_scratch); *o++ = '\n';
    for (uint32_t k = 0; k < g_ovl_n; k++) {
        o = put_s(o, "buf=0x"); o = put_hex(o, (uint32_t)g_ovl_buf[k]);
        o = put_s(o, " blk=0x"); o = put_hex(o, g_ovl_addr[k]); *o++ = '\n';
    }
    hb_fs_write(REPORT_PATH, g_rep, (uint32_t)(o - g_rep));
}

static bool ovl_write(void)
{
    g_ovl_hdr[0] = OVL_MAGIC;
    g_ovl_hdr[1] = 2;
    g_ovl_hdr[2] = BLK_BYTES;
    g_ovl_hdr[3] = g_ovl_n;
    g_ovl_hdr[4] = g_ovl_us;
    g_ovl_hdr[5] = g_ovl_retries;
    g_ovl_hdr[6] = g_ovl_want;
    g_ovl_hdr[7] = g_ovl_missing;
    if (!hb_fs_stream_open(OVL_PATH)) return false;
    bool ok = hb_fs_stream_write(g_ovl_hdr, sizeof g_ovl_hdr) &&
              hb_fs_stream_write(g_ovl_addr, 4u * g_ovl_n);
    for (uint32_t k = 0; ok && k < g_ovl_n; k++)
        ok = hb_fs_stream_write(g_ovl_buf[k], BLK_BYTES);
    return hb_fs_stream_close() && ok;
}

enum { S_IDLE, S_RUN, S_OK, S_FAIL };
static int  g_state;
static char g_fail_stage[16];

/* setjmp-style capture of the calling thread's r0-r15 + CPSR (Thumb bit). */
static void capture_ctx(void)
{
    uint32_t *p = g_ctx;
    __asm__ volatile(
        "stmia %[p], {r0-r12}\n\t"      /* [0..12] = r0-r12 (p's reg = p)   */
        "str   sp,  [%[p], #52]\n\t"    /* [13] = sp (r13)                  */
        "str   lr,  [%[p], #56]\n\t"    /* [14] = lr (r14)                  */
        "mov   r0,  pc\n\t"
        "str   r0,  [%[p], #60]\n\t"    /* [15] = pc (r15)                  */
        "mrs   r0,  cpsr\n\t"
        "str   r0,  [%[p], #64]\n\t"    /* [16] = CPSR (T bit = bit 5)      */
        :
        : [p] "r"(p)
        : "r0", "memory");
    g_ctx[17] = 0x4d415244u;            /* 'DRAM' marker in the spare word  */
}

/* The whole dump, run in ONE call. Fills report + sets g_fail_stage on error. */
static bool run_dump(void)
{
    uint32_t ram_written = 0, iram_written = 0, ram_ms = 0, hash_ms = 0;
    uint32_t torn = 0, torn_pre = 0, torn_during = 0;
    bool ok_ctx = false, ok_ram = false, ok_iram = false, ok_torn = false, ok_ovl = false;
    uint32_t *h = g_torn + 4;

    /* ---- everything that touches the FS or the heap happens BEFORE capture */
    hb_fs_mkdir(OUT_DIR);
    uint8_t *iram_copy = (uint8_t *)hb_os_alloc(IRAM_BYTES);
    uint8_t *iram_bad = 0;              /* v5: never copy over SDK scratch */
    if (iram_copy && (uint32_t)iram_copy < SDK_SCRATCH_HI &&
        (uint32_t)iram_copy + IRAM_BYTES > SDK_SCRATCH_LO) {
        iram_bad = iram_copy;
        iram_copy = (uint8_t *)hb_os_alloc(IRAM_BYTES);
    }
    if (!iram_copy) { hb_os_free(iram_bad); put_s(g_fail_stage, "ALLOC"); return false; }
    g_heap_free0 = hb_os_heap_free();
    g_heap_largest0 = hb_os_heap_largest();
    ovl_prepare();                      /* v4: pick + allocate overlay blocks */
    report_prepared();                  /* v5: breadcrumb (overwritten at end) */
    ok_ram = hb_fs_stream_open(RAM_PATH);

    nand_wait_idle();                   /* v3: let the NAND driver go idle   */
    /* v6: short masked section. FMC must still be idle once masked (an op
     * may have started in the gap), else unmask, let it finish, retry. */
    g_irq_mask = irq_save();
    while (!fmc_idle() && g_idle_retries < FMC_IDLE_RETRIES) {
        irq_restore(g_irq_mask);
        g_idle_retries++;
        nand_wait_idle();
        g_irq_mask = irq_save();
    }
    capture_ctx();                      /* <-- register snapshot (T0)        */
    g_ctx[16] |= 0xc0u;                 /* I+F masked at T0 (usr MRS may not say) */
    uint32_t mus0 = hb_time_uptime_us();

    /* IRAM + the masked blocks: atomic with the ctx. */
    for (uint32_t i = 0; i < IRAM_BYTES / 4; i++)
        ((uint32_t *)iram_copy)[i] = ((const volatile uint32_t *)IRAM_BASE)[i];
    for (uint32_t k = 0; k < g_ovl_n; k++)
        if (g_ovl_masked[k])
            copy_blk(g_ovl_buf[k], (const volatile uint32_t *)g_ovl_addr[k]);
    g_mask_us = hb_time_uptime_us() - mus0;
    irq_restore(g_irq_mask);            /* <-- end of the masked section     */

    /* v5: verified copies of the other volatile blocks, IRQs on. */
    uint32_t us0 = hb_time_uptime_us();
    ovl_copy_verified();
    g_ovl_us = hb_time_uptime_us() - us0;

    /* h0: reference hash of every DRAM block, as close to T0 as possible. */
    uint32_t t0 = hb_time_uptime_ms();
    for (uint32_t i = 0; i < RAM_BLKS; i++)
        h[3 * i] = blk_hash((const volatile uint32_t *)(RAM_BASE + i * BLK_BYTES),
                            BLK_BYTES);
    uint32_t t1 = hb_time_uptime_ms();
    hash_ms = t1 - t0;

    /* Stream DRAM; hash each block right before and right after its write. */
    if (ok_ram) {
        for (uint32_t c = 0; c < RAM_CHUNKS; c++) {
            uint32_t base = RAM_BASE + c * CHUNK_BYTES;
            for (uint32_t k = 0; k < BLKS_PER_CHUNK; k++)
                h[3 * (c * BLKS_PER_CHUNK + k) + 1] =
                    blk_hash((const volatile uint32_t *)(base + k * BLK_BYTES), BLK_BYTES);
            if (!hb_fs_stream_write((const void *)base, CHUNK_BYTES)) { ok_ram = false; break; }
            ram_written += CHUNK_BYTES;
            for (uint32_t k = 0; k < BLKS_PER_CHUNK; k++)
                h[3 * (c * BLKS_PER_CHUNK + k) + 2] =
                    blk_hash((const volatile uint32_t *)(base + k * BLK_BYTES), BLK_BYTES);
        }
        if (!hb_fs_stream_close()) ok_ram = false;
    }
    ram_ms = hb_time_uptime_ms() - t1;
    hb_trace_log("DRAMRAM ", ok_ram ? 1 : 0, ram_written);

    /* ---- after the stream: the rest no longer affects the RAM image ---- */
    ok_ovl = ovl_write();
    ovl_release();
    ok_iram = hb_fs_write(IRAM_PATH, iram_copy, IRAM_BYTES);
    if (ok_iram) iram_written = IRAM_BYTES;
    hb_os_free(iram_copy);
    hb_os_free(iram_bad);

    ok_ctx = hb_fs_write(CTX_PATH, g_ctx, sizeof g_ctx);

    for (uint32_t i = 0; i < RAM_BLKS; i++) {
        bool pre = h[3 * i] != h[3 * i + 1], dur = h[3 * i + 1] != h[3 * i + 2];
        torn_pre += pre;
        torn_during += dur;
        torn += pre || dur;
    }
    g_torn[0] = 0x4e524f54u;            /* 'TORN' */
    g_torn[1] = 2;
    g_torn[2] = BLK_BYTES;
    g_torn[3] = RAM_BLKS;
    ok_torn = hb_fs_write(TORN_PATH, g_torn, sizeof g_torn);
    hb_trace_log("DRAMTORN", torn, torn_during);

    /* report.txt */
    char *o = g_rep;
    o = put_s(o, "version=6\n");
    o = put_s(o, "nand_idle_wait_ms="); o = put_dec(o, g_idle_ms);   *o++ = '\n';
    o = put_s(o, "fmc_status_at_capture=0x"); o = put_hex(o, g_fmc_status); *o++ = '\n';
    o = put_s(o, "heap_largest_after_alloc="); o = put_dec(o, g_heap_largest1); *o++ = '\n';
    o = put_s(o, "overlay_scratch_rejected="); o = put_dec(o, g_ovl_scratch); *o++ = '\n';
    o = put_s(o, "overlay_unverified="); o = put_dec(o, g_ovl_unverified); *o++ = '\n';
    o = put_s(o, "overlay_masked="); o = put_dec(o, g_ovl_nmasked); *o++ = '\n';
    o = put_s(o, "overlay_masked_fallback="); o = put_dec(o, g_ovl_fallback); *o++ = '\n';
    o = put_s(o, "masked_section_us="); o = put_dec(o, g_mask_us); *o++ = '\n';
    o = put_s(o, "irq_mask_saved=0x"); o = put_hex(o, g_irq_mask); *o++ = '\n';
    o = put_s(o, "fmc_idle_retries="); o = put_dec(o, g_idle_retries); *o++ = '\n';
    o = put_s(o, "overlay_retries="); o = put_dec(o, g_ovl_retries); *o++ = '\n';
    o = put_s(o, "overlay_blocks="); o = put_dec(o, ok_ovl ? g_ovl_n : 0u); *o++ = '\n';
    o = put_s(o, "overlay_wanted="); o = put_dec(o, g_ovl_want);   *o++ = '\n';
    o = put_s(o, "overlay_missing="); o = put_dec(o, g_ovl_missing); *o++ = '\n';
    o = put_s(o, "overlay_prev_torn="); o = put_dec(o, g_ovl_prev); *o++ = '\n';
 o = put_s(o, "overlay_window_us="); o = put_dec(o, g_ovl_us);    *o++ = '\n';
    o = put_s(o, "ctx_bytes=");      o = put_dec(o, ok_ctx ? (uint32_t)sizeof g_ctx : 0u); *o++ = '\n';
    o = put_s(o, "ram_full_bytes="); o = put_dec(o, ram_written);  *o++ = '\n';
    o = put_s(o, "iram_bytes=");     o = put_dec(o, iram_written); *o++ = '\n';
    o = put_s(o, "hash_ms=");        o = put_dec(o, hash_ms);      *o++ = '\n';
    o = put_s(o, "ram_dump_ms=");    o = put_dec(o, ram_ms);       *o++ = '\n';
    o = put_s(o, "cpsr=0x");         o = put_hex(o, g_ctx[16]);    *o++ = '\n';
    o = put_s(o, "torn_blocks=");    o = put_dec(o, torn);         *o++ = '\n';
    o = put_s(o, "torn_before_write="); o = put_dec(o, torn_pre);  *o++ = '\n';
    o = put_s(o, "torn_during_write="); o = put_dec(o, torn_during); *o++ = '\n';
    o = put_s(o, "errors=");
    if (ok_ctx && ok_ram && ok_iram && ok_torn && ok_ovl) o = put_s(o, "none");
    else {
        if (!ok_ctx)  o = put_s(o, "ctx ");
        if (!ok_ram)  o = put_s(o, "ram ");
        if (!ok_iram) o = put_s(o, "iram ");
        if (!ok_torn) o = put_s(o, "torn ");
        if (!ok_ovl)  o = put_s(o, "overlay ");
    }
    *o++ = '\n';
    o = put_s(o, (ok_ctx && ok_ram && ok_iram && ok_torn && ok_ovl) ? "ALL_DONE\n" : "INCOMPLETE\n");
    hb_fs_write(REPORT_PATH, g_rep, (uint32_t)(o - g_rep));

    if (!ok_ram)  { put_s(g_fail_stage, "RAM");  return false; }
    if (!ok_iram) { put_s(g_fail_stage, "IRAM"); return false; }
    if (!ok_ctx)  { put_s(g_fail_stage, "CTX");  return false; }
    if (!ok_torn) { put_s(g_fail_stage, "TORN"); return false; }
    if (!ok_ovl)  { put_s(g_fail_stage, "OVERLAY"); return false; }
    return true;
}

/* ------------------------------------------------------------------- view --- */
static void draw(void)
{
    hb_raw_fill(BG);
    text(8, 12, "DUMPRAM", 3, ACC);
    text(150, 22, "V6", 2, DIM);
    text(8, 44, "64M DRAM 0X08000000", 1, DIM);
    text(8, 52, "+IRAM -> /APPS/DATA/PROBE", 1, DIM);

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

    bool running = g_state == S_RUN;
    hb_raw_fill_rect(BTN_X, BTN_Y, BTN_W, BTN_H, running ? BTN : ACC);
    hb_raw_rect_outline(BTN_X, BTN_Y, BTN_W, BTN_H, 2, FG);
    /* "DUMPRAM" is 7 chars -> 7*4*3=84 px at scale 3 */
    text(BTN_X + (BTN_W - 84) / 2, BTN_Y + (BTN_H - 15) / 2, "DUMPRAM", 3,
         running ? DIM : BG);

    text(12, 280, running ? "DUMPING - HOLD" : "TAP TO DUMP", 2,
         running ? FG : DIM);
    if (running) text(12, 304, "DO NOT TOUCH", 2, OKC);
}

/* ------------------------------------------------------------------ entry --- */
static int g_prev_down;

void hb_raw_init(int w, int h)
{
    (void)w; (void)h;
    g_state = S_IDLE;
    g_fail_stage[0] = 0;
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
            /* Paint the "DUMPING" frame ONCE, then run the whole dump with no
             * further redraws so nothing perturbs the capture->dump window. */
            g_state = S_RUN;
            draw();
            bool ok = run_dump();
            g_state = ok ? S_OK : S_FAIL;
            draw();
        }
        return;
    }

    /* S_RUN / S_OK / S_FAIL: sit there; the user reboots manually. */
}
