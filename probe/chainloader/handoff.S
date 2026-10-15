/*
 * handoff.S -- SRAM hand-off stub and CPU takeover bridge for iPod nano 7G
 * (spec: docs/tasks/piece3-handoff.md, plan: docs/research/dualboot-chainloader-plan.md).
 *
 * Provides:
 *   handoff_to_payload(const void *payload, uint32_t size)
 *
 * Sequence:
 *   1. Copies position-independent takeover trampoline to SRAM 0x22000000.
 *   2. Copies warm-reset trap stub to SRAM 0x22008100.
 *   3. Populates handoff ABI block (struct n7_handoff) at SRAM 0x22009000.
 *   4. Escalates to SVC mode (CPSR=0x13) via verified RetailOS dispatch table
 *      hook at 0x08917714 + svc #70 (or direct branch if already privileged).
 *   5. Trampoline executes at SRAM 0x22000000 in ARM state:
 *      - CPSID aif (disables Asynchronous aborts, IRQs, FIQs)
 *      - D-cache clean and invalidate by set/way (CCSIDR geometry)
 *      - DSB
 *      - SCTLR: clears M (MMU), C (D-cache), Z (branch pred), I (I-cache), V (vectors)
 *      - Invalidates I-cache (ICIALLU), branch predictor (BPIALL), and TLB (TLBIALL)
 *      - DSB; ISB
 *      - Overlap-safe word copy of payload to physical DRAM 0x08000000
 *      - DSB; ISB
 *      - Sets boot ABI registers: r0=0, r1=0x1097, r2=0x22009000,
 *        sp=0x22010000, lr=0x22008100 (warm reset handler)
 *      - Branches to 0x08000000
 *
 * Notes on constraints:
 *   - The trampoline is 100% position-independent, pure register-based ARM code.
 *   - Zero relocations, zero literal pools inside the trampoline.
 *   - Not self-modifying.
 *   - Zero stack accesses after MMU is disabled (registers r8..r11 hold state).
 */

    .syntax unified
    .arm
    .section .text, "ax"

    .global handoff_to_payload
    .type handoff_to_payload, %function

/*
 * void handoff_to_payload(const void *payload, uint32_t size)
 *
 * Parameters:
 *   r0 = source payload pointer in DRAM
 *   r1 = size of payload in bytes
 */
handoff_to_payload:
    /* Preserve arguments in high registers across copy and SVC entry */
    mov     r8, r0              /* r8 = source payload */
    mov     r9, r1              /* r9 = payload size */

    /* Step 1: Copy trampoline into internal SRAM at 0x22000000 */
    adr     r2, _trampoline_src
    adr     r3, _trampoline_src_end
    movw    r4, #0x0000
    movt    r4, #0x2200         /* r4 = 0x22000000 */
.Lcopy_trampoline_loop:
    cmp     r2, r3
    bhs     .Lcopy_trampoline_done
    ldr     r5, [r2], #4
    str     r5, [r4], #4
    b       .Lcopy_trampoline_loop
.Lcopy_trampoline_done:

    /* Step 2: Copy warm reset trap handler into SRAM at 0x22008100 */
    adr     r2, _reset_stub_src
    adr     r3, _reset_stub_src_end
    movw    r4, #0x8100
    movt    r4, #0x2200         /* r4 = 0x22008100 */
.Lcopy_reset_loop:
    cmp     r2, r3
    bhs     .Lcopy_reset_done
    ldr     r5, [r2], #4
    str     r5, [r4], #4
    b       .Lcopy_reset_loop
.Lcopy_reset_done:

    /* Step 3: Populate Handoff ABI Block (struct n7_handoff) at 0x22009000 */
    movw    r4, #0x9000
    movt    r4, #0x2200         /* r4 = 0x22009000 */

    movw    r2, #0x374e
    movt    r2, #0x4c43         /* 0x4c43374e = "N7CL" magic */
    str     r2, [r4, #0]        /* handoff->magic */

    mov     r2, #1              /* version = 1 */
    str     r2, [r4, #4]        /* handoff->version */

    mov     r2, #0x08000000     /* dram_base = 0x08000000 */
    str     r2, [r4, #8]        /* handoff->dram_base */

    mov     r2, #0x04000000     /* dram_size = 64 MiB */
    str     r2, [r4, #12]       /* handoff->dram_size */

    mov     r2, #0x08000000     /* entry_point = 0x08000000 */
    str     r2, [r4, #16]       /* handoff->entry_point */

    mov     r2, #0
    str     r2, [r4, #20]       /* handoff->initrd_start = 0 */
    str     r2, [r4, #24]       /* handoff->initrd_size = 0 */
    str     r2, [r4, #28]       /* handoff->cmdline_ptr = 0 */

    movw    r2, #0x0000
    movt    r2, #0x3c80         /* 0x3c800000 (watchdog MMIO base) */
    str     r2, [r4, #32]       /* handoff->watchdog_mmio */

    movw    r2, #0x0000
    movt    r2, #0x3ce0         /* 0x3ce00000 (UART0 MMIO base) */
    str     r2, [r4, #36]       /* handoff->uart_mmio */

    /* Ensure memory writes have completed to SRAM */
    dsb     sy
    isb     sy

    /* Step 4: Check CPU privilege mode */
    mrs     r2, cpsr
    and     r2, r2, #0x1f
    cmp     r2, #0x13           /* SVC mode (0x13)? */
    beq     .Lenter_sram_direct

    /* User mode (0x10): Escalate via SVC table slot 0 at 0x08917714 */
    movw    r2, #0x7714
    movt    r2, #0x0891         /* r2 = 0x08917714 */
    movw    r3, #0x0000
    movt    r3, #0x2200         /* r3 = 0x22000000 (SRAM trampoline) */
    str     r3, [r2]            /* Hook slot 0 */
    dsb     sy
    isb     sy

    /* Fire SVC #70 (0x46) with r12=0. The kernel dispatcher at 0x08002390
     * loads table slot 0 [0x08917714] and performs `bx r3` to 0x22000000
     * in SVC mode. Arguments in r0 (payload) and r1 (size) pass through intact. */
    mov     r0, r8              /* r0 = payload */
    mov     r1, r9              /* r1 = size */
    mov     r12, #0             /* selector = 0 */
    svc     #70

    /* Dead loop in case SVC returns */
.Ldead_loop:
    wfi
    b       .Ldead_loop

.Lenter_sram_direct:
    /* Already in SVC mode: branch directly to SRAM trampoline */
    mov     r0, r8
    mov     r1, r9
    movw    r3, #0x0000
    movt    r3, #0x2200
    bx      r3

    .size handoff_to_payload, . - handoff_to_payload

/* ============================================================================
 * SRAM Takeover Trampoline
 * Copied to 0x22000000. Pure position-independent ARM code.
 * Does NOT touch stack. Uses r0-r12, sp, lr.
 * ============================================================================
 */
    .align 4
    .global _trampoline_src
    .type _trampoline_src, %function
_trampoline_src:
    /* 1. Mask CPU interrupts */
    cpsid   aif

    /* 2. Preserve arguments in registers (NO stack access!) */
    mov     r8, r0              /* r8 = source payload pointer */
    mov     r9, r1              /* r9 = payload size */
    mov     r10, #0x08000000    /* r10 = destination (DRAM base 0x08000000) */

    /* 3. Clean and Invalidate L1 Data Cache to Point of Coherency (PoC) */
    dsb     sy
    mov     r0, #0              /* Select Level 1 Data Cache */
    mcr     p15, 2, r0, c0, c0, 0   /* CSSELR: Cache Size Selection Register */
    isb     sy
    mrc     p15, 1, r0, c0, c0, 0   /* CCSIDR: Cache Size ID Register */

    /* Extract line length shift: log2(bytes) = (CCSIDR[2:0] + 4) */
    and     r2, r0, #7
    add     r2, r2, #4          /* r2 = line length shift (e.g. 5 for 32B) */

    /* Extract associativity: CCSIDR[12:3] = ways - 1 */
    movw    r3, #0x3ff
    and     r3, r3, r0, lsr #3  /* r3 = ways - 1 (e.g. 3 for 4-way) */
    clz     r4, r3              /* r4 = way shift (32 - log2(ways)) */

    /* Extract number of sets: CCSIDR[27:13] = sets - 1 */
    movw    r5, #0x7fff
    and     r5, r5, r0, lsr #13 /* r5 = sets - 1 (e.g. 127 for 128 sets) */

.Ltramp_way_loop:
    mov     r6, r5              /* r6 = current set index (sets - 1 down to 0) */
.Ltramp_set_loop:
    lsl     r7, r3, r4          /* Way number in high bits [31 : 32-A] */
    lsl     r11, r6, r2         /* Set number in bits [B-1 : L] */
    orr     r7, r7, r11         /* Combine: level 0 is in bits [3:1] = 0 */
    mcr     p15, 0, r7, c7, c14, 2  /* DCCISW: Clean & Invalidate by Set/Way */
    subs    r6, r6, #1
    bge     .Ltramp_set_loop
    subs    r3, r3, #1
    bge     .Ltramp_way_loop

    /* 4. Ensure all cache maintenance has finished */
    dsb     sy

    /* 5. Disable MMU, Caches, Branch Prediction in SCTLR (CP15 c1, c0, 0)
     * Clear bits:
     *   Bit  0: M (MMU enable)
     *   Bit  2: C (Data and unified cache enable)
     *   Bit 11: Z (Branch prediction enable)
     *   Bit 12: I (Instruction cache enable)
     *   Bit 13: V (High vectors select: 0 = normal / VBAR)
     * Mask = 0x0001 | 0x0004 | 0x0800 | 0x1000 | 0x2000 = 0x3805
     */
    mrc     p15, 0, r0, c1, c0, 0
    movw    r1, #0x3805
    bic     r0, r0, r1
    mcr     p15, 0, r0, c1, c0, 0

    /* 6. Invalidate I-cache, Branch Target Cache, and Unified TLB */
    mov     r0, #0
    mcr     p15, 0, r0, c7, c5, 0   /* ICIALLU: Invalidate all instruction caches */
    mcr     p15, 0, r0, c7, c5, 6   /* BPIALL: Invalidate entire branch predictor array */
    mcr     p15, 0, r0, c8, c7, 0   /* TLBIALL: Invalidate entire unified TLB */

    /* 7. Memory and instruction synchronization barriers */
    dsb     sy
    isb     sy

    /* 8. Relocate payload to DRAM physical base 0x08000000 */
    cmp     r8, r10             /* If source == destination, skip copy */
    beq     .Ltramp_copy_done

    mov     r0, r10             /* r0 = dest (0x08000000) */
    mov     r1, r8              /* r1 = src (staging buffer) */
    mov     r2, r9              /* r2 = bytes remaining */

.Ltramp_copy_words:
    cmp     r2, #4
    blo     .Ltramp_copy_bytes
    ldr     r3, [r1], #4
    str     r3, [r0], #4
    sub     r2, r2, #4
    b       .Ltramp_copy_words

.Ltramp_copy_bytes:
    cmp     r2, #0
    beq     .Ltramp_copy_done
    ldrb    r3, [r1], #1
    strb    r3, [r0], #1
    sub     r2, r2, #1
    b       .Ltramp_copy_bytes

.Ltramp_copy_done:
    dsb     sy
    isb     sy

    /* 9. Establish standard boot convention register ABI:
     *   r0 = 0 (ARM Linux boot convention)
     *   r1 = 0x00001097 (Machine architecture ID)
     *   r2 = 0x22009000 (Pointer to struct n7_handoff in SRAM)
     *   r3..r7 = 0
     *   sp = 0x22010000 (Top of private SRAM stack)
     *   lr = 0x22008100 (Warm reset trap handler)
     */
    mov     r0, #0
    movw    r1, #0x1097
    movt    r1, #0x0000         /* r1 = 0x00001097 */
    movw    r2, #0x9000
    movt    r2, #0x2200         /* r2 = 0x22009000 */
    mov     r3, #0
    mov     r4, #0
    mov     r5, #0
    mov     r6, #0
    mov     r7, #0
    movw    sp, #0x0000
    movt    sp, #0x2201         /* sp = 0x22010000 */
    movw    lr, #0x8100
    movt    lr, #0x2200         /* lr = 0x22008100 */

    /* 10. Jump to payload entry point at 0x08000000 */
    mov     r12, #0x08000000
    bx      r12

.Ltramp_hang:
    wfi
    b       .Ltramp_hang
_trampoline_src_end:
    .size _trampoline_src, . - _trampoline_src

/* ============================================================================
 * SRAM Warm-Reset Trap Handler
 * Copied to 0x22008100.
 * If payload returns via `bx lr`, this triggers a clean hardware reset back
 * to stock RetailOS via PMGR CLKCON 0x3c500050.
 * ============================================================================
 */
    .align 4
    .global _reset_stub_src
    .type _reset_stub_src, %function
_reset_stub_src:
    movw    r0, #0x4a00
    movt    r0, #0x0010         /* r0 = 0x00104a00 (watchdog config) */
    movw    r1, #0x0000
    movt    r1, #0x3c80         /* r1 = 0x3c800000 (REG_WATCHDOG) */
    str     r0, [r1]

    movw    r1, #0x0000
    movt    r1, #0x3c50         /* r1 = 0x3c500000 (CLKCON base) */
    mov     r0, #0xa5
    str     r0, [r1, #0x50]     /* str 0xa5 to 0x3c500050 (PMGR reset) */
.Lreset_spin:
    wfi
    b       .Lreset_spin
_reset_stub_src_end:
    .size _reset_stub_src, . - _reset_stub_src

/* ============================================================================
 * Embedded Default Payload (Piece 1 payload.bin)
 * Statically packaged as a fallback when no external payload is found on disk.
 * ============================================================================
 */
    .section .rodata
    .global default_payload_bin
    .global default_payload_bin_end
    .align 4
default_payload_bin:
    .incbin "payload.bin"
default_payload_bin_end:
