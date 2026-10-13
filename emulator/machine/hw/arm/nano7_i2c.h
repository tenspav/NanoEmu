/*
 * nano7_i2c.h -- S3C-style I2C controllers (0x3c600000 / 0x3c900000) for
 * the iPod nano 7G machine. See nano7_i2c.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NANO7_I2C_H
#define NANO7_I2C_H

#include "exec/memory.h"
#include "hw/irq.h"

#define NANO7_IRQ_I2C0  21
#define NANO7_IRQ_I2C1  22

typedef struct Nano7I2CBus Nano7I2CBus;

/* A slave: start(rx) after its address is ACKed, write returns ACK */
typedef struct Nano7I2CSlave {
    uint8_t addr;
    void *opaque;
    void (*start)(void *opaque, bool rx);
    bool (*write)(void *opaque, uint8_t b);
    uint8_t (*read)(void *opaque);
    void (*stop)(void *opaque);
} Nano7I2CSlave;

Nano7I2CBus *nano7_i2c_attach(MemoryRegion *parent, hwaddr base,
                              qemu_irq irq, const char *name);
void nano7_i2c_add_slave(Nano7I2CBus *bus, const Nano7I2CSlave *slave);
/* a slave answering with a plain 256-byte register file (pointer = 1st
 * written byte, auto-increment) */
void nano7_i2c_add_regfile_slave(Nano7I2CBus *bus, uint8_t addr);

#endif
