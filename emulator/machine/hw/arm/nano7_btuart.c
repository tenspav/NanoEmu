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
 *     return parameters of a BCM2076B1; nothing is ever in range, so
 *     inquiries end empty and connections time out; or
 *   - bt-hci=PATH | tcp:[HOST]:PORT: a listening socket that carries the
 *     raw H4 byte stream both ways (no framing added), for a host bridge
 *     (a real adapter, BlueZ vhci, or a scripted controller).
 *
 * Replies reach the RX FIFO after a short delay (the controller's turn-
 * around), never inside the TX write. NANO7_BT_TRACE=1 logs every packet
 * (default: the first 400, compact).
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

#define BT_SIZE         0x1000
#define BT_RXQ          65536           /* power of two */
#define BT_REPLY_NS     200000          /* controller turn-around */
#define BT_BYTE_NS      4167            /* one byte at 2.4 Mbaud */
#define BT_LOG_MAX      400

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
    /* host -> controller H4 parser */
    uint8_t pkt[4 + 65536];
    uint32_t pkt_len, pkt_need;
    /* fake controller state */
    uint8_t scan_enable;
    uint8_t local_name[248];
    uint8_t cod[3];
    uint8_t ssp_mode, le_host;
    uint32_t baud;
    /* socket backend */
    CharBackend chr;
    bool use_chr;
    /* stats + trace */
    bool trace;
    unsigned log_lines;
    uint64_t tx_bytes, rx_bytes, cmds, events, acl_tx;
    bool rd_seen;
} Nano7BtUartState;

static Nano7BtUartState *bt_dev;

static const uint8_t bt_bdaddr[6] = { 0x01, 0x00, 0x00, 0x37, 0x4e, 0x02 };

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

static void bt_command(Nano7BtUartState *s, uint16_t op, const uint8_t *par,
                       uint8_t plen)
{
    uint8_t r[252] = { 0 };         /* r[0] = status */
    uint8_t n = 1;

    s->cmds++;
    switch (op) {
    /* link control: Command Status, then the completion event */
    case 0x0401: {                  /* Inquiry: nobody around */
        uint8_t done = 0;

        bt_cmd_status(s, op, 0);
        bt_event(s, 0x01, &done, 1);
        return;
    }
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
    case 0x0419: {                  /* Remote_Name_Request: page timeout */
        uint8_t rn[255] = { 0x04 };

        bt_cmd_status(s, op, 0);
        if (plen >= 6) {
            memcpy(rn + 1, par, 6);
        }
        bt_event(s, 0x07, rn, 255);
        return;
    }
    case 0x0406:                    /* Disconnect: no such connection */
        bt_cmd_status(s, op, 0x02);
        return;
    case 0x0402:                    /* Inquiry_Cancel */
        break;

    /* controller & baseband */
    case 0x0c03:                    /* Reset */
        s->scan_enable = 0;
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
    case 0x0c44:                    /* Read_Inquiry_Mode */
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
        memcpy(r + 1, bt_bdaddr, 6);
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
    case 0xfc18:                    /* Update_UART_Baud_Rate */
        if (plen >= 6) {
            s->baud = par[2] | par[3] << 8 | par[4] << 16 |
                      (uint32_t)par[5] << 24;
            info_report("nano7-bt: controller UART -> %u baud", s->baud);
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
    case 2: {                       /* ACL: no links exist; return credit */
        uint8_t nc[5] = { 1, p[1], p[2] & 0x0f, 1, 0 };

        s->acl_tx++;
        bt_event(s, 0x13, nc, 5);
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
        s->pending &= ~(v & ST_PENDING);
        /* RX data still waiting: the threshold/timeout interrupt recurs */
        if (bt_rx_count(s)) {
            s->pending |= ST_RXTO;
        }
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
        VMSTATE_UINT8_ARRAY(pkt, Nano7BtUartState, 4 + 65536),
        VMSTATE_UINT32(pkt_len, Nano7BtUartState),
        VMSTATE_UINT32(pkt_need, Nano7BtUartState),
        VMSTATE_UINT8(scan_enable, Nano7BtUartState),
        VMSTATE_UINT8_ARRAY(local_name, Nano7BtUartState, 248),
        VMSTATE_UINT8_ARRAY(cod, Nano7BtUartState, 3),
        VMSTATE_UINT8(ssp_mode, Nano7BtUartState),
        VMSTATE_UINT8(le_host, Nano7BtUartState),
        VMSTATE_UINT32(baud, Nano7BtUartState),
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
                "name \"%.32s\"%s\n", s->tx_bytes, s->cmds, s->acl_tx,
                s->rx_bytes, s->events, s->scan_enable,
                (const char *)s->local_name,
                s->use_chr ? " [socket backend]" : "");
    }
}

void nano7_btuart_attach(MemoryRegion *parent, hwaddr base, qemu_irq irq,
                         const char *hci_socket)
{
    Nano7BtUartState *s = g_new0(Nano7BtUartState, 1);

    s->irq = irq;
    s->trace = getenv("NANO7_BT_TRACE") != NULL;
    s->baud = 115200;
    s->cod[0] = 0x14; s->cod[1] = 0x04; s->cod[2] = 0x20;  /* audio */
    memcpy(s->local_name, "iPod", 4);
    s->reply_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, bt_reply_timer, s);
    nano7_clock_track(s->reply_timer, true);
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
}
