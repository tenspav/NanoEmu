/*
 * nano7_mbxr.h -- software renderer behind the PowerVR MBX model
 * (nano7_mbx.c): parses the TA stream the OS GL ES driver writes and draws
 * the scene into the render target. See nano7_mbxr.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NANO7_MBXR_H
#define NANO7_MBXR_H

#include <stdint.h>
#include <stdbool.h>

typedef struct MbxR MbxR;

MbxR *mbxr_new(void);
/* scene start (+0x800 = 1): drop the previous scene's triangles */
void mbxr_scene_begin(MbxR *r);
/* one word written to the TA FIFO (0x3b800000) */
void mbxr_fifo_word(MbxR *r, uint32_t w);
/* doorbell (0x3bc00000): the words since the last one form a segment */
void mbxr_doorbell(MbxR *r, uint32_t v);
/* render job (+0x6ec): draw the scene into the target described by the
 * core registers (core[] = 0x3b000000.. as 32-bit words) */
void mbxr_render(MbxR *r, const uint32_t *core);
/* drop parsed state (after loading a state saved without the renderer) */
void mbxr_reset(MbxR *r);
/* save-state description of the renderer state (persistent TA state,
 * vertex program and constants, the segment and scene being built);
 * nano7_mbx.c saves it through its MbxR pointer */
extern const struct VMStateDescription vmstate_mbxr;

/* cumulative GPU workload, for the usage HUD (nano7_hud.c) */
typedef struct MbxrStats {
    uint64_t frames;            /* render jobs */
    uint64_t verts;             /* vertices processed */
    uint64_t vgp_ins;           /* vertex program instructions executed */
    uint64_t frags;             /* covered pixels rasterized */
} MbxrStats;
void mbxr_get_stats(const MbxR *r, MbxrStats *st);

#endif
