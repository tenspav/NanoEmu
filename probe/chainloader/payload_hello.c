/*
 * payload_hello.c -- Bare-metal hello payload for iPod nano 7G (S5L8747)
 *
 * Implements:
 * 1. Framebuffer configuration & bright green (0x00FF00) solid fill on 240x432 LCD
 * 2. Home button polling via GPIO 0x56 (0x3cf00144 bit 6, active-low)
 * 3. System reset via Watchdog (0x3c800000) and PMGR reset (0x3c500050)
 *
 * Constraints:
 * - No libc / no OS calls / no dynamic memory
 * - Runs in physical DRAM at 0x08000000
 * - Flat memory model, MMU & caches disabled by chainloader
 */

typedef unsigned int   uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char  uint8_t;

/* --- LCD Controller (Base: 0x38900000) --- */
#define LCD_BASE            0x38900000u
#define REG_LCD_ENABLE      (*(volatile uint32_t *)(LCD_BASE + 0x04))
#define REG_LCD_WIN0_CTL    (*(volatile uint32_t *)(LCD_BASE + 0x20))
#define REG_LCD_WIN0_BASE   (*(volatile uint32_t *)(LCD_BASE + 0x24))
#define REG_LCD_WIN0_STRIDE (*(volatile uint32_t *)(LCD_BASE + 0x28))
#define REG_LCD_WIN0_SRC    (*(volatile uint32_t *)(LCD_BASE + 0x2c))
#define REG_LCD_WIN0_SIZE   (*(volatile uint32_t *)(LCD_BASE + 0x30))
#define REG_LCD_WIN0_DST    (*(volatile uint32_t *)(LCD_BASE + 0x34))

#define LCD_WIDTH           240u
#define LCD_HEIGHT          432u
#define LCD_PIXELS          (LCD_WIDTH * LCD_HEIGHT)

/* Full-screen 32-bit ARGB8888 layer: fmt 7 (bits 11:8), alpha 0xff, bit16|bit21 */
#define LCD_WIN0_CTL_ARGB   0x002107ffu
/* Bit 4: Window 0 enable */
#define LCD_ENABLE_WIN0     (1u << 4)

/* Fallback DRAM framebuffer base address (2 MiB into DRAM) */
#define DRAM_FALLBACK_FB    0x08200000u

/* Bright green in 32-bit ARGB8888: Alpha=0xFF, Red=0x00, Green=0xFF, Blue=0x00 */
#define COLOR_BRIGHT_GREEN  0x0000ff00u

/* --- GPIO Controller (Base: 0x3cf00000) --- */
/*
 * GPIO Bank 10 level register (+0x04 = 0x3cf00144).
 * Pin 6 (GPIO 0x56) is the active-low PMU event/interrupt line.
 * When Home button (PMU Reg 3 bit 0) is pressed, the PMU pulls this line LOW (0).
 * Idle state is HIGH (1).
 */
#define GPIO_BANK10_DAT     (*(volatile uint32_t *)0x3cf00144u)
#define GPIO_PMU_IRQ_MASK   (1u << 6)

/* --- System Reset / Watchdog --- */
/* Watchdog timer register */
#define REG_WATCHDOG        (*(volatile uint32_t *)0x3c800000u)
/* PMGR / CLKCON reset control register */
#define REG_PMGR_RESET      (*(volatile uint32_t *)0x3c500050u)

/* RetailOS shutdown / reset magic constants */
#define WATCHDOG_ARM_MAGIC  0x00104a00u
#define SYSTEM_RESET_MAGIC  0x000000a5u

void payload_main(uint32_t r0, uint32_t r1, uint32_t r2)
{
    (void)r0;
    (void)r1;
    (void)r2;

    /* 1. Determine framebuffer physical address.
     * If Window 0 is already configured with a valid DRAM buffer
     * (0x08000000..0x0c000000), reuse it. Otherwise, set fallback to 0x08200000.
     */
    uint32_t fb_phys = REG_LCD_WIN0_BASE & 0x7fffffffu;
    if (fb_phys < 0x08000000u || fb_phys >= (0x0c000000u - (LCD_PIXELS * 4u))) {
        fb_phys = DRAM_FALLBACK_FB;
        REG_LCD_WIN0_BASE = fb_phys;
    }

    /* 2. Configure LCD Window 0 */
    REG_LCD_WIN0_CTL    = LCD_WIN0_CTL_ARGB;
    REG_LCD_WIN0_STRIDE = LCD_WIDTH;
    REG_LCD_WIN0_SRC    = 0;
    REG_LCD_WIN0_SIZE   = (LCD_WIDTH << 16) | LCD_HEIGHT;
    REG_LCD_WIN0_DST    = 0;
    REG_LCD_ENABLE     |= LCD_ENABLE_WIN0;

    /* 3. Fill framebuffer with bright green (0x00FF00) */
    volatile uint32_t *fb = (volatile uint32_t *)fb_phys;
    for (uint32_t i = 0; i < LCD_PIXELS; i++) {
        fb[i] = COLOR_BRIGHT_GREEN;
    }

    /* Ensure framebuffer writes are committed before waiting */
    __asm__ volatile ("dsb" ::: "memory");

    /* 4. Poll Home button:
     * GPIO 0x56 (0x3cf00144 bit 6) is the PMU event line.
     * Idle is HIGH (bit 6 == 1). When pressed, line goes LOW (bit 6 == 0).
     */
    while ((GPIO_BANK10_DAT & GPIO_PMU_IRQ_MASK) != 0) {
        __asm__ volatile ("" ::: "memory");
    }

    /* 5. Trigger System Reset:
     * - Configure watchdog timer at 0x3c800000
     * - Write 0xa5 to PMGR reset register at 0x3c500050
     * - Write 0xa5 to watchdog unlock at 0x3c800000
     */
    REG_WATCHDOG   = WATCHDOG_ARM_MAGIC;
    REG_PMGR_RESET = SYSTEM_RESET_MAGIC;
    REG_WATCHDOG   = SYSTEM_RESET_MAGIC;

    /* Return to _start (which executes `bx lr` back to handoff trap) */
}
