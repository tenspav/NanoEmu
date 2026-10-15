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
 * "tcp:[HOST]:PORT" to listen on for the raw H4 stream.
 * peer: simulated device in range of the fake controller, "gamepad"
 * (NULL = default), "gamepad-connect" (also pages the iPod) or "none".
 * pad: the gamepad's input script "SEC:CONTROLS[:MS]/..." (relative to
 * HID up; NULL = a short demo, "none" = no input), see nano7_btuart.c */
void nano7_btuart_attach(MemoryRegion *parent, hwaddr base, qemu_irq irq,
                         const char *hci_socket, const char *peer,
                         const char *pad);

/* live pad input (nano7_ui.c): in pad mode (F8) the mapped keys drive the
 * fake gamepad; returns true if the key was taken */
bool nano7_btuart_pad_key(int qcode, bool down);
void nano7_btuart_pad_mode(bool on);
/* a real host gamepad's state (nano7_hostpad.c): buttons 1-16 = bits 0-15,
 * axes X Y Z Rz in -127..127; added to the keys held in pad mode */
void nano7_btuart_pad_host(uint16_t buttons, const int8_t axis[4]);
/* 0 = pad mode off, 1 = on but no HID link yet, 2 = on and connected */
int nano7_btuart_pad_state(void);

#endif
