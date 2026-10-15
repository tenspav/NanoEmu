/*
 * nano7_hud.h -- in-emulator usage HUD (CPU / GPU load graphs) drawn under
 * the display by nano7_ui.c. See nano7_hud.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NANO7_HUD_H
#define NANO7_HUD_H

#include <stdint.h>
#include <stdbool.h>

#define NANO7_HUD_ROWS  47      /* strip height in panel pixels (x zoom) */

/* start state from NANO7_HUD (run.sh --hud) */
void nano7_hud_init(void);
bool nano7_hud_on(void);
/* show / hide; the sampler only runs while shown */
void nano7_hud_set(bool on);
/* draw the strip: w x NANO7_HUD_ROWS XRGB pixels (row stride w) */
void nano7_hud_draw(uint32_t *buf, int w);
/* the latest window: frames per second, CPU / GPU load 0..1, OS heap
 * bytes (0 = unknown) */
void nano7_hud_stats(float *fps, float *cpu, float *gpu, uint32_t *heap_used,
                     uint32_t *heap_total);
/* the panel showed a new frame (counts toward the HUD's FPS) */
void nano7_hud_lcd_frame(void);

#endif
