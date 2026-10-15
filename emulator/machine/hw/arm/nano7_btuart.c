/*
 * nano7_btuart.c -- UART port 1 (0x3db00000, VIC IRQ 25) of the iPod nano
 * 7G (S5L8747) with the Bluetooth controller behind it: a Broadcom
 * BCM2076B1 (BT + FM) speaking HCI H4.
 *
 * Register use, as RetailOS drives it (Session 32 RE, snap6; port table
 * 0x089180fc: port 1 = 0x3db00000, DMA request TX 4 / RX 5, IRQ 25):
 *   +0x00  line control  3 (8N1)              reset 0x08550b44
 *   +0x04  control       0x405, |0xc TX DMA, |3 RX DMA, |0x15880 (port 1)
 *   +0x08  FIFO control  7 (enable + reset both FIFOs)
 *   +0x0c  modem control 1 = RTS, 0x11 = auto flow control (open 0x08550d4c)
 *   +0x10  status        bit0 = RX data available (read-only); interrupt
 *                        pending bits, write 1 to clear (decoder 0x08001868
 *                        loops while +0x10 & 0x178): bit3 / bit4 = RX
 *                        (timeout / threshold), bit5 = TX, bit6 = error
 *                        (cause in +0x14), bit8 = autobaud
 *   +0x14  error status  (0 here)
 *   +0x18  FIFO status   bit9 = TX FIFO full (polled before CPU TX writes)
 *   +0x20  TX data       the BT driver feeds it by DMA (engine 1, req 4)
 *   +0x24  RX data       read by the IRQ 25 handler 0x08150b8c while bit0 is
 *                        set, into a 32 KB ring; then event 0x45 wakes
 *                        'BT Uart RxLoop' (0x081920ac)
 *   +0x28  baud divisor  115200 at power-on, 2.4 Mbaud after HCI 0xfc18
 *
 * Behind the UART either
 *   - the built-in fake controller (default): parses the host's H4 stream
 *     and answers every command with a Command Complete (or Command Status
 *     for the link-control commands that work that way) carrying plausible
 *     return parameters of a BCM2076B1. One simulated peer is in range
 *     (bt-peer=gamepad, the default; none = nobody): a HID gamepad
 *     "NanoPad", class 0x002508 (Peripheral / Gamepad), EIR = name + HID
 *     UUID 0x1124, reported ~0.4 s into every inquiry as an (Extended /
 *     RSSI / standard) Inquiry Result per Write_Inquiry_Mode, and its
 *     Remote_Name_Request succeeds. Inquiries end after their real length.
 *     The peer accepts connections (Create_Connection) and, with
 *     bt-peer=gamepad-connect, pages the iPod itself ~3 s after the host
 *     turns page scan on (Connection_Request, like a HID device
 *     reconnecting; it then opens HID control and leaves security to the
 *     host). Pairing (host-initiated): stored link key (the peer's key is fixed, so
 *     a key the OS saved earlier keeps matching), else Secure Simple
 *     Pairing as a NoInputNoOutput device (IO capability exchange, numeric
 *     comparison 123456, Link_Key_Notification type 4) or legacy PIN; then
 *     authentication / encryption. On the ACL link it answers L2CAP
 *     signalling (info, echo, config, disconnect), accepts SDP (PSM 1) and
 *     the HID PSMs 0x11 / 0x13 and refuses the others; its SDP server holds
 *     a HID record (gamepad subclass, a 16-button / 4-axis report
 *     descriptor) and a Device ID record. Once the link is encrypted it
 *     opens HID control (PSM 0x11) and interrupt (0x13) itself, as a
 *     reconnecting gamepad does; with the interrupt channel up it sends
 *     HIDP input reports (0xa1 + 6 bytes: buttons 1-16, X, Y, Z, Rz) from
 *     the bt-pad script (times relative to HID up) and live from the host
 *     keyboard in pad mode (F8; keys at bt_pad_live) and from a real host
 *     gamepad (nano7_hostpad.c), and answers
 *     GET/SET_REPORT, GET/SET_PROTOCOL, SET_IDLE on control; or
 *   - bt-hci=PATH | tcp:[HOST]:PORT: a listening socket that carries the
 *     raw H4 byte stream both ways (no framing added), for a host bridge
 *     (a real adapter, BlueZ vhci, or a scripted controller).
 *
 * Replies reach the RX FIFO after a short delay (the controller's turn-
 * around), never inside the TX write. NANO7_BT_TRACE=1 logs every packet
 * (default: the first 2000, compact; patchram Write_RAM records are only
 * counted).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "nano7_btuart.h"
#include "nano7_clock.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"
#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "chardev/char.h"
#include "chardev/char-fe.h"
#include "migration/vmstate.h"
#include "ui/input.h"

#define BT_SIZE         0x1000
#define BT_RXQ          65536           /* power of two */
#define BT_REPLY_NS     200000          /* controller turn-around */
#define BT_BYTE_NS      4167            /* one byte at 2.4 Mbaud */
#define BT_LOG_MAX      2000

#define ST_RXDATA       (1u << 0)
#define ST_RXTO         (1u << 3)
#define ST_RX           (1u << 4)
#define ST_TX           (1u << 5)
#define ST_ERR          (1u << 6)
#define ST_PENDING      0x178u

typedef struct Nano7BtUartState {
    MemoryRegion mr;
    qemu_irq irq;
    uint32_t regs[0x40 / 4];
    uint32_t pending;               /* +0x10 interrupt bits */
    /* RX FIFO (controller -> host) */
    uint8_t rxq[BT_RXQ];
    uint32_t rx_head, rx_tail;      /* free-running indices */
    /* replies produced by the fake controller, not yet on the wire */
    uint8_t outq[BT_RXQ];
    uint32_t out_head, out_tail;
    QEMUTimer *reply_timer;
    QEMUTimer *inquiry_timer;       /* running HCI_Inquiry */
    QEMUTimer *peer_timer;          /* the peer answers the inquiry */
    bool peer;                      /* simulated gamepad in range */
    uint8_t inquiry_mode;           /* 0 standard, 1 RSSI, 2 extended */
    uint32_t peer_results;
    /* simulated peer link (one ACL connection) */
    bool peer_connect;              /* bt-peer=gamepad-connect */
    bool connect_done;
    QEMUTimer *connect_timer;
    QEMUTimer *auth_timer;          /* peer starts auth / pairing timeout */
    uint8_t link;                   /* 0 none, 1 request pending, 2 up */
    uint8_t link_incoming, auth_host, auth_step, link_enc;
    uint8_t sig_id;
    uint16_t ch_psm[4], ch_hcid[4]; /* L2CAP channels; ours = 0x40 + i */
    uint8_t ch_state[4];            /* 0 free, 1 config, 2 open */
    uint8_t ch_mine[4];             /* the peer opened it */
    /* HID gamepad */
    bool hid_up;
    uint16_t pad_buttons;
    uint8_t pad_axis[4];            /* X, Y, Z, Rz (int8 values) */
    QEMUTimer *pad_timer;
    uint32_t pad_next;              /* next script step */
    int64_t pad_t0;
    uint32_t reports;
    uint32_t pairings, l2cap_rx;
    /* host -> controller H4 parser */
    uint8_t pkt[4 + 65536];
    uint32_t pkt_len, pkt_need;
    /* fake controller state */
    uint8_t scan_enable;
    uint8_t local_name[248];
    uint8_t cod[3];
    uint8_t ssp_mode, le_host;
    uint8_t bdaddr[6];
    uint32_t baud;
    /* socket backend */
    CharBackend chr;
    bool use_chr;
    /* stats + trace */
    bool trace;
    unsigned log_lines;
    uint64_t tx_bytes, rx_bytes, cmds, events, acl_tx;
    uint32_t patch_records, patch_bytes;
    bool rd_seen;
} Nano7BtUartState;

static Nano7BtUartState *bt_dev;

/* simulated peer: HID gamepad (locally administered address) */
static const uint8_t bt_peer_addr[6] = { 0x01, 0x44, 0x41, 0x50, 0x4e, 0x02 };
static const char bt_peer_name[] = "NanoPad";
static uint8_t bt_peer_cod[3] = { 0x08, 0x25, 0x00 };   /* 0x002508 */
#define BT_PEER_RSSI    0xd3                    /* -45 dBm */
#define BT_PEER_CLKOFF  0x9234                  /* bit15 = valid */
#define BT_PEER_DELAY   400000000LL             /* into the inquiry */

#define BT_PEER_HANDLE  0x0040
#define BT_PEER_CONFIRM 123456                  /* numeric comparison */
#define BT_CONNECT_NS   3000000000LL            /* peer pages the iPod */
#define BT_PEER_AUTH_NS 1000000000LL            /* then wants a bond */
#define BT_PAIR_TMO_NS  30000000000LL           /* LMP response timeout */
/* the gamepad's half of the bond: fixed, so a key the OS stored in an
 * earlier run (NAND store) matches again after a reboot */
static const uint8_t bt_peer_key[16] = "NanoPad-LinkKey!";

/* until the host writes the device's own address (BCM 0xfc01) */
static const uint8_t bt_bdaddr_default[6] = { 0x01, 0x00, 0x00, 0x37, 0x4e,
                                              0x02 };

static uint32_t bt_rx_count(Nano7BtUartState *s)
{
    return s->rx_head - s->rx_tail;
}

static void bt_update(Nano7BtUartState *s)
{
    qemu_set_irq(s->irq, (s->pending & ST_PENDING) != 0);
}

static void bt_rx_push(Nano7BtUartState *s, const uint8_t *b, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n && bt_rx_count(s) < BT_RXQ; i++) {
        s->rxq[s->rx_head++ & (BT_RXQ - 1)] = b[i];
    }
    s->rx_bytes += i;
    if (i) {
        s->pending |= ST_RX;
        bt_update(s);
    }
}

/* ---- trace ----------------------------------------------------------- */

static const char *bt_cmd_name(uint16_t op)
{
    switch (op) {
    case 0x0401: return "Inquiry";
    case 0x0402: return "Inquiry_Cancel";
    case 0x0405: return "Create_Connection";
    case 0x0406: return "Disconnect";
    case 0x0409: return "Accept_Connection_Request";
    case 0x040b: return "Link_Key_Request_Reply";
    case 0x040c: return "Link_Key_Request_Negative_Reply";
    case 0x0419: return "Remote_Name_Request";
    case 0x042b: return "IO_Capability_Request_Reply";
    case 0x042c: return "User_Confirmation_Request_Reply";
    case 0x0c01: return "Set_Event_Mask";
    case 0x0c03: return "Reset";
    case 0x0c05: return "Set_Event_Filter";
    case 0x0c13: return "Write_Local_Name";
    case 0x0c14: return "Read_Local_Name";
    case 0x0c16: return "Write_Connection_Accept_Timeout";
    case 0x0c18: return "Write_Page_Timeout";
    case 0x0c1a: return "Write_Scan_Enable";
    case 0x0c1e: return "Write_Inquiry_Scan_Activity";
    case 0x0c20: return "Write_Authentication_Enable";
    case 0x0c23: return "Read_Class_Of_Device";
    case 0x0c24: return "Write_Class_Of_Device";
    case 0x0c33: return "Host_Buffer_Size";
    case 0x0c45: return "Write_Inquiry_Mode";
    case 0x0c52: return "Write_Extended_Inquiry_Response";
    case 0x0c56: return "Write_Simple_Pairing_Mode";
    case 0x0c6d: return "Write_LE_Host_Supported";
    case 0x1001: return "Read_Local_Version";
    case 0x1002: return "Read_Local_Supported_Commands";
    case 0x1003: return "Read_Local_Supported_Features";
    case 0x1004: return "Read_Local_Extended_Features";
    case 0x1005: return "Read_Buffer_Size";
    case 0x1009: return "Read_BD_ADDR";
    case 0x2001: return "LE_Set_Event_Mask";
    case 0x2002: return "LE_Read_Buffer_Size";
    case 0x2003: return "LE_Read_Local_Supported_Features";
    case 0x2006: return "LE_Set_Advertising_Parameters";
    case 0x2008: return "LE_Set_Advertising_Data";
    case 0x200a: return "LE_Set_Advertise_Enable";
    case 0x200b: return "LE_Set_Scan_Parameters";
    case 0x200c: return "LE_Set_Scan_Enable";
    case 0xfc01: return "BCM_Write_BD_ADDR";
    case 0xfc15: return "BCM_FM";
    case 0xfc18: return "BCM_Update_UART_Baud_Rate";
    case 0xfc1c: return "BCM_Write_SCO_PCM_Int_Param";
    case 0xfc1e: return "BCM_Write_PCM_Data_Format";
    case 0xfc27: return "BCM_Write_Sleep_Mode";
    case 0xfc2e: return "BCM_Download_Minidriver";
    case 0xfc4c: return "BCM_Write_RAM";
    case 0xfc4e: return "BCM_Launch_RAM";
    case 0xfc6d: return "BCM_Write_I2SPCM_Interface_Param";
    case 0xfc79: return "BCM_Read_Verbose_Config_Version";
    default:     return "";
    }
}

static void bt_trace(Nano7BtUartState *s, bool to_host, const uint8_t *p,
                     uint32_t n)
{
    uint32_t i, show;

    if (!s->trace && s->log_lines >= BT_LOG_MAX) {
        return;
    }
    /* patchram download: 151 records, summarized at Launch_RAM */
    if (!s->trace && n >= 6 && ((!to_host && p[0] == 1 && p[1] == 0x4c &&
                                 p[2] == 0xfc) ||
                                (to_host && p[0] == 4 && p[1] == 0x0e &&
                                 p[4] == 0x4c && p[5] == 0xfc))) {
        return;
    }
    s->log_lines++;
    fprintf(stderr, "[nano7-bt] t=%.3f %s ", nano7_now_ns() / 1e9,
            to_host ? "<" : ">");
    if (!to_host && p[0] == 1 && n >= 4) {
        uint16_t op = p[1] | p[2] << 8;

        fprintf(stderr, "cmd 0x%04x %s len %u", op, bt_cmd_name(op), p[3]);
    } else if (to_host && p[0] == 4 && n >= 3) {
        fprintf(stderr, "evt 0x%02x len %u", p[1], p[2]);
        if (p[1] == 0x0e && n >= 7) {
            fprintf(stderr, " (complete 0x%04x status %u)", p[4] | p[5] << 8,
                    p[6]);
        } else if (p[1] == 0x0f && n >= 7) {
            fprintf(stderr, " (status %u for 0x%04x)", p[3], p[5] | p[6] << 8);
        }
    } else if (p[0] == 2 && n >= 5) {
        fprintf(stderr, "acl handle 0x%03x len %u", (p[1] | p[2] << 8) & 0xfff,
                p[3] | p[4] << 8);
    } else {
        fprintf(stderr, "type %u len %u", p[0], n);
    }
    show = s->trace ? MIN(n, 64u) : MIN(n, 16u);
    fprintf(stderr, " :");
    for (i = 0; i < show; i++) {
        fprintf(stderr, " %02x", p[i]);
    }
    fprintf(stderr, "%s\n", show < n ? " .." : "");
    fflush(stderr);
}

/* ---- fake BCM2076B1 controller -------------------------------------- */

static void bt_reply(Nano7BtUartState *s, const uint8_t *p, uint32_t n)
{
    uint32_t i;

    bt_trace(s, true, p, n);
    s->events++;
    for (i = 0; i < n && s->out_head - s->out_tail < BT_RXQ; i++) {
        s->outq[s->out_head++ & (BT_RXQ - 1)] = p[i];
    }
    if (!timer_pending(s->reply_timer)) {
        nano7_timer_mod(s->reply_timer, nano7_now_ns() + BT_REPLY_NS +
                        (int64_t)n * BT_BYTE_NS);
    }
}

static void bt_reply_timer(void *opaque)
{
    Nano7BtUartState *s = opaque;
    uint8_t b[512];
    uint32_t n = 0;

    while (s->out_tail != s->out_head && n < sizeof(b)) {
        b[n++] = s->outq[s->out_tail++ & (BT_RXQ - 1)];
    }
    bt_rx_push(s, b, n);
    if (s->out_tail != s->out_head) {
        nano7_timer_mod(s->reply_timer, nano7_now_ns() + n * BT_BYTE_NS);
    }
}

static void bt_event(Nano7BtUartState *s, uint8_t code, const uint8_t *par,
                     uint8_t n);

static void bt_inquiry_timer(void *opaque)
{
    Nano7BtUartState *s = opaque;
    uint8_t done = 0;               /* Inquiry Complete, status 0 */

    timer_del(s->peer_timer);
    bt_event(s, 0x01, &done, 1);
}

/* the gamepad answers: one response in the format Write_Inquiry_Mode set */
static void bt_peer_timer(void *opaque)
{
    Nano7BtUartState *s = opaque;
    uint8_t r[255] = { 1 };         /* Num_Responses */
    uint8_t *p = r + 1;

    memcpy(p, bt_peer_addr, 6);
    p[6] = 1;                       /* page scan repetition mode R1 */
    switch (s->inquiry_mode) {
    case 2:                         /* Extended Inquiry Result */
        p[7] = 0;                   /* reserved */
        memcpy(p + 8, bt_peer_cod, 3);
        p[11] = BT_PEER_CLKOFF & 0xff;
        p[12] = BT_PEER_CLKOFF >> 8;
        p[13] = BT_PEER_RSSI;
        {
            uint8_t *e = p + 14;    /* 240-byte EIR, zero padded */
            bool a2dp = getenv("NANO7_BT_PEER_A2DP") != NULL;
            size_t nl = strlen(bt_peer_name);

            *e++ = 1 + nl;
            *e++ = 0x09;            /* Complete Local Name */
            memcpy(e, bt_peer_name, nl);
            e += nl;
            /* NANO7_BT_PEER_A2DP=1 also lists Audio Sink 0x110b: the only
             * way past the stock A2DP discovery vote 0x082b4dfc besides
             * an Audio/Video class (Session 37) */
            *e++ = a2dp ? 5 : 3;
            *e++ = 0x03;            /* Complete list of 16-bit UUIDs */
            *e++ = 0x24;            /* 0x1124 HID */
            *e++ = 0x11;
            if (a2dp) {
                *e++ = 0x0b;        /* 0x110b Audio Sink */
                *e++ = 0x11;
            }
        }
        bt_event(s, 0x2f, r, 255);
        break;
    case 1:                         /* Inquiry Result with RSSI */
        p[7] = 0;                   /* reserved */
        memcpy(p + 8, bt_peer_cod, 3);
        p[11] = BT_PEER_CLKOFF & 0xff;
        p[12] = BT_PEER_CLKOFF >> 8;
        p[13] = BT_PEER_RSSI;
        bt_event(s, 0x22, r, 15);
        break;
    default:                        /* Inquiry Result */
        p[7] = 0;                   /* reserved x2 */
        p[8] = 0;
        memcpy(p + 9, bt_peer_cod, 3);
        p[12] = BT_PEER_CLKOFF & 0xff;
        p[13] = BT_PEER_CLKOFF >> 8;
        bt_event(s, 0x02, r, 15);
        break;
    }
    if (s->peer_results++ == 0) {
        info_report("nano7-bt: peer \"%s\" (gamepad) reported to the "
                    "inquiry (mode %u)", bt_peer_name, s->inquiry_mode);
    }
}

static void bt_event(Nano7BtUartState *s, uint8_t code, const uint8_t *par,
                     uint8_t n)
{
    uint8_t b[260];

    b[0] = 4;
    b[1] = code;
    b[2] = n;
    memcpy(b + 3, par, n);
    bt_reply(s, b, 3 + n);
}

static void bt_cmd_complete(Nano7BtUartState *s, uint16_t op,
                            const uint8_t *ret, uint8_t n)
{
    uint8_t p[255];

    p[0] = 1;                       /* Num_HCI_Command_Packets */
    p[1] = op;
    p[2] = op >> 8;
    memcpy(p + 3, ret, n);
    bt_event(s, 0x0e, p, 3 + n);
}

static void bt_cmd_status(Nano7BtUartState *s, uint16_t op, uint8_t status)
{
    uint8_t p[4] = { status, 1, op, op >> 8 };

    bt_event(s, 0x0f, p, 4);
}

/* ---- simulated peer: connection, pairing, L2CAP ---------------------- */

static bool bt_is_peer(Nano7BtUartState *s, const uint8_t *a, uint8_t plen)
{
    return s->peer && plen >= 6 && !memcmp(a, bt_peer_addr, 6);
}

static bool bt_is_link(Nano7BtUartState *s, const uint8_t *par, uint8_t plen)
{
    return s->link == 2 && plen >= 2 &&
           ((par[0] | par[1] << 8) & 0xfff) == BT_PEER_HANDLE;
}

/* event with a status + connection handle prefix */
static void bt_link_event(Nano7BtUartState *s, uint8_t code, uint8_t status,
                          const uint8_t *more, uint8_t n)
{
    uint8_t p[32] = { status, BT_PEER_HANDLE & 0xff, BT_PEER_HANDLE >> 8 };

    memcpy(p + 3, more, n);
    bt_event(s, code, p, 3 + n);
}

/* event with a BD_ADDR prefix (pairing events) */
static void bt_addr_event(Nano7BtUartState *s, uint8_t code,
                          const uint8_t *pre, uint8_t npre,
                          const uint8_t *more, uint8_t n)
{
    uint8_t p[40];

    memcpy(p, pre, npre);
    memcpy(p + npre, bt_peer_addr, 6);
    memcpy(p + npre + 6, more, n);
    bt_event(s, code, p, npre + 6 + n);
}

static void bt_l2cap_send(Nano7BtUartState *s, uint16_t cid,
                          const uint8_t *pl, uint16_t n)
{
    uint8_t b[5 + 4 + 1000];

    if (n > 1000) {
        return;
    }
    b[0] = 2;
    b[1] = BT_PEER_HANDLE & 0xff;
    b[2] = (BT_PEER_HANDLE >> 8) | 0x20;    /* PB = first, flushable */
    b[3] = n + 4;
    b[4] = (n + 4) >> 8;
    b[5] = n;
    b[6] = n >> 8;
    b[7] = cid;
    b[8] = cid >> 8;
    memcpy(b + 9, pl, n);
    bt_reply(s, b, 9 + n);
}

/* one L2CAP signalling command from the peer */
static void bt_sig_send(Nano7BtUartState *s, uint8_t code, uint8_t id,
                        const uint8_t *d, uint8_t n)
{
    uint8_t c[64] = { code, id, n, 0 };

    memcpy(c + 4, d, n);
    bt_l2cap_send(s, 0x0001, c, 4 + n);
}

static void bt_sig_config_req(Nano7BtUartState *s, uint16_t host_cid)
{
    uint8_t d[4] = { host_cid, host_cid >> 8, 0, 0 };   /* no options */

    bt_sig_send(s, 0x04, ++s->sig_id, d, 4);
}

/* ---- the gamepad's input: bt-pad script -------------------------------
 * "SEC:CONTROLS[:MS]/..." with SEC relative to HID up; CONTROLS joined by
 * '+': a b x y l r select start home | b1..b16 | up down left right (left
 * stick) | lx= ly= rx= ry= -127..127; MS = hold (default 100), then
 * neutral. "none" = no input. */
#define PAD_MAX 128

typedef struct PadStep {
    int64_t t;                      /* ns after HID up */
    uint16_t buttons;
    int8_t axis[4];
} PadStep;

static PadStep pad_steps[PAD_MAX];
static uint32_t pad_nsteps;

static const char *const pad_names[] = {
    "a", "b", "x", "y", "l", "r", "select", "start", "home",
};

static bool pad_parse_controls(const char *c, PadStep *st)
{
    g_auto(GStrv) tok = g_strsplit(c, "+", -1);
    int i, k;

    for (i = 0; tok[i]; i++) {
        const char *t = tok[i];
        static const char *const ax[4] = { "lx=", "ly=", "rx=", "ry=" };
        bool done = false;

        for (k = 0; k < 9 && !done; k++) {
            if (!strcmp(t, pad_names[k])) {
                st->buttons |= 1u << k;
                done = true;
            }
        }
        if (!done && t[0] == 'b' && atoi(t + 1) >= 1 && atoi(t + 1) <= 16) {
            st->buttons |= 1u << (atoi(t + 1) - 1);
            done = true;
        }
        for (k = 0; k < 4 && !done; k++) {
            if (g_str_has_prefix(t, ax[k])) {
                st->axis[k] = MAX(-127, MIN(127, atoi(t + 3)));
                done = true;
            }
        }
        if (!done && !strcmp(t, "up"))    { st->axis[1] = -127; done = true; }
        if (!done && !strcmp(t, "down"))  { st->axis[1] = 127; done = true; }
        if (!done && !strcmp(t, "left"))  { st->axis[0] = -127; done = true; }
        if (!done && !strcmp(t, "right")) { st->axis[0] = 127; done = true; }
        if (!done) {
            return false;
        }
    }
    return true;
}

static int pad_step_cmp(const void *a, const void *b)
{
    const PadStep *x = a, *y = b;

    return x->t < y->t ? -1 : x->t > y->t;
}

static void pad_parse_script(const char *script)
{
    g_auto(GStrv) ent = NULL;
    int i;

    pad_nsteps = 0;
    if (!script || !strcmp(script, "none")) {
        return;
    }
    ent = g_strsplit(script, "/", -1);
    for (i = 0; ent[i] && pad_nsteps + 2 <= PAD_MAX; i++) {
        g_auto(GStrv) f = g_strsplit(ent[i], ":", 3);
        PadStep press = { 0 }, rel = { 0 };
        double sec;
        int ms;

        if (!f[0] || !f[1]) {
            error_report("nano7: bt-pad: bad entry '%s'", ent[i]);
            exit(1);
        }
        sec = atof(f[0]);
        ms = f[2] ? atoi(f[2]) : 100;
        press.t = sec * 1e9;
        if (!pad_parse_controls(f[1], &press)) {
            error_report("nano7: bt-pad: unknown control in '%s'", ent[i]);
            exit(1);
        }
        rel.t = press.t + (int64_t)ms * 1000000;
        pad_steps[pad_nsteps++] = press;
        pad_steps[pad_nsteps++] = rel;      /* back to neutral */
    }
    qsort(pad_steps, pad_nsteps, sizeof(PadStep), pad_step_cmp);
}

/* HIDP DATA | Input on the interrupt channel */
static void bt_hid_send_input(Nano7BtUartState *s)
{
    uint8_t r[7] = { 0xa1, s->pad_buttons, s->pad_buttons >> 8,
                     s->pad_axis[0], s->pad_axis[1], s->pad_axis[2],
                     s->pad_axis[3] };
    int i;

    for (i = 0; i < 4; i++) {
        if (s->ch_state[i] == 2 && s->ch_psm[i] == 0x13) {
            bt_l2cap_send(s, s->ch_hcid[i], r, sizeof(r));
            if (s->reports++ < 64) {
                info_report("nano7-bt: HID input report #%u: buttons 0x%04x "
                            "X %d Y %d Z %d Rz %d", s->reports,
                            s->pad_buttons, (int8_t)s->pad_axis[0],
                            (int8_t)s->pad_axis[1], (int8_t)s->pad_axis[2],
                            (int8_t)s->pad_axis[3]);
            }
            return;
        }
    }
}

static void bt_pad_arm(Nano7BtUartState *s)
{
    if (s->hid_up && s->pad_next < pad_nsteps) {
        nano7_timer_mod(s->pad_timer, s->pad_t0 + pad_steps[s->pad_next].t);
    }
}

static void bt_pad_timer(void *opaque)
{
    Nano7BtUartState *s = opaque;
    PadStep *st;

    if (!s->hid_up || s->pad_next >= pad_nsteps) {
        return;
    }
    st = &pad_steps[s->pad_next++];
    s->pad_buttons = st->buttons;
    memcpy(s->pad_axis, st->axis, 4);
    bt_hid_send_input(s);
    bt_pad_arm(s);
}

/* ---- the gamepad's input: live, from the host keyboard ----------------
 * In pad mode (F8, nano7_ui.c) these keys drive the pad instead of the
 * iPod; every change sends a report at once (state = the keys held):
 *   W A S D  left stick      arrows  right stick (Z / Rz)
 *   J = A  K = B  U = X  I = Y  Q = L  E = R
 *   Enter = start  Backspace = select  Esc = home
 * The bt-pad script keeps running alongside; the last change wins. */
static const struct {
    int qcode;
    int8_t button;                  /* bit, or -1 */
    int8_t axis, val;               /* axis 0..3 with its deflection */
} pad_keymap[] = {
    { Q_KEY_CODE_W, -1, 1, -127 }, { Q_KEY_CODE_S, -1, 1, 127 },
    { Q_KEY_CODE_A, -1, 0, -127 }, { Q_KEY_CODE_D, -1, 0, 127 },
    { Q_KEY_CODE_UP, -1, 3, -127 }, { Q_KEY_CODE_DOWN, -1, 3, 127 },
    { Q_KEY_CODE_LEFT, -1, 2, -127 }, { Q_KEY_CODE_RIGHT, -1, 2, 127 },
    { Q_KEY_CODE_J, 0 }, { Q_KEY_CODE_K, 1 }, { Q_KEY_CODE_U, 2 },
    { Q_KEY_CODE_I, 3 }, { Q_KEY_CODE_Q, 4 }, { Q_KEY_CODE_E, 5 },
    { Q_KEY_CODE_BACKSPACE, 6 }, { Q_KEY_CODE_RET, 7 },
    { Q_KEY_CODE_ESC, 8 },
};
static uint32_t pad_held;           /* bit i = pad_keymap[i] held */
static bool pad_mode;
/* a real gamepad on the host (nano7_hostpad.c), merged with the keys */
static uint16_t host_buttons;
static int8_t host_axis[4];

static void bt_pad_live(void)
{
    Nano7BtUartState *s = bt_dev;
    uint16_t buttons = host_buttons;
    int axis[4] = { host_axis[0], host_axis[1], host_axis[2], host_axis[3] };
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(pad_keymap); i++) {
        if (!(pad_held & (1u << i))) {
            continue;
        }
        if (pad_keymap[i].button >= 0) {
            buttons |= 1u << pad_keymap[i].button;
        } else {
            axis[pad_keymap[i].axis] += pad_keymap[i].val;
        }
    }
    s->pad_buttons = buttons;
    for (i = 0; i < 4; i++) {
        s->pad_axis[i] = MAX(-127, MIN(127, axis[i]));
    }
    if (s->hid_up) {
        bt_hid_send_input(s);
    } else {
        static bool told;

        if (!told) {
            told = true;
            info_report("nano7-bt: pad input ignored - the HID link is not up "
                        "(needs a HID host app on the iPod, e.g. "
                        "probe/hidhost)");
        }
    }
}

bool nano7_btuart_pad_key(int qcode, bool down)
{
    unsigned i;

    if (!pad_mode || !bt_dev) {
        return false;
    }
    for (i = 0; i < ARRAY_SIZE(pad_keymap); i++) {
        if (pad_keymap[i].qcode == qcode) {
            uint32_t held = down ? pad_held | 1u << i : pad_held & ~(1u << i);

            if (held != pad_held) {     /* ignore auto-repeat */
                pad_held = held;
                bt_pad_live();
            }
            return true;
        }
    }
    return false;
}

void nano7_btuart_pad_host(uint16_t buttons, const int8_t axis[4])
{
    if (!bt_dev) {
        return;
    }
    host_buttons = buttons;
    memcpy(host_axis, axis, 4);
    bt_pad_live();
}

void nano7_btuart_pad_mode(bool on)
{
    pad_mode = on && bt_dev;
    if (!pad_mode && pad_held && bt_dev) {
        pad_held = 0;               /* release everything */
        bt_pad_live();
    }
}

int nano7_btuart_pad_state(void)
{
    return !pad_mode ? 0 : bt_dev->hid_up ? 2 : 1;
}

static void bt_hid_up(Nano7BtUartState *s)
{
    if (s->hid_up) {
        return;
    }
    s->hid_up = true;
    s->pad_buttons = 0;
    memset(s->pad_axis, 0, 4);
    s->pad_next = 0;
    s->pad_t0 = nano7_now_ns();
    info_report("nano7-bt: HID link up (control + interrupt), %u scripted "
                "report steps", pad_nsteps);
    bt_hid_send_input(s);           /* initial neutral state */
    bt_pad_arm(s);
}

static void bt_link_reset(Nano7BtUartState *s)
{
    timer_del(s->auth_timer);
    s->link = 0;
    s->auth_step = 0;
    s->link_enc = 0;
    memset(s->ch_state, 0, sizeof(s->ch_state));
    memset(s->ch_mine, 0, sizeof(s->ch_mine));
    s->hid_up = false;
    timer_del(s->pad_timer);
}

static void bt_link_up(Nano7BtUartState *s, bool incoming)
{
    uint8_t cc[11] = { 0, BT_PEER_HANDLE & 0xff, BT_PEER_HANDLE >> 8 };

    bt_link_reset(s);
    memcpy(cc + 3, bt_peer_addr, 6);
    cc[9] = 1;                      /* ACL */
    bt_event(s, 0x03, cc, 11);
    s->link = 2;
    s->link_incoming = incoming;
    info_report("nano7-bt: peer \"%s\" connected (%s), handle 0x%04x",
                bt_peer_name, incoming ? "it paged the iPod" : "iPod paged it",
                BT_PEER_HANDLE);
}

/* the side that wants authentication starts with the link key lookup */
static void bt_auth_start(Nano7BtUartState *s, bool by_host)
{
    s->auth_host = by_host;
    s->auth_step = 1;
    bt_addr_event(s, 0x17, NULL, 0, NULL, 0);           /* Link_Key_Request */
    nano7_timer_mod(s->auth_timer, nano7_now_ns() + BT_PAIR_TMO_NS);
}

/* the peer opens an L2CAP channel to the iPod */
static void bt_peer_open(Nano7BtUartState *s, uint16_t psm)
{
    int i;

    for (i = 0; i < 4; i++) {
        if (s->ch_state[i] && s->ch_psm[i] == psm) {
            return;                 /* already there */
        }
    }
    for (i = 0; i < 4; i++) {
        if (!s->ch_state[i]) {
            uint8_t d[4] = { psm, psm >> 8, 0x40 + i, 0 };

            s->ch_psm[i] = psm;
            s->ch_hcid[i] = 0;
            s->ch_state[i] = 1;
            s->ch_mine[i] = 1;
            bt_sig_send(s, 0x02, ++s->sig_id, d, 4);
            info_report("nano7-bt: peer opens L2CAP PSM 0x%04x", psm);
            return;
        }
    }
}

/* encrypted link: a gamepad connects HID control first, then interrupt */
static void bt_peer_link_secure(Nano7BtUartState *s)
{
    bt_peer_open(s, 0x11);
}

static void bt_auth_done(Nano7BtUartState *s, uint8_t status)
{
    uint8_t on = 1;

    s->auth_step = 0;
    timer_del(s->auth_timer);
    if (s->auth_host) {
        bt_link_event(s, 0x06, status, NULL, 0);        /* Auth Complete */
    } else if (status == 0) {
        /* the peer asked for it: it switches encryption on itself */
        s->link_enc = 1;
        bt_link_event(s, 0x08, 0, &on, 1);              /* Encryption_Change */
        bt_peer_link_secure(s);
    } else {
        uint8_t dc = 0x05;                              /* auth failure */

        bt_link_event(s, 0x05, 0, &dc, 1);              /* Disconnection */
        bt_link_reset(s);
    }
    if (status) {
        info_report("nano7-bt: peer authentication failed (0x%02x)", status);
    }
}

static void bt_paired(Nano7BtUartState *s, uint8_t key_type)
{
    uint8_t k[17];

    memcpy(k, bt_peer_key, 16);
    k[16] = key_type;
    bt_addr_event(s, 0x18, NULL, 0, k, 17);         /* Link_Key_Notification */
    s->pairings++;
    info_report("nano7-bt: paired with \"%s\" (link key type %u)",
                bt_peer_name, key_type);
    bt_auth_done(s, 0);
}

/* commands that concern the peer or its link; false = not ours */
static bool bt_peer_command(Nano7BtUartState *s, uint16_t op,
                            const uint8_t *par, uint8_t plen)
{
    uint8_t r[8] = { 0 };
    uint8_t st0 = 0;

    switch (op) {
    case 0x0405:                    /* Create_Connection */
        if (!bt_is_peer(s, par, plen) || s->link) {
            return false;
        }
        bt_cmd_status(s, op, 0);
        bt_link_up(s, false);
        return true;
    case 0x0409:                    /* Accept_Connection_Request */
        if (!bt_is_peer(s, par, plen) || s->link != 1) {
            return false;
        }
        bt_cmd_status(s, op, 0);
        bt_link_up(s, true);
        /* the gamepad opens HID once the link is set up */
        nano7_timer_mod(s->auth_timer, nano7_now_ns() + BT_PEER_AUTH_NS);
        return true;
    case 0x040a: {                  /* Reject_Connection_Request */
        uint8_t cc[11] = { plen >= 7 ? par[6] : 0x0f };

        if (!bt_is_peer(s, par, plen) || s->link != 1) {
            return false;
        }
        bt_cmd_status(s, op, 0);
        memcpy(cc + 3, bt_peer_addr, 6);
        cc[9] = 1;
        bt_event(s, 0x03, cc, 11);
        s->link = 0;
        info_report("nano7-bt: the iPod rejected the peer's connection "
                    "(reason 0x%02x)", cc[0]);
        return true;
    }
    case 0x0406:                    /* Disconnect */
        if (!bt_is_link(s, par, plen)) {
            return false;
        }
        bt_cmd_status(s, op, 0);
        r[0] = 0x16;                /* connection terminated by local host */
        bt_link_event(s, 0x05, 0, r, 1);
        bt_link_reset(s);
        info_report("nano7-bt: peer disconnected by the iPod (reason 0x%02x)",
                    plen >= 3 ? par[2] : 0);
        return true;
    case 0x041b: {                  /* Read_Remote_Supported_Features */
        static const uint8_t f[8] = { 0xbf, 0xfe, 0x8f, 0xfe,
                                      0x98, 0x19, 0x09, 0x83 }; /* +SSP */
        if (!bt_is_link(s, par, plen)) {
            return false;
        }
        bt_cmd_status(s, op, 0);
        bt_link_event(s, 0x0b, 0, f, 8);
        return true;
    }
    case 0x041c: {                  /* Read_Remote_Extended_Features */
        static const uint8_t f0[8] = { 0xbf, 0xfe, 0x8f, 0xfe,
                                       0x98, 0x19, 0x09, 0x83 };
        uint8_t e[10] = { plen >= 3 ? par[2] : 0, 1 };

        if (!bt_is_link(s, par, plen)) {
            return false;
        }
        if (e[0] == 0) {
            memcpy(e + 2, f0, 8);
        } else if (e[0] == 1) {
            e[2] = 0x01;            /* Secure Simple Pairing (host) */
        }
        bt_cmd_status(s, op, 0);
        bt_link_event(s, 0x23, 0, e, 10);
        return true;
    }
    case 0x041d: {                  /* Read_Remote_Version_Information */
        static const uint8_t v[5] = { 4, 0x0a, 0x00, 0x2b, 0x21 }; /* 2.1 */

        if (!bt_is_link(s, par, plen)) {
            return false;
        }
        bt_cmd_status(s, op, 0);
        bt_link_event(s, 0x0c, 0, v, 5);
        return true;
    }
    case 0x0411:                    /* Authentication_Requested */
        if (!bt_is_link(s, par, plen)) {
            return false;
        }
        bt_cmd_status(s, op, 0);
        bt_auth_start(s, true);
        return true;
    case 0x0413:                    /* Set_Connection_Encryption */
        if (!bt_is_link(s, par, plen)) {
            return false;
        }
        bt_cmd_status(s, op, 0);
        s->link_enc = plen >= 3 ? par[2] : 1;
        bt_link_event(s, 0x08, 0, &s->link_enc, 1);
        info_report("nano7-bt: link encryption %s",
                    s->link_enc ? "on" : "off");
        if (s->link_enc) {
            bt_peer_link_secure(s);
        }
        return true;
    case 0x040f:                    /* Change_Connection_Packet_Type */
        if (!bt_is_link(s, par, plen)) {
            return false;
        }
        bt_cmd_status(s, op, 0);
        bt_link_event(s, 0x1d, 0, par + 2, 2);
        return true;
    case 0x0803:                    /* Sniff_Mode */
    case 0x0804:                    /* Exit_Sniff_Mode */
        if (!bt_is_link(s, par, plen)) {
            return false;
        }
        bt_cmd_status(s, op, 0);
        r[0] = op == 0x0803 ? 2 : 0;
        if (op == 0x0803 && plen >= 6) {
            r[1] = par[4];          /* interval = Sniff_Min_Interval */
            r[2] = par[5];
        }
        bt_link_event(s, 0x14, 0, r, 3);
        return true;
    case 0x080b: {                  /* Switch_Role */
        uint8_t st = 0;

        if (!bt_is_peer(s, par, plen) || s->link != 2) {
            return false;
        }
        bt_cmd_status(s, op, 0);
        r[0] = plen >= 7 ? par[6] : 0;
        bt_addr_event(s, 0x12, &st, 1, r, 1);
        return true;
    }
    /* Command Complete with the handle echoed (+ a few fields) */
    case 0x0809:                    /* Role_Discovery: master */
    case 0x080c:                    /* Read_Link_Policy_Settings */
    case 0x080d:                    /* Write_Link_Policy_Settings */
    case 0x0811:                    /* Sniff_Subrating */
    case 0x0c36:                    /* Read_Link_Supervision_Timeout */
    case 0x0c37:                    /* Write_Link_Supervision_Timeout */
    case 0x1401:                    /* Read_Failed_Contact_Counter */
    case 0x1405:                    /* Read_RSSI */
    case 0x1408: {                  /* Read_Encryption_Key_Size */
        uint8_t c[8] = { s->link == 2 ? 0 : 0x02 };
        uint8_t n = 3;

        if (plen < 2) {
            return false;
        }
        c[1] = par[0];
        c[2] = par[1];
        switch (op) {
        case 0x0809: c[3] = 0; n = 4; break;
        case 0x080c: c[3] = 0x05; n = 5; break;   /* role switch + sniff */
        case 0x0c36: c[3] = 0x00; c[4] = 0x7d; n = 5; break;
        case 0x1401: n = 5; break;
        case 0x1405: c[3] = BT_PEER_RSSI; n = 4; break;
        case 0x1408: c[3] = 16; n = 4; break;
        }
        bt_cmd_complete(s, op, c, n);
        return true;
    }

    /* pairing replies from the host */
    case 0x040b:                    /* Link_Key_Request_Reply */
        if (!bt_is_peer(s, par, plen) || s->auth_step != 1) {
            return false;
        }
        bt_addr_event(s, 0x0e, (uint8_t[]){ 1, op, op >> 8, 0 }, 4, NULL, 0);
        if (plen >= 22 && !memcmp(par + 6, bt_peer_key, 16)) {
            info_report("nano7-bt: peer authenticated with the stored link "
                        "key");
            bt_auth_done(s, 0);
        } else {
            bt_auth_done(s, 0x06);  /* PIN or key missing */
        }
        return true;
    case 0x040c:                    /* Link_Key_Request_Negative_Reply */
        if (!bt_is_peer(s, par, plen) || s->auth_step != 1) {
            return false;
        }
        bt_addr_event(s, 0x0e, (uint8_t[]){ 1, op, op >> 8, 0 }, 4, NULL, 0);
        if (s->ssp_mode) {
            s->auth_step = 2;
            bt_addr_event(s, 0x31, NULL, 0, NULL, 0);   /* IO_Capability_Req */
        } else {
            s->auth_step = 4;
            bt_addr_event(s, 0x16, NULL, 0, NULL, 0);   /* PIN_Code_Request */
        }
        return true;
    case 0x042b: {                  /* IO_Capability_Request_Reply */
        static const uint8_t io[3] = { 0x03, 0x00, 0x04 }; /* NoInputNoOutput,
                                                           * no OOB, general
                                                           * bonding */
        uint32_t v = BT_PEER_CONFIRM;
        uint8_t num[4] = { v, v >> 8, v >> 16, v >> 24 };

        if (!bt_is_peer(s, par, plen) || s->auth_step != 2) {
            return false;
        }
        bt_addr_event(s, 0x0e, (uint8_t[]){ 1, op, op >> 8, 0 }, 4, NULL, 0);
        info_report("nano7-bt: SSP: iPod IO capability %u, auth req 0x%02x",
                    plen >= 7 ? par[6] : 0, plen >= 9 ? par[8] : 0);
        bt_addr_event(s, 0x32, NULL, 0, io, 3);     /* IO_Capability_Resp */
        s->auth_step = 3;
        bt_addr_event(s, 0x33, NULL, 0, num, 4);    /* User_Confirmation_Req */
        return true;
    }
    case 0x0434:                    /* IO_Capability_Request_Negative_Reply */
    case 0x042d:                    /* User_Confirmation_Request_Neg_Reply */
    case 0x040e: {                  /* PIN_Code_Request_Negative_Reply */
        uint8_t st = 0x05;          /* authentication failure */

        if (!bt_is_peer(s, par, plen) || !s->auth_step) {
            return false;
        }
        bt_addr_event(s, 0x0e, (uint8_t[]){ 1, op, op >> 8, 0 }, 4, NULL, 0);
        if (op != 0x040e) {
            bt_addr_event(s, 0x36, &st, 1, NULL, 0);    /* SSP Complete */
        }
        bt_auth_done(s, 0x05);
        return true;
    }
    case 0x042c:                    /* User_Confirmation_Request_Reply */
        if (!bt_is_peer(s, par, plen) || s->auth_step != 3) {
            return false;
        }
        bt_addr_event(s, 0x0e, (uint8_t[]){ 1, op, op >> 8, 0 }, 4, NULL, 0);
        bt_addr_event(s, 0x36, &st0, 1, NULL, 0);   /* Simple_Pairing_Complete */
        bt_paired(s, 0x04);         /* unauthenticated P-192 */
        return true;
    case 0x040d:                    /* PIN_Code_Request_Reply: any PIN */
        if (!bt_is_peer(s, par, plen) || s->auth_step != 4) {
            return false;
        }
        bt_addr_event(s, 0x0e, (uint8_t[]){ 1, op, op >> 8, 0 }, 4, NULL, 0);
        bt_paired(s, 0x00);         /* combination key */
        return true;
    default:
        return false;
    }
}

/* incoming link: the peer starts authentication; any pairing step the
 * host leaves unanswered fails like an LMP response timeout */
static void bt_auth_timer(void *opaque)
{
    Nano7BtUartState *s = opaque;
    uint8_t st = 0x22;              /* LMP response timeout */

    if (s->link != 2) {
        return;
    }
    if (!s->auth_step) {
        /* a reconnecting gamepad opens HID control and leaves security to
         * the host: RetailOS drops a pairing the remote starts (its
         * security manager 0x080f5178 answers IO_Capability_Request only
         * while it authenticates itself, state 0x0895c0b8 = 5) */
        if (s->link_incoming && !s->link_enc) {
            bt_peer_open(s, 0x11);
        }
        return;
    }
    info_report("nano7-bt: pairing step %u unanswered by the iPod for 30 s",
                s->auth_step);
    if (s->auth_step >= 2 && s->auth_step <= 3) {
        bt_addr_event(s, 0x36, &st, 1, NULL, 0);    /* SSP Complete */
    }
    bt_auth_done(s, 0x22);
}

/* the peer pages the iPod (bt-peer=gamepad-connect) */
static void bt_connect_timer(void *opaque)
{
    Nano7BtUartState *s = opaque;
    uint8_t cr[10];

    if (s->link || !(s->scan_enable & 2)) {
        return;
    }
    memcpy(cr, bt_peer_addr, 6);
    memcpy(cr + 6, bt_peer_cod, 3);
    cr[9] = 1;                      /* ACL */
    s->link = 1;
    bt_event(s, 0x04, cr, 10);      /* Connection_Request */
    info_report("nano7-bt: peer \"%s\" pages the iPod", bt_peer_name);
}

/* ---- the peer's SDP server ------------------------------------------- */

typedef struct SdpAttr {
    uint16_t id;
    uint16_t len;
    uint8_t v[160];
} SdpAttr;

typedef struct SdpRecord {
    uint32_t handle;
    uint16_t uuids[6];              /* for ServiceSearchPattern matching */
    int nuuid, nattr;
    SdpAttr a[24];
} SdpRecord;

static SdpRecord bt_sdp[3];
static int bt_nsdp;

/* data element writer (sequences always with a 16-bit length) */
typedef struct SdpW {
    uint8_t *b;
    int n, max;
} SdpW;

static void de_raw(SdpW *w, const void *d, int n)
{
    if (w->n + n <= w->max) {
        memcpy(w->b + w->n, d, n);
    }
    w->n += n;
}

static void de_u8(SdpW *w, uint8_t v)
{
    uint8_t b[2] = { 0x08, v };
    de_raw(w, b, 2);
}

static void de_u16(SdpW *w, uint16_t v)
{
    uint8_t b[3] = { 0x09, v >> 8, v };
    de_raw(w, b, 3);
}

static void de_u32(SdpW *w, uint32_t v)
{
    uint8_t b[5] = { 0x0a, v >> 24, v >> 16, v >> 8, v };
    de_raw(w, b, 5);
}

static void de_uuid(SdpW *w, uint16_t v)
{
    uint8_t b[3] = { 0x19, v >> 8, v };
    de_raw(w, b, 3);
}

static void de_bool(SdpW *w, bool v)
{
    uint8_t b[2] = { 0x28, v };
    de_raw(w, b, 2);
}

static void de_str(SdpW *w, const void *d, uint8_t n)
{
    uint8_t b[2] = { 0x25, n };
    de_raw(w, b, 2);
    de_raw(w, d, n);
}

static int de_seq(SdpW *w)
{
    uint8_t b[3] = { 0x36, 0, 0 };
    int at = w->n;

    de_raw(w, b, 3);
    return at;
}

static void de_end(SdpW *w, int at)
{
    int len = w->n - at - 3;

    if (at + 3 <= w->max) {
        w->b[at + 1] = len >> 8;
        w->b[at + 2] = len;
    }
}

static SdpW bt_sdp_attr(SdpRecord *r, uint16_t id)
{
    SdpAttr *a = &r->a[r->nattr++];
    SdpW w = { a->v, 0, sizeof(a->v) };

    a->id = id;
    return w;
}

#define SDP_ATTR(r, id, w) for (SdpW w = bt_sdp_attr(r, id), *_once = &w; \
                                _once; (r)->a[(r)->nattr - 1].len = w.n, \
                                _once = NULL)

/* gamepad: 16 buttons + X, Y, Z, Rz signed bytes (6-byte input report) */
static const uint8_t bt_hid_report_desc[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01,             /* GD, Gamepad, App */
    0x05, 0x09, 0x19, 0x01, 0x29, 0x10,             /* Buttons 1..16 */
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x10, 0x81, 0x02,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35,
    0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02,
    0xc0,
};

static void bt_sdp_build(void)
{
    SdpRecord *r = &bt_sdp[0];
    int q, q2, q3;

    if (bt_nsdp) {
        return;
    }
    bt_nsdp = 2;
    /* HID */
    r->handle = 0x00010000;
    r->uuids[0] = 0x1124; r->uuids[1] = 0x0100; r->uuids[2] = 0x0011;
    r->uuids[3] = 0x1002; r->nuuid = 4;
    SDP_ATTR(r, 0x0000, w) { de_u32(&w, r->handle); }
    SDP_ATTR(r, 0x0001, w) { q = de_seq(&w); de_uuid(&w, 0x1124);
                             de_end(&w, q); }
    SDP_ATTR(r, 0x0004, w) { q = de_seq(&w);
                             q2 = de_seq(&w); de_uuid(&w, 0x0100);
                             de_u16(&w, 0x0011); de_end(&w, q2);
                             q2 = de_seq(&w); de_uuid(&w, 0x0011);
                             de_end(&w, q2); de_end(&w, q); }
    SDP_ATTR(r, 0x0005, w) { q = de_seq(&w); de_uuid(&w, 0x1002);
                             de_end(&w, q); }
    SDP_ATTR(r, 0x0006, w) { q = de_seq(&w); de_u16(&w, 0x656e);
                             de_u16(&w, 0x006a); de_u16(&w, 0x0100);
                             de_end(&w, q); }
    SDP_ATTR(r, 0x0009, w) { q = de_seq(&w); q2 = de_seq(&w);
                             de_uuid(&w, 0x1124); de_u16(&w, 0x0101);
                             de_end(&w, q2); de_end(&w, q); }
    SDP_ATTR(r, 0x000d, w) { q = de_seq(&w); q2 = de_seq(&w);
                             q3 = de_seq(&w); de_uuid(&w, 0x0100);
                             de_u16(&w, 0x0013); de_end(&w, q3);
                             q3 = de_seq(&w); de_uuid(&w, 0x0011);
                             de_end(&w, q3); de_end(&w, q2);
                             de_end(&w, q); }
    SDP_ATTR(r, 0x0100, w) { de_str(&w, bt_peer_name,
                                    strlen(bt_peer_name)); }
    SDP_ATTR(r, 0x0101, w) { de_str(&w, "Gamepad", 7); }
    SDP_ATTR(r, 0x0102, w) { de_str(&w, "NanoRework", 10); }
    SDP_ATTR(r, 0x0201, w) { de_u16(&w, 0x0111); }   /* HID parser */
    SDP_ATTR(r, 0x0202, w) { de_u8(&w, 0x08); }      /* subclass gamepad */
    SDP_ATTR(r, 0x0203, w) { de_u8(&w, 0x00); }      /* country */
    SDP_ATTR(r, 0x0204, w) { de_bool(&w, true); }    /* virtual cable */
    SDP_ATTR(r, 0x0205, w) { de_bool(&w, true); }    /* reconnect init */
    SDP_ATTR(r, 0x0206, w) { q = de_seq(&w); q2 = de_seq(&w);
                             de_u8(&w, 0x22);       /* report descriptor */
                             de_str(&w, bt_hid_report_desc,
                                    sizeof(bt_hid_report_desc));
                             de_end(&w, q2); de_end(&w, q); }
    SDP_ATTR(r, 0x0207, w) { q = de_seq(&w); q2 = de_seq(&w);
                             de_u16(&w, 0x0409); de_u16(&w, 0x0100);
                             de_end(&w, q2); de_end(&w, q); }
    SDP_ATTR(r, 0x020b, w) { de_u16(&w, 0x0100); }   /* profile version */
    SDP_ATTR(r, 0x020c, w) { de_u16(&w, 0x0c80); }   /* supervision tmo */
    SDP_ATTR(r, 0x020d, w) { de_bool(&w, false); }   /* normally conn. */
    SDP_ATTR(r, 0x020e, w) { de_bool(&w, false); }   /* boot device */
    /* Device ID (pid.codes test VID 0x1209) */
    r = &bt_sdp[1];
    r->handle = 0x00010001;
    r->uuids[0] = 0x1200; r->uuids[1] = 0x0100; r->uuids[2] = 0x0001;
    r->uuids[3] = 0x1002; r->nuuid = 4;
    SDP_ATTR(r, 0x0000, w) { de_u32(&w, r->handle); }
    SDP_ATTR(r, 0x0001, w) { q = de_seq(&w); de_uuid(&w, 0x1200);
                             de_end(&w, q); }
    SDP_ATTR(r, 0x0004, w) { q = de_seq(&w);
                             q2 = de_seq(&w); de_uuid(&w, 0x0100);
                             de_u16(&w, 0x0001); de_end(&w, q2);
                             q2 = de_seq(&w); de_uuid(&w, 0x0001);
                             de_end(&w, q2); de_end(&w, q); }
    SDP_ATTR(r, 0x0005, w) { q = de_seq(&w); de_uuid(&w, 0x1002);
                             de_end(&w, q); }
    SDP_ATTR(r, 0x0009, w) { q = de_seq(&w); q2 = de_seq(&w);
                             de_uuid(&w, 0x1200); de_u16(&w, 0x0103);
                             de_end(&w, q2); de_end(&w, q); }
    SDP_ATTR(r, 0x0200, w) { de_u16(&w, 0x0103); }   /* spec */
    SDP_ATTR(r, 0x0201, w) { de_u16(&w, 0x1209); }   /* vendor */
    SDP_ATTR(r, 0x0202, w) { de_u16(&w, 0x4e50); }   /* product */
    SDP_ATTR(r, 0x0203, w) { de_u16(&w, 0x0100); }   /* version */
    SDP_ATTR(r, 0x0204, w) { de_bool(&w, true); }    /* primary */
    SDP_ATTR(r, 0x0205, w) { de_u16(&w, 0x0002); }   /* VID source USB */
    if (!getenv("NANO7_BT_PEER_A2DP")) {
        return;
    }
    /* NANO7_BT_PEER_A2DP=1: also an A2DP Audio Sink (AVDTP 1.3), the only
     * service the stock OS connects to - exercises its pairing flow */
    bt_nsdp = 3;
    r = &bt_sdp[2];
    r->handle = 0x00010002;
    r->uuids[0] = 0x110b; r->uuids[1] = 0x0100; r->uuids[2] = 0x0019;
    r->uuids[3] = 0x1002; r->uuids[4] = 0x110d; r->nuuid = 5;
    SDP_ATTR(r, 0x0000, w) { de_u32(&w, r->handle); }
    SDP_ATTR(r, 0x0001, w) { q = de_seq(&w); de_uuid(&w, 0x110b);
                             de_end(&w, q); }
    SDP_ATTR(r, 0x0004, w) { q = de_seq(&w);
                             q2 = de_seq(&w); de_uuid(&w, 0x0100);
                             de_u16(&w, 0x0019); de_end(&w, q2);
                             q2 = de_seq(&w); de_uuid(&w, 0x0019);
                             de_u16(&w, 0x0103); de_end(&w, q2);
                             de_end(&w, q); }
    SDP_ATTR(r, 0x0005, w) { q = de_seq(&w); de_uuid(&w, 0x1002);
                             de_end(&w, q); }
    SDP_ATTR(r, 0x0009, w) { q = de_seq(&w); q2 = de_seq(&w);
                             de_uuid(&w, 0x110d); de_u16(&w, 0x0103);
                             de_end(&w, q2); de_end(&w, q); }
    SDP_ATTR(r, 0x0100, w) { de_str(&w, "Audio Sink", 10); }
    SDP_ATTR(r, 0x0311, w) { de_u16(&w, 0x0001); }   /* headphone */
}

/* data element header: type, payload length, header length (0 = bad) */
static int de_hdr(const uint8_t *p, const uint8_t *end, uint8_t *type,
                  uint32_t *len)
{
    static const uint8_t fixed[5] = { 1, 2, 4, 8, 16 };
    uint8_t sz;

    if (p >= end) {
        return 0;
    }
    *type = p[0] >> 3;
    sz = p[0] & 7;
    if (*type == 0) {
        *len = 0;
        return 1;
    }
    if (sz < 5) {
        *len = fixed[sz];
        return 1;
    }
    if (sz == 5 && end - p >= 2) {
        *len = p[1];
        return 2;
    }
    if (sz == 6 && end - p >= 3) {
        *len = p[1] << 8 | p[2];
        return 3;
    }
    if (sz == 7 && end - p >= 5) {
        *len = (uint32_t)p[1] << 24 | p[2] << 16 | p[3] << 8 | p[4];
        return 5;
    }
    return 0;
}

/* ServiceSearchPattern: a sequence of UUIDs (16/32/128 -> 16-bit) */
static int sdp_parse_uuids(const uint8_t **pp, const uint8_t *end,
                           uint16_t *out, int max)
{
    const uint8_t *p = *pp, *e;
    uint8_t t;
    uint32_t l;
    int h = de_hdr(p, end, &t, &l), n = 0;

    if (!h || t != 6 || p + h + l > end) {
        return -1;
    }
    p += h;
    e = p + l;
    while (p < e) {
        h = de_hdr(p, e, &t, &l);
        if (!h || t != 3 || p + h + l > e) {
            return -1;
        }
        if (n < max) {
            out[n++] = l == 2 ? p[h] << 8 | p[h + 1] : p[h + 2] << 8 | p[h + 3];
        }
        p += h + l;
    }
    *pp = e;
    return n;
}

/* AttributeIDList: uint16 ids or uint32 ranges; stored as ranges */
static int sdp_parse_attrs(const uint8_t **pp, const uint8_t *end,
                           uint32_t *rng, int max)
{
    const uint8_t *p = *pp, *e;
    uint8_t t;
    uint32_t l;
    int h = de_hdr(p, end, &t, &l), n = 0;

    if (!h || t != 6 || p + h + l > end) {
        return -1;
    }
    p += h;
    e = p + l;
    while (p < e) {
        h = de_hdr(p, e, &t, &l);
        if (!h || t != 1 || p + h + l > e) {
            return -1;
        }
        if (n < max) {
            rng[n++] = l == 2 ? (uint32_t)(p[h] << 8 | p[h + 1]) * 0x10001u
                              : (uint32_t)p[h] << 24 | p[h + 1] << 16 |
                                p[h + 2] << 8 | p[h + 3];
        }
        p += h + l;
    }
    *pp = e;
    return n;
}

static bool sdp_match(const SdpRecord *r, const uint16_t *u, int n)
{
    int i, j;

    for (i = 0; i < n; i++) {
        for (j = 0; j < r->nuuid && r->uuids[j] != u[i]; j++) {
        }
        if (j == r->nuuid) {
            return false;
        }
    }
    return n > 0;
}

/* one record's attribute list (sequence of id / value pairs) */
static void sdp_put_record(SdpW *w, const SdpRecord *r, const uint32_t *rng,
                           int nr)
{
    int q = de_seq(w), i, k;

    for (i = 0; i < r->nattr; i++) {
        for (k = 0; k < nr; k++) {
            if (r->a[i].id >= rng[k] >> 16 && r->a[i].id <= (rng[k] & 0xffff)) {
                de_u16(w, r->a[i].id);
                de_raw(w, r->a[i].v, r->a[i].len);
                break;
            }
        }
    }
    de_end(w, q);
}

static void bt_sdp_pdu(Nano7BtUartState *s, uint16_t host_cid,
                       const uint8_t *p, uint16_t n)
{
    uint8_t full[1600], out[700];
    SdpW w = { full, 0, sizeof(full) };
    const uint8_t *q, *end = p + n;
    uint16_t uu[12], maxb, tid, off = 0, chunk;
    uint32_t rng[16];
    uint8_t pdu, err = 0, resp;
    int nu = 0, nr, i, o;

    bt_sdp_build();
    if (n < 5) {
        return;
    }
    pdu = p[0];
    tid = p[1] << 8 | p[2];
    q = p + 5;
    switch (pdu) {
    case 0x02:                      /* ServiceSearchRequest */
        nu = sdp_parse_uuids(&q, end, uu, 12);
        if (nu < 0 || end - q < 3) {
            err = 3;
            break;
        }
        resp = 0x03;
        {
            uint8_t h[64];
            int cnt = 0;

            for (i = 0; i < bt_nsdp; i++) {
                if (sdp_match(&bt_sdp[i], uu, nu)) {
                    stl_be_p(h + 4 * cnt++, bt_sdp[i].handle);
                }
            }
            o = 0;
            out[o++] = resp;
            out[o++] = tid >> 8;
            out[o++] = tid;
            out[o++] = 0;
            out[o++] = 5 + 4 * cnt;
            out[o++] = 0;
            out[o++] = cnt;
            out[o++] = 0;
            out[o++] = cnt;
            memcpy(out + o, h, 4 * cnt);
            o += 4 * cnt;
            out[o++] = 0;           /* no continuation */
            bt_l2cap_send(s, host_cid, out, o);
        }
        info_report("nano7-bt: SDP ServiceSearch (%d UUIDs, first 0x%04x)",
                    nu, nu > 0 ? uu[0] : 0);
        return;
    case 0x04: {                    /* ServiceAttributeRequest */
        uint32_t hd;
        const SdpRecord *r = NULL;

        if (end - q < 6) {
            err = 3;
            break;
        }
        hd = ldl_be_p(q);
        maxb = q[4] << 8 | q[5];
        q += 6;
        nr = sdp_parse_attrs(&q, end, rng, 16);
        for (i = 0; i < bt_nsdp; i++) {
            if (bt_sdp[i].handle == hd) {
                r = &bt_sdp[i];
            }
        }
        if (nr < 0 || q >= end) {
            err = 3;
            break;
        }
        if (!r) {
            err = 2;                /* invalid record handle */
            break;
        }
        sdp_put_record(&w, r, rng, nr);
        resp = 0x05;
        info_report("nano7-bt: SDP ServiceAttribute 0x%08x", hd);
        goto reply;
    }
    case 0x06:                      /* ServiceSearchAttributeRequest */
        nu = sdp_parse_uuids(&q, end, uu, 12);
        if (nu < 0 || end - q < 2) {
            err = 3;
            break;
        }
        maxb = q[0] << 8 | q[1];
        q += 2;
        nr = sdp_parse_attrs(&q, end, rng, 16);
        if (nr < 0 || q >= end) {
            err = 3;
            break;
        }
        {
            int sq = de_seq(&w);

            for (i = 0; i < bt_nsdp; i++) {
                if (sdp_match(&bt_sdp[i], uu, nu)) {
                    sdp_put_record(&w, &bt_sdp[i], rng, nr);
                }
            }
            de_end(&w, sq);
        }
        resp = 0x07;
        info_report("nano7-bt: SDP ServiceSearchAttribute (%d UUIDs, first "
                    "0x%04x): %d bytes", nu, nu > 0 ? uu[0] : 0, w.n);
        goto reply;
    default:
        err = 1;                    /* unsupported version / PDU */
        break;
    }
    /* ErrorResponse */
    out[0] = 0x01;
    out[1] = tid >> 8;
    out[2] = tid;
    out[3] = 0;
    out[4] = 2;
    out[5] = 0;
    out[6] = err;
    bt_l2cap_send(s, host_cid, out, 7);
    return;

reply:
    /* continuation state: 2-byte offset into the full attribute list */
    if (q < end && q[0] == 2 && end - q >= 3) {
        off = q[1] << 8 | q[2];
    }
    if (off > w.n || w.n > (int)sizeof(full)) {
        off = 0;
    }
    chunk = MIN((int)MIN(maxb, 600), w.n - off);
    o = 0;
    out[o++] = resp;
    out[o++] = tid >> 8;
    out[o++] = tid;
    o += 2;                         /* parameter length, below */
    out[o++] = chunk >> 8;
    out[o++] = chunk;
    memcpy(out + o, full + off, chunk);
    o += chunk;
    if (off + chunk < w.n) {
        out[o++] = 2;
        out[o++] = (off + chunk) >> 8;
        out[o++] = off + chunk;
    } else {
        out[o++] = 0;
    }
    out[3] = (o - 5) >> 8;
    out[4] = o - 5;
    bt_l2cap_send(s, host_cid, out, o);
}

/* HIDP from the host on control (0x11) / interrupt (0x13) */
static void bt_hidp(Nano7BtUartState *s, uint16_t psm, uint16_t hcid,
                    const uint8_t *m, uint16_t n)
{
    uint8_t type = m[0] >> 4, prm = m[0] & 0x0f;
    uint8_t hs = 0x00;              /* HANDSHAKE successful */

    if (psm == 0x13) {
        if (m[0] == 0xa2) {         /* DATA | Output: e.g. rumble / LEDs */
            info_report("nano7-bt: HID output report from the iPod, %u "
                        "bytes", n - 1);
        }
        return;
    }
    switch (type) {
    case 0x1:                       /* HID_CONTROL */
        if (prm == 0x5) {           /* VIRTUAL_CABLE_UNPLUG */
            info_report("nano7-bt: HID virtual cable unplug");
        }
        return;                     /* no handshake for HID_CONTROL */
    case 0x4: {                     /* GET_REPORT */
        uint8_t r[7] = { 0xa1, s->pad_buttons, s->pad_buttons >> 8,
                         s->pad_axis[0], s->pad_axis[1], s->pad_axis[2],
                         s->pad_axis[3] };

        if ((prm & 3) != 1) {       /* only an input report exists */
            hs = 0x04;              /* ERR_INVALID_PARAMETER */
            break;
        }
        bt_l2cap_send(s, hcid, r, sizeof(r));
        return;
    }
    case 0x6: {                     /* GET_PROTOCOL: report protocol */
        uint8_t r[2] = { 0xa0, 0x01 };

        bt_l2cap_send(s, hcid, r, 2);
        return;
    }
    case 0x5:                       /* SET_REPORT */
    case 0x7:                       /* SET_PROTOCOL */
    case 0x9:                       /* SET_IDLE */
        break;
    case 0x8: {                     /* GET_IDLE */
        uint8_t r[2] = { 0xa0, 0x00 };

        bt_l2cap_send(s, hcid, r, 2);
        return;
    }
    default:
        hs = 0x03;                  /* ERR_UNSUPPORTED_REQUEST */
        break;
    }
    bt_l2cap_send(s, hcid, &hs, 1);
}

/* host -> peer ACL data: L2CAP signalling, SDP, HIDP */
static void bt_peer_acl(Nano7BtUartState *s, const uint8_t *p, uint32_t n)
{
    uint16_t len, cid, i;

    if (n < 9 || s->link != 2 ||
        ((p[1] | p[2] << 8) & 0xfff) != BT_PEER_HANDLE ||
        (p[2] & 0x30) == 0x10) {    /* continuation fragments: ignored */
        return;
    }
    len = MIN(p[5] | p[6] << 8, n - 9);
    cid = p[7] | p[8] << 8;
    s->l2cap_rx++;
    if (cid != 0x0001) {
        for (i = 0; i < 4; i++) {
            if (!s->ch_state[i] || cid != 0x40 + i) {
                continue;
            }
            if (s->ch_psm[i] == 0x01) {
                bt_sdp_pdu(s, s->ch_hcid[i], p + 9, len);
            } else if ((s->ch_psm[i] == 0x11 || s->ch_psm[i] == 0x13) &&
                       len >= 1) {
                bt_hidp(s, s->ch_psm[i], s->ch_hcid[i], p + 9, len);
            } else if (s->l2cap_rx < 64) {
                info_report("nano7-bt: %u bytes on L2CAP PSM 0x%02x",
                            len, s->ch_psm[i]);
            }
        }
        return;
    }
    p += 9;
    while (len >= 4) {
        uint8_t code = p[0], id = p[1];
        uint16_t cl = MIN(p[2] | p[3] << 8, len - 4);
        const uint8_t *d = p + 4;
        uint8_t o[16] = { 0 };

        switch (code) {
        case 0x02:                  /* Connection Request (PSM, SCID) */
            if (cl >= 4) {
                uint16_t psm = d[0] | d[1] << 8;
                int slot = -1;

                for (i = 0; i < 4 && (psm == 0x01 || psm == 0x11 ||
                                      psm == 0x13 ||
                                      (psm == 0x19 && bt_nsdp == 3)); i++) {
                    if (!s->ch_state[i]) {
                        slot = i;
                        break;
                    }
                }
                o[2] = d[2];        /* SCID */
                o[3] = d[3];
                if (slot >= 0) {
                    s->ch_psm[slot] = psm;
                    s->ch_hcid[slot] = d[2] | d[3] << 8;
                    s->ch_state[slot] = 1;
                    o[0] = 0x40 + slot;
                } else {
                    o[4] = 0x02;    /* refused: PSM not supported */
                }
                bt_sig_send(s, 0x03, id, o, 8);
                info_report("nano7-bt: iPod opens L2CAP PSM 0x%04x: %s", psm,
                            slot >= 0 ? "accepted" : "refused");
                if (slot >= 0) {
                    bt_sig_config_req(s, s->ch_hcid[slot]);
                }
            }
            break;
        case 0x03:                  /* Connection Response to our request */
            if (cl >= 8) {
                uint16_t res = d[4] | d[5] << 8;
                uint16_t sc = d[2] | d[3] << 8;     /* our CID */
                int k = sc - 0x40;

                if (k < 0 || k >= 4 || !s->ch_mine[k] || s->ch_state[k] != 1) {
                    break;
                }
                info_report("nano7-bt: iPod answers the peer's L2CAP PSM "
                            "0x%04x: result 0x%04x%s", s->ch_psm[k], res,
                            res == 0 ? " (accepted)" :
                            res == 1 ? " (pending)" :
                            res == 2 ? " (PSM not supported)" :
                            res == 3 ? " (security block)" : "");
                if (res == 0) {
                    s->ch_hcid[k] = d[0] | d[1] << 8;
                    bt_sig_config_req(s, s->ch_hcid[k]);
                } else if (res != 1) {
                    s->ch_state[k] = 0;
                    s->ch_mine[k] = 0;
                }
            }
            break;
        case 0x04:                  /* Configuration Request (DCID = ours) */
            if (cl >= 4) {
                uint16_t dcid = d[0] | d[1] << 8;

                if (dcid >= 0x40 && dcid < 0x44 && s->ch_state[dcid - 0x40]) {
                    uint16_t h = s->ch_hcid[dcid - 0x40];

                    o[0] = h;
                    o[1] = h >> 8;
                    bt_sig_send(s, 0x05, id, o, 6);     /* success */
                } else {
                    o[0] = 0x02;    /* invalid CID */
                    o[2] = d[0];
                    o[3] = d[1];
                    bt_sig_send(s, 0x01, id, o, 6);
                }
            }
            break;
        case 0x05:                  /* Configuration Response */
            for (i = 0; i < 4 && cl >= 6; i++) {
                if (s->ch_state[i] == 1 && (d[0] | d[1] << 8) == 0x40 + i &&
                    !(d[4] | d[5] << 8)) {
                    s->ch_state[i] = 2;
                    info_report("nano7-bt: L2CAP PSM 0x%02x open",
                                s->ch_psm[i]);
                    if (s->ch_psm[i] == 0x11 && s->ch_mine[i]) {
                        bt_peer_open(s, 0x13);  /* then HID interrupt */
                    } else if (s->ch_psm[i] == 0x13) {
                        bt_hid_up(s);
                    }
                }
            }
            break;
        case 0x06:                  /* Disconnection Request */
            if (cl >= 4) {
                uint16_t dcid = d[0] | d[1] << 8;

                memcpy(o, d, 4);
                bt_sig_send(s, 0x07, id, o, 4);
                if (dcid >= 0x40 && dcid < 0x44) {
                    if (s->ch_psm[dcid - 0x40] == 0x13 && s->hid_up) {
                        s->hid_up = false;
                        timer_del(s->pad_timer);
                        info_report("nano7-bt: HID interrupt channel closed "
                                    "by the iPod");
                    }
                    s->ch_state[dcid - 0x40] = 0;
                    s->ch_mine[dcid - 0x40] = 0;
                }
            }
            break;
        case 0x08:                  /* Echo Request */
            bt_sig_send(s, 0x09, id, d, MIN(cl, 32));
            break;
        case 0x0a:                  /* Information Request */
            if (cl >= 2) {
                o[0] = d[0];
                o[1] = d[1];
                if (d[0] == 2) {    /* extended features: none */
                    bt_sig_send(s, 0x0b, id, o, 8);
                } else if (d[0] == 3) {
                    o[4] = 0x02;    /* fixed channels: signalling */
                    bt_sig_send(s, 0x0b, id, o, 12);
                } else {
                    o[2] = 1;       /* not supported */
                    bt_sig_send(s, 0x0b, id, o, 4);
                }
            }
            break;
        case 0x01: case 0x07: case 0x09: case 0x0b:
            break;                  /* responses to us */
        default:
            bt_sig_send(s, 0x01, id, o, 2);     /* not understood */
            break;
        }
        p += 4 + cl;
        len -= 4 + cl;
    }
}

static void bt_command(Nano7BtUartState *s, uint16_t op, const uint8_t *par,
                       uint8_t plen)
{
    uint8_t r[252] = { 0 };         /* r[0] = status */
    uint8_t n = 1;

    s->cmds++;
    if (bt_peer_command(s, op, par, plen)) {
        return;
    }
    switch (op) {
    /* link control: Command Status, then the completion event */
    case 0x0401:                    /* Inquiry: nobody around; it ends
                                     * after Inquiry_Length x 1.28 s */
        bt_cmd_status(s, op, 0);
        nano7_timer_mod(s->inquiry_timer, nano7_now_ns() +
                        (int64_t)MAX(plen >= 4 ? par[3] : 1, 1) * 1280000000LL);
        if (s->peer) {
            nano7_timer_mod(s->peer_timer, nano7_now_ns() + BT_PEER_DELAY);
        }
        return;
    case 0x0405: {                  /* Create_Connection: page timeout */
        uint8_t cc[11] = { 0x04 };

        bt_cmd_status(s, op, 0);
        if (plen >= 6) {
            memcpy(cc + 3, par, 6);
        }
        cc[9] = 1;                  /* ACL */
        bt_event(s, 0x03, cc, 11);
        return;
    }
    case 0x0419: {                  /* Remote_Name_Request: the peer
                                     * answers, anyone else page timeout */
        uint8_t rn[255] = { 0x04 };

        bt_cmd_status(s, op, 0);
        if (plen >= 6) {
            memcpy(rn + 1, par, 6);
            if (s->peer && !memcmp(par, bt_peer_addr, 6)) {
                rn[0] = 0;
                memcpy(rn + 7, bt_peer_name, strlen(bt_peer_name));
            }
        }
        bt_event(s, 0x07, rn, 255);
        return;
    }
    case 0x0406:                    /* Disconnect: no such connection */
    case 0x0411:                    /* Authentication_Requested */
    case 0x0413:                    /* Set_Connection_Encryption */
    case 0x041b:                    /* Read_Remote_Supported_Features */
    case 0x041c:                    /* Read_Remote_Extended_Features */
    case 0x041d:                    /* Read_Remote_Version_Information */
        bt_cmd_status(s, op, 0x02);
        return;
    case 0x0402:                    /* Inquiry_Cancel */
        timer_del(s->inquiry_timer);
        timer_del(s->peer_timer);
        break;

    /* controller & baseband */
    case 0x0c03:                    /* Reset */
        s->scan_enable = 0;
        s->inquiry_mode = 0;
        timer_del(s->inquiry_timer);
        timer_del(s->peer_timer);
        timer_del(s->connect_timer);
        bt_link_reset(s);
        s->baud = 115200;
        break;
    case 0x0c13:                    /* Write_Local_Name */
        memset(s->local_name, 0, sizeof(s->local_name));
        memcpy(s->local_name, par, MIN(plen, 248));
        break;
    case 0x0c14:                    /* Read_Local_Name */
        memcpy(r + 1, s->local_name, 248);
        n = 249;
        break;
    case 0x0c1a:                    /* Write_Scan_Enable */
        if (plen >= 1) {
            s->scan_enable = par[0];
            if (s->peer_connect && !s->connect_done && (par[0] & 2)) {
                s->connect_done = true;     /* once per run */
                nano7_timer_mod(s->connect_timer,
                                nano7_now_ns() + BT_CONNECT_NS);
            }
            info_report("nano7-bt: scan enable = %u (%s%s)", par[0],
                        par[0] & 1 ? "inquiry " : "",
                        par[0] & 2 ? "page" : "");
        }
        break;
    case 0x0c19:                    /* Read_Scan_Enable */
        r[1] = s->scan_enable;
        n = 2;
        break;
    case 0x0c23:                    /* Read_Class_Of_Device */
        memcpy(r + 1, s->cod, 3);
        n = 4;
        break;
    case 0x0c24:                    /* Write_Class_Of_Device */
        if (plen >= 3) {
            memcpy(s->cod, par, 3);
        }
        break;
    case 0x0c56:                    /* Write_Simple_Pairing_Mode */
        s->ssp_mode = plen ? par[0] : 0;
        break;
    case 0x0c55:                    /* Read_Simple_Pairing_Mode */
        r[1] = s->ssp_mode;
        n = 2;
        break;
    case 0x0c6d:                    /* Write_LE_Host_Supported */
        s->le_host = plen ? par[0] : 0;
        break;
    case 0x0c6c:                    /* Read_LE_Host_Supported */
        r[1] = s->le_host;
        n = 3;
        break;
    case 0x0c58:                    /* Read_Inquiry_Response_Tx_Power */
        r[1] = 4;
        n = 2;
        break;
    case 0x0c15:                    /* Read_Connection_Accept_Timeout */
    case 0x0c17:                    /* Read_Page_Timeout */
        r[1] = 0x00;
        r[2] = 0x20;
        n = 3;
        break;
    case 0x0c1b:                    /* Read_Page_Scan_Activity */
    case 0x0c1d:                    /* Read_Inquiry_Scan_Activity */
        r[1] = 0x00; r[2] = 0x08; r[3] = 0x12; r[4] = 0x00;
        n = 5;
        break;
    case 0x0c45:                    /* Write_Inquiry_Mode */
        s->inquiry_mode = plen ? par[0] : 0;
        break;
    case 0x0c44:                    /* Read_Inquiry_Mode */
        r[1] = s->inquiry_mode;
        n = 2;
        break;
    case 0x0c46:                    /* Read_Page_Scan_Type */
    case 0x0c42:                    /* Read_Inquiry_Scan_Type */
        n = 2;
        break;

    /* informational */
    case 0x1001:                    /* Read_Local_Version_Information */
        r[1] = 6;                   /* HCI 4.0 */
        r[2] = 0x00; r[3] = 0x10;   /* HCI revision */
        r[4] = 6;                   /* LMP 4.0 */
        r[5] = 0x0f; r[6] = 0x00;   /* Broadcom */
        r[7] = 0x0b; r[8] = 0x41;   /* LMP subversion (BCM2076B1-like) */
        n = 9;
        break;
    case 0x1002:                    /* Read_Local_Supported_Commands */
        memset(r + 1, 0xff, 64);
        n = 65;
        break;
    case 0x1003: {                  /* Read_Local_Supported_Features */
        static const uint8_t f[8] = { 0xbf, 0xfe, 0xcf, 0xfe,
                                      0xdb, 0xff, 0x7b, 0x87 };
        memcpy(r + 1, f, 8);
        n = 9;
        break;
    }
    case 0x1004:                    /* Read_Local_Extended_Features */
        r[1] = plen ? par[0] : 0;
        r[2] = 1;                   /* max page */
        if (r[1] == 0) {
            static const uint8_t f[8] = { 0xbf, 0xfe, 0xcf, 0xfe,
                                          0xdb, 0xff, 0x7b, 0x87 };
            memcpy(r + 3, f, 8);
        } else if (r[1] == 1) {
            r[3] = 0x01;            /* SSP host support mirrors */
        }
        n = 11;
        break;
    case 0x1005:                    /* Read_Buffer_Size */
        r[1] = 0xfd; r[2] = 0x03;   /* ACL 1021 */
        r[3] = 64;                  /* SCO 64 */
        r[4] = 8; r[5] = 0;         /* 8 ACL packets */
        r[6] = 0; r[7] = 0;         /* 0 SCO */
        n = 8;
        break;
    case 0x1009:                    /* Read_BD_ADDR */
        memcpy(r + 1, s->bdaddr, 6);
        n = 7;
        break;
    case 0x1405:                    /* Read_RSSI */
        n = 4;
        break;

    /* LE */
    case 0x2002:                    /* LE_Read_Buffer_Size */
        r[1] = 27; r[2] = 0; r[3] = 8;
        n = 4;
        break;
    case 0x2003:                    /* LE_Read_Local_Supported_Features */
        r[1] = 0x01;
        n = 9;
        break;
    case 0x2007:                    /* LE_Read_Advertising_Channel_Tx_Power */
        r[1] = 4;
        n = 2;
        break;
    case 0x200f:                    /* LE_Read_White_List_Size */
        r[1] = 8;
        n = 2;
        break;
    case 0x201c:                    /* LE_Read_Supported_States */
        memset(r + 1, 0xff, 5);
        n = 9;
        break;

    /* Broadcom vendor */
    case 0xfc4c:                    /* Write_RAM (patchram record) */
        s->patch_records++;
        s->patch_bytes += plen > 4 ? plen - 4 : 0;
        break;
    case 0xfc4e:                    /* Launch_RAM */
        info_report("nano7-bt: patchram: %u Write_RAM records, %u bytes, "
                    "launched", s->patch_records, s->patch_bytes);
        s->patch_records = s->patch_bytes = 0;
        break;
    case 0xfc18:                    /* Update_UART_Baud_Rate */
        if (plen >= 6) {
            s->baud = par[2] | par[3] << 8 | par[4] << 16 |
                      (uint32_t)par[5] << 24;
            info_report("nano7-bt: controller UART -> %u baud", s->baud);
        }
        break;
    case 0xfc01:                    /* Write_BD_ADDR */
        if (plen >= 6) {
            memcpy(s->bdaddr, par, 6);
            info_report("nano7-bt: BD_ADDR %02x:%02x:%02x:%02x:%02x:%02x",
                        par[5], par[4], par[3], par[2], par[1], par[0]);
        }
        break;
    case 0xfc79:                    /* Read_Verbose_Config_Version_Info */
        r[1] = 0x0b; r[2] = 0x02; r[3] = 0x04; r[4] = 0x56; r[5] = 0x00;
        n = 7;
        break;
    default:
        break;                      /* status 0, no return parameters */
    }
    bt_cmd_complete(s, op, r, n);
}

static void bt_fake_packet(Nano7BtUartState *s, const uint8_t *p, uint32_t n)
{
    bt_trace(s, false, p, n);
    switch (p[0]) {
    case 1:
        bt_command(s, p[1] | p[2] << 8, p + 4, p[3]);
        break;
    case 2: {                       /* ACL: return the credit, then the
                                     * peer answers its L2CAP part */
        uint8_t nc[5] = { 1, p[1], p[2] & 0x0f, 1, 0 };

        s->acl_tx++;
        bt_event(s, 0x13, nc, 5);
        bt_peer_acl(s, p, n);
        break;
    }
    default:
        break;
    }
}

/* host -> controller byte stream: frame H4 packets */
static void bt_fake_byte(Nano7BtUartState *s, uint8_t b)
{
    if (s->pkt_len == 0) {
        switch (b) {
        case 1: s->pkt_need = 4; break;     /* cmd: op16 len8 */
        case 2: s->pkt_need = 5; break;     /* acl: hdl16 len16 */
        case 3: s->pkt_need = 4; break;     /* sco: hdl16 len8 */
        default:
            if (s->log_lines < BT_LOG_MAX) {
                s->log_lines++;
                fprintf(stderr, "[nano7-bt] stray byte 0x%02x from host\n",
                        b);
            }
            return;
        }
    }
    s->pkt[s->pkt_len++] = b;
    if (s->pkt_len == s->pkt_need) {
        if (s->pkt_need == 4 && s->pkt[0] != 2 && s->pkt[3]) {
            s->pkt_need += s->pkt[3];
            return;
        }
        if (s->pkt_need == 5 && s->pkt[0] == 2 && (s->pkt[3] | s->pkt[4])) {
            s->pkt_need += s->pkt[3] | s->pkt[4] << 8;
            return;
        }
        bt_fake_packet(s, s->pkt, s->pkt_len);
        s->pkt_len = 0;
    }
}

static void bt_tx_byte(Nano7BtUartState *s, uint8_t b)
{
    s->tx_bytes++;
    if (s->use_chr) {
        /* blocking write: the host bridge is expected to keep up */
        qemu_chr_fe_write_all(&s->chr, &b, 1);
    } else {
        bt_fake_byte(s, b);
    }
}

/* ---- socket backend ------------------------------------------------- */

static int bt_chr_can_receive(void *opaque)
{
    Nano7BtUartState *s = opaque;

    return BT_RXQ - bt_rx_count(s);
}

static void bt_chr_receive(void *opaque, const uint8_t *buf, int size)
{
    bt_rx_push(opaque, buf, size);
}

static void bt_chr_event(void *opaque, QEMUChrEvent ev)
{
    if (ev == CHR_EVENT_OPENED) {
        info_report("nano7-bt: host bridge connected");
    } else if (ev == CHR_EVENT_CLOSED) {
        info_report("nano7-bt: host bridge disconnected");
    }
}

/* ---- MMIO ----------------------------------------------------------- */

static uint64_t bt_read(void *opaque, hwaddr off, unsigned size)
{
    Nano7BtUartState *s = opaque;
    uint32_t v;

    switch (off) {
    case 0x10:
        return s->pending | (bt_rx_count(s) ? ST_RXDATA : 0);
    case 0x14:
        return 0;
    case 0x18:                      /* FIFO status: TX never full; RX count */
        return MIN(bt_rx_count(s), 0x7fu);
    case 0x24:
        if (!bt_rx_count(s)) {
            return 0;
        }
        v = s->rxq[s->rx_tail++ & (BT_RXQ - 1)];
        if (!s->rd_seen) {
            s->rd_seen = true;
            info_report("nano7-bt: host reads the RX FIFO (pc 0x%08x)",
                        current_cpu ?
                        ARM_CPU(current_cpu)->env.regs[15] : 0);
        }
        return v;
    default:
        return off < sizeof(s->regs) ? s->regs[off >> 2] : 0;
    }
}

static void bt_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    Nano7BtUartState *s = opaque;
    uint32_t v = value;

    switch (off) {
    case 0x10:
        /* W1C. Bytes still in the FIFO do not re-raise the RX bit: the
         * decoder 0x08001868 loops on +0x10 before the handler drains */
        s->pending &= ~(v & ST_PENDING);
        bt_update(s);
        break;
    case 0x08:
        s->regs[off >> 2] = v;
        if (v & 2) {                /* RX FIFO reset */
            s->rx_tail = s->rx_head;
        }
        break;
    case 0x20:
        bt_tx_byte(s, v);
        break;
    default:
        if (off < sizeof(s->regs)) {
            s->regs[off >> 2] = v;
        }
        break;
    }
}

static const MemoryRegionOps bt_ops = {
    .read = bt_read,
    .write = bt_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static int bt_post_load(void *opaque, int version_id)
{
    bt_update(opaque);
    return 0;
}

static const VMStateDescription vmstate_bt = {
    .name = "nano7-btuart",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = bt_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Nano7BtUartState, 0x40 / 4),
        VMSTATE_UINT32(pending, Nano7BtUartState),
        VMSTATE_UINT8_ARRAY(rxq, Nano7BtUartState, BT_RXQ),
        VMSTATE_UINT32(rx_head, Nano7BtUartState),
        VMSTATE_UINT32(rx_tail, Nano7BtUartState),
        VMSTATE_UINT8_ARRAY(outq, Nano7BtUartState, BT_RXQ),
        VMSTATE_UINT32(out_head, Nano7BtUartState),
        VMSTATE_UINT32(out_tail, Nano7BtUartState),
        VMSTATE_TIMER_PTR(reply_timer, Nano7BtUartState),
        VMSTATE_TIMER_PTR(inquiry_timer, Nano7BtUartState),
        VMSTATE_TIMER_PTR(peer_timer, Nano7BtUartState),
        VMSTATE_UINT8(inquiry_mode, Nano7BtUartState),
        VMSTATE_TIMER_PTR(connect_timer, Nano7BtUartState),
        VMSTATE_TIMER_PTR(auth_timer, Nano7BtUartState),
        VMSTATE_BOOL(connect_done, Nano7BtUartState),
        VMSTATE_UINT8(link, Nano7BtUartState),
        VMSTATE_UINT8(link_incoming, Nano7BtUartState),
        VMSTATE_UINT8(auth_host, Nano7BtUartState),
        VMSTATE_UINT8(auth_step, Nano7BtUartState),
        VMSTATE_UINT8(link_enc, Nano7BtUartState),
        VMSTATE_UINT8(sig_id, Nano7BtUartState),
        VMSTATE_UINT16_ARRAY(ch_psm, Nano7BtUartState, 4),
        VMSTATE_UINT16_ARRAY(ch_hcid, Nano7BtUartState, 4),
        VMSTATE_UINT8_ARRAY(ch_state, Nano7BtUartState, 4),
        VMSTATE_UINT8_ARRAY(ch_mine, Nano7BtUartState, 4),
        VMSTATE_BOOL(hid_up, Nano7BtUartState),
        VMSTATE_UINT16(pad_buttons, Nano7BtUartState),
        VMSTATE_UINT8_ARRAY(pad_axis, Nano7BtUartState, 4),
        VMSTATE_TIMER_PTR(pad_timer, Nano7BtUartState),
        VMSTATE_UINT32(pad_next, Nano7BtUartState),
        VMSTATE_INT64(pad_t0, Nano7BtUartState),
        VMSTATE_UINT8_ARRAY(pkt, Nano7BtUartState, 4 + 65536),
        VMSTATE_UINT32(pkt_len, Nano7BtUartState),
        VMSTATE_UINT32(pkt_need, Nano7BtUartState),
        VMSTATE_UINT8(scan_enable, Nano7BtUartState),
        VMSTATE_UINT8_ARRAY(local_name, Nano7BtUartState, 248),
        VMSTATE_UINT8_ARRAY(cod, Nano7BtUartState, 3),
        VMSTATE_UINT8(ssp_mode, Nano7BtUartState),
        VMSTATE_UINT8(le_host, Nano7BtUartState),
        VMSTATE_UINT32(baud, Nano7BtUartState),
        VMSTATE_UINT8_ARRAY(bdaddr, Nano7BtUartState, 6),
        VMSTATE_END_OF_LIST()
    },
};

static void bt_summary(void)
{
    Nano7BtUartState *s = bt_dev;

    if (s && (s->tx_bytes || s->rx_bytes)) {
        fprintf(stderr, "[nano7-bt] summary: host sent %" PRIu64 " bytes (%"
                PRIu64 " commands, %" PRIu64 " ACL), controller sent %"
                PRIu64 " bytes (%" PRIu64 " events), scan enable %u, local "
                "name \"%.32s\"%s; peer: %u pairings, link %s, %u L2CAP "
                "frames, %u HID reports\n", s->tx_bytes, s->cmds, s->acl_tx,
                s->rx_bytes, s->events, s->scan_enable,
                (const char *)s->local_name,
                s->use_chr ? " [socket backend]" : "", s->pairings,
                s->link == 2 ? "up" : "down", s->l2cap_rx, s->reports);
    }
}

void nano7_btuart_attach(MemoryRegion *parent, hwaddr base, qemu_irq irq,
                         const char *hci_socket, const char *peer,
                         const char *pad)
{
    Nano7BtUartState *s = g_new0(Nano7BtUartState, 1);

    s->irq = irq;
    s->trace = getenv("NANO7_BT_TRACE") != NULL;
    s->baud = 115200;
    s->cod[0] = 0x14; s->cod[1] = 0x04; s->cod[2] = 0x20;  /* audio */
    memcpy(s->local_name, "iPod", 4);
    memcpy(s->bdaddr, bt_bdaddr_default, 6);
    s->reply_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, bt_reply_timer, s);
    nano7_clock_track(s->reply_timer, true);
    s->inquiry_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, bt_inquiry_timer, s);
    nano7_clock_track(s->inquiry_timer, true);
    s->peer_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, bt_peer_timer, s);
    nano7_clock_track(s->peer_timer, true);
    /* experiments: NANO7_BT_PEER_COD=0x240418 (headphones) etc. */
    if (getenv("NANO7_BT_PEER_COD")) {
        uint32_t cod = strtoul(getenv("NANO7_BT_PEER_COD"), NULL, 0);

        bt_peer_cod[0] = cod;
        bt_peer_cod[1] = cod >> 8;
        bt_peer_cod[2] = cod >> 16;
    }
    s->connect_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, bt_connect_timer, s);
    nano7_clock_track(s->connect_timer, true);
    s->auth_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, bt_auth_timer, s);
    nano7_clock_track(s->auth_timer, true);
    s->pad_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, bt_pad_timer, s);
    nano7_clock_track(s->pad_timer, true);
    pad_parse_script(pad ? pad : "1:a/2:b/3:right:300/4:a+b:200/5:start");
    if (!peer || !strcmp(peer, "gamepad")) {
        s->peer = true;
    } else if (!strcmp(peer, "gamepad-connect")) {
        s->peer = s->peer_connect = true;
    } else if (strcmp(peer, "none")) {
        error_report("nano7: bt-peer must be gamepad, gamepad-connect or "
                     "none");
        exit(1);
    }
    if (hci_socket && *hci_socket) {
        g_autofree char *spec = NULL;
        Chardev *chr;

        if (g_str_has_prefix(hci_socket, "tcp:")) {
            spec = g_strdup_printf("%s,server=on,wait=off", hci_socket);
        } else {
            spec = g_strdup_printf("unix:%s,server=on,wait=off", hci_socket);
        }
        chr = qemu_chr_new("nano7-bt-hci", spec, NULL);
        if (!chr) {
            error_report("nano7: bt-hci: cannot open '%s'", spec);
            exit(1);
        }
        qemu_chr_fe_init(&s->chr, chr, &error_fatal);
        qemu_chr_fe_set_handlers(&s->chr, bt_chr_can_receive, bt_chr_receive,
                                 bt_chr_event, NULL, s, NULL, true);
        s->use_chr = true;
        info_report("nano7-bt: HCI H4 stream on %s (fake controller off)",
                    spec);
    }
    memory_region_init_io(&s->mr, NULL, &bt_ops, s, "nano7.btuart", BT_SIZE);
    memory_region_add_subregion(parent, base, &s->mr);
    vmstate_register_any(NULL, &vmstate_bt, s);
    bt_dev = s;
    atexit(bt_summary);
    /* run.sh --pad-keys: start in pad mode (F8) */
    if (getenv("NANO7_PAD_KEYS") && strcmp(getenv("NANO7_PAD_KEYS"), "0")) {
        nano7_btuart_pad_mode(true);
    }
}
