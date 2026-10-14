/*
 * nano7_btuart.h -- Bluetooth HCI UART (port 1, 0x3db00000, IRQ 25) with a
 * fake Broadcom controller or a host socket behind it. See nano7_btuart.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NANO7_BTUART_H
#define NANO7_BTUART_H

#include "exec/memory.h"
#include "hw/irq.h"

#define NANO7_IRQ_BTUART 25

/* hci_socket: NULL = built-in fake controller; else a UNIX socket path or
 * "tcp:[HOST]:PORT" to listen on for the raw H4 stream */
void nano7_btuart_attach(MemoryRegion *parent, hwaddr base, qemu_irq irq,
                         const char *hci_socket);

#endif
