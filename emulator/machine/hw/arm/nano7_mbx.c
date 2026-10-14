/*
 * S5L8747 MBX Lite: register/command completion draft, not a renderer.
 * Reference: docs/research/gpu-mbx.md (gpu-mbx-spec.md was unavailable).
 * Register-pair jobs are applied by firmware, not fetched by +0x6ec.
 * Completion is synchronous; FIFO words are consumed immediately, hence
 * +0x12c occupancy is always zero (0x7b words free).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "nano7_mbx.h"
#include "hw/sysbus.h"
#include "qemu/module.h"
#include "migration/vmstate.h"

#define MBX_CORE_WORDS (0x4000 / 4)
#define MBX_PLANE_WORDS (0x1000 / 4)
#define MBX_STATUS  (0x12c / 4)
#define MBX_ENABLE  (0x130 / 4)
#define MBX_CLEAR   (0x134 / 4)
#define MBX_MMU     (0x1020 / 4)

typedef struct Nano7MbxState Nano7MbxState;
typedef struct Nano7MbxWindow {
    MemoryRegion mr;
    Nano7MbxState *s;
    unsigned index;
} Nano7MbxWindow;

struct Nano7MbxState {
    SysBusDevice parent_obj;
    Nano7MbxWindow window[7];
    qemu_irq irq;
    uint32_t core[MBX_CORE_WORDS];
    uint32_t plane[3][MBX_PLANE_WORDS];
    uint32_t sync_left[3];
};

OBJECT_DECLARE_SIMPLE_TYPE(Nano7MbxState, NANO7_MBX)

static void mbx_update_irq(Nano7MbxState *s)
{
    qemu_set_irq(s->irq, (s->core[MBX_STATUS] &
                         s->core[MBX_ENABLE] & 0xffff) != 0);
}

static void mbx_core_reset(Nano7MbxState *s)
{
    memset(s->core, 0, sizeof(s->core));
    memset(s->sync_left, 0, sizeof(s->sync_left));
    s->core[0xf00 / 4] = 0x01020003;
    s->core[0xf10 / 4] = 0x00010500;
    /* Identification signature checked by the driver. Remaining ID words
     * are unknown; do not invent feature bits. */
    s->core[0xff0 / 4] = 0xb105f00d;
    mbx_update_irq(s);
}

static void mbx_reset(DeviceState *dev)
{
    Nano7MbxState *s = NANO7_MBX(dev);
    unsigned i;

    memset(s->plane, 0, sizeof(s->plane));
    for (i = 0; i < 3; i++) {
        s->plane[i][0] = 2; /* disabled/idle shutdown acknowledgement */
    }
    s->plane[2][0x40 / 4] = 1; /* video output ready */
    mbx_core_reset(s);
}

static uint64_t mbx_read(void *opaque, hwaddr off, unsigned size)
{
    Nano7MbxWindow *w = opaque;
    Nano7MbxState *s = w->s;

    if (w->index < 3) {
        return s->plane[w->index][off / 4];
    }
    if (w->index == 3) {
        return s->core[off / 4];
    }
    return 0; /* write-only FIFO apertures */
}

static void mbx_write(void *opaque, hwaddr off, uint64_t value,
                      unsigned size)
{
    Nano7MbxWindow *w = opaque;
    Nano7MbxState *s = w->s;
    uint32_t v = value;
    unsigned n = off / 4;

    if (w->index < 3) {
        s->plane[w->index][n] = v;
        if (!off) {
            /* Enabled has bit0, disabled acknowledges with bit1. */
            s->plane[w->index][n] = (v & ~2u) | ((v & 1) ? 0 : 2);
        } else if (w->index == 0 && off == 8 && (v & 1)) {
            /* TODO: scale/crop pixels using +0x28..+0x34 and plane state. */
            s->plane[0][n] = v & ~1u;
        }
        return;
    }
    if (w->index >= 4) {
        unsigned fifo = w->index - 4;

        /* All addresses in each aperture feed the same stream. */
        if (v == 0xf0000000) {
            s->sync_left[fifo] = 6;
        } else if (v == 0x70000000 && s->sync_left[fifo]) {
            if (!--s->sync_left[fifo]) {
                /* Draft barrier completion: tiled + callback notification.
                 * TODO: confirm command event encoding from hardware traces. */
                s->core[MBX_STATUS] |= 0x420;
                mbx_update_irq(s);
            }
        } else {
            s->sync_left[fifo] = 0;
            /* TODO: decode MBX command packets, tile and rasterize. */
        }
        return;
    }
    switch (off) {
    case 0x80:
        if (v & 0x111) {
            mbx_core_reset(s);
        }
        s->core[n] = v;
        break;
    case 0x12c:
    case 0xf00:
    case 0xf10:
    case 0xff0 ... 0xffc:
        return; /* status and identification are read-only */
    case 0x134:
        s->core[MBX_STATUS] &= ~(v & 0xffff);
        break;
    case 0x808:
        s->core[n] = v & ~1u;
        if (v & 1) {
            /* TODO: render to guest surfaces; no guest pixels are changed. */
            s->core[MBX_STATUS] |= 0x10;
        }
        break;
    case 0x1020:
        /* Draft handshake: status mirrors translation enable; invalidation
         * completes immediately and bit8 self-clears. No DMA translation yet. */
        s->core[MBX_MMU] = (v & ~0x10100u) | ((v & 1) << 16);
        break;
    default:
        /* Includes eight MMU roots, pipeline configuration and job pointer.
         * The CPU worker writes (offset,value) pairs before +0x6ec. */
        s->core[n] = v;
        break;
    }
    mbx_update_irq(s);
}

static const MemoryRegionOps mbx_ops = {
    .read = mbx_read,
    .write = mbx_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static int mbx_post_load(void *opaque, int version_id)
{
    mbx_update_irq(opaque);
    return 0;
}

static const VMStateDescription vmstate_mbx = {
    .name = TYPE_NANO7_MBX,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = mbx_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(core, Nano7MbxState, MBX_CORE_WORDS),
        VMSTATE_UINT32_2DARRAY(plane, Nano7MbxState, 3, MBX_PLANE_WORDS),
        VMSTATE_UINT32_ARRAY(sync_left, Nano7MbxState, 3),
        VMSTATE_END_OF_LIST()
    },
};

static void mbx_init(Object *obj)
{
    Nano7MbxState *s = NANO7_MBX(obj);
    unsigned i;

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    for (i = 0; i < 7; i++) {
        Nano7MbxWindow *w = &s->window[i];
        g_autofree char *name = g_strdup_printf("nano7.mbx.%u", i);

        w->s = s;
        w->index = i;
        memory_region_init_io(&w->mr, obj, &mbx_ops, w, name,
                              i < 3 ? 0x1000 : i == 3 ? 0x4000 : 0x200000);
        sysbus_init_mmio(SYS_BUS_DEVICE(obj), &w->mr);
    }
    mbx_reset(DEVICE(obj));
}

static void mbx_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = mbx_reset;
    dc->vmsd = &vmstate_mbx;
    dc->desc = "S5L8747 MBX Lite completion stub";
}

static const TypeInfo mbx_type = {
    .name = TYPE_NANO7_MBX,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Nano7MbxState),
    .instance_init = mbx_init,
    .class_init = mbx_class_init,
};

static void mbx_register_types(void)
{
    type_register_static(&mbx_type);
}
type_init(mbx_register_types)

void nano7_mbx_attach(MemoryRegion *parent, qemu_irq irq)
{
    static const hwaddr bases[] = {
        0x39100000, 0x39200000, 0x39300000, 0x3b000000,
        0x3b800000, 0x3ba00000, 0x3bc00000,
    };
    DeviceState *dev = qdev_new(TYPE_NANO7_MBX);
    SysBusDevice *bus = SYS_BUS_DEVICE(dev);
    unsigned i;

    sysbus_realize_and_unref(bus, &error_fatal);
    sysbus_connect_irq(bus, 0, irq);
    for (i = 0; i < ARRAY_SIZE(bases); i++) {
        memory_region_add_subregion_overlap(parent, bases[i],
                                           sysbus_mmio_get_region(bus, i), 0);
    }
}
