/*
 * nano7_disp.c -- 0x39000000 display pipe (layer compositor) for the iPod
 * nano 7G (S5L8747). Frame done on VIC IRQ 37.
 *
 * Register use, as RetailOS drives it (Session 14 RE; the driver is a
 * command-ring interpreter 0x081baae0, register pointer table 0x0890b788):
 *   +0x00  status        bit0 idle/ready (init + power-off spin for it),
 *                        bit1 frame running (submit 0x083bab28 won't kick)
 *   +0x04  kick          ring command 10 ORs in 1 (one-shot)
 *   +0x08  irq enable    bit0 (power-on 0x081bb6f4 sets, power-off clears)
 *   +0x0c  irq status    bit0 frame done, W1C (ISR 0x081bbec6 writes |1)
 *   +0x10.. layer 0      +0x10 format (6 = ARGB8888), +0x14 FB address,
 *                        +0x1c stride (pixels), +0x24 size (w << 16 | h)
 *   +0x30.. layer 1      +0x30 format, +0x34 FB, +0x3c stride, +0x40 size
 *   +0x260 version       driver wants >= 0x60005
 * Other offsets (+0x280 blend, +0x60.. LUTs, +0x220..) are stored.
 *
 * Model: a kick sets +0x00 = 2 (running); 16 ms later +0x00 = 1, +0x0c
 * bit0 = 1, IRQ 37 while (+0x0c & +0x08 & 1). With disp-dump=DIR each
 * frame whose layer contents changed is written as DIR/frame-NNNN-lK.ppm
 * (alpha dropped).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "nano7_disp.h"
#include "hw/core/cpu.h"
#include "qemu/timer.h"
#include "target/arm/cpu.h"
#include "exec/address-spaces.h"

#define DISP_SIZE       0x1000
#define DISP_FRAME_NS   16000000    /* ~60 Hz */
#define DISP_LOG_MAX    64      /* then every 256th kick */
#define DISP_DUMP_MAX   500
#define DISP_MAX_W      1024
#define DISP_MAX_H      1024

typedef struct Nano7DispState {
    MemoryRegion mr;
    uint32_t regs[DISP_SIZE / 4];
    uint32_t status, irq_status;
    QEMUTimer *timer;
    qemu_irq irq;
    char *dump_dir;
    uint64_t kicks, frames, dumps;
    uint32_t last_hash[2];
    unsigned log_lines;
} Nano7DispState;

static Nano7DispState *disp_singleton;

static uint32_t disp_guest_pc(void)
{
    return current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0;
}

static void disp_summary(void)
{
    if (disp_singleton) {
        fprintf(stderr, "[nano7-disp] summary: %" PRIu64 " kicks, %" PRIu64
                " frames done, %" PRIu64 " layer dumps\n",
                disp_singleton->kicks, disp_singleton->frames,
                disp_singleton->dumps);
    }
}

static void disp_update(Nano7DispState *s)
{
    qemu_set_irq(s->irq, (s->irq_status & s->regs[0x08 / 4] & 1) != 0);
}

/* Write one layer as a binary PPM if its contents changed since the last
 * dump. fmt/addr/stride/size are word offsets of that layer's registers. */
static void disp_dump_layer(Nano7DispState *s, int l, hwaddr fmt,
                            hwaddr addr, hwaddr stride, hwaddr size)
{
    uint32_t fb = s->regs[addr / 4], sz = s->regs[size / 4];
    uint32_t w = sz >> 16, h = sz & 0xffff, pitch = s->regs[stride / 4];
    uint32_t hash = 2166136261u, x, y;
    g_autofree uint32_t *row = NULL;
    g_autofree uint8_t *rgb = NULL;
    g_autofree char *path = NULL;
    FILE *f;

    if (!fb || !w || !h || w > DISP_MAX_W || h > DISP_MAX_H ||
        s->regs[fmt / 4] != 6) {
        return;
    }
    if (pitch < w) {
        pitch = w;
    }
    row = g_new(uint32_t, w);
    rgb = g_malloc((size_t)w * h * 3);
    for (y = 0; y < h; y++) {
        address_space_read(&address_space_memory,
                           (fb & 0x7fffffff) + (hwaddr)y * pitch * 4,
                           MEMTXATTRS_UNSPECIFIED, row, w * 4);
        for (x = 0; x < w; x++) {
            uint32_t p = le32_to_cpu(row[x]);   /* ARGB8888 */
            uint8_t *o = rgb + ((size_t)y * w + x) * 3;

            o[0] = p >> 16;
            o[1] = p >> 8;
            o[2] = p;
            hash = (hash ^ p) * 16777619u;
        }
    }
    if (s->dumps && hash == s->last_hash[l]) {
        return;
    }
    s->last_hash[l] = hash;
    if (s->dumps >= DISP_DUMP_MAX) {
        return;
    }
    path = g_strdup_printf("%s/frame-%04" PRIu64 "-l%d.ppm", s->dump_dir,
                           s->frames, l);
    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[nano7-disp] cannot write %s\n", path);
        return;
    }
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    fwrite(rgb, 3, (size_t)w * h, f);
    fclose(f);
    s->dumps++;
    fprintf(stderr, "[nano7-disp] frame %" PRIu64 " layer %d FB 0x%08x "
            "%ux%u -> %s\n", s->frames, l, fb, w, h, path);
    fflush(stderr);
}

static void disp_frame_done(void *opaque)
{
    Nano7DispState *s = opaque;

    s->frames++;
    if (s->dump_dir) {
        disp_dump_layer(s, 0, 0x10, 0x14, 0x1c, 0x24);
        disp_dump_layer(s, 1, 0x30, 0x34, 0x3c, 0x40);
    }
    s->status = 1;
    s->irq_status |= 1;
    disp_update(s);
}

static uint64_t disp_read(void *opaque, hwaddr offset, unsigned size)
{
    Nano7DispState *s = opaque;

    switch (offset) {
    case 0x00:
        return s->status;
    case 0x04:
        return 0;
    case 0x0c:
        return s->irq_status;
    default:
        return s->regs[offset / 4];
    }
}

static void disp_write(void *opaque, hwaddr offset, uint64_t value,
                       unsigned size)
{
    Nano7DispState *s = opaque;
    uint32_t v = value;

    switch (offset) {
    case 0x04:
        if (v & 1) {
            s->kicks++;
            if (s->log_lines++ < DISP_LOG_MAX || !(s->kicks & 0xff)) {
                fprintf(stderr, "[nano7-disp] t=%.3f s pc=0x%08x kick #%" PRIu64
                        " L0 fb 0x%08x size 0x%08x, L1 fb 0x%08x size "
                        "0x%08x, irq en %u\n",
                        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9,
                        disp_guest_pc(), s->kicks,
                        s->regs[0x14 / 4], s->regs[0x24 / 4],
                        s->regs[0x34 / 4], s->regs[0x40 / 4],
                        s->regs[0x08 / 4] & 1);
                fflush(stderr);
            }
            s->status = 2;
            timer_mod_ns(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                         DISP_FRAME_NS);
        }
        break;
    case 0x0c:
        s->irq_status &= ~v;
        break;
    default:
        s->regs[offset / 4] = v;
        break;
    }
    disp_update(s);
}

static const MemoryRegionOps disp_ops = {
    .read = disp_read,
    .write = disp_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

void nano7_disp_attach(MemoryRegion *parent, hwaddr base, qemu_irq irq,
                       const char *dump_dir)
{
    Nano7DispState *s = g_new0(Nano7DispState, 1);

    s->irq = irq;
    s->status = 1;
    s->regs[0x260 / 4] = 0x60005;
    if (dump_dir) {
        s->dump_dir = g_strdup(dump_dir);
        g_mkdir_with_parents(dump_dir, 0755);
    }
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, disp_frame_done, s);
    disp_singleton = s;
    atexit(disp_summary);
    memory_region_init_io(&s->mr, NULL, &disp_ops, s, "nano7.disp",
                          DISP_SIZE);
    memory_region_add_subregion_overlap(parent, base, &s->mr, 0);
}
