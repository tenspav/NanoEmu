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
#include "qapi/error.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"
#include "qemu/timer.h"
#include "nano7_clock.h"
#include "nano7_mbxr.h"

#define MBX_CORE_WORDS (0x4000 / 4)
#define MBX_PLANE_WORDS (0x1000 / 4)
#define MBX_STATUS  (0x12c / 4)
#define MBX_ENABLE  (0x130 / 4)
#define MBX_CLEAR   (0x134 / 4)
#define MBX_MMU     (0x1020 / 4)
#define MBX_TA_NS       50000           /* end of TA list -> TA done */
#define MBX_RENDER_NS   500000          /* TA done -> render done */
#define MBX_JOB_NS      100000          /* +0x6ec render job -> done */

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
    QEMUTimer *ta_timer;        /* TA done / render done sequence */
    QEMUTimer *job_timer;       /* +0x6ec render job done */
    bool ta_armed;              /* +0x800 seen, end of list pending */
    bool ta_end, render_pending;  /* end-of-list seen / render running */
    uint64_t scenes, renders, jobs;
    MbxR *r;                    /* software renderer (nano7_mbxr.c) */
    bool trace;                 /* NANO7_MBX_TRACE: log writes */
    uint64_t trace_n;
};

OBJECT_DECLARE_SIMPLE_TYPE(Nano7MbxState, NANO7_MBX)

static void mbx_update_irq(Nano7MbxState *s)
{
    qemu_set_irq(s->irq, (s->core[MBX_STATUS] &
                         s->core[MBX_ENABLE] & 0xffff) != 0);
}

/* end of list consumed: TA done (if a scene was started), then render
 * done MBX_RENDER_NS later */
static void mbx_ta_done(void *opaque)
{
    Nano7MbxState *s = opaque;

    if (s->ta_end) {
        s->ta_end = false;
        /* NOT bit 5: that is the TA parameter-buffer overflow (partial
         * render) event - its handler 0x0814fb10 flags ctx+0x98, and the
         * next FIFO space wait then asks for a depth store (bridge call
         * 0x1b -> 0x080b14f8), which 1.1.2 never supports (asserts). A
         * scene that fits renders by itself and only reports bit 4. */
        s->ta_armed = false;
        s->render_pending = true;
        nano7_timer_mod(s->ta_timer, nano7_now_ns() + MBX_RENDER_NS);
    } else if (s->render_pending) {
        s->render_pending = false;
        s->core[MBX_STATUS] |= 0x10;
        s->renders++;
        mbx_update_irq(s);
    }
}

static void mbx_job_done(void *opaque)
{
    Nano7MbxState *s = opaque;

    /* bits 2 and 6 together (bit 3 / 2|3 alone leave the slot busy) */
    s->core[MBX_STATUS] |= 0x04 | 0x40;
    s->jobs++;
    mbx_update_irq(s);
}

static Nano7MbxState *mbx_singleton;

static void mbx_summary(void)
{
    Nano7MbxState *s = mbx_singleton;

    if (s && s->scenes) {
        fprintf(stderr, "[nano7-mbx] summary: %" PRIu64 " scenes, %" PRIu64
                " renders done, %" PRIu64 " render jobs (nothing drawn)\n",
                s->scenes, s->renders, s->jobs);
    }
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
    /* interrupt enable as the driver's init 0x0812b90c leaves it: the
     * snapshot resumes after that init, and the IRQ 53 handler 0x08191de4
     * only saves/restores +0x130 around its acks */
    s->core[MBX_ENABLE] = 0x8775;
    /* MMU: the driver init 0x084850e4 wrote the physical addresses of its
     * eight page tables (object 0x099655f4: 0x0995c000 + n * 4 KB, snap6)
     * to +0x1000.. before T0; the renderer translates GPU addresses with
     * them (Session 32). A cold boot rewrites them. */
    for (unsigned i = 0; i < 8; i++) {
        s->core[(0x1000 >> 2) + i] = 0x0995c000 + 0x1000 * i;
    }
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

    if (s->trace && s->trace_n++ < 200000) {
        fprintf(stderr, "[nano7-mbx] W%u +0x%05x = 0x%08x pc=0x%08x\n",
                w->index, (unsigned)off, v,
                current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0);
    }

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
        /* TA scene stream (Session 31): batches into W4, each committed
         * by a doorbell word 1 to W6 (0x08180b10); the scene ends with the
         * end-of-list control word 0xf0000000 (other 0xf....... words carry
         * flags and are followed by more data). The hardware then reports
         * TA done (bit 5) and, chaining the render itself for a plain
         * frame, render done (bit 4: the handler 0x08191e64 marks the slot
         * rendered = state 3 and wakes ctx+0x2c). The fence 0x08192194
         * writes the same word with no scene armed and waits for bit 4. */
        if (fifo == 0) {
            mbxr_fifo_word(s->r, v);
        } else if (fifo == 2) {
            mbxr_doorbell(s->r);
        }
        if (fifo == 0 && v == 0xf0000000) {
            s->ta_end = true;
            nano7_timer_mod(s->ta_timer, nano7_now_ns() + MBX_TA_NS);
        }
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
        /* software-settable events: the driver writes 1 (wake-up IRQ,
         * 0x083f1e8c) or 0x20 (fake TA done, 0x0812c37a) */
        s->core[MBX_STATUS] |= v & 0xffff;
        break;
    case 0x6ec:
        /* render job kick: the scheduler 0x0813645a writes the job's
         * register pairs (+0x60c..+0x6dc: parameter base, target address,
         * size, stride) via 0x083f1a88, then +0x6ec. Completion = bits 2|3
         * and 6 together (0x08148c48 frees the render slot, sets ctx+0x30) */
        s->core[n] = v;
        mbxr_render(s->r, s->core);
        nano7_timer_mod(s->job_timer, nano7_now_ns() + MBX_JOB_NS);
        break;
    case 0x800:
        /* TA start (scene begin, 0x0812c394) */
        s->core[n] = v & ~1u;
        if (v & 1) {
            s->ta_armed = true;
            mbxr_scene_begin(s->r);
            s->scenes++;
        }
        break;
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
            /* render (ISP/TSP) of the scene, written by 0x08195e50 once
             * the TA is done; it polls bit 4 inline. Then the job is done:
             * bits 2|3 and 6 together let 0x08148c48 free the render slot.
             * TODO: render to guest surfaces; no pixels are changed yet. */
            s->core[MBX_STATUS] |= 0x10 | 0x0c | 0x40;
            s->renders++;
        }
        break;
    case 0x838:
        /* Transfer job (+0x824 source, +0x828/+0x82c size, +0x83c target,
         * written by the pair worker 0x083f1a88): the caller 0x080b7eb4
         * polls status bit 6 with that interrupt masked (Session 27).
         * Completes at once; no pixels are moved. */
        s->core[n] = v & ~1u;
        if (v & 1) {
            s->core[MBX_STATUS] |= 0x40;
        }
        break;
    case 0x818:
    case 0x81c:
        /* Render jobs of the GL flush (Session 31): the submit 0x0813175c
         * claims a slot (0/1), writes it to +0x810, then +0x81c = 1 (the
         * pair-list job 0x08131b8c uses +0x818 = 1) and polls a word that
         * the IRQ 53 handler sets on status bit 8 (slot 0) / 9 (slot 1),
         * 0.5 s timeout -> assert. Completes at once; nothing is drawn. */
        s->core[n] = v & ~1u;
        if (v & 1) {
            s->core[MBX_STATUS] |= 0x100u << (s->core[0x810 / 4] & 1);
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
    mbxr_reset(((Nano7MbxState *)opaque)->r);
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
        VMSTATE_TIMER_PTR(ta_timer, Nano7MbxState),
        VMSTATE_TIMER_PTR(job_timer, Nano7MbxState),
        VMSTATE_BOOL(ta_armed, Nano7MbxState),
        VMSTATE_BOOL(ta_end, Nano7MbxState),
        VMSTATE_BOOL(render_pending, Nano7MbxState),
        VMSTATE_END_OF_LIST()
    },
};

static void mbx_init(Object *obj)
{
    Nano7MbxState *s = NANO7_MBX(obj);
    unsigned i;

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->trace = getenv("NANO7_MBX_TRACE") != NULL;
    mbx_singleton = s;
    s->r = mbxr_new();
    atexit(mbx_summary);
    s->ta_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, mbx_ta_done, s);
    nano7_clock_track(s->ta_timer, true);
    s->job_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, mbx_job_done, s);
    nano7_clock_track(s->job_timer, true);
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
