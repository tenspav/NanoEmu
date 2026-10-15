/*
 * nano7_i2s.h -- I2S controller 0 (audio out) for the iPod nano 7G
 * machine. See nano7_i2s.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NANO7_I2S_H
#define NANO7_I2S_H

#include "exec/memory.h"

/* audio_out: PCM sink (raw s16le stereo, "*.wav" adds a WAV header, a FIFO
 * gets the raw stream without blocking); audiodev: id of a QEMU -audiodev
 * to play through (coreaudio, pa, ...); both NULL = samples are discarded */
void nano7_i2s_attach(MemoryRegion *parent, hwaddr base,
                      const char *audio_out, const char *audiodev);

/* host audio mute (the macOS menu's Mute; the guest keeps playing) */
void nano7_i2s_set_mute(bool mute);
bool nano7_i2s_muted(void);

#endif
