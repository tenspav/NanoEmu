/*
 * shutdown.c -- hw_shutdown_all(): stop the RetailOS hardware activity before
 * the chainloader takes the CPU (docs/tasks/piece2-shutdown.md). Order and
 * register choice: docs/research/dualboot-chainloader-plan.md phase 3,
 * checked against the NanoEmu device models (emulator/machine/hw/arm/);
 * where they disagree the model wins (it boots the real OS), and PIECE2.md
 * lists every write with its source and the open questions.
 *
 * Self-contained: no OS call, no SDK call, no libc. Every wait is bounded
 * by the timer block's free-running 1 MHz counter (0x3c700084, untouched by
 * stopping the channels) with a loop-count backstop in case it is frozen.
 * NAND: register ops only -- no command, address or data phase is started.
 */
#include "shutdown.h"

struct hw_shutdown_log hw_shutdown_last;

static inline uint32_t rd32(uint32_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uint32_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void dsb(void) { __asm__ volatile("dsb" ::: "memory"); }
static inline void isb(void) { __asm__ volatile("isb" ::: "memory"); }

static inline uint32_t cpsr_read(void)
{
    uint32_t r;
    __asm__ volatile("mrs %0, cpsr" : "=r"(r));
    return r;
}
/* no-op in user mode (CPS is ignored there, no fault) */
static inline void irq_off(void) { __asm__ volatile("cpsid if" ::: "memory"); }

/* ------------------------------------------------------------ registers --- */
#define DRAM_LO          0x08000000u
#define DRAM_HI          0x0c000000u

/* display (docs/research/lcd.md, direct-display-path.md, nano7_lcd.c,
 * nano7_disp.c, nano7_gpio.c) */
#define OS_DISP_OBJ_PTR  0x08ae642cu   /* RetailOS 1.1.2: display object     */
#define DISP_OBJ_QUIESCE 0x772u        /* byte: TE callback skips commit+push */
#define GPIO_G1_PEND     0x397000a4u   /* GPIO irq group 1 pending, W1C      */
#define GPIO_G1_MASK     0x397000c4u   /* GPIO irq group 1 mask (IRQ 32)     */
#define GPIO_TE_BIT      (1u << 6)     /* GPIO 14 = panel TE                 */
#define PANEL_XFER_BUSY  0x3830008cu   /* bits 1:0 = frame push in progress  */
#define BLIT_BASE        0x39000000u   /* 2D blitter (IRQ 37)                */
#define BLIT_STATUS      (BLIT_BASE + 0x00)  /* bit1 running, bit0 idle      */
#define BLIT_IRQEN       (BLIT_BASE + 0x08)  /* bit0 frame-done enable       */
#define BLIT_IRQST       (BLIT_BASE + 0x0c)  /* bit0 frame done, W1C         */

/* FMC / FMSS (docs/research/nand-fmc.md, nano7_fmc.c; IRQ 54) */
#define FMC_BASE         0x38a00000u
#define FMC_CS_CFG       (FMC_BASE + 0x000)  /* 1 = deselect / bus idle      */
#define FMC_CTRL         (FMC_BASE + 0x004)  /* 0x20 = abort polling/data    */
#define FMC_INT_STATUS   (FMC_BASE + 0x048)  /* phase-done flags, W1C        */
#define FMSS_CS_STATUS   (FMC_BASE + 0xc0c)  /* sequencer done/err, W1C      */
#define FMSS_IRQEN       (FMC_BASE + 0xc10)  /* bit0 sequencer irq enable    */
#define FMC_PEND_MASK    0x0181000eu         /* bits 1,2,3,20,23,24          */
#define FMC_CTRL_ABORT   0x20u

/* DMA (nano7_dma.c; engine 1 IRQ 16, engine 2 IRQ 17) */
#define DMA_NCH          8u
#define DMA_STATUS(b)    ((b) + 0x04)        /* completion, bit n, RO        */
#define DMA_ACK(b)       ((b) + 0x08)        /* W1C of the completion bits   */
#define DMA_NEXT(b, n)   ((b) + 0x108 + 0x20 * (n))  /* 0 = stop after pass  */
#define DMA_CTRL(b, n)   ((b) + 0x110 + 0x20 * (n))  /* bit0 = run           */
static const uint32_t DMA_ENGINES[2] = { 0x38200000u, 0x38700000u };

/* timers (nano7_timer.c; all channels on IRQ 8, OS tick = channel B) */
#define TMR_BASE         0x3c700000u
#define TMR_CON(c)       (TMR_BASE + (c) + 0x00)  /* 14:12 irq en, 18:16 pend */
#define TMR_CMD(c)       (TMR_BASE + (c) + 0x04)  /* bit0 start               */
#define TMR_TSTAT        (TMR_BASE + 0x118)  /* E..H pending, W1C           */
#define TMR_USEC         (TMR_BASE + 0x084)  /* free-running 1 MHz, low word */
#define TMR_CON_IRQEN    0x7000u
#define TMR_CON_PEND     0x70000u            /* W1C on channels A..D        */
static const uint32_t TMR_CHAN[8] = {
    0x000, 0x020, 0x040, 0x060, 0x0a0, 0x0c0, 0x0e0, 0x100,
};

/* VIC (nano7_vic.c: PL192 pair + glue) */
#define VIC0             0x38e00000u
#define VIC1             0x38e01000u
#define VIC_INTENABLE(b) ((b) + 0x010)
#define VIC_INTENCLEAR(b) ((b) + 0x014)
#define VIC_SOFTINTCLEAR(b) ((b) + 0x01c)
#define VIC_ADDRESS(b)   ((b) + 0xf00)
#define VIC_GLUE_CLR0    0x38e02008u         /* bank 0 edge clear (1<<n)    */
#define VIC_GLUE_CLR1    0x38e0200cu         /* bank 1 edge clear           */
#define VIC_EOI_VALUE    0x01010101u         /* what the OS writes per IRQ  */

/* timeouts (us) */
#define LCD_WAIT_US      50000u   /* > 2 frames at 60 Hz                       */
#define BLIT_WAIT_US     50000u
#define FMC_SETTLE_US    20000u   /* > NAND block erase (tBERS ~ 10 ms max)    */
#define DMA_DRAIN_US     50000u   /* > one 4 KiB audio pass (~23 ms at 44.1k)  */

/* --------------------------------------------------------------- waits --- */
typedef struct {
    uint32_t t0, us, spins, max_spins;
    uint32_t *flags;
} deadline_t;

static void deadline_start(deadline_t *d, uint32_t us, uint32_t *flags)
{
    d->t0 = rd32(TMR_USEC);
    d->us = us;
    d->spins = 0;
    d->max_spins = us * 16u;      /* only matters if the counter is frozen */
    d->flags = flags;
}

static uint32_t deadline_elapsed(const deadline_t *d)
{
    return rd32(TMR_USEC) - d->t0;
}

static int deadline_expired(deadline_t *d)
{
    uint32_t el = deadline_elapsed(d);

    if (el >= d->us) {
        return 1;
    }
    if (++d->spins >= d->max_spins) {
        if (el == 0) {
            *d->flags |= HW_SD_NO_CLOCK;
        }
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------- a. display ------ */
/* The panel is command-mode MIPI DSI with its own GRAM: it only changes when
 * the OS pushes a frame from its TE callback (0x0807558c -> commit 0x08080b70
 * -> DSI push 0x080a06c8, which writes 0x38300080 = 1 ... 0). Stopping the
 * pushes leaves the last frame on the glass. The LCD window/enable registers
 * (0x38900004 bits 4/5, +0x00, +0x1b10) are deliberately NOT touched: their
 * blanking behaviour on hardware is unknown, and in NanoEmu clearing a window
 * enable composites black. */
static void stop_display(struct hw_shutdown_log *l, uint32_t *flags)
{
    deadline_t d;
    uint32_t v, obj;

    /* 1. the OS's own quiesce flag, in case a TE IRQ still gets through
     *    (user-mode caller: CPSID did nothing). DRAM, not MMIO. */
    obj = rd32(OS_DISP_OBJ_PTR);
    if (obj >= DRAM_LO && obj < DRAM_HI - DISP_OBJ_QUIESCE) {
        *(volatile uint8_t *)(obj + DISP_OBJ_QUIESCE) = 1;
        l->disp_obj = obj;
    }

    /* 2. TE interrupt off at its GPIO group (0x397000c4 bit 6), drop a
     *    latched edge (0x397000a4 = 0x40, W1C) */
    wr32(GPIO_G1_MASK, rd32(GPIO_G1_MASK) & ~GPIO_TE_BIT);
    wr32(GPIO_G1_PEND, GPIO_TE_BIT);
    dsb();

    /* 3. let a push already on the DSI link finish (0x3830008c bits 1:0) */
    deadline_start(&d, LCD_WAIT_US, flags);
    while ((v = rd32(PANEL_XFER_BUSY)) & 3u) {
        if (deadline_expired(&d)) {
            *flags |= HW_SD_LCD_BUSY;
            break;
        }
    }
    l->lcd_busy = v;

    /* 4. 2D blitter (a bus master writing DRAM): wait out a running blit
     *    (+0x00 bit1), then irq enable 0 (+0x08) and ack done (+0x0c = 1) */
    deadline_start(&d, BLIT_WAIT_US, flags);
    while ((v = rd32(BLIT_STATUS)) & 2u) {
        if (deadline_expired(&d)) {
            *flags |= HW_SD_BLIT_BUSY;
            break;
        }
    }
    l->blit_status = v;
    wr32(BLIT_IRQEN, 0);
    wr32(BLIT_IRQST, 1);
    dsb();
}

/* ------------------------------------------------------- b. FMC / NAND --- */
/* Never abort an in-flight program/erase: give it FMC_SETTLE_US to finish
 * (early exit as soon as the controller reads idle), then only ack, abort the
 * polling engine and deselect. +0xc00 (FMSS kick), +0x08 (command latch) and
 * +0x34 (data DMA) are never written: those start NAND operations. */
static void stop_fmc(struct hw_shutdown_log *l, uint32_t *flags)
{
    deadline_t d;

    /* sequencer interrupt off first so a late completion raises nothing */
    wr32(FMSS_IRQEN, 0);
    dsb();

    deadline_start(&d, FMC_SETTLE_US, flags);
    while ((rd32(FMC_INT_STATUS) & FMC_PEND_MASK) != 0 ||
           rd32(FMSS_CS_STATUS) != 0) {
        if (deadline_expired(&d)) {
            /* latched done flags nobody acked (IRQs off) or a slow chip:
             * either way the settle time has passed, carry on */
            *flags |= HW_SD_FMC_PENDING;
            break;
        }
    }
    l->fmc_wait_us = deadline_elapsed(&d);
    l->fmc_status = rd32(FMC_INT_STATUS);
    l->fmss_status = rd32(FMSS_CS_STATUS);

    wr32(FMC_CTRL, FMC_CTRL_ABORT);     /* stop status polling / PIO   */
    wr32(FMC_CS_CFG, 1);                /* deselect every chip         */
    dsb();
    /* ack last: the abort itself may set phase-done flags */
    wr32(FMC_INT_STATUS, 0xffffffffu);
    wr32(FMSS_CS_STATUS, 0xffffffffu);
    dsb();
}

/* ----------------------------------------------------------- c. DMA ------ */
/* Per channel: NEXT = 0 so a running pass is its last (the audio ring links
 * to itself and would never end), wait for CTRL bit0 to drop, abort what is
 * still running at the timeout, then ack the completion bits. */
static void stop_dma(struct hw_shutdown_log *l, uint32_t *flags)
{
    deadline_t d;
    uint32_t e, n, left = 0;

    for (e = 0; e < 2; e++) {
        uint32_t b = DMA_ENGINES[e];
        for (n = 0; n < DMA_NCH; n++) {
            wr32(DMA_NEXT(b, n), 0);
            (void)rd32(DMA_NEXT(b, n));          /* read back: posted write */
            if (rd32(DMA_CTRL(b, n)) & 1u) {
                l->dma_active[e] |= 1u << n;
            }
        }
        left |= l->dma_active[e] << (8 * e);
    }
    dsb();

    deadline_start(&d, DMA_DRAIN_US, flags);
    while (left) {
        for (e = 0; e < 2; e++) {
            for (n = 0; n < DMA_NCH; n++) {
                uint32_t bit = 1u << (8 * e + n);
                if ((left & bit) && !(rd32(DMA_CTRL(DMA_ENGINES[e], n)) & 1u)) {
                    left &= ~bit;
                }
            }
        }
        if (left && deadline_expired(&d)) {
            break;
        }
    }

    for (e = 0; e < 2; e++) {
        uint32_t b = DMA_ENGINES[e];
        for (n = 0; n < DMA_NCH; n++) {
            uint32_t c = rd32(DMA_CTRL(b, n));
            if (c & 1u) {
                wr32(DMA_CTRL(b, n), c & ~1u);   /* abort: run bit off */
                l->dma_aborted[e] |= 1u << n;
                *flags |= HW_SD_DMA_ABORTED;
                if (rd32(DMA_CTRL(b, n)) & 1u) {
                    *flags |= HW_SD_DMA_STUCK;
                }
            }
        }
        wr32(DMA_ACK(b), 0xffffffffu);
        dsb();
        l->dma_status[e] = rd32(DMA_STATUS(b));
    }
}

/* -------------------------------------------------------- d. timers ------ */
/* All 8 channels, not just the OS tick (channel B, 500 Hz): CMD = 0 stops,
 * CON loses its irq enables (14:12); channels A..D keep their pending bits in
 * CON 18:16 (W1C), E..H in TSTAT. The 1 MHz counter at +0x80/+0x84 is a
 * separate free-running counter and keeps going. */
static void stop_timers(struct hw_shutdown_log *l)
{
    uint32_t n;

    for (n = 0; n < 8; n++) {
        uint32_t c = TMR_CHAN[n];
        uint32_t con = rd32(TMR_CON(c));

        if (rd32(TMR_CMD(c)) & 1u) {
            l->tmr_running |= 1u << n;
        }
        wr32(TMR_CMD(c), 0);
        if (n < 4) {
            wr32(TMR_CON(c), (con & ~(TMR_CON_IRQEN | TMR_CON_PEND)) | TMR_CON_PEND);
        } else {
            wr32(TMR_CON(c), con & ~TMR_CON_IRQEN);
        }
    }
    wr32(TMR_TSTAT, 0x07070707u);
    dsb();
}

/* ----------------------------------------------------------- e. VIC ------ */
/* Sources are already quiet; now mask every line in both banks, drop soft
 * interrupts, EOI any in-service vector, clear the glue edge latches the way
 * the OS init does, and leave the CPU with I and F set. */
static void stop_vic(struct hw_shutdown_log *l)
{
    static const uint32_t bank[2] = { VIC0, VIC1 };
    uint32_t i;

    for (i = 0; i < 2; i++) {
        l->vic_enable[i] = rd32(VIC_INTENABLE(bank[i]));
        wr32(VIC_INTENCLEAR(bank[i]), 0xffffffffu);
        wr32(VIC_SOFTINTCLEAR(bank[i]), 0xffffffffu);
        (void)rd32(VIC_ADDRESS(bank[i]));
        wr32(VIC_ADDRESS(bank[i]), VIC_EOI_VALUE);
    }
    wr32(VIC_GLUE_CLR0, 0xffffffffu);
    wr32(VIC_GLUE_CLR1, 0xffffffffu);
    dsb();
    for (i = 0; i < 2; i++) {
        l->vic_enable_after[i] = rd32(VIC_INTENABLE(bank[i]));
    }
    irq_off();
    isb();
}

/* ---------------------------------------------------------------- entry --- */
uint32_t hw_shutdown_all(void)
{
    struct hw_shutdown_log *l = &hw_shutdown_last;
    uint32_t flags = 0, *p = (uint32_t *)l, i;

    /* no memset: no libc */
    for (i = 0; i < sizeof(*l) / 4; i++) {
        p[i] = 0;
    }
    /* IRQs off for the whole sequence so no OS ISR re-arms what we stop
     * (audio ring, TE push, timer tick); the VIC mask at the end makes it
     * permanent. */
    l->cpsr_in = cpsr_read();
    irq_off();
    if ((l->cpsr_in & 0x1fu) == 0x10u) {
        flags |= HW_SD_NOT_PRIV;
    }

    stop_display(l, &flags);    /* a */
    stop_fmc(l, &flags);        /* b */
    stop_dma(l, &flags);        /* c */
    stop_timers(l);             /* d */
    stop_vic(l);                /* e */

    l->cpsr_out = cpsr_read();
    if ((l->cpsr_out & 0xc0u) != 0xc0u) {
        flags |= HW_SD_NOT_PRIV;
    }
    l->flags = flags;
    return flags;
}
