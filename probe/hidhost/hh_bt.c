/*
 * hh_bt.c -- see hh_bt.h. Bluetooth HID host on the OS's
 * Open Interface L2CAP layer.
 *
 * OS entry points (RetailOS 1.1.2, Thumb; boot-progress.md Sessions 43/49):
 *   Register   0x080f7764(psm, connInd)  0 ok, 0x192 already, 0x68 table full
 *   Unregister 0x080f7580(psm)           0 ok, 0x8d channel live, 0x78 unknown
 *   Accept     0x083d0fb4(cfm, disc, data, cid, params, accept, secdesc)
 *              params NULL = defaults; accept 0 = reject; secdesc kept by
 *              pointer (0x18 bytes, must stay alive)
 *   Disconnect 0x08410eb8(cid)
 *   callbacks  connInd(psm, bd6, cid)   on the stack thread, Accept inside OK
 *              cfm(cid, a, mtu, status)  status 0 = open
 *              disc(cid, reason)
 *              data(cid, buf, len)       buf = L2CAP payload, valid during call
 *   PSM table  *0x0896d92c: 47 x {u16 psm; fn}, NULL while Bluetooth is off
 *   power      0x08004f64(mgr = 0x084127a0(), 1)  async Bluetooth power-on
 */
#include "hh_bt.h"
#include "hb_sdk.h"
#include "hb_heap.h"
#include <stddef.h>
#include <string.h>

#define FN(a)           ((a) | 1u)
#define A_L2_REGISTER   0x080f7764u
#define A_L2_ACCEPT     0x083d0fb4u
#define A_L2_DISCONNECT 0x08410eb8u
#define A_PSM_TABLE     0x0896d92cu
/* power manager (Session 49 RE): getter, async power-on(mgr, client 1 = BT) */
#define A_PWR_MGR_GET   0x084127a0u
#define A_PWR_ON        0x08004f64u
#define PSM_SLOTS       47

#define PSM_CTRL        0x11
#define PSM_INTR        0x13
/* HID service security: the OS authenticates outgoing channels and bonds. */
#ifndef HH_SEC_UUID
#define HH_SEC_UUID     0x1124u
#endif

typedef uint32_t (*l2_register_t)(uint32_t psm, void *conn_ind);
typedef uint32_t (*l2_accept_t)(void *cfm, void *disc, void *data, uint32_t cid,
                                void *params, uint32_t accept, void *secdesc);
typedef uint32_t (*l2_disconnect_t)(uint32_t cid);
typedef void (*vfn_t)(void);

/* ---- trampolines (OS heap, survive the app) ---------------------------- */

/* ARM: if (*slot == expect) tail-jump target (args untouched) else return 0 */
static const uint32_t tramp_code[9] = {
    0xe52d4004,     /* push   {r4}                 */
    0xe59f4018,     /* ldr    r4, slot             */
    0xe5944000,     /* ldr    r4, [r4]             */
    0xe59fc014,     /* ldr    ip, expect           */
    0xe154000c,     /* cmp    r4, ip               */
    0xe49d4004,     /* pop    {r4}                 */
    0x059ff00c,     /* ldreq  pc, target (interworking) */
    0xe3a00000,     /* mov    r0, #0               */
    0xe12fff1e,     /* bx     lr                   */
};

typedef struct hh_tramp {
    uint32_t code[9];
    uint32_t slot, expect, target;
} hh_tramp;

enum { T_CONNIND, T_CFM, T_DISC, T_DATA, T_WRITE, T_N };

#define HH_MAGIC 0x32424848u    /* 'HHB1' */

typedef struct hh_persist {
    uint32_t magic;
    uint32_t secdesc[3][6];     /* control, interrupt: {0x00040003, uuid, 0..} */
    uint8_t tx[64];
    hh_tramp t[T_N];
} hh_persist;

/* ---- state ---------------------------------------------------------------- */

static hh_persist *P;
static hh_state_t st;
static uint32_t t_state;                /* ms when st was entered */
static uint32_t reg_err;
static char status[64];
static hid_desc descriptor;
static volatile bool descriptor_ok, sdp_open, tx_busy;
static volatile uint16_t cid_sdp;
static bool outgoing_started, ctrl_started, intr_started;
static volatile bool sdp_sent;
static uint8_t sdp_bytes[1024], continuation[17];
static unsigned sdp_size, continuation_size;
static uint16_t transaction;
static bool pairing_confirmed;
/* NanoPad default; callers can select another known address before connecting. */
static uint8_t target[6];

/* written on the stack thread, read on the UI task */
static volatile uint16_t cid_ctrl, cid_intr;
static volatile bool open_ctrl, open_intr;
static volatile uint32_t n_reports, n_ctrl_pkts, n_conn_ind;
static volatile uint8_t peer[6];
static volatile bool have_peer;
static volatile uint8_t last[32];
static volatile int last_len;
static volatile uint32_t last_seq;

/* tiny formatting (no libc printf in GL builds) */
char *put_s(char *p, const char *s)
{
    while (*s) {
        *p++ = *s++;
    }
    *p = 0;
    return p;
}

char *put_u(char *p, uint32_t v)
{
    char t[12];
    int n = 0;

    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) {
        *p++ = t[--n];
    }
    *p = 0;
    return p;
}

char *put_d(char *p, int32_t v)
{
    if (v < 0) {
        *p++ = '-';
        v = -v;
    }
    return put_u(p, (uint32_t)v);
}

char *put_x(char *p, uint32_t v, int digits)
{
    static const char hex[] = "0123456789ABCDEF";

    while (digits--) {
        *p++ = hex[(v >> (4 * digits)) & 15];
    }
    *p = 0;
    return p;
}

static void set_state(hh_state_t s)
{
    if (st != s) {
        st = s;
        t_state = hb_time_uptime_ms();
    }
}

/* ---- callbacks (BT stack thread, stack mutex held) ------------------------ */

/* Decode bounded SDP data elements recursively; HIDDescriptorList contains
 * a descriptor type (0x22) followed by a text-string data element. */
static bool read_elements(const uint8_t *p, unsigned n, unsigned depth)
{
    unsigned pos = 0;
    bool report = false;
    if (depth > 8) return false;
    while (pos < n) {
        unsigned tag = p[pos++], size = tag & 7, type = tag >> 3, len;
        if (type == 0) len = 0;
        else if (size < 5) len = 1u << size;
        else {
            unsigned bytes = 1u << (size - 5), k;
            if (bytes > n - pos) return false;
            len = 0;
            for (k = 0; k < bytes; k++) len = (len << 8) | p[pos++];
        }
        if (len > n - pos) return false;
        if (type == 4 && report) return hid_parse(p + pos, len, &descriptor);
        if (type == 6 && read_elements(p + pos, len, depth + 1)) return true;
        report = type == 1 && len == 1 && p[pos] == 0x22;
        pos += len;
    }
    return false;
}

static void cb_write(uint32_t cid, void *buf, uint32_t len, uint32_t error)
{
    (void)cid; (void)buf; (void)len;
    tx_busy = false;
    if (error) reg_err = error;
}

static void cb_data(uint32_t cid, const uint8_t *buf, uint32_t len)
{
    int n, i;

    if (cid == cid_sdp) {
        unsigned bytes, c;
        if (len < 8 || (((unsigned)buf[3] << 8) | buf[4]) != len - 5 ||
            buf[0] != 7 ||
            (((unsigned)buf[1] << 8) | buf[2]) != transaction) return;
        bytes = ((unsigned)buf[5] << 8) | buf[6];
        if (bytes > len - 8 || bytes > sizeof sdp_bytes - sdp_size) return;
        c = buf[7 + bytes];
        if (c > 16 || c > len - 8 - bytes) return;
        memcpy(sdp_bytes + sdp_size, buf + 7, bytes);
        sdp_size += bytes;
        continuation_size = c;
        memcpy(continuation, buf + 8 + bytes, c);
        if (c) sdp_sent = false;
        else descriptor_ok = read_elements(sdp_bytes, sdp_size, 0);
        return;
    }
    if (cid == cid_intr && len >= 2 && buf[0] == 0xa1) {   /* DATA | Input */
        n = (int)len - 1;
        if (n > (int)sizeof last) {
            n = sizeof last;
        }
        for (i = 0; i < n; i++) {
            last[i] = buf[1 + i];
        }
        last_len = n;
        last_seq++;
        n_reports++;
    } else {
        n_ctrl_pkts++;      /* control traffic (HANDSHAKE etc.): counted only */
    }
}

static void cb_cfm(uint32_t cid, uint32_t a, uint32_t mtu, uint32_t status_)
{
    (void)a;
    (void)mtu;
    if (status_) {
        reg_err = status_;
        return;
    }
    if (cid == cid_sdp) {
        sdp_open = true;
    } else if (cid == cid_ctrl) {
        open_ctrl = true;
    } else if (cid == cid_intr) {
        open_intr = true;
    }
}

static void cb_disc(uint32_t cid, uint32_t reason)
{
    (void)reason;
    if (cid == cid_sdp) {
        sdp_open = false;
        cid_sdp = 0;
    } else if (cid == cid_ctrl) {
        open_ctrl = false;
        cid_ctrl = 0;
    } else if (cid == cid_intr) {
        open_intr = false;
        cid_intr = 0;
    }
}

static void cb_conn_ind(uint32_t psm, const uint8_t *bd, uint32_t cid)
{
    l2_accept_t accept = (l2_accept_t)FN(A_L2_ACCEPT);
    int i;

    n_conn_ind++;
    psm &= 0xffff;
    cid &= 0xffff;
    if (psm == PSM_CTRL && !cid_ctrl) {
        cid_ctrl = cid;
        for (i = 0; i < 6; i++) {
            peer[i] = bd[i];
        }
        have_peer = true;
        accept(&P->t[T_CFM], &P->t[T_DISC], &P->t[T_DATA], cid, 0, 1,
               P->secdesc[0]);
    } else if (psm == PSM_INTR && !cid_intr) {
        cid_intr = cid;
        accept(&P->t[T_CFM], &P->t[T_DISC], &P->t[T_DATA], cid, 0, 1,
               P->secdesc[1]);
    } else {
        accept(0, 0, 0, cid, 0, 0, 0);                      /* reject */
    }
}

/* ---- setup ----------------------------------------------------------------- */

static volatile uint32_t *psm_table(void)
{
    return *(volatile uint32_t *volatile *)A_PSM_TABLE;
}

/* our trampoline block, if a previous launch registered PSM 0x11 with it */
static hh_persist *find_registered(void)
{
    volatile uint32_t *t = psm_table();
    int i;

    if (!t) {
        return 0;
    }
    for (i = 0; i < PSM_SLOTS; i++) {
        if ((t[2 * i] & 0xffff) == PSM_CTRL) {
            uint32_t fn = t[2 * i + 1];
            hh_persist *p = (hh_persist *)(fn - offsetof(hh_persist, t));

            if (fn >= 0x08000000u && fn < 0x0c000000u && !(fn & 3) &&
                p->magic == HH_MAGIC) {
                return p;
            }
            return 0;
        }
    }
    return 0;
}

static void arm_trampolines(hh_persist *p, void *token)
{
    static const vfn_t targets[T_N] = {
        (vfn_t)cb_conn_ind, (vfn_t)cb_cfm, (vfn_t)cb_disc, (vfn_t)cb_data, (vfn_t)cb_write,
    };
    int i;

    for (i = 0; i < T_N; i++) {
        memcpy(p->t[i].code, tramp_code, sizeof tramp_code);
        p->t[i].slot = (uint32_t)&hb_shared->aux_block;
        p->t[i].expect = (uint32_t)token;
        p->t[i].target = (uint32_t)targets[i];      /* Thumb bit set */
    }
    hb_icache_invalidate((uint32_t)p, sizeof *p);
}

bool hh_bt_init(void)
{
    void *token;


    static const uint8_t nanopad[6] = {2, 0x4e, 0x50, 0x41, 0x44, 1};
    memcpy(target, nanopad, 6);
    st = HH_OFF;
    t_state = hb_time_uptime_ms();
    if (!hb_shared) {
        set_state(HH_ERROR);
        return false;
    }
    /* liveness token: the resident frees it and clears the slot on the next
     * launch, which disarms every trampoline */
    if (HB_APP_AUX_BLOCK) {
        hb_os_free(HB_APP_AUX_BLOCK);
        HB_APP_AUX_BLOCK = 0;
    }
    token = hb_os_alloc(16);
    P = find_registered();
    if (!P) {
        P = hb_os_alloc(sizeof *P);
    }
    if (!token || !P) {
        set_state(HH_ERROR);
        return false;
    }
    HB_APP_AUX_BLOCK = token;
    P->magic = HH_MAGIC;
    memset(P->secdesc, 0, sizeof P->secdesc);
    P->secdesc[0][0] = 0x00040003;
    P->secdesc[0][1] = HH_SEC_UUID;
    P->secdesc[1][0] = 0x00040003;
    P->secdesc[1][1] = HH_SEC_UUID;
    P->secdesc[2][0] = 0x00040003;
    P->secdesc[2][1] = 0x1000; /* SDP is publicly readable */
    arm_trampolines(P, token);
    return true;
}

/* register both PSMs (already ours from an earlier launch = fine) */
static bool register_psms(void)
{
    l2_register_t reg = (l2_register_t)FN(A_L2_REGISTER);
    uint32_t e1, e2;

    /* no stack mutex: Register takes none and the OS's own services call it
     * bare (0x084e9c44, 0x080f0238) */
    e1 = reg(PSM_CTRL, &P->t[T_CONNIND]);
    e2 = reg(PSM_INTR, &P->t[T_CONNIND]);
    if ((e1 && e1 != 0x192) || (e2 && e2 != 0x192)) {
        reg_err = e1 ? e1 : e2;
        return false;
    }
    return true;
}

bool hh_bt_power_on(void)
{
    typedef void *(*get_t)(void);
    typedef uint32_t (*pwr_t)(void *mgr, uint32_t client);
    void *mgr = ((get_t)FN(A_PWR_MGR_GET))();

    /* queued to the BT stack manager; 0 = accepted */
    if (!mgr || ((pwr_t)FN(A_PWR_ON))(mgr, 1)) {
        return false;
    }
    set_state(HH_POWERING);
    return true;
}

bool hh_bt_connect(const uint8_t addr[6])
{
    if (outgoing_started) return false;
    memcpy(target, addr, 6);
    return true;
}

static uint32_t connect_channel(unsigned psm, volatile uint16_t *cid, unsigned sec)
{
    typedef uint32_t (*connect_t)(void *, void *, void *, uint32_t,
                                  const void *, void *, void *, void *);
    return ((connect_t)FN(0x083d8a60))(&P->t[T_CFM], &P->t[T_DISC],
        &P->t[T_DATA], psm, target, 0, (void *)cid, P->secdesc[sec]);
}

/* UI task: serialize calls with the OS Bluetooth stack. Write's buffer lives
 * in the trampoline allocation until its completion callback. */
static void outgoing_tick(void)
{
    typedef uint32_t (*write_t)(void *, uint32_t, void *, void *, uint32_t);
    static const uint8_t request[] = {
        6, 0, 0, 0, 0, 0x35, 3, 0x19, 0x11, 0x24,
        3, 0xff, 0x35, 3, 0x09, 2, 6
    };
    ((vfn_t)FN(0x08404e74))();
    /* RetailOS dispatches SSP confirmation for an unknown service to an
     * audio-only agent which leaves it unanswered. Its pending request at
     * 08b302e0 contains the OS-order address and numeric value (+12). Only
     * confirm the peer explicitly selected by this app, through SECMGR's
     * own reply API (which keeps its normal command completion callback). */
    if (ctrl_started && !pairing_confirmed && !open_ctrl &&
        !memcmp((const void *)0x08b302e0u, target, 6)) {
        typedef uint32_t (*confirm_t)(uint32_t, const uint8_t *);
        pairing_confirmed = true;
        ((confirm_t)FN(0x084135e4))(1, target);
    }
    if (!outgoing_started) {
        outgoing_started = true;
        memcpy((void *)peer, target, 6);
        have_peer = true;
        reg_err = connect_channel(1, &cid_sdp, 2);
        if (reg_err) outgoing_started = false;
    } else if (sdp_open && !sdp_sent && !tx_busy) {
        unsigned len = sizeof request + 1 + continuation_size;
        memcpy(P->tx, request, sizeof request);
        transaction++;
        P->tx[1] = transaction >> 8; P->tx[2] = transaction;
        P->tx[3] = (len - 5) >> 8; P->tx[4] = len - 5;
        P->tx[sizeof request] = continuation_size;
        memcpy(P->tx + sizeof request + 1, continuation, continuation_size);
        sdp_sent = tx_busy = true;
        reg_err = ((write_t)FN(0x080f77e4))(&P->t[T_WRITE], cid_sdp,
                                           0, P->tx, len);
        if (reg_err) tx_busy = false;
    } else if (descriptor_ok && !ctrl_started && !cid_ctrl) {
        ctrl_started = true;
        reg_err = connect_channel(PSM_CTRL, &cid_ctrl, 0);
    } else if (open_ctrl && !intr_started && !cid_intr) {
        intr_started = true;
        reg_err = connect_channel(PSM_INTR, &cid_intr, 1);
    }
    ((vfn_t)FN(0x08404e94))();
}

void hh_bt_tick(void)
{
    uint32_t secs = (hb_time_uptime_ms() - t_state) / 1000;
    char *p;

    if (!P) {
        put_s(status, "ERROR: NO OS HEAP");
        return;
    }
    if (!psm_table()) {
        if (st != HH_POWERING) {
            set_state(HH_OFF);
        }
        cid_ctrl = cid_intr = 0;
        open_ctrl = open_intr = false;
    } else if (st == HH_OFF || st == HH_POWERING) {
        set_state(register_psms() ? HH_LISTENING : HH_ERROR);
    } else if (st != HH_ERROR) {
        set_state(open_intr ? HH_CONNECTED :
                  open_ctrl ? HH_CONTROL : HH_LISTENING);
    }
    if (psm_table() && st != HH_ERROR) outgoing_tick();
    p = status;
    switch (st) {
    case HH_OFF:
        p = put_s(p, "BLUETOOTH OFF  ");
        p = put_u(p, secs);
        p = put_s(p, "s");
        break;
    case HH_POWERING:
        p = put_s(p, "BUSY: BLUETOOTH ON...  ");
        p = put_u(p, secs);
        p = put_s(p, "s");
        break;
    case HH_LISTENING:
        p = put_s(p, "LISTENING PSM 11/13  ");
        p = put_u(p, secs);
        p = put_s(p, "s E ");
        p = put_x(p, reg_err, 3);
        break;
    case HH_CONTROL:
        p = put_s(p, "HID CONTROL UP, WAIT INTR  ");
        p = put_u(p, secs);
        p = put_s(p, "s");
        break;
    case HH_CONNECTED:
        p = put_s(p, "HID CONNECTED  ");
        p = put_u(p, n_reports);
        p = put_s(p, " REPORTS");
        break;
    case HH_ERROR:
        p = put_s(p, "ERROR: L2CAP REGISTER ");
        p = put_x(p, reg_err, 3);
        break;
    }
    *p = 0;
}

hh_state_t hh_bt_state(void) { return st; }
const char *hh_bt_status(void) { return status; }
uint32_t hh_bt_reports(void) { return n_reports; }
uint32_t hh_bt_descriptor_bytes(void) { return descriptor_ok ? sdp_size : 0; }

int hh_bt_last_report(uint8_t *buf, int max)
{
    int n = last_len, i;

    if (n > max) {
        n = max;
    }
    for (i = 0; i < n; i++) {
        buf[i] = last[i];
    }
    return n;
}

bool hh_bt_pad(hid_pad *out)
{
    uint8_t r[32];
    uint32_t seq;
    int n;

    do {
        seq = last_seq;
        n = hh_bt_last_report(r, sizeof r);
    } while (seq != last_seq);
    memset(out, 0, sizeof *out);
    out->hat = -1;
    if (!n_reports) {
        return false;
    }
    return descriptor_ok && hid_decode(&descriptor, r, n, out);
}

bool hh_bt_peer(uint8_t addr[6])
{
    int i;

    for (i = 0; i < 6; i++) {
        addr[i] = peer[i];
    }
    return have_peer;
}
