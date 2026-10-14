/*
 * nano7_touch.c -- the iPod nano 7G multitouch controller as RetailOS sees
 * it: serial port 2 at 0x3d200000 (VIC IRQ 11) plus the report-ready line
 * GPIO 0x26 (group 1 bit 30, VIC IRQ 32). The host mouse drives it: press,
 * drag and release become touch reports (one contact).
 *
 * Driver (Session 21 RE, snap6; docs/research/touch-input.md): the ready
 * callback 0x08163ab1 masks GPIO 0x26 and wakes MultitouchTask (event
 * 0x43), which runs 0x0816b5d8: a 16-byte length query, then a fetch of
 * L + 5 bytes, then re-arms the line (0x083f6f3c(0x26, 1): mask bit set,
 * pending cleared). Each transfer is full duplex through 0x083f4ee8:
 *   +0x00 control   bits 2/3 = flush both FIFOs (driver spins on levels)
 *   +0x04 xfer ctl  read back; bit8 = START (cleared by the handler)
 *   +0x08 status    bit0 / bit22 done, bit1 TX wants data, bit5 RX has
 *                   data (W1C); bits 10:6 TX level, 15:11 RX level (live)
 *   +0x0c CS        bit1 clear = selected     +0x10 TX (word, low byte)
 *   +0x20 RX pop    +0x34 / +0x4c lengths (written, = transfer size)
 * Handler 0x08000fec: drains RX while the level is nonzero, fills TX on
 * bit1, signals event 0x44 on bit0/bit22, writes the status back.
 * Replies (by tx[2]): query (0) -> 16 bytes, LE16 L at 1, LE16 sum of
 * bytes 0..13 at 14 (L = 0: nothing to report); fetch (1) -> EA, 01, LE16
 * L, header checksum (bytes 0..4 sum to 0), payload (L - 2 bytes), LE16
 * payload sum. Payload: [0] 0x44, [2] record offset, [0x10] count, [0x11]
 * stride; record: id, state (3 down, 4 move, >= 5 up), type (1), 0, s16 X,
 * s16 Y raw. Pixels: x = (rawX + 75) * 239 / 2845, y = 431 - (rawY + 75) *
 * 431 / 5250 (inverted here). Down / move / up are separate reports, each
 * announced by a falling edge on GPIO 0x26 once the OS has re-armed it.
 *
 * Model: transfers complete ~20 us after the last TX byte; the ready line
 * goes low while a report is queued and the line is armed, and back high
 * when the chip select is released after the transfer that served it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "nano7_touch.h"
#include "nano7_gpio.h"
#include "qemu/timer.h"
#include "ui/input.h"
#include "nano7_ui.h"
#include "migration/vmstate.h"
#include "nano7_clock.h"

#define TP_SIZE         0x1000
#define TP_MAX_XFER     0x200
#define TP_QUEUE        32
#define TP_XFER_NS      20000
#define TP_POLL_NS      2000000         /* ready-line re-assert check */
#define TP_PAYLOAD      0x22            /* one contact record */
#define TP_REC_OFF      0x12
#define TP_REC_STRIDE   0x10
#define TP_W            240
#define TP_H            432

#define ST_DONE         (1u << 0)
#define ST_TXREQ        (1u << 1)
#define ST_RXDATA       (1u << 5)
#define ST_DONE2        (1u << 22)

typedef struct TouchReport {
    uint8_t state;
    int16_t x, y;                       /* raw controller coordinates */
} TouchReport;

typedef struct Nano7TouchState {
    MemoryRegion mr;
    uint32_t regs[TP_SIZE / 4];
    uint32_t flags;                     /* latched status bits */
    qemu_irq irq;
    QEMUTimer *xfer_timer, *poll_timer;
    uint8_t tx[TP_MAX_XFER], rx[TP_MAX_XFER];
    unsigned tx_len, tx_want, rx_len, rx_pos;
    bool busy, served;                  /* transfer active / took a report */
    bool line_low;
    TouchReport q[TP_QUEUE];
    unsigned q_head, q_n;
    int px, py;                         /* pointer position, display px */
    int64_t abs_u, abs_v;               /* raw absolute pointer position */
    int last_px, last_py;               /* position of the last report */
    bool down;
    bool moved;
    uint64_t reports, fetches;
} Nano7TouchState;

static void tp_update(Nano7TouchState *s)
{
    qemu_set_irq(s->irq, (s->flags & (ST_DONE | ST_TXREQ | ST_RXDATA |
                                      ST_DONE2)) != 0);
}

static uint32_t tp_status(Nano7TouchState *s)
{
    unsigned rx_level = MIN(s->rx_len - s->rx_pos, 31u);

    return s->flags | rx_level << 11;   /* TX level always 0 */
}

/* drive the ready line low when a report waits and the OS listens */
static void tp_poll(void *opaque)
{
    Nano7TouchState *s = opaque;

    if (s->q_n && !s->line_low && !s->busy &&
        nano7_gpio_irq_armed(NANO7_GPIO_TOUCH)) {
        s->line_low = true;
        nano7_gpio_set_level(NANO7_GPIO_TOUCH, 0);
    }
    if (s->q_n) {
        timer_mod_ns(s->poll_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                     TP_POLL_NS);
    }
}

static void tp_queue(Nano7TouchState *s, uint8_t state, int px, int py)
{
    TouchReport *r;

    px = MAX(0, MIN(px, TP_W - 1));
    py = MAX(0, MIN(py, TP_H - 1));
    if (state == 4 && s->q_n) {         /* coalesce queued moves */
        TouchReport *last = &s->q[(s->q_head + s->q_n - 1) % TP_QUEUE];

        if (last->state == 4) {
            r = last;
            goto fill;
        }
    }
    if (s->q_n == TP_QUEUE) {
        return;
    }
    r = &s->q[(s->q_head + s->q_n++) % TP_QUEUE];
fill:
    r->state = state;
    /* invert x = (rawX + 75) * 239 / 2845,
     *        y = 431 - (rawY + 75) * 431 / 5250 */
    r->x = (px * 2845 + 238) / 239 - 75;
    r->y = ((TP_H - 1 - py) * 5250 + 430) / 431 - 75;
    if (s->reports++ < 64) {
        fprintf(stderr, "[nano7-touch] t=%.3f s %s at (%d, %d) raw (%d, %d)\n",
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9,
                state == 3 ? "down" : state == 4 ? "move" : "up", px, py,
                r->x, r->y);
    }
    tp_poll(s);
}

static void tp_put16(uint8_t *p, unsigned v)
{
    p[0] = v;
    p[1] = v >> 8;
}

/* all TX bytes are in: build the full-duplex reply */
static void tp_reply(Nano7TouchState *s)
{
    unsigned n = s->tx_want, i, sum = 0;
    uint8_t *r = s->rx;

    memset(r, 0, n);
    s->served = false;
    if (n >= 3 && s->tx[2] == 0 && n >= 16) {           /* length query */
        r[0] = 0xea;
        tp_put16(r + 1, s->q_n ? TP_PAYLOAD + 2 : 0);
        for (i = 0; i < 14; i++) {
            sum += r[i];
        }
        tp_put16(r + 14, sum);
    } else if (n >= 3 && s->tx[2] == 1 && s->q_n &&
               n == TP_PAYLOAD + 2 + 5) {               /* fetch */
        TouchReport *rep = &s->q[s->q_head];
        uint8_t *p = r + 5;

        r[0] = 0xea;
        r[1] = 0x01;
        tp_put16(r + 2, TP_PAYLOAD + 2);
        r[4] = -(r[0] + r[1] + r[2] + r[3]);
        p[0] = 0x44;
        p[2] = TP_REC_OFF;
        p[0x10] = 1;
        p[0x11] = TP_REC_STRIDE;
        p[TP_REC_OFF + 0] = 1;                          /* contact id */
        p[TP_REC_OFF + 1] = rep->state;
        p[TP_REC_OFF + 2] = 1;                          /* type: finger */
        tp_put16(p + TP_REC_OFF + 4, (uint16_t)rep->x);
        tp_put16(p + TP_REC_OFF + 6, (uint16_t)rep->y);
        for (i = 0; i < TP_PAYLOAD; i++) {
            sum += p[i];
        }
        tp_put16(r + 5 + TP_PAYLOAD, sum);
        s->q_head = (s->q_head + 1) % TP_QUEUE;
        s->q_n--;
        s->served = true;
        s->fetches++;
    }
    s->rx_len = n;
    s->rx_pos = 0;
}

static void tp_xfer_done(void *opaque)
{
    Nano7TouchState *s = opaque;

    tp_reply(s);
    s->busy = false;
    s->flags |= ST_DONE | ST_DONE2 | ST_RXDATA;
    tp_update(s);
}

static uint64_t tp_read(void *opaque, hwaddr off, unsigned size)
{
    Nano7TouchState *s = opaque;

    switch (off) {
    case 0x08:
        return tp_status(s);
    case 0x20:
        return s->rx_pos < s->rx_len ? s->rx[s->rx_pos++] : 0;
    default:
        return s->regs[off / 4];
    }
}

static void tp_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    Nano7TouchState *s = opaque;
    uint32_t v = value, old = s->regs[off / 4];

    switch (off) {
    case 0x00:
        if (v & 0xc) {                  /* flush FIFOs */
            s->rx_len = s->rx_pos = 0;
        }
        break;
    case 0x04:
        if ((v & 0x100) && !(old & 0x100) && !s->busy) {    /* START */
            s->tx_want = MIN(s->regs[0x4c / 4] ? s->regs[0x4c / 4] :
                             s->regs[0x34 / 4], TP_MAX_XFER);
            s->tx_len = 0;
            s->busy = true;
            s->flags |= ST_TXREQ;
        }
        break;
    case 0x08:
        s->flags &= ~v;
        break;
    case 0x0c:
        if ((v & 2) && !(old & 2) && s->line_low && s->served) {
            s->line_low = false;        /* report delivered: line idle */
            nano7_gpio_set_level(NANO7_GPIO_TOUCH, 1);
            s->served = false;
            tp_poll(s);
        }
        break;
    case 0x10:
        if (s->busy && s->tx_len < s->tx_want) {
            s->tx[s->tx_len++] = v;
            if (s->tx_len == s->tx_want) {
                s->flags &= ~ST_TXREQ;
                timer_mod_ns(s->xfer_timer,
                             qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                             TP_XFER_NS);
            }
        }
        break;
    }
    s->regs[off / 4] = v;
    tp_update(s);
}

static const MemoryRegionOps tp_ops = {
    .read = tp_read,
    .write = tp_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/* host mouse (absolute: VNC, GTK, SDL) -> touch reports */
static void tp_input_event(DeviceState *dev, QemuConsole *src,
                           InputEvent *evt)
{
    Nano7TouchState *s = (Nano7TouchState *)dev;

    switch (evt->type) {
    case INPUT_EVENT_KIND_ABS: {
        InputMoveEvent *m = evt->u.abs.data;
        int64_t v = m->value - INPUT_EVENT_ABS_MIN;
        int64_t range = INPUT_EVENT_ABS_MAX - INPUT_EVENT_ABS_MIN + 1;

        if (m->axis == INPUT_AXIS_X) {
            s->abs_u = v;
        } else if (m->axis == INPUT_AXIS_Y) {
            s->abs_v = v;
        }
        /* the console may be rotated / zoomed (nano7_ui.c) */
        nano7_ui_map_touch(s->abs_u, s->abs_v, range, &s->px, &s->py);
        s->moved = true;
        break;
    }
    case INPUT_EVENT_KIND_BTN: {
        InputBtnEvent *b = evt->u.btn.data;

        if (b->button != INPUT_BUTTON_LEFT || b->down == s->down) {
            break;
        }
        s->down = b->down;
        tp_queue(s, b->down ? 3 : 5, s->px, s->py);
        s->last_px = s->px;
        s->last_py = s->py;
        s->moved = false;
        break;
    }
    default:
        break;
    }
}

static void tp_input_sync(DeviceState *dev)
{
    Nano7TouchState *s = (Nano7TouchState *)dev;

    if (s->down && s->moved &&
        (s->px != s->last_px || s->py != s->last_py)) {
        tp_queue(s, 4, s->px, s->py);
        s->last_px = s->px;
        s->last_py = s->py;
    }
    s->moved = false;
}

static const QemuInputHandler tp_input_handler = {
    .name  = "nano7 touchscreen",
    .mask  = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = tp_input_event,
    .sync  = tp_input_sync,
};

static int tp_post_load(void *opaque, int version_id)
{
    tp_update(opaque);
    return 0;
}

static const VMStateDescription vmstate_touch = {
    .name = "nano7-touch",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = tp_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Nano7TouchState, TP_SIZE / 4),
        VMSTATE_UINT32(flags, Nano7TouchState),
        VMSTATE_TIMER_PTR(xfer_timer, Nano7TouchState),
        VMSTATE_TIMER_PTR(poll_timer, Nano7TouchState),
        VMSTATE_UINT8_ARRAY(tx, Nano7TouchState, TP_MAX_XFER),
        VMSTATE_UINT8_ARRAY(rx, Nano7TouchState, TP_MAX_XFER),
        VMSTATE_UINT32(tx_len, Nano7TouchState),
        VMSTATE_UINT32(tx_want, Nano7TouchState),
        VMSTATE_UINT32(rx_len, Nano7TouchState),
        VMSTATE_UINT32(rx_pos, Nano7TouchState),
        VMSTATE_BOOL(busy, Nano7TouchState),
        VMSTATE_BOOL(served, Nano7TouchState),
        VMSTATE_BOOL(line_low, Nano7TouchState),
        VMSTATE_BUFFER_UNSAFE(q, Nano7TouchState, 1,
                              sizeof(TouchReport) * TP_QUEUE),
        VMSTATE_UINT32(q_head, Nano7TouchState),
        VMSTATE_UINT32(q_n, Nano7TouchState),
        VMSTATE_END_OF_LIST()
    }
};

void nano7_touch_attach(MemoryRegion *parent, hwaddr base, qemu_irq irq)
{
    Nano7TouchState *s = g_new0(Nano7TouchState, 1);
    QemuInputHandlerState *hs;

    s->irq = irq;
    s->xfer_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tp_xfer_done, s);
    s->poll_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tp_poll, s);
    nano7_clock_track(s->xfer_timer, false);
    nano7_clock_track(s->poll_timer, false);
    nano7_gpio_model_pin(NANO7_GPIO_TOUCH, 1);
    vmstate_register_any(NULL, &vmstate_touch, s);
    memory_region_init_io(&s->mr, NULL, &tp_ops, s, "nano7.touch", TP_SIZE);
    memory_region_add_subregion_overlap(parent, base, &s->mr, 0);
    /* the handler only uses dev as an opaque cookie */
    hs = qemu_input_handler_register((DeviceState *)s, &tp_input_handler);
    qemu_input_handler_activate(hs);
}
