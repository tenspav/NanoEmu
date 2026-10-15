/*
 * hid_parse.h -- tiny HID report descriptor parser for gamepads: finds the
 * input fields for buttons (usage page 9), the Generic Desktop axes (X, Y,
 * Z, Rx, Ry, Rz, slider, dial) and the hat switch, then decodes input
 * reports into one normalized gamepad state.
 */
#ifndef HID_PARSE_H
#define HID_PARSE_H

#include <stdint.h>
#include <stdbool.h>

#define HID_MAX_FIELDS  48
#define HID_AXES        8       /* X Y Z Rx Ry Rz slider dial */

typedef struct hid_field {
    uint8_t report_id;          /* 0 = reports carry no ID byte */
    uint16_t bit_off;           /* after the ID byte */
    uint8_t bit_size;
    uint8_t count;
    uint16_t usage_page;
    uint16_t usage_min;         /* first usage (or the single usage) */
    uint16_t usage_max;
    int32_t log_min, log_max;
    bool variable;              /* Main item bit 1 */
} hid_field;

typedef struct hid_desc {
    hid_field f[HID_MAX_FIELDS];
    int nf;
    bool has_ids;
} hid_desc;

typedef struct hid_pad {
    uint32_t buttons;           /* bit n = button n+1 */
    int16_t axis[HID_AXES];     /* -32767..32767, centred */
    int8_t hat;                 /* -1 = centred, 0..7 = N NE E SE S SW W NW */
} hid_pad;

/* parse; false = malformed / nothing a gamepad would use */
bool hid_parse(const uint8_t *d, int n, hid_desc *out);
/* decode one input report (as sent after the 0xa1 HIDP header) */
bool hid_decode(const hid_desc *h, const uint8_t *r, int n, hid_pad *pad);

#endif
