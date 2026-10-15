/*
 * hid_parse.c -- see hid_parse.h. Handles the item subset gamepads use:
 * Usage Page / Usage / Usage Min / Usage Max / Logical Min / Logical Max /
 * Report Size / Report Count / Report ID, Input, Collection, Push / Pop.
 */
#include "hid_parse.h"

typedef struct gstate {
    uint16_t page;
    int32_t lmin, lmax;
    uint8_t size, count, id;
} gstate;

static int32_t item_sval(const uint8_t *p, int n)
{
    switch (n) {
    case 1: return (int8_t)p[0];
    case 2: return (int16_t)(p[0] | p[1] << 8);
    case 4: return (int32_t)(p[0] | p[1] << 8 | p[2] << 16 |
                             (uint32_t)p[3] << 24);
    default: return 0;
    }
}

static uint32_t item_uval(const uint8_t *p, int n)
{
    switch (n) {
    case 1: return p[0];
    case 2: return p[0] | p[1] << 8;
    case 4: return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
    default: return 0;
    }
}

bool hid_parse(const uint8_t *d, int n, hid_desc *out)
{
    gstate g = { 0 }, stack[4];
    int sp = 0, i = 0;
    uint16_t usages[16];
    int nu = 0;
    uint16_t umin = 0, umax = 0;
    bool have_range = false;
    uint16_t bitpos[256] = { 0 };   /* per report ID */

    out->nf = 0;
    out->has_ids = false;
    while (i < n) {
        uint8_t b = d[i];
        int sz = (b & 3) == 3 ? 4 : (b & 3);
        int type = (b >> 2) & 3, tag = b >> 4;
        const uint8_t *v = d + i + 1;

        if (b == 0xfe) {                /* long item: skip */
            if (i + 2 >= n) {
                return false;
            }
            i += 3 + d[i + 1];
            continue;
        }
        if (i + 1 + sz > n) {
            return false;
        }
        if (type == 1) {                /* global */
            switch (tag) {
            case 0x0: g.page = item_uval(v, sz); break;
            case 0x1: g.lmin = item_sval(v, sz); break;
            case 0x2: g.lmax = item_sval(v, sz); break;
            case 0x7: g.size = item_uval(v, sz); break;
            case 0x8: g.id = item_uval(v, sz); out->has_ids = true; break;
            case 0x9: g.count = item_uval(v, sz); break;
            case 0xa: if (sp < 4) stack[sp++] = g; break;
            case 0xb: if (sp > 0) g = stack[--sp]; break;
            }
        } else if (type == 2) {         /* local */
            uint32_t u = item_uval(v, sz);

            switch (tag) {
            case 0x0: if (nu < 16) usages[nu++] = u; break;
            case 0x1: umin = u; have_range = true; break;
            case 0x2: umax = u; have_range = true; break;
            }
        } else if (type == 0) {         /* main */
            if (tag == 0x8) {           /* Input */
                uint32_t flags = item_uval(v, sz);
                bool constant = flags & 1;

                if (!constant && out->nf < HID_MAX_FIELDS && g.size &&
                    g.count) {
                    hid_field *f = &out->f[out->nf++];

                    f->report_id = g.id;
                    f->bit_off = bitpos[g.id];
                    f->bit_size = g.size;
                    f->count = g.count;
                    f->usage_page = g.page;
                    f->usage_min = have_range ? umin : (nu ? usages[0] : 0);
                    f->usage_max = have_range ? umax :
                                   (nu ? usages[nu - 1] : 0);
                    f->log_min = g.lmin;
                    f->log_max = g.lmax;
                    f->variable = flags & 2;
                    /* several single usages on one item (X, Y, Z, Rz):
                     * one field per usage, in order */
                    if (!have_range && nu > 1) {
                        out->nf--;
                        for (int k = 0; k < nu && out->nf < HID_MAX_FIELDS;
                             k++) {
                            hid_field *e = &out->f[out->nf++];

                            *e = *f;
                            e->count = 1;
                            e->bit_off = bitpos[g.id] + k * g.size;
                            e->usage_min = e->usage_max = usages[k];
                            if (k == nu - 1) {      /* last usage repeats */
                                e->count = g.count - k;
                            }
                        }
                    }
                }
                bitpos[g.id] += g.size * g.count;
            } else if (tag == 0x9 || tag == 0xb) {      /* Output, Feature */
                /* other report kinds: their own bit space, not tracked */
            }
            nu = 0;
            have_range = false;
            umin = umax = 0;
        }
        i += 1 + sz;
    }
    return out->nf > 0;
}

static uint32_t get_bits(const uint8_t *r, int nbytes, int off, int len)
{
    uint32_t v = 0;
    int k;

    for (k = 0; k < len; k++) {
        int bit = off + k;

        if (bit / 8 >= nbytes) {
            break;
        }
        if (r[bit / 8] >> (bit % 8) & 1) {
            v |= 1u << k;
        }
    }
    return v;
}

static int16_t norm_axis(const hid_field *f, uint32_t raw)
{
    int32_t v, lo = f->log_min, hi = f->log_max;

    if (lo < 0) {                       /* signed field: sign-extend */
        v = (int32_t)(raw << (32 - f->bit_size)) >> (32 - f->bit_size);
    } else {
        v = raw;
    }
    if (hi <= lo) {
        return 0;
    }
    if (v < lo) {
        v = lo;
    }
    if (v > hi) {
        v = hi;
    }
    return (int16_t)((int64_t)(v - lo) * 65534 / (hi - lo) - 32767);
}

bool hid_decode(const hid_desc *h, const uint8_t *r, int n, hid_pad *pad)
{
    uint8_t id = 0;
    int i, k;
    bool any = false;

    if (h->has_ids) {
        if (n < 1) {
            return false;
        }
        id = r[0];
        r++;
        n--;
    }
    for (i = 0; i < h->nf; i++) {
        const hid_field *f = &h->f[i];

        if (f->report_id != id) {
            continue;
        }
        any = true;
        for (k = 0; k < f->count; k++) {
            uint32_t raw = get_bits(r, n, f->bit_off + k * f->bit_size,
                                    f->bit_size);
            uint16_t usage = f->usage_min + k;

            if (f->usage_page == 0x09 && f->variable) {         /* buttons */
                if (usage >= 1 && usage <= 32) {
                    if (raw) {
                        pad->buttons |= 1u << (usage - 1);
                    } else {
                        pad->buttons &= ~(1u << (usage - 1));
                    }
                }
            } else if (f->usage_page == 0x01) {                 /* desktop */
                if (usage >= 0x30 && usage <= 0x37) {
                    pad->axis[usage - 0x30] = norm_axis(f, raw);
                } else if (usage == 0x39) {                     /* hat */
                    int32_t span = f->log_max - f->log_min + 1;

                    pad->hat = ((int32_t)raw < f->log_min ||
                                (int32_t)raw > f->log_max) ? -1 :
                               (int8_t)((raw - f->log_min) * 8 / span);
                }
            }
        }
    }
    return any;
}
