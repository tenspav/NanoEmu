/*
 * nano7_hostpad.h -- a real host gamepad (Linux evdev) drives the fake
 * Bluetooth HID gamepad. See nano7_hostpad.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NANO7_HOSTPAD_H
#define NANO7_HOSTPAD_H

/* NANO7_HOST_PAD (run.sh --host-pad auto | PATH); after the BT UART */
void nano7_hostpad_init(void);

#endif
