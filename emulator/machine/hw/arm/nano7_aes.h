/*
 * nano7_aes.h -- AES engine (0x38c00000) for the iPod nano 7G machine.
 * See nano7_aes.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NANO7_AES_H
#define NANO7_AES_H

#include "exec/memory.h"

/* gid_key / uid_key: optional 32-hex-digit AES-128 keys (NULL = unknown) */
void nano7_aes_attach(MemoryRegion *parent, hwaddr base, const char *gid_key,
                      const char *uid_key);

#endif
