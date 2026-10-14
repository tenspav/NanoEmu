/*
 * nano7_i2c.c -- S3C-style I2C controllers for the iPod nano 7G (S5L8747):
 * bus 0 at 0x3c600000 (VIC IRQ 21), bus 1 at 0x3c900000 (VIC IRQ 22).
 *
 * Register use, as the RetailOS IRAM driver drives it (0x22000522..
 * 0x22001448, ISR 0x22001214; DRAM read xfer 0x083ff2a8; Session 17 RE):
 *   +0x00 IICCON   bit7 ACK enable, bit4 pending (write 0 = resume),
 *                  bit13/bit8 irq enables (init 0x181)
 *   +0x04 IICSTAT  bits7:6 mode (11 master TX, 10 master RX), bit4 enable,
 *                  write bit5 1 = START / 0 = STOP; read bit5 busy,
 *                  bit0 last ACK (0 = ACK)
 *   +0x08 IICADD   +0x0c IICDS (address/data byte)
 *   +0x20 status2  bit8 byte done, bit13 stop done (W1C)
 * Write xfer: IICSTAT 0xd0, IICDS = addr<<1, IICSTAT 0xf0, per IRQ write
 * IICDS + resume, STOP = IICSTAT 0xd0 with status2/IICCON bit13. Read xfer:
 * IICSTAT 0x90/0xb0, bytes read from IICDS, NACK the last (IICCON bit7 = 0).
 * A PMU register read is [reg] written + STOP, then a 1-byte read.
 *
 * Model: each byte (address, data out, data in) completes ~20 us after it
 * is started: IICCON bit4 + status2 bit8; a STOP sets status2 bit13. The
 * IRQ line is status2 & 0x2100. Absent slaves NACK their address.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "nano7_i2c.h"
#include "qemu/timer.h"
#include "migration/vmstate.h"

#define I2C_SIZE        0x1000
#define I2C_BYTE_NS     20000
#define I2C_MAX_SLAVES  8

struct Nano7I2CBus {
    MemoryRegion mr;
    char *name;
    uint32_t regs[I2C_SIZE / 4];
    uint32_t con, stat, ds, st2;
    bool busy, nack, rx, rx_last, ds_written;
    Nano7I2CSlave slaves[I2C_MAX_SLAVES];
    unsigned nslaves;
    Nano7I2CSlave *cur;
    int32_t cur_idx;            /* save state: index of cur (-1 = none) */
    QEMUTimer *timer;
    qemu_irq irq;
    uint64_t xfers;
};

typedef struct I2CRegfileSlave {
    uint8_t regs[256];
    uint8_t ptr;
    bool have_ptr;
} I2CRegfileSlave;

static void i2c_update(Nano7I2CBus *b)
{
    qemu_set_irq(b->irq, (b->st2 & 0x2100) != 0);
}

/* the byte in flight finished */
static void i2c_byte_done(void *opaque)
{
    Nano7I2CBus *b = opaque;

    b->con |= 0x10;
    b->st2 |= 0x100;
    i2c_update(b);
}

static void i2c_kick(Nano7I2CBus *b)
{
    timer_mod_ns(b->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                 I2C_BYTE_NS);
}

static Nano7I2CSlave *i2c_find(Nano7I2CBus *b, uint8_t addr)
{
    unsigned i;

    for (i = 0; i < b->nslaves; i++) {
        if (b->slaves[i].addr == addr) {
            return &b->slaves[i];
        }
    }
    return NULL;
}

static uint64_t i2c_read(void *opaque, hwaddr off, unsigned size)
{
    Nano7I2CBus *b = opaque;

    switch (off) {
    case 0x00: return b->con;
    case 0x04: return (b->stat & ~0x21u) | (b->busy ? 0x20 : 0) |
                      (b->nack ? 1 : 0);
    case 0x0c: return b->ds;
    case 0x20: return b->st2;
    default:   return b->regs[off / 4];
    }
}

static void i2c_write(void *opaque, hwaddr off, uint64_t value,
                      unsigned size)
{
    Nano7I2CBus *b = opaque;
    uint32_t v = value;

    switch (off) {
    case 0x00: {
        bool resume = (b->con & 0x10) && !(v & 0x10);

        b->con = v;
        if (!resume || !b->busy) {
            break;
        }
        if (!b->rx && b->ds_written) {
            b->ds_written = false;
            b->nack = !(b->cur && b->cur->write(b->cur->opaque, b->ds));
            i2c_kick(b);
        } else if (b->rx && !b->rx_last) {
            b->ds = b->cur ? b->cur->read(b->cur->opaque) : 0xff;
            b->rx_last = !(v & 0x80);   /* NACKed byte = the last one */
            i2c_kick(b);
        }
        break;
    }
    case 0x04:
        b->stat = v;
        if ((v & 0x30) == 0x30 && !b->busy) {          /* START */
            b->busy = true;
            b->rx = b->ds & 1;
            b->rx_last = false;
            b->ds_written = false;
            b->cur = i2c_find(b, b->ds >> 1);
            b->nack = !b->cur;
            if (b->cur && b->cur->start) {
                b->cur->start(b->cur->opaque, b->rx);
            }
            b->xfers++;
            i2c_kick(b);
        } else if (!(v & 0x20) && b->busy) {           /* STOP */
            b->busy = false;
            if (b->cur && b->cur->stop) {
                b->cur->stop(b->cur->opaque);
            }
            b->cur = NULL;
            b->st2 |= 0x2000;
        }
        break;
    case 0x0c:
        b->ds = v & 0xff;
        b->ds_written = true;
        break;
    case 0x20:
        b->st2 &= ~v;
        break;
    default:
        b->regs[off / 4] = v;
        break;
    }
    i2c_update(b);
}

static const MemoryRegionOps i2c_ops = {
    .read = i2c_read,
    .write = i2c_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static int i2c_pre_save(void *opaque)
{
    Nano7I2CBus *b = opaque;

    b->cur_idx = b->cur ? b->cur - b->slaves : -1;
    return 0;
}

static int i2c_post_load(void *opaque, int version_id)
{
    Nano7I2CBus *b = opaque;

    b->cur = b->cur_idx >= 0 && b->cur_idx < b->nslaves ?
             &b->slaves[b->cur_idx] : NULL;
    i2c_update(b);
    return 0;
}

static const VMStateDescription vmstate_i2c = {
    .name = "nano7-i2c",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = i2c_pre_save,
    .post_load = i2c_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Nano7I2CBus, I2C_SIZE / 4),
        VMSTATE_UINT32(con, Nano7I2CBus),
        VMSTATE_UINT32(stat, Nano7I2CBus),
        VMSTATE_UINT32(ds, Nano7I2CBus),
        VMSTATE_UINT32(st2, Nano7I2CBus),
        VMSTATE_BOOL(busy, Nano7I2CBus),
        VMSTATE_BOOL(nack, Nano7I2CBus),
        VMSTATE_BOOL(rx, Nano7I2CBus),
        VMSTATE_BOOL(rx_last, Nano7I2CBus),
        VMSTATE_BOOL(ds_written, Nano7I2CBus),
        VMSTATE_INT32(cur_idx, Nano7I2CBus),
        VMSTATE_TIMER_PTR(timer, Nano7I2CBus),
        VMSTATE_UINT64(xfers, Nano7I2CBus),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_i2c_regfile = {
    .name = "nano7-i2c-regfile",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, I2CRegfileSlave, 256),
        VMSTATE_UINT8(ptr, I2CRegfileSlave),
        VMSTATE_BOOL(have_ptr, I2CRegfileSlave),
        VMSTATE_END_OF_LIST()
    }
};

Nano7I2CBus *nano7_i2c_attach(MemoryRegion *parent, hwaddr base,
                              qemu_irq irq, const char *name)
{
    Nano7I2CBus *b = g_new0(Nano7I2CBus, 1);
    g_autofree char *mrname = g_strdup_printf("nano7.%s", name);

    b->name = g_strdup(name);
    b->irq = irq;
    b->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, i2c_byte_done, b);
    vmstate_register_any(NULL, &vmstate_i2c, b);
    memory_region_init_io(&b->mr, NULL, &i2c_ops, b, mrname, I2C_SIZE);
    memory_region_add_subregion_overlap(parent, base, &b->mr, 0);
    return b;
}

void nano7_i2c_add_slave(Nano7I2CBus *b, const Nano7I2CSlave *slave)
{
    assert(b->nslaves < I2C_MAX_SLAVES);
    b->slaves[b->nslaves++] = *slave;
}

static void rf_start(void *opaque, bool rx)
{
    I2CRegfileSlave *r = opaque;

    if (!rx) {
        r->have_ptr = false;
    }
}

static bool rf_write(void *opaque, uint8_t v)
{
    I2CRegfileSlave *r = opaque;

    if (!r->have_ptr) {
        r->ptr = v;
        r->have_ptr = true;
    } else {
        r->regs[r->ptr++] = v;
    }
    return true;
}

static uint8_t rf_read(void *opaque)
{
    I2CRegfileSlave *r = opaque;

    return r->regs[r->ptr++];
}

void nano7_i2c_add_regfile_slave(Nano7I2CBus *b, uint8_t addr)
{
    Nano7I2CSlave sl = {
        .addr = addr, .opaque = g_new0(I2CRegfileSlave, 1),
        .start = rf_start, .write = rf_write, .read = rf_read,
    };

    vmstate_register_any(NULL, &vmstate_i2c_regfile, sl.opaque);

    nano7_i2c_add_slave(b, &sl);
}
