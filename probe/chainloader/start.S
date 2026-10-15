/*
 * start.S -- Bare-metal entry point for iPod nano 7G payload
 *
 * Linked to run at physical DRAM 0x08000000.
 * Saves entry handoff registers, initializes DRAM stack,
 * zeros out BSS, and calls payload_main().
 */

    .syntax unified
    .arm
    .section .text.entry, "ax"
    .global _start
    .type _start, %function

_start:
    /* Preserve handoff registers across startup:
     * r0: Boot convention (0)
     * r1: Machine architecture ID (0x00001097)
     * r2: Handoff block pointer (0x22009000)
     * lr: Trap return handler (0x22008100)
     */
    mov     r4, r0
    mov     r5, r1
    mov     r6, r2
    mov     r7, lr

    /* Set up small stack in DRAM */
    ldr     sp, =_stack_top

    /* Zero-fill BSS section */
    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    mov     r2, #0
.Lclear_bss:
    cmp     r0, r1
    strlt   r2, [r0], #4
    blt     .Lclear_bss

    /* Restore handoff arguments for payload_main(r0, r1, r2) */
    mov     r0, r4
    mov     r1, r5
    mov     r2, r6

    /* Call C entry point */
    bl      payload_main

    /* If payload_main returns, return via handoff LR to trigger warm reset */
    mov     lr, r7
    bx      lr

    /* Fallback halt loop */
.Lhalt:
    wfi
    b       .Lhalt

    .size _start, . - _start
