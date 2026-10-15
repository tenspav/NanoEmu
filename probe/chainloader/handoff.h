/*
 * handoff.h -- SRAM hand-off stub and CPU takeover interface
 * (spec: docs/tasks/piece3-handoff.md, plan: docs/research/dualboot-chainloader-plan.md).
 *
 * Provides handoff_to_payload():
 *   1. Copies a position-independent trampoline into internal SRAM (0x22000000)
 *      and a warm-reset return stub at 0x22008100.
 *   2. Invokes the RetailOS supervisor-mode dispatch hook at 0x08917714
 *      to escalate to SVC mode (CPSR = 0x13).
 *   3. The SRAM trampoline:
 *      - disables CPU interrupts (CPSID aif)
 *      - cleans and invalidates the L1 data cache (CP15 DCCISW by set/way)
 *      - executes DSB
 *      - disables MMU, caches, and branch prediction in SCTLR (CP15 c1)
 *      - invalidates I-cache (ICIALLU), branch predictor (BPIALL), and TLB (TLBIALL)
 *      - executes DSB and ISB
 *      - relocates payload from DRAM staging buffer to physical DRAM 0x08000000
 *      - sets standard boot register ABI (r0=0, r1=0x1097, r2=0x22009000,
 *        sp=0x22010000, lr=0x22008100)
 *      - branches to 0x08000000
 */

#ifndef CHAINLOADER_HANDOFF_H
#define CHAINLOADER_HANDOFF_H

#include <stdint.h>

/* Memory map constants */
#define SRAM_BASE               0x22000000u
#define SRAM_SIZE               (256u * 1024u)
#define SRAM_TRAMPOLINE_ADDR    0x22000000u
#define SRAM_RESET_STUB_ADDR    0x22008100u
#define SRAM_HANDOFF_BLOCK_ADDR 0x22009000u
#define SRAM_STACK_TOP          0x22010000u

#define DRAM_PHYS_BASE          0x08000000u
#define DRAM_SIZE               (64u * 1024u * 1024u)

/* RetailOS SVC dispatcher hook */
#define SVC_HOOK_TABLE_ADDR     0x08917714u
#define ARCH_ID_NANO7           0x00001097u
#define HANDOFF_MAGIC           0x4c43374eu   /* "N7CL" */
#define HANDOFF_VERSION         1u

/* Handoff ABI structure placed at 0x22009000 */
struct n7_handoff {
    uint32_t magic;           /* 0x4c43374e ("N7CL") */
    uint32_t version;         /* 1 */
    uint32_t dram_base;       /* 0x08000000 */
    uint32_t dram_size;       /* 0x04000000 (64 MiB) */
    uint32_t entry_point;     /* 0x08000000 */
    uint32_t initrd_start;    /* 0 */
    uint32_t initrd_size;     /* 0 */
    uint32_t cmdline_ptr;     /* 0 */
    uint32_t watchdog_mmio;   /* 0x3c800000 */
    uint32_t uart_mmio;       /* 0x3ce00000 */
};

/* Embedded default payload (piece 1 payload.bin fallback) */
extern const uint8_t default_payload_bin[];
extern const uint8_t default_payload_bin_end[];
#define DEFAULT_PAYLOAD_SIZE ((uint32_t)(default_payload_bin_end - default_payload_bin))

/*
 * handoff_to_payload -- copy trampoline into SRAM 0x22000000, escalate to SVC
 * mode via the service hook at 0x08917714, disable MMU and caches, relocate
 * payload to DRAM 0x08000000, and branch to 0x08000000.
 *
 * Parameters:
 *   payload - pointer to staged payload binary in DRAM
 *   size    - size of payload binary in bytes
 *
 * Does not touch the stack after MMU-off. Never returns.
 */
void handoff_to_payload(const void *payload, uint32_t size) __attribute__((noreturn));

#endif /* CHAINLOADER_HANDOFF_H */
