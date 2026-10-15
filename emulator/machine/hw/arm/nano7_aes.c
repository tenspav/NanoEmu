/*
 * nano7_aes.c -- AES engine at 0x38c00000 for the iPod nano 7G (S5L8747).
 *
 * Register use, as RetailOS drives it (Session 32 RE, driver 0x0841140c
 * aes(enc, keytype, key, src, dst, len, iv)):
 *   +0x00  enable        1 before the job, 0 after
 *   +0x04  go            1 starts the job
 *   +0x08  reset         write 1, poll until it reads 0 (skipped for type 3)
 *   +0x0c  status        write 7 clears; bit0 = done (polled)
 *   +0x14  mode          0xe | encrypt (bit0)
 *   +0x18  length        bytes (multiple of 16)
 *   +0x20  source        +0x24 its length
 *   +0x28  destination   +0x2c its length (+0x30/+0x34: same again)
 *   +0x5c..+0x68  custom key (AES-128), only for key type 0
 *   +0x6c  key type      0 custom, 1/2 hardware keys (GID / UID class),
 *                        3 = keep the previous key; +0x88 = ~type
 *   +0x70  1 before every job
 *   +0x74..+0x80  IV (zeros when the caller passes none)
 *
 * Model: GO runs the whole job at once (AES-128-CBC over guest memory) and
 * sets status bit0. The hardware keys are burned into the silicon and not
 * known here: with aes-gid-key= / aes-uid-key= (32 hex digits) they are
 * used; otherwise the job is a plain copy, and logged with its caller and
 * first bytes so the consumer can be identified.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "nano7_aes.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"
#include "exec/address-spaces.h"
#include "migration/vmstate.h"
#include "crypto/aes.h"
#include "qemu/error-report.h"

#define AES_REGS        0x100
#define AES_MAX_JOB     (16u << 20)
#define AES_LOG_MAX     64

typedef struct Nano7AesState {
    MemoryRegion mr;
    uint32_t regs[AES_REGS / 4];
    uint32_t status;
    uint8_t hwkey[3][16];
    bool have_hwkey[3];
    unsigned logged;
    uint64_t jobs, hw_jobs, passthru;
} Nano7AesState;

static Nano7AesState *aes_dev;

/* return address of the driver's caller: 0x0841140c pushes r4-r10, lr */
static uint32_t aes_caller(void)
{
    uint8_t b[4];
    CPUState *cs = current_cpu;

    if (!cs || cpu_memory_rw_debug(cs, ARM_CPU(cs)->env.regs[13] + 0x1c,
                                   b, 4, false)) {
        return 0;
    }
    return ldl_le_p(b);
}

static void aes_run(Nano7AesState *s)
{
    uint32_t mode = s->regs[0x14 >> 2];
    uint32_t len = s->regs[0x18 >> 2];
    uint32_t src = s->regs[0x20 >> 2];
    uint32_t dst = s->regs[0x28 >> 2];
    uint32_t type = s->regs[0x6c >> 2];
    bool enc = mode & 1;
    uint8_t key[16], iv[16], *buf;
    bool cipher = true;
    AES_KEY k;
    uint32_t i, j;

    s->jobs++;
    len &= ~15u;
    if (len > AES_MAX_JOB) {
        len = AES_MAX_JOB;
    }
    for (i = 0; i < 4; i++) {
        stl_be_p(key + 4 * i, s->regs[(0x5c >> 2) + i]);
        stl_be_p(iv + 4 * i, s->regs[(0x74 >> 2) + i]);
    }
    if (type >= 1 && type <= 3) {
        s->hw_jobs++;
        if (type <= 2 && s->have_hwkey[type]) {
            memcpy(key, s->hwkey[type], 16);
        } else {
            cipher = false;
        }
    }
    buf = g_malloc(len ? len : 1);
    address_space_read(&address_space_memory, src, MEMTXATTRS_UNSPECIFIED,
                       buf, len);
    if (s->logged < AES_LOG_MAX) {
        s->logged++;
        fprintf(stderr, "[nano7-aes] %s key %u%s len %u 0x%08x -> 0x%08x "
                "caller 0x%08x in", enc ? "encrypt" : "decrypt", type,
                cipher ? "" : " (unknown: copy)", len, src, dst,
                aes_caller());
        for (i = 0; i < MIN(len, 16u); i++) {
            fprintf(stderr, " %02x", buf[i]);
        }
        fprintf(stderr, "\n");
        fflush(stderr);
    }
    if (cipher) {
        /* AES-128-CBC, CBC chained over the whole job */
        if (enc) {
            AES_set_encrypt_key(key, 128, &k);
        } else {
            AES_set_decrypt_key(key, 128, &k);
        }
        for (i = 0; i < len; i += 16) {
            uint8_t t[16];

            if (enc) {
                for (j = 0; j < 16; j++) {
                    t[j] = buf[i + j] ^ iv[j];
                }
                AES_encrypt(t, buf + i, &k);
                memcpy(iv, buf + i, 16);
            } else {
                memcpy(t, buf + i, 16);
                AES_decrypt(t, buf + i, &k);
                for (j = 0; j < 16; j++) {
                    buf[i + j] ^= iv[j];
                }
                memcpy(iv, t, 16);
            }
        }
    } else {
        s->passthru++;
    }
    address_space_write(&address_space_memory, dst, MEMTXATTRS_UNSPECIFIED,
                        buf, len);
    g_free(buf);
    s->status |= 1;
}

static uint64_t aes_read(void *opaque, hwaddr offset, unsigned size)
{
    Nano7AesState *s = opaque;

    switch (offset) {
    case 0x04:
    case 0x08:
        return 0;               /* go / reset self-clear */
    case 0x0c:
        return s->status;
    default:
        return offset < AES_REGS ? s->regs[offset >> 2] : 0;
    }
}

static void aes_write(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    Nano7AesState *s = opaque;
    uint32_t v = value;

    if (offset >= AES_REGS) {
        return;
    }
    switch (offset) {
    case 0x04:
        if (v & 1) {
            aes_run(s);
        }
        break;
    case 0x08:
        if (v & 1) {
            s->status = 0;
        }
        break;
    case 0x0c:
        s->status &= ~v;
        break;
    default:
        s->regs[offset >> 2] = v;
        break;
    }
}

static const MemoryRegionOps aes_ops = {
    .read = aes_read,
    .write = aes_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static const VMStateDescription vmstate_aes = {
    .name = "nano7-aes",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Nano7AesState, AES_REGS / 4),
        VMSTATE_UINT32(status, Nano7AesState),
        VMSTATE_END_OF_LIST()
    },
};

static void aes_summary(void)
{
    if (aes_dev && aes_dev->jobs) {
        fprintf(stderr, "[nano7-aes] summary: %" PRIu64 " jobs, %" PRIu64
                " with a hardware key (%" PRIu64 " copied: key unknown)\n",
                aes_dev->jobs, aes_dev->hw_jobs, aes_dev->passthru);
    }
}

static bool aes_parse_key(const char *hex, uint8_t *out)
{
    int i;

    if (!hex || strlen(hex) != 32) {
        return false;
    }
    for (i = 0; i < 16; i++) {
        char t[3] = { hex[2 * i], hex[2 * i + 1], 0 };
        char *end;

        out[i] = strtoul(t, &end, 16);
        if (*end) {
            return false;
        }
    }
    return true;
}

void nano7_aes_attach(MemoryRegion *parent, hwaddr base, const char *gid_key,
                      const char *uid_key)
{
    Nano7AesState *s = g_new0(Nano7AesState, 1);

    if (gid_key) {
        s->have_hwkey[1] = aes_parse_key(gid_key, s->hwkey[1]);
        if (!s->have_hwkey[1]) {
            error_report("nano7: aes-gid-key needs 32 hex digits");
            exit(1);
        }
    }
    if (uid_key) {
        s->have_hwkey[2] = aes_parse_key(uid_key, s->hwkey[2]);
        if (!s->have_hwkey[2]) {
            error_report("nano7: aes-uid-key needs 32 hex digits");
            exit(1);
        }
    }
    memory_region_init_io(&s->mr, NULL, &aes_ops, s, "nano7.aes", 0x1000);
    memory_region_add_subregion(parent, base, &s->mr);
    vmstate_register_any(NULL, &vmstate_aes, s);
    aes_dev = s;
    atexit(aes_summary);
}
