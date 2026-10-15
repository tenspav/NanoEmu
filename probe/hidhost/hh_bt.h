/*
 * hh_bt.h -- Bluetooth HID host on top of RetailOS 1.1.2's own Bluetooth
 * stack (Open Interface L2CAP). It connects to a known pad address, reads
 * HIDDescriptorList over SDP, then opens authenticated HID control/interrupt
 * channels. The OS owns pairing and the bond database. Incoming reconnects
 * use the same HID security policy. OS entry points: boot-progress.md.
 *
 * Foreground app: every OS callback goes through a trampoline in OS heap
 * that checks the app is still loaded (the resident clears its aux block on
 * every launch), so a stale callback after an app switch is a no-op.
 */
#ifndef HH_BT_H
#define HH_BT_H

#include <stdint.h>
#include <stdbool.h>
#include "hid_parse.h"

typedef enum {
    HH_OFF,             /* Bluetooth is off (no L2CAP PSM table) */
    HH_POWERING,        /* asked the OS to switch Bluetooth on */
    HH_LISTENING,       /* PSMs registered, waiting for a gamepad */
    HH_CONTROL,         /* HID control channel open */
    HH_CONNECTED,       /* control + interrupt: reports flow */
    HH_ERROR,
} hh_state_t;

/* once per launch, after LVGL is up; false = no OS heap for the trampolines */
bool hh_bt_init(void);
/* every frame (UI task): power / registration state machine */
void hh_bt_tick(void);
/* ask the OS to switch Bluetooth on (as Settings > Bluetooth does) */
bool hh_bt_power_on(void);
/* Select a known peer in OS display order before Bluetooth powers on. */
bool hh_bt_connect(const uint8_t addr[6]);

hh_state_t hh_bt_state(void);
/* one line for the screen, e.g. "LISTENING  PSM 0x11 0x13" */
const char *hh_bt_status(void);
/* decoded pad state; false until the first report */
bool hh_bt_pad(hid_pad *out);
uint32_t hh_bt_reports(void);
/* last raw input report (after the 0xa1 header) */
int hh_bt_last_report(uint8_t *buf, int max);
/* Peer address in OS display order (most significant byte first). */
bool hh_bt_peer(uint8_t addr[6]);

uint32_t hh_bt_descriptor_bytes(void);

/* tiny formatters: append, NUL-terminate, return the new end */
char *put_s(char *p, const char *s);
char *put_u(char *p, uint32_t v);
char *put_d(char *p, int32_t v);
char *put_x(char *p, uint32_t v, int digits);

#endif
