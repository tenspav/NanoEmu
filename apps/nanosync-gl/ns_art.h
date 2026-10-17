/* NanoSync - cover art (ID3 APIC) -> RGB565 .ithmb + ArtworkDB for the iPod nano 7. Integers only (no float/VFP).
   Layout: formats 1010=240x240, 1013=50x50, 1015=58x58, 1016=57x57; root 6; image ids start at 100;
   the .ithmb files grow by appending; the same cover in the same batch = the same image. Pixels: baseline JPEG decoded with TJpgDec (ChaN)
   already descaled (1/1..1/8) and progressive JPEG by a dedicated decoder (DC + low-frequency AC per block, output at 1/4); the final
   reduction is an integer AREA box filter (fractional weights of 1/16 pixel), without the whole image in memory. */
#ifndef NS_ART_H
#define NS_ART_H
#include <stdint.h>
#include <stddef.h>
#include "tjpgd.h"

#define NS_FMT_N 4
typedef struct { uint16_t id, w, h, stride; uint32_t psize; } ns_fmt;     /* psize = stride * h */
extern const ns_fmt NS_FMT[NS_FMT_N];

/* ID3v2.3/2.4: find the cover image (prefers type 3 = front). Returns 1 = JPEG found (off/len in memory p), 0 = no cover,
   -1 = there is a cover but it is unsupported (PNG, unsynchronized ID3, compressed frame...). */
int ns_id3_cover(const uint8_t *p, uint32_t n, uint32_t *off, uint32_t *len);

#define NS_CV_MAXW 2048u
#define NS_PG_NCO 16u           /* coefficients per luminance block: (u,v) in {0..3}^2 (zigzag 0-9,11,12,13,17,18,24) -> 4x4 output = 1/2 size */
/* Progressive JPEG: low-frequency coefficients per 8x8 block (luma 16: DC int16 + 15 AC int8; chroma 4: DC + 3 AC) -> output at 1/2 size (4x4 per block) */
typedef struct {
    const uint8_t *d; uint32_t n, pos, W, H, hmax, vmax, mcux, mcuy, ri, rleft, eobrun, idx, total, bitbuf, bitcnt, hit;
    uint8_t nc, sof, phase, ns, Ss, Se, Ah, Al, skip, ci[3]; int32_t pred[3];
    struct { uint8_t id, h, v, tq, td, ta, nac; uint32_t bw, bh, cbw, cbh; int16_t *dc; int8_t *ac; } c[3];
    uint16_t qt[4][NS_PG_NCO];
    struct { uint8_t ok; uint8_t vals[256]; int32_t maxcode[18], valptr[18], mincode[18]; } ht[2][4];
    uint32_t mx_i, my_i, auxsz[2]; uint8_t *aux[2];
} ns_pg;
typedef struct {
    JDEC jd; const uint8_t *src; uint32_t srclen, srcpos;
    uint8_t sc; uint32_t W0, H0, Ws, Hs, mx, my, x, y, bandrows;
    struct { uint32_t Wd, Hd, ox, oy, RB, flushed; uint16_t *mx0, *my0; uint8_t *wx0, *wy0; uint32_t *cw, *rw, *acc; uint8_t *out; } t[NS_FMT_N];
    int err, prog; ns_pg pg;
} ns_cv;
/* out[4]: images (psize bytes each, zeroed here). pool: TJpgDec work area (>= 12 KB). scratch: maps/weights/accumulators (ns_cv_scratch_need).
   aux/auxsz and aux2/aux2sz: two temporary memory regions (progressive JPEG coefficients: luma 17 B + chroma 5 B per block; ~600 KB cover 1400x1400 in 4:2:0).
   Returns: 0 ok; -1 unsupported format; -2 no memory; -3 image too large/too small for progressive. */
int  ns_cv_begin(ns_cv *c, const uint8_t *img, uint32_t len, uint8_t *pool, uint32_t poolsz, uint8_t *scratch, uint32_t scsz, uint8_t *const out[NS_FMT_N], uint8_t *aux, uint32_t auxsz, uint8_t *aux2, uint32_t aux2sz);
int  ns_cv_step(ns_cv *c, uint32_t nmcu);            /* 0 = continue, 1 = done, <0 = error (corrupt data) */
uint32_t ns_cv_scratch_need(void);

/* ArtworkDB: old/old_len = current ArtworkDB (NULL/0 = does not exist). Parse: next_id and the highest image id. Returns 0 ok, -1 unexpected format,
   -2 existing MHIF with a different size (refused). */
int ns_adb_info(const uint8_t *old, uint32_t old_len, uint32_t *next_id, uint32_t *max_img_id);
/* Build the new ArtworkDB (create or append). n images: ids[i], owner[i] (dbid of the first track), srcsz[i] (original width*height*3),
   offs[k][i] = offset of image i in the file of format k. next_id = next free id after the new ones. Returns the size, or 0 if cap is too small / bad format. */
uint32_t ns_adb_build(const uint8_t *old, uint32_t old_len, uint32_t n, const uint32_t *ids, const uint64_t *owner, const uint32_t *srcsz,
                      const uint32_t offs[NS_FMT_N][8], uint32_t next_id, uint8_t *out, uint32_t cap);
#endif
