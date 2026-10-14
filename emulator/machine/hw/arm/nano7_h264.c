/*
 * Apple M2 0x396xxxxx completion stub (historical nano7_h264 task name).
 * h264-deep.md corrects h264-decoder.md: IRQ45 is MPEG-4/H.263, whereas
 * actual H.264 uses 0x38f00000/IRQ35. This implements only the requested map.
 * No descriptor is DMA-fetched: doorbell values 2/0x102 are commands.
 * Firmware's five-u32 setup record is {Y/input, chroma, plane_separation,
 * aux0, aux1}; it programs +0x60018/20/24/78/7c, +0x60080=0.
 * +0x6001c is an input END ADDRESS, not a byte count; pointers mask bit31.
 * The unrelated H.264 seven-u32 surface record is {surface*, Y>>10,
 * chroma>>10, aux>>10, Y_phys, chroma_phys, aux_phys} (not this engine).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "nano7_h264.h"
#include "hw/sysbus.h"
#include "qemu/module.h"
#include "migration/vmstate.h"

#define M2_SIZE 0x70000
#define M2_WORDS (M2_SIZE / 4)
#define M2_TOP (0x00000 / 4)
#define M2_FRAME (0x60000 / 4)

typedef struct Nano7H264State {
    SysBusDevice parent_obj;
    MemoryRegion mr;
    qemu_irq irq;
    uint32_t regs[M2_WORDS];
} Nano7H264State;

OBJECT_DECLARE_SIMPLE_TYPE(Nano7H264State, NANO7_H264)

static void m2_update_irq(Nano7H264State *s)
{
    /* No enable mask for this top-level path has been established.
     * Top W1C drops the line even if child status is still pending. */
    qemu_set_irq(s->irq, (s->regs[M2_TOP] & 0x142) != 0);
}

static void m2_reset(DeviceState *dev)
{
    Nano7H264State *s = NANO7_H264(dev);

    memset(s->regs, 0, sizeof(s->regs));
    m2_update_irq(s);
}

static uint64_t m2_read(void *opaque, hwaddr off, unsigned size)
{
    Nano7H264State *s = opaque;

    switch (off) {
    case 0x1004:
    case 0x30004:
    case 0x30020:
        return 0; /* software reset complete / sequencer idle */
    default:
        return s->regs[off / 4];
    }
}

static void m2_complete(Nano7H264State *s)
{
    /* TODO: parse bitstream, decode and write guest output surfaces once
     * codec and plane layout are confirmed. Leave guest RAM untouched. */
    s->regs[M2_FRAME] |= 2;
    s->regs[M2_TOP] |= 0x40;
    m2_update_irq(s);
}

static void m2_write(void *opaque, hwaddr off, uint64_t value,
                     unsigned size)
{
    Nano7H264State *s = opaque;
    uint32_t v = value;
    unsigned n = off / 4;

    switch (off) {
    case 0x00000:
    case 0x10000:
    case 0x30100:
    case 0x50000:
    case 0x60000:
        s->regs[n] &= ~v; /* W1C; +0x30100=0x20 also arms in firmware */
        m2_update_irq(s);
        break;
    case 0x0000c:
        if (!v) {
            m2_reset(DEVICE(s)); /* power/reset asserted */
        }
        s->regs[n] = v;
        break;
    case 0x1004:
        s->regs[n] = 0; /* reset request completes immediately */
        break;
    case 0x30004:
    case 0x30020:
        break; /* idle status is read-only */
    case 0x30110:
        s->regs[n] = v;
        if (v == 2 || v == 0x102) {
            m2_complete(s);
        }
        break;
    case 0x6000c:
        s->regs[n] = v;
        if (v == 3) {
            m2_complete(s);
        }
        break;
    case 0x60018:
    case 0x6001c:
    case 0x60020:
    case 0x60024:
    case 0x6002c:
    case 0x60034:
    case 0x6003c:
    case 0x60044:
    case 0x60078:
    case 0x6007c:
    case 0x60080:
        s->regs[n] = v & 0x7fffffff;
        break;
    default:
        /* Includes all scaling matrices, profile, slice and work registers. */
        s->regs[n] = v;
        break;
    }
}

static const MemoryRegionOps m2_ops = {
    .read = m2_read,
    .write = m2_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static int m2_post_load(void *opaque, int version_id)
{
    m2_update_irq(opaque);
    return 0;
}

static const VMStateDescription vmstate_m2 = {
    .name = TYPE_NANO7_H264,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = m2_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Nano7H264State, M2_WORDS),
        VMSTATE_END_OF_LIST()
    },
};

static void m2_init(Object *obj)
{
    Nano7H264State *s = NANO7_H264(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    memory_region_init_io(&s->mr, obj, &m2_ops, s, "nano7.m2", M2_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mr);
    m2_reset(DEVICE(obj));
}

static void m2_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = m2_reset;
    dc->vmsd = &vmstate_m2;
    dc->desc = "S5L8747 Apple M2 IRQ45 completion stub";
}

static const TypeInfo m2_type = {
    .name = TYPE_NANO7_H264,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Nano7H264State),
    .instance_init = m2_init,
    .class_init = m2_class_init,
};

static void m2_register_types(void)
{
    type_register_static(&m2_type);
}
type_init(m2_register_types)

void nano7_h264_attach(MemoryRegion *parent, hwaddr base, qemu_irq irq)
{
    DeviceState *dev = qdev_new(TYPE_NANO7_H264);
    SysBusDevice *bus = SYS_BUS_DEVICE(dev);

    sysbus_realize_and_unref(bus, &error_fatal);
    sysbus_connect_irq(bus, 0, irq);
    memory_region_add_subregion_overlap(parent, base,
                                       sysbus_mmio_get_region(bus, 0), 0);
}
