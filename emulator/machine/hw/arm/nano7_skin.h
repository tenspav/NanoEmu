/*
 * nano7_skin.h -- the device frame drawn around the screen (the desktop
 * front end's iPod nano 7G body with clickable buttons). See nano7_skin.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NANO7_SKIN_H
#define NANO7_SKIN_H

#include <stdint.h>
#include <stdbool.h>

/* hit-test results besides the NANO7_BTN_* button ids */
#define NANO7_SKIN_NONE     (-1)
#define NANO7_SKIN_SCREEN   (-2)

/* side panel button ids (besides the NANO7_BTN_* iPod buttons) */
#define NANO7_SKIN_P_STOCK  100
#define NANO7_SKIN_P_SAFE   101
#define NANO7_SKIN_P_MAX    102
#define NANO7_SKIN_P_PAUSE  103
#define NANO7_SKIN_P_RESET  104
#define NANO7_SKIN_P_MUTE   105
#define NANO7_SKIN_P_ROTATE 106
#define NANO7_SKIN_P_SHOT   107

/* the side panel right of the device, in units */
#define NANO7_PANEL_W       200

/* what the side panel shows (compared bytewise: zero it before filling) */
typedef struct Nano7PanelInfo {
    float fps, cpu, gpu;                /* cpu / gpu: load 0..1 */
    float cpu_mhz, gpu_mhz, temp;
    float ram_used, ram_total;          /* MB, total 0 = unknown */
    int profile;                        /* NANO7_PROFILE_*, -1 = guest */
    int rot;
    bool paused, muted;
} Nano7PanelInfo;

/* device canvas size in LCD-pixel units (multiply by the zoom) */
void nano7_skin_size(int *w, int *h);
/* draw the device at zoom z into buf (w * z by h * z, row stride w * z):
 * body, buttons (pressed: bit (1 << NANO7_BTN_x) of `pressed`) and the
 * panel frame lcd (240x432, may be NULL = dark) scaled by z */
void nano7_skin_draw(uint32_t *buf, int z, const uint32_t *lcd,
                     unsigned pressed);
/* device point (units, fractional) -> NANO7_SKIN_SCREEN with the panel
 * pixel in *px / *py, a NANO7_BTN_* id, or NANO7_SKIN_NONE */
int nano7_skin_hit(double x, double y, int *px, int *py);
/* panel pixel of a device point, clamped to the screen (drags off it) */
void nano7_skin_screen_xy(double x, double y, int *px, int *py);
/* the side panel (NANO7_PANEL_W x h_units units at zoom z) into buf with
 * row stride `stride` pixels; pressed = the id held down or -1 */
void nano7_skin_panel_draw(uint32_t *buf, int stride, int z, int h_units,
                           const Nano7PanelInfo *pi, int pressed);
/* panel point (units from the panel's top-left) -> button id or
 * NANO7_SKIN_NONE */
int nano7_skin_panel_hit(double x, double y);
/* top-left of the screen on the device canvas (units, unrotated) */
void nano7_skin_screen_origin(int *x, int *y);
/* the canvas background colour */
uint32_t nano7_skin_bg(void);

#endif
