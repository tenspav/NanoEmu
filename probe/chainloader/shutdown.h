/*
 * shutdown.h -- orderly hardware shutdown before the chainloader hands the
 * CPU to a bare-metal payload (spec: docs/tasks/piece2-shutdown.md, plan:
 * docs/research/dualboot-chainloader-plan.md phase 3; every register written
 * is listed in PIECE2.md).
 *
 * hw_shutdown_all() stops, in this order:
 *   a. display   no new frame pushes to the panel (its GRAM keeps the last one)
 *   b. FMC/NAND  let any operation finish, ack, mask its IRQ, deselect the chip
 *   c. DMA       both engines, all 8 channels: drain, then disable
 *   d. timers    all 8 channels stopped, interrupt enables + pending cleared
 *   e. VIC       every line masked, then returns with CPSR I and F set
 *
 * It calls nothing (no OS service, no SDK, no libc); timeouts come from the
 * timer block's free-running 1 MHz counter, which keeps counting after step d.
 * Register writes only: no NAND command, address or data is ever issued.
 *
 * Caller contract:
 *   - Run it privileged (SVC, after the 0x08917714 hook). From user mode the
 *     MMIO writes still land (apps have MMIO access) but CPS is a no-op, so
 *     the return value has HW_SD_NOT_PRIV set and IRQs are still on.
 *   - Before calling, with the OS still alive: close every file and wait for
 *     the FMC to go idle (DumpRAM's nand_wait_idle), so no filesystem
 *     operation is in progress. Step b only covers what is already in flight.
 *   - There is no way back to the OS afterwards except a reset.
 */
#ifndef CHAINLOADER_SHUTDOWN_H
#define CHAINLOADER_SHUTDOWN_H

#include <stdint.h>

/* hw_shutdown_all() return flags: 0 = every step finished cleanly */
#define HW_SD_NOT_PRIV      (1u << 0)   /* not in a privileged mode: CPSID ignored */
#define HW_SD_LCD_BUSY      (1u << 1)   /* panel transport still busy at timeout   */
#define HW_SD_BLIT_BUSY     (1u << 2)   /* 2D blitter still running at timeout     */
#define HW_SD_FMC_PENDING   (1u << 3)   /* FMC never read idle within the settle   */
#define HW_SD_DMA_ABORTED   (1u << 4)   /* >= 1 channel did not drain: aborted     */
#define HW_SD_DMA_STUCK     (1u << 5)   /* >= 1 channel still runs after the abort */
#define HW_SD_NO_CLOCK      (1u << 6)   /* 1 MHz counter frozen: loop-count bounds */

/* What each step saw; filled by hw_shutdown_all() (read it from a debugger,
 * a RAM dump, or the payload, since nothing can be printed afterwards). */
struct hw_shutdown_log {
    uint32_t cpsr_in;           /* CPSR at entry                               */
    uint32_t disp_obj;          /* [0x08ae642c]: OS display object (0 = skip)  */
    uint32_t lcd_busy;          /* 0x3830008c at the end of the wait           */
    uint32_t blit_status;       /* 0x39000000 at the end of the wait           */
    uint32_t fmc_status;        /* 0x38a00048 before the ack                   */
    uint32_t fmss_status;       /* 0x38a00c0c before the ack                   */
    uint32_t fmc_wait_us;       /* how long the FMC took to read idle          */
    uint32_t dma_active[2];     /* per engine: channels running at entry (bit n) */
    uint32_t dma_aborted[2];    /* per engine: channels that had to be aborted */
    uint32_t dma_status[2];     /* per engine: +0x04 completion status at end  */
    uint32_t tmr_running;       /* timer channels that were running (bit n)    */
    uint32_t vic_enable[2];     /* VIC0/VIC1 INTENABLE before masking          */
    uint32_t vic_enable_after[2]; /* ... and after (expected 0)                */
    uint32_t cpsr_out;          /* CPSR on return                              */
    uint32_t flags;             /* the return value                            */
};

extern struct hw_shutdown_log hw_shutdown_last;

uint32_t hw_shutdown_all(void);

#endif /* CHAINLOADER_SHUTDOWN_H */
