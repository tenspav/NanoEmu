/*
 * nano7_gpio.h -- GPIO interrupt groups (0x39700080..) and modeled GPIO
 * input levels for the iPod nano 7G machine. See nano7_gpio.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NANO7_GPIO_H
#define NANO7_GPIO_H

#include "exec/memory.h"
#include "hw/irq.h"

#define NANO7_GPIO_PMU_IRQ  0x56    /* PMU event line, active-low */

/* vic_in: the VIC's input lines (groups raise IRQ 33/32/31/3/2) */
void nano7_gpio_attach(MemoryRegion *parent, qemu_irq *vic_in);
/* drive a modeled input pin; a falling edge latches its pending bit */
void nano7_gpio_set_level(unsigned n, int level);

#endif
