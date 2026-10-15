/*
 * nano7_accel.h -- LIS3DH accelerometer on I2C1 0x18 with an emulated
 * tilt. See nano7_accel.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NANO7_ACCEL_H
#define NANO7_ACCEL_H

#include "nano7_i2c.h"

void nano7_accel_attach(Nano7I2CBus *bus);
/* Left / Right arrow: tilt the device (nano7_ui.c); true if taken */
bool nano7_accel_key(int qcode, bool down);

#endif
