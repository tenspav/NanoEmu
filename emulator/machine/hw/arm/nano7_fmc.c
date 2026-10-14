/*
 * nano7_fmc.c -- minimal stateful PPN NAND flash controller (FMC) model
 * for the iPod nano 7G machine (0x38a00000).
 *
 * Models just enough of the command / address / data-in flow for the
 * RetailOS PPN driver's device detection (see workers/claude-ppn.txt):
 *
 *   +0x00  CE select: base | (1<<ce)<<1 | 1 selects, 1 deselects
 *   +0x04  "go": 2 = cmd, 1 = address, 0x22 = data-in arm,
 *          0x1e0 = PIO read start, 0xea = status poll, 0x20 = stop
 *   +0x08  command byte (issued on write)
 *   +0x0c/+0x10  address bytes (LSB first), +0x2c = count-1
 *   +0x28  bytes popped from the data-in FIFO (firmware writes 0)
 *   +0x48  completion status, write-1-to-clear
 *   +0x4c  polled NAND status byte
 *   +0x80  data-in FIFO (32-bit pops)
 *
 * Every "go" sets all completion bits (1/2/3/20/23/24), like the old
 * all-ones regfile did. Responses: 2 PPN CEs (CE0/CE1, version 1.0.0),
 * CE2..7 absent (0xff). FMSS page programs/reads go through a sparse NAND
 * page store (data + 16-byte sftl meta per 4 KB sector; see "NAND page
 * store" below); other reads are blank flash (0xff).
 * NANO7_FMC_TRACE=file logs every register access (c = CPU, v = sequencer).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "nano7_fmc.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"
#include "hw/irq.h"
#include "exec/address-spaces.h"
#include "qemu/timer.h"

#define FMC_SIZE        0x1000
#define FMC_NUM_CE      2
#define FMC_BUF_MAX     512
#define FMC_DONE_BITS   ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 20) | \
                         (1u << 23) | (1u << 24))
#define FMC_LOG_MAX     600

/*
 * FMSS command sequencer (+0xc00 window), RE'd from the RetailOS PPN driver
 * (_performPPNRead/_performPPNProgram, programs at 0x0896e920/eb00/f620):
 *   +0xc00 kick (driver writes 0xfff5)   +0xc04 sequence base (DRAM)
 *   +0xc0c CS_IRQ status, W1C, bit0 = done (2,3 = errors)
 *   +0xc10 CS irq enable (ISR 0x8161e8c clears it)   +0xc38 seq pointer
 *   +0xc4c program length (set by the program itself)
 *   +0xd00..+0xd7c CS_GEN_REG mailbox: args in, result code in +0xd00
 * Program = 8-byte insns: w0 = op<<24 | d<<16 | imm16 (s = imm16 & 0xff),
 * w1 = operand. Internal regs r0..r15, r8 = PC (byte offset).
 */
#define FMSS_KICK       0xc00
#define FMSS_SEQ        0xc04
#define FMSS_STATUS     0xc0c
#define FMSS_IRQEN      0xc10
#define FMSS_PC         0xc38
#define FMSS_LEN        0xc4c
#define FMSS_MBOX       0xd00
#define FMSS_MAX_STEPS  200000


/*
 * NAND page store. The PPN programs (RE'd from FMSS traces, Session 13):
 *   program: cmd 0x8a (first) / 0x87 (next) + 4 address bytes (+0x0c = page
 *     address: cau<<19 | block<<7 | page), then per 4 KB sector: meta in
 *     +0x60..+0x6c, go 0x34 (meta out), +0x34 = DRAM address, 4 x 1 KB data
 *     gos (0x260/0x2e4); cmd 0x17 confirms the page.
 *   read: cmd 0x07/0x0a + 8 address bytes (+0x0c byte0 = sector count << 4,
 *     byte2 = first sector << 4; +0x10 = page address), queued per CE and
 *     served in order by cmd 0x7a: per sector go 0x32 (meta in -> +0x60..
 *     +0x6c, which the program copies to the FTL meta buffer), +0x34 = DRAM
 *     address, 4 x 1 KB data gos (0x62/0x1e2).
 * The sector is moved whole at the +0x34 write; the 1 KB gos only pace it.
 * Meta (sftl, 16 B LE): +0 type (1 = data), +1 flags (bit1 uECC), +2/+4 seq,
 * +8 LBA (4 KB units), +0xc = +4. s_verify_meta panics on an LBA mismatch.
 * Records live in a file (nand-store=, else an unlinked temp file): each is
 * a 0x80-byte header (magic, ce, addr, valid sectors, 4 metas) + 16 KB data,
 * appended on first program; the index is rebuilt from the headers on open.
 */
#define NAND_SECT       4096
#define NAND_SECTS      4
#define NAND_HDR        0x80
#define NAND_REC        (NAND_HDR + NAND_SECTS * NAND_SECT)
#define NAND_MAGIC      0x4750374eu     /* "N7PG" */
#define NAND_RDQ        64
#define NAND_MISS_MAX   256
#define NAND_META_BYTES 16

typedef struct NandHdr {
    uint32_t magic, ce, addr, valid;
    uint32_t meta[NAND_SECTS][4];
    uint32_t pad[(NAND_HDR - 80) / 4];
} NandHdr;

typedef struct NandRead {
    uint32_t addr;
    uint8_t first, count;
} NandRead;

typedef struct NandMiss {       /* never-programmed sector read in a run */
    uint32_t ce, addr, sect, data_dst, meta_dst;
} NandMiss;

enum { NAND_IDLE, NAND_PROG, NAND_READ };

typedef struct NandStore {
    int fd;
    GHashTable *idx;            /* (ce << 32 | addr) -> slot + 1 */
    uint32_t slots;
    int mode;
    NandHdr cur;
    int64_t slot;               /* -1: page not in the store */
    int sect, last_sect;
    unsigned chunk;             /* +0x34 writes in this sector */
    NandRead rdq[FMC_NUM_CE][NAND_RDQ];
    unsigned rdq_head[FMC_NUM_CE], rdq_n[FMC_NUM_CE];
    int miss_policy;            /* NAND_MISS_* */
    int disk_fd;                /* nand-disk=: 4 KB-LBA image for misses */
    NandMiss miss[NAND_MISS_MAX];
    unsigned nmiss;
    bool cur_miss, want_meta_dst;
    uint32_t data_lo;           /* lowest read data DMA address this run */
    uint64_t pages_prog, sect_prog, sect_hit, sect_miss, oracle_fixed;
    uint64_t oracle_skip;
} NandStore;

enum { NAND_MISS_BLANK, NAND_MISS_ORACLE };

typedef struct Nano7FmcState {
    MemoryRegion mr;
    hwaddr base;
    uint32_t regs[FMC_SIZE / 4];
    bool written[FMC_SIZE / 4];
    int ce;                 /* selected chip enable, -1 = none */
    uint8_t cur_cmd;        /* last "primary" command byte */
    uint32_t status;        /* +0x48 */
    uint32_t popped;        /* +0x28 */
    uint8_t buf[FMC_BUF_MAX];
    unsigned buf_len, buf_pos;
    unsigned log_lines;
    uint32_t last_trace[3];   /* collapse repeated identical polls */
    uint8_t last_cmd;         /* last command byte written to +0x08 */
    qemu_irq irq;             /* FMSS completion, VIC line 54 */
    uint32_t cs_status;       /* +0xc0c */
    uint32_t fmss_pc;
    bool in_vm;               /* suppress tracing of sequencer accesses */
    bool resume_kick_ok;      /* next base-less kick = pre-capture seq: done */
    uint64_t fmss_runs, dma_in, dma_out;
    NandStore nand;
} Nano7FmcState;

static Nano7FmcState *fmc_singleton;
static FILE *fmc_tf;                /* NANO7_FMC_TRACE=file: raw reg trace */

static void fmss_run(Nano7FmcState *s);

/* Totals survive the log cap: printed at exit (QEMU exits on SIGTERM too). */
static void fmc_summary(void)
{
    if (fmc_singleton) {
        fprintf(stderr, "[nano7-fmc] summary: %" PRIu64 " FMSS runs, DMA in %"
                PRIu64 " B / out %" PRIu64 " B\n", fmc_singleton->fmss_runs,
                fmc_singleton->dma_in, fmc_singleton->dma_out);
        fprintf(stderr, "[nano7-fmc] NAND store: %" PRIu64 " pages / %" PRIu64
                " sectors programmed, %u pages held; read sectors: %" PRIu64
                " hit, %" PRIu64 " miss (%" PRIu64 " oracle-filled)\n",
                fmc_singleton->nand.pages_prog, fmc_singleton->nand.sect_prog,
                g_hash_table_size(fmc_singleton->nand.idx),
                fmc_singleton->nand.sect_hit, fmc_singleton->nand.sect_miss,
                fmc_singleton->nand.oracle_fixed);
    }
}
static void fmss_update_irq(Nano7FmcState *s)
{
    if (s->irq) {
        qemu_set_irq(s->irq, (s->cs_status & 0xd) &&
                             (s->regs[FMSS_IRQEN / 4] & 1));
    }
}

static uint32_t fmc_guest_pc(void)
{
    return current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0;
}

static void G_GNUC_PRINTF(2, 3) fmc_log(Nano7FmcState *s, const char *fmt, ...)
{
    va_list ap;

    if (s->log_lines >= FMC_LOG_MAX) {
        return;
    }
    if (++s->log_lines == FMC_LOG_MAX) {
        fprintf(stderr, "[nano7-fmc] log limit reached, going quiet\n");
        return;
    }
    fprintf(stderr, "[nano7-fmc] pc=0x%08x ", fmc_guest_pc());
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* Trace the FMSS / upper register window (+0x800..), polls collapsed. */
static void fmc_trace(Nano7FmcState *s, bool wr, hwaddr off, uint32_t v)
{
    uint32_t key[3] = { wr, off, v };

    if (off < 0x800 || s->in_vm || !memcmp(key, s->last_trace, sizeof(key))) {
        return;
    }
    memcpy(s->last_trace, key, sizeof(key));
    fmc_log(s, "%s +0x%03x %s 0x%08x", wr ? "W" : "R", (unsigned)off,
            wr ? "=" : "->", v);
}

static void fmc_put32(uint8_t *p, uint32_t v)
{
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

/* Device parameters page (cmd 0x92), checked by 0x0812ad5c. */
static void fmc_fill_params(uint8_t *p)
{
    memset(p, 0, FMC_BUF_MAX);
    fmc_put32(p + 0x10, 8);         /* caus_per_channel  1..64 */
    fmc_put32(p + 0x14, 4);         /* cau_bits          1..7 */
    fmc_put32(p + 0x18, 0x1000);    /* blocks_per_cau    0x400..0x2134 */
    fmc_put32(p + 0x1c, 12);        /* block_bits        10..15 */
    fmc_put32(p + 0x20, 128);       /* pages_per_block   64..512 */
    fmc_put32(p + 0x24, 64);        /* pages_per_block_slc 64..256 */
    fmc_put32(p + 0x28, 7);         /* page_address_bits 5..9 (1<<n=ppb) */
    fmc_put32(p + 0x2c, 1);         /* 1..2 */
    fmc_put32(p + 0x30, 2);         /* 1..4 */
    fmc_put32(p + 0x34, 0x4000);    /* page_size 0x2020..0x4040 */
    memset(p + 0xa0, 20, 7);        /* timings tRC..tWP 1..255 */
    memset(p + 0xa8, 20, 3);
    fmc_put32(p + 0xe0, 0x200);     /* 4..0x2000 */
    fmc_put32(p + 0xe4, 0x200);     /* 2..0x2000 */
    fmc_put32(p + 0xe8, 0x200);     /* 4..0x2000 */
    fmc_put32(p + 0xec, 0x200);     /* 4..0x2000 */
    fmc_put32(p + 0xf0, 10);        /* 1..50 */
    fmc_put32(p + 0xf4, 10);        /* 1..50 */
    fmc_put32(p + 0xf8, 100);       /* 1..200 */
    fmc_put32(p + 0xfc, 100);       /* 1..200 */
}

static void fmc_set_buf(Nano7FmcState *s, const void *data, unsigned len)
{
    memcpy(s->buf, data, len);
    s->buf_len = len;
    s->buf_pos = 0;
}


/* ------------------------------------------------- NAND page store --- */
static int64_t nand_lookup(NandStore *n, uint32_t ce, uint32_t addr)
{
    uint64_t k = (uint64_t)ce << 32 | addr;
    gpointer v = g_hash_table_lookup(n->idx, &k);

    return v ? (int64_t)GPOINTER_TO_SIZE(v) - 1 : -1;
}

static int64_t nand_insert(NandStore *n, uint32_t ce, uint32_t addr,
                           uint32_t slot)
{
    uint64_t *k = g_new(uint64_t, 1);

    *k = (uint64_t)ce << 32 | addr;
    g_hash_table_replace(n->idx, k, GSIZE_TO_POINTER((gsize)slot + 1));
    return slot;
}

static void nand_open(NandStore *n, const char *path)
{
    NandHdr h;
    uint32_t i;
    off_t size;

    if (n->fd >= 0) {
        close(n->fd);
    }
    if (n->idx) {
        g_hash_table_destroy(n->idx);
    }
    n->idx = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
    n->slots = 0;
    n->slot = -1;
    if (path) {
        n->fd = open(path, O_RDWR | O_CREAT, 0644);
        if (n->fd < 0) {
            fprintf(stderr, "[nano7-fmc] nand-store %s: %s\n", path,
                    strerror(errno));
            exit(1);
        }
    } else {
        gchar *tmp = NULL;

        n->fd = g_file_open_tmp("nano7-nand-XXXXXX", &tmp, NULL);
        if (n->fd < 0) {
            fprintf(stderr, "[nano7-fmc] cannot create a temp NAND store\n");
            exit(1);
        }
        unlink(tmp);
        g_free(tmp);
    }
    size = lseek(n->fd, 0, SEEK_END);
    for (i = 0; i < size / NAND_REC; i++) {
        if (pread(n->fd, &h, sizeof(h), (off_t)i * NAND_REC) == sizeof(h) &&
            h.magic == NAND_MAGIC) {
            nand_insert(n, h.ce, h.addr, i);
        }
    }
    n->slots = size / NAND_REC;
    if (path) {
        fprintf(stderr, "[nano7-fmc] nand-store %s: %u pages\n", path,
                g_hash_table_size(n->idx));
    }
}

static void nand_end(Nano7FmcState *s)
{
    NandStore *n = &s->nand;

    if (n->mode == NAND_PROG && n->slot >= 0 &&
        pwrite(n->fd, &n->cur, sizeof(n->cur), n->slot * NAND_REC) !=
        sizeof(n->cur)) {
        fmc_log(s, "NAND store write failed: %s", strerror(errno));
    }
    n->mode = NAND_IDLE;
}

static void nand_prog_begin(Nano7FmcState *s, uint32_t addr)
{
    NandStore *n = &s->nand;
    int64_t slot = nand_lookup(n, s->ce, addr);

    nand_end(s);
    if (slot < 0) {
        slot = nand_insert(n, s->ce, addr, n->slots++);
    }
    memset(&n->cur, 0, sizeof(n->cur));
    n->cur.magic = NAND_MAGIC;
    n->cur.ce = s->ce;
    n->cur.addr = addr;
    n->slot = slot;
    n->sect = -1;
    n->last_sect = NAND_SECTS - 1;
    n->mode = NAND_PROG;
    n->pages_prog++;
}

static void nand_read_queue(Nano7FmcState *s, uint32_t w0, uint32_t addr)
{
    NandStore *n = &s->nand;
    NandRead *r;

    if (n->rdq_n[s->ce] == NAND_RDQ) {
        fmc_log(s, "NAND read queue full (ce %d)", s->ce);
        return;
    }
    r = &n->rdq[s->ce][(n->rdq_head[s->ce] + n->rdq_n[s->ce]++) % NAND_RDQ];
    r->addr = addr;
    r->count = (w0 & 0xff) >> 4;
    r->first = (w0 >> 20) & 0xf;
}

/* cmd 0x7a: data out for the oldest queued read on this CE */
static void nand_read_begin(Nano7FmcState *s)
{
    NandStore *n = &s->nand;
    NandRead r;

    nand_end(s);
    if (s->ce < 0 || s->ce >= FMC_NUM_CE || !n->rdq_n[s->ce]) {
        fmc_log(s, "NAND 0x7a with no queued read (ce %d)", s->ce);
        return;
    }
    r = n->rdq[s->ce][n->rdq_head[s->ce]];
    n->rdq_head[s->ce] = (n->rdq_head[s->ce] + 1) % NAND_RDQ;
    n->rdq_n[s->ce]--;
    n->slot = nand_lookup(n, s->ce, r.addr);
    if (n->slot < 0 || pread(n->fd, &n->cur, sizeof(n->cur),
                             n->slot * NAND_REC) != sizeof(n->cur) ||
        n->cur.magic != NAND_MAGIC) {
        memset(&n->cur, 0, sizeof(n->cur));
        n->slot = -1;
    }
    n->cur.ce = s->ce;
    n->cur.addr = r.addr;
    n->sect = (int)r.first - 1;
    n->last_sect = MIN((int)r.first + r.count - 1, NAND_SECTS - 1);
    n->mode = NAND_READ;
}

static bool nand_sect_ok(NandStore *n)
{
    return n->sect >= 0 && n->sect <= n->last_sect;
}

static bool nand_hit(NandStore *n)
{
    return n->slot >= 0 && (n->cur.valid >> n->sect & 1);
}

/* go 0x34 (program): the 16-byte meta in +0x60..+0x6c goes out */
static void nand_meta_out(Nano7FmcState *s)
{
    NandStore *n = &s->nand;

    n->sect++;
    n->chunk = 0;
    if (nand_sect_ok(n)) {
        memcpy(n->cur.meta[n->sect], &s->regs[0x60 / 4], NAND_META_BYTES);
        n->cur.valid |= 1u << n->sect;
    }
}

/* go 0x32 (read): the sector's meta comes in to +0x60..+0x6c */
static void nand_meta_in(Nano7FmcState *s)
{
    NandStore *n = &s->nand;
    unsigned i;

    n->sect++;
    n->chunk = 0;
    n->cur_miss = false;
    if (!nand_sect_ok(n)) {
        return;
    }
    if (nand_hit(n)) {
        memcpy(&s->regs[0x60 / 4], n->cur.meta[n->sect], NAND_META_BYTES);
        n->sect_hit++;
        if (getenv("NANO7_NAND_TRACE")) {
            fprintf(stderr, "[nano7-nand] t=%.3f s hit ce%d page 0x%x s%d lba "
                    "0x%x\n", qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9,
                    s->ce, n->cur.addr, n->sect, n->cur.meta[n->sect][2]);
        }
    } else {
        memset(&s->regs[0x60 / 4], 0xff, NAND_META_BYTES);
        n->sect_miss++;
        n->cur_miss = true;
        if (n->nmiss < NAND_MISS_MAX) {
            NandMiss *m = &n->miss[n->nmiss++];

            memset(m, 0, sizeof(*m));
            m->ce = s->ce;
            m->addr = n->cur.addr;
            m->sect = n->sect;
            n->want_meta_dst = true;
        }
    }
    for (i = 0x60; i < 0x70; i += 4) {
        s->written[i / 4] = true;
    }
}

/*
 * +0x34 write inside a page. Programs write it once per 1 KB chunk (4 per
 * sector), reads once per sector (the engine then streams 4 KB). Each write
 * moves the rest of the sector from chunk * 1 KB on; later writes override.
 */
static bool nand_dma(Nano7FmcState *s, uint32_t dram)
{
    NandStore *n = &s->nand;
    static uint8_t buf[NAND_SECT];
    unsigned pos, len;
    off_t off;

    if (n->mode == NAND_IDLE) {
        return false;
    }
    if (!nand_sect_ok(n)) {
        return true;
    }
    pos = MIN(n->chunk++, 3u) * 1024;
    len = NAND_SECT - pos;
    off = n->slot * NAND_REC + NAND_HDR + n->sect * NAND_SECT + pos;
    dram &= 0x7fffffff;     /* read buffers come with bit31 set */
    if (n->mode == NAND_PROG) {
        address_space_read(&address_space_memory, dram,
                           MEMTXATTRS_UNSPECIFIED, buf, len);
        if (pwrite(n->fd, buf, len, off) != len) {
            fmc_log(s, "NAND store write failed: %s", strerror(errno));
        }
        if (!pos) {
            n->sect_prog++;
        }
        s->dma_out += 1024;
        return true;
    }
    if (!pos) {
        n->data_lo = MIN(n->data_lo, dram);
    }
    if (nand_hit(n) && pread(n->fd, buf, len, off) == len) {
        /* data from the store */
    } else {
        memset(buf, 0xff, len);
        if (n->cur_miss && n->nmiss && !pos) {
            n->miss[n->nmiss - 1].data_dst = dram;
        }
    }
    address_space_write(&address_space_memory, dram, MEMTXATTRS_UNSPECIFIED,
                        buf, len);
    s->dma_in += len;
    return true;
}

static void fmc_do_cmd(Nano7FmcState *s, uint8_t cmd)
{
    s->last_cmd = cmd;
    if (!s->in_vm) {
        fmc_log(s, "CMD 0x%02x ce=%d", cmd, s->ce);
    }
    if (cmd == 0x7a) {
        nand_read_begin(s);
    } else if (s->nand.mode != NAND_IDLE) {
        nand_end(s);        /* 0x17 confirms a program; reads end on any */
    }
    switch (cmd) {
    case 0x90: /* READID */
    case 0x92: /* read device parameters */
    case 0xee: /* get feature / controller version */
    case 0xef: /* set features */
    case 0x00: /* page read */
    case 0xff: /* reset */
        s->cur_cmd = cmd;
        s->buf_len = s->buf_pos = 0;
        break;
    default:   /* E7/77/7D/97/7A/30/70...: keep the pending response */
        break;
    }
}

static void fmc_do_addr(Nano7FmcState *s)
{
    static const uint8_t ppn_id[8] = { 'P', 'P', 'N', 0x01, 0x00, 0x00 };
    static const uint8_t mfg_id[8] = { 0x45 };
    static const char ctrl_ver[17] = "NANO7EMU PPN1.00";
    unsigned n = (s->regs[0x2c / 4] & 7) + 1;
    uint8_t a[8], absent[16];
    bool present = s->ce >= 0 && s->ce < FMC_NUM_CE;
    unsigned i;

    for (i = 0; i < n; i++) {
        a[i] = s->regs[(0x0c + (i & ~3u)) / 4] >> (8 * (i & 3));
    }
    if (!s->in_vm) fmc_log(s, "ADDR n=%u %02x %02x %02x %02x %02x (cmd 0x%02x ce=%d)",
            n, a[0], n > 1 ? a[1] : 0, n > 2 ? a[2] : 0, n > 3 ? a[3] : 0,
            n > 4 ? a[4] : 0, s->cur_cmd, s->ce);
    if (present && n == 4 && (s->last_cmd == 0x8a || s->last_cmd == 0x87)) {
        nand_prog_begin(s, s->regs[0x0c / 4]);
    } else if (present && n == 8 &&
               (s->last_cmd == 0x07 || s->last_cmd == 0x0a)) {
        nand_read_queue(s, s->regs[0x0c / 4], s->regs[0x10 / 4]);
    }
    memset(absent, 0xff, sizeof(absent));
    switch (s->cur_cmd) {
    case 0x90:
        if (!present) {
            fmc_set_buf(s, absent, 8);
        } else if (a[0] == 0x30) {
            fmc_set_buf(s, mfg_id, 8);
        } else {
            fmc_set_buf(s, ppn_id, 8);
        }
        break;
    case 0xee:
        if (present) {
            fmc_set_buf(s, ctrl_ver, 16);
        } else {
            fmc_set_buf(s, absent, 16);
        }
        break;
    case 0x92:
        if (present) {
            fmc_fill_params(s->buf);
            s->buf_len = FMC_BUF_MAX;
            s->buf_pos = 0;
        } else {
            s->buf_len = s->buf_pos = 0;
        }
        break;
    default:
        s->buf_len = s->buf_pos = 0;
        break;
    }
}

static uint32_t fmc_pop(Nano7FmcState *s)
{
    uint32_t v = 0;
    unsigned i;

    for (i = 0; i < 4; i++) {
        uint8_t b = s->buf_pos < s->buf_len ? s->buf[s->buf_pos] : 0xff;
        s->buf_pos++;
        v |= (uint32_t)b << (8 * i);
    }
    s->popped += 4;
    return v;
}

static uint64_t fmc_read(void *opaque, hwaddr offset, unsigned size)
{
    Nano7FmcState *s = opaque;
    uint32_t idx = offset >> 2;
    uint32_t v;

    switch (offset & ~3) {
    case 0x28:
        v = s->popped;
        break;
    case 0x48:
        v = s->status;
        break;
    case 0x40:
        /* bit0/bit1 are strobes the sequencer polls for completion */
        v = (s->written[idx] ? s->regs[idx] : 0) & ~3u;
        break;
    case 0x4c:
        /* PPN status byte: 0x62 after 0x79 (op status: ready, array
         * ready, bit1 pass), else 0x40 (ready, no error) */
        v = s->last_cmd == 0x79 ? 0x62 : 0x40;
        break;
    case 0x60: case 0x64: case 0x68: case 0x6c:
        v = s->written[idx] ? s->regs[idx] : 0;   /* ECC results: clean */
        break;
    case FMSS_STATUS:
        v = s->cs_status;
        break;
    case FMSS_PC:
        v = s->fmss_pc;
        break;
    case 0x80:
        v = fmc_pop(s);
        if (s->buf_pos <= 16 || s->buf_pos % 128 == 0) {
            fmc_log(s, "DATA[%u] = 0x%08x", s->buf_pos - 4, v);
        }
        break;
    default:
        v = s->written[idx] ? s->regs[idx] : 0xffffffff;
        break;
    }
    fmc_trace(s, false, offset, v);
    if (fmc_tf && (offset & ~3) != 0x48 && (offset & ~3) != 0x40 &&
        (offset & ~3) < 0xd00) {
        fprintf(fmc_tf, "%c R %03x %08x\n", s->in_vm ? 'v' : 'c',
                (unsigned)(offset & ~3), v);
    }
    return (v >> (8 * (offset & 3))) & MAKE_64BIT_MASK(0, size * 8);
}

static void fmc_write(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    Nano7FmcState *s = opaque;
    uint32_t idx = offset >> 2;
    uint32_t v = value;

    if (size != 4) {
        unsigned sh = 8 * (offset & 3);
        uint32_t m = MAKE_64BIT_MASK(sh, size * 8);
        v = (s->regs[idx] & ~m) | ((v << sh) & m);
    }
    fmc_trace(s, true, offset, v);
    if (fmc_tf && (offset & ~3) != 0x48) {
        fprintf(fmc_tf, "%c W %03x %08x\n", s->in_vm ? 'v' : 'c',
                (unsigned)(offset & ~3), v);
    }
    switch (offset & ~3) {
    case 0x00:
        if (v == 1) {
            s->ce = -1;
        } else if (v & 1) {
            uint32_t mask = (v >> 1) & 0xff;
            s->ce = mask ? ctz32(mask) : -1;
        }
        break;
    case 0x04:
        switch (v) {
        case 0x01:
            fmc_do_addr(s);
            break;
        case 0x02: case 0x22: case 0x1e0: case 0xea: case 0x20:
            break;
        default:
            if (!s->in_vm) {
                fmc_log(s, "GO 0x%08x (cmd 0x%02x ce=%d)", v, s->cur_cmd,
                        s->ce);
            }
            break;
        }
        /* DMA data phase (FMSS read/program): +0x34 = DRAM address,
         * +0x30 = length-1. bit8 = flash->DRAM (no backing store: blank
         * 0xff pages), bit9 = DRAM->flash (accepted, discarded). */
        if (s->nand.mode == NAND_PROG && v == 0x34) {
            nand_meta_out(s);
        } else if (s->nand.mode == NAND_READ && v == 0x32) {
            nand_meta_in(s);
        }
        if (s->nand.mode == NAND_IDLE && (v & 0x300) &&
            s->written[0x34 / 4]) {
            uint32_t len = (s->regs[0x30 / 4] & 0xffff) + 1;
            if (v & 0x100) {
                static uint8_t blank[0x10000];
                if (!blank[0]) {
                    memset(blank, 0xff, sizeof(blank));
                }
                address_space_write(&address_space_memory, s->regs[0x34 / 4],
                                    MEMTXATTRS_UNSPECIFIED, blank,
                                    MIN(len, sizeof(blank)));
                s->dma_in += len;
            } else {
                s->dma_out += len;
            }
        }
        s->status |= FMC_DONE_BITS;
        break;
    case 0x08:
        /* the command is issued on this write (the shutdown-path reset
         * waits for bit1 without a +0x04 "go"); +0x04 = 2 only re-arms */
        fmc_do_cmd(s, v & 0xff);
        s->status |= FMC_DONE_BITS;
        break;
    case 0x28:
        s->popped = v;
        break;
    case 0x34:
        nand_dma(s, v);
        break;
    case 0x48:
        s->status &= ~v;    /* write-1-to-clear */
        break;
    case FMSS_STATUS:
        s->cs_status &= ~v; /* write-1-to-clear (0x8156008 writes 0xd) */
        s->regs[idx] = v;
        s->written[idx] = true;
        fmss_update_irq(s);
        return;
    case FMSS_IRQEN:
        s->regs[idx] = v;
        s->written[idx] = true;
        fmss_update_irq(s);
        return;
    case FMSS_KICK:
        s->regs[idx] = v;
        s->written[idx] = true;
        if (v) {
            fmss_run(s);
        }
        return;
    }
    s->regs[idx] = v;
    s->written[idx] = true;
}


/*
 * nand-miss=oracle: a sector that was programmed before the snapshot has no
 * content here. Give it the meta s_verify_meta (0x0854b9f0, snap6 build)
 * will expect: G = 0x08d0c6a0, extents {u64 start, u64 count} at [G+0x2d8],
 * offset [G+0x2dc]; batch entry i walks them as the verify does. The read
 * program stages meta in a driver buffer (no FTL slot in its descriptors),
 * so i comes from the data DMA order: the batch fills the FTL buffer 4 KB
 * per entry, i = (data - lowest data address in this run) / 4 KB.
 * Data comes from nand-disk= (LBA * 4 KB) or zeros. Runs after the FMSS
 * program, before its completion IRQ, so the driver never sees the blank.
 */
#define SFTL_G          0x08d0c6a0u

static uint32_t nand_gl(uint32_t a)
{
    return address_space_ldl_le(&address_space_memory, a,
                                MEMTXATTRS_UNSPECIFIED, NULL);
}

static void nand_oracle(Nano7FmcState *s)
{
    NandStore *n = &s->nand;
    uint32_t ext0 = nand_gl(SFTL_G + 0x2d8), off0 = nand_gl(SFTL_G + 0x2dc);
    static uint8_t data[NAND_SECT];
    unsigned k;

    for (k = 0; k < n->nmiss; k++) {
        NandMiss *m = &n->miss[k];
        uint32_t ext = ext0, off = off0, meta[4], i, j;
        uint64_t lba = 0;

        if (!m->meta_dst || !m->data_dst ||
            (m->data_dst - n->data_lo) % NAND_SECT ||
            (m->data_dst - n->data_lo) / NAND_SECT >= 256) {
            if (n->oracle_skip++ < 32) {
                fprintf(stderr, "[nano7-nand] miss ce%u page 0x%x s%u: data "
                        "0x%08x not in a 4 KB batch from 0x%08x, left "
                        "blank\n", m->ce, m->addr, m->sect, m->data_dst,
                        n->data_lo);
            }
            continue;
        }
        i = (m->data_dst - n->data_lo) / NAND_SECT;
        for (j = 0; j <= i; j++, off++) {
            uint64_t cnt = nand_gl(ext + 8) | (uint64_t)nand_gl(ext + 12) << 32;

            if (off >= cnt) {
                off = 0;
                ext += 16;
            }
            lba = (nand_gl(ext) | (uint64_t)nand_gl(ext + 4) << 32) + off;
        }
        meta[0] = 0x00000001;   /* type 1 (data), no uECC, seq 0 */
        meta[1] = 0;
        meta[2] = lba;
        meta[3] = 0;
        address_space_write(&address_space_memory, m->meta_dst,
                            MEMTXATTRS_UNSPECIFIED, meta, sizeof(meta));
        memset(data, 0, sizeof(data));
        if (n->disk_fd >= 0 && pread(n->disk_fd, data, NAND_SECT,
                                     (off_t)lba * NAND_SECT) < 0) {
            memset(data, 0, sizeof(data));
        }
        address_space_write(&address_space_memory, m->data_dst,
                            MEMTXATTRS_UNSPECIFIED, data, NAND_SECT);
        if (n->oracle_fixed++ < 32 || !(n->oracle_fixed & 0x3ff) ||
            getenv("NANO7_NAND_TRACE")) {
            fprintf(stderr, "[nano7-nand] miss ce%u page 0x%x s%u -> oracle "
                    "lba 0x%" PRIx64 " (sftl slot %u, data 0x%08x, #%" PRIu64
                    ")\n", m->ce, m->addr, m->sect, lba, i, m->data_dst,
                    n->oracle_fixed);
        }
    }
}

void nano7_fmc_nand_config(const char *store, const char *miss,
                           const char *disk)
{
    Nano7FmcState *s = fmc_singleton;

    if (!s) {
        return;
    }
    if (store) {
        nand_open(&s->nand, store);
    }
    if (miss && !strcmp(miss, "oracle")) {
        s->nand.miss_policy = NAND_MISS_ORACLE;
    } else if (miss && strcmp(miss, "blank")) {
        fprintf(stderr, "[nano7-fmc] nand-miss=%s: want blank|oracle\n", miss);
        exit(1);
    }
    if (disk) {
        s->nand.disk_fd = open(disk, O_RDONLY);
        if (s->nand.disk_fd < 0) {
            fprintf(stderr, "[nano7-fmc] nand-disk %s: %s\n", disk,
                    strerror(errno));
            exit(1);
        }
    }
}

/* ---------------------------------------------------- FMSS sequencer --- */
static uint32_t fmss_rd(Nano7FmcState *s, uint32_t off)
{
    return fmc_read(s, off, 4);
}

static void fmss_wr(Nano7FmcState *s, uint32_t off, uint32_t v)
{
    fmc_write(s, off, v, 4);
}

static uint32_t fmss_ldl(uint32_t a)
{
    return address_space_ldl_le(&address_space_memory, a,
                                MEMTXATTRS_UNSPECIFIED, NULL);
}

static void fmss_run(Nano7FmcState *s)
{
    uint32_t base = s->regs[FMSS_SEQ / 4];
    uint32_t r[16] = { 0 };
    uint32_t pc = 0, steps = 0, result;
    bool halted = false, fault = false;

    s->fmss_runs++;
    if (!s->written[FMSS_SEQ / 4] && s->resume_kick_ok) {
        /* The sequence was armed (+0xc04, mailbox) before the snapshot and
         * ran on the device; report it complete with result 0 (success). */
        s->resume_kick_ok = false;
        s->regs[FMSS_MBOX / 4] = 0;
        s->written[FMSS_MBOX / 4] = true;
        s->cs_status |= 1;
        s->regs[FMSS_IRQEN / 4] = 1;    /* every program enables it first */
        s->written[FMSS_IRQEN / 4] = true;
        fmc_log(s, "FMSS kick #%" PRIu64 " with no sequence base: reported "
                "complete (resume)", s->fmss_runs);
        fmss_update_irq(s);
        return;
    }
    if (!s->written[FMSS_SEQ / 4]) {
        /* e.g. a resumed thread kicking a sequence set up before capture:
         * +0xc04 / mailbox were MMIO and are not in the snapshot */
        fmc_log(s, "FMSS kick #%" PRIu64 " with no sequence base -> error",
                s->fmss_runs);
        s->cs_status |= 4;
        fmss_update_irq(s);
        return;
    }
    if (fmc_tf) {
        fprintf(fmc_tf, "RUN %" PRIu64 " seq %08x mbox", s->fmss_runs, base);
        for (unsigned i = 0; i < 0x30; i += 4) {
            fprintf(fmc_tf, " %08x", s->regs[(FMSS_MBOX + i) / 4]);
        }
        fputc('\n', fmc_tf);
    }
    s->nand.nmiss = 0;
    s->nand.want_meta_dst = false;
    s->nand.data_lo = UINT32_MAX;
    s->in_vm = true;
    while (!halted && steps++ < FMSS_MAX_STEPS) {
        uint32_t len = s->written[FMSS_LEN / 4] ? s->regs[FMSS_LEN / 4] : 0x4000;
        uint32_t w0, w1, op, d, imm16, sr, a, b, v = 0, npc = pc + 8;
        bool alu = true;

        if (pc >= len) {
            break;
        }
        w0 = fmss_ldl(base + pc);
        w1 = fmss_ldl(base + pc + 4);
        op = w0 >> 24;
        d = (w0 >> 16) & 0xf;
        imm16 = w0 & 0xffff;
        sr = w0 & 0xf;
        r[8] = pc;
        /* ALU form: imm != 0 -> d = R[s] op imm; imm == 0 -> d = R[d] op R[s] */
        a = w1 ? r[sr] : r[d];
        b = w1 ? w1 : r[sr];
        switch (op) {
        case 0x00: halted = true; alu = false; break;          /* HALT */
        case 0x01: fmss_wr(s, imm16, w1); alu = false; break;  /* MMIO = imm */
        case 0x02: fmss_wr(s, imm16, r[d]); alu = false; break; /* MMIO = R */
        case 0x03: v = fmss_ldl(r[sr]); break;                 /* R = mem */
        case 0x04: v = fmss_rd(s, imm16) & w1; break;          /* R = MMIO&m */
        case 0x05: v = w1; break;                              /* R = imm */
        case 0x06: v = r[sr]; break;                           /* R = R */
        case 0x07: alu = false; break;  /* wait FMC event: model is instant */
        case 0x0a: v = a & b; break;
        case 0x0b: v = a | b; break;
        case 0x0c: v = a + b; break;
        case 0x0d: v = a - b; break;
        case 0x13: v = a << (b & 31); break;
        case 0x14: v = a >> (b & 31); break;
        case 0x0e: if (r[d]) { npc = w1; } alu = false; break; /* BNEZ */
        case 0x17: if (!r[d]) { npc = w1; } alu = false; break; /* BEQZ */
        case 0x11:                                             /* mem = R */
            if (s->nand.want_meta_dst && s->nand.nmiss) {
                /* first store after a missed meta-in: its FTL meta slot */
                s->nand.miss[s->nand.nmiss - 1].meta_dst = r[sr] & 0x7fffffff;
                s->nand.want_meta_dst = false;
            }
            address_space_stl_le(&address_space_memory, r[sr], r[d],
                                 MEMTXATTRS_UNSPECIFIED, NULL);
            alu = false;
            break;
        case 0x12: v = fmss_rd(s, FMSS_MBOX + 0x20); break;   /* R = GEN8 */
        default:
            fault = true;
            halted = true;
            alu = false;
            break;
        }
        if (alu) {
            if (d == 8) {
                npc = v;            /* r8 is the program counter */
            } else {
                r[d] = v;
            }
        }
        pc = npc;
    }
    s->in_vm = false;
    nand_end(s);
    if (s->nand.nmiss && s->nand.miss_policy == NAND_MISS_ORACLE) {
        nand_oracle(s);
    }
    s->fmss_pc = pc;
    result = s->regs[FMSS_MBOX / 4];
    if (fault || steps >= FMSS_MAX_STEPS) {
        s->cs_status |= 4;          /* sequence error */
        fmc_log(s, "FMSS seq 0x%08x %s at pc 0x%x (insn %08x) after %u steps",
                base, fault ? "UNKNOWN OP" : "RUNAWAY", pc,
                fmss_ldl(base + pc), steps);
    } else {
        s->cs_status |= 1;          /* done */
    }
    if (s->fmss_runs <= 40 || (s->fmss_runs & (s->fmss_runs - 1)) == 0) {
        fmc_log(s, "FMSS run #%" PRIu64 " seq 0x%08x args %08x %08x %08x %08x "
                "-> d00=0x%x steps=%u dma in/out %" PRIu64 "/%" PRIu64,
                s->fmss_runs, base, s->regs[(FMSS_MBOX + 4) / 4],
                s->regs[(FMSS_MBOX + 8) / 4], s->regs[(FMSS_MBOX + 0xc) / 4],
                s->regs[(FMSS_MBOX + 0x10) / 4], result, steps,
                s->dma_in, s->dma_out);
    }
    fmss_update_irq(s);
}

void nano7_fmc_fmss_resume_done(void)
{
    Nano7FmcState *s = fmc_singleton;

    /* A sequence was in flight on the device when RAM was captured;
     * report it finished so the waiting driver thread continues. */
    if (s) {
        s->cs_status |= 1;
        s->regs[FMSS_IRQEN / 4] = 1;
        s->written[FMSS_IRQEN / 4] = true;
        fmss_update_irq(s);
    }
}

void nano7_fmc_fmss_resume_kick(void)
{
    if (fmc_singleton) {
        fmc_singleton->resume_kick_ok = true;
    }
}

static const MemoryRegionOps fmc_ops = {
    .read = fmc_read,
    .write = fmc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

void nano7_fmc_attach(MemoryRegion *parent, hwaddr base, qemu_irq irq)
{
    Nano7FmcState *s = g_new0(Nano7FmcState, 1);

    s->irq = irq;
    fmc_singleton = s;
    atexit(fmc_summary);
    if (getenv("NANO7_FMC_TRACE")) {
        fmc_tf = fopen(getenv("NANO7_FMC_TRACE"), "w");
    }
    s->base = base;
    s->ce = -1;
    s->nand.fd = -1;
    s->nand.disk_fd = -1;
    nand_open(&s->nand, NULL);
    memory_region_init_io(&s->mr, NULL, &fmc_ops, s, "nano7.fmc", FMC_SIZE);
    memory_region_add_subregion_overlap(parent, base, &s->mr, 0);
}
