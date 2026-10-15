/*
 * nano7.c -- QEMU machine model for the Apple iPod nano 7G (2012).
 *
 * SoC: Samsung/Apple S5L8747-class, single Cortex-A5 (ARMv7-A).
 * QEMU 9.1 no longer ships a cortex-a5 TCG CPU model, so cortex-a7
 * (also ARMv7-A) is used as the stand-in; RetailOS runs soft-float
 * and panics on NEON, which the guest simply never executes.
 *
 * Memory map (flat, VA == PA, from reverse engineering):
 *   0x08000000  64 MB   DRAM (RetailOS loads at 0x08000000)
 *   0x20000000  256 KB  VROM (boot ROM)
 *   0x22000000  256 KB  IRAM (live ARM vector table at 0x22000000)
 *   0x10000000..0x3fffffff  catch-all unimplemented-MMIO logger
 *   0x00000000..0x07ffffff  catch-all (low vectors area)
 *
 * Machine properties:
 *   bootrom=<file>    VROM image loaded at 0x20000000 (missing/short file
 *                     is padded with an ARM "b ." infinite-loop stub)
 *   os-image=<file>   image preloaded at DRAM base 0x08000000
 *   entry=<addr>      reset PC (default 0x20000000 = VROM base);
 *                     set to 0x08000000 to skip the bootrom
 *
 * The generic loader also works: -device loader,file=...,addr=...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "exec/address-spaces.h"
#include "hw/boards.h"
#include "hw/core/cpu.h"
#include "hw/loader.h"
#include "sysemu/reset.h"
#include "sysemu/sysemu.h"
#include "qapi/visitor.h"
#include "target/arm/cpu.h"
#include "target/arm/internals.h"
#include "qemu/timer.h"
#include "nano7_unimpl.h"
#include "nano7_regfile.h"
#include "nano7_fmc.h"
#include "nano7_disp.h"
#include "nano7_lcd.h"
#include "nano7_touch.h"
#include "nano7_ui.h"
#include "nano7_dma.h"
#include "nano7_codec.h"
#include "nano7_i2s.h"
#include "nano7_adsp.h"
#include "nano7_mbx.h"
#include "nano7_gpio.h"
#include "nano7_i2c.h"
#include "nano7_pmu.h"
#include "nano7_power.h"
#include "nano7_hostpad.h"
#include "nano7_torn.h"
#include "qemu/cutils.h"
#include "nano7_vic.h"
#include "nano7_timer.h"
#include "nano7_spicmd.h"
#include "nano7_aes.h"
#include "nano7_btuart.h"

#define NANO7_DRAM_BASE  0x08000000
#define NANO7_DRAM_SIZE  (64 * MiB)
#define NANO7_VROM_BASE  0x20000000
#define NANO7_VROM_SIZE  (256 * KiB)
#define NANO7_IRAM_BASE  0x22000000
#define NANO7_IRAM_SIZE  (256 * KiB)
#define NANO7_IRAM_VEC_SIZE 0x1000

#define NANO7_IRQ_TIMER  8
#define NANO7_IRQ_FMSS   54   /* FMSS sequencer done (ISR 0x8161e8c) */

#define NANO7_STUB_INSN  0xeafffffe /* ARM "b ." infinite loop */

#define TYPE_NANO7_MACHINE MACHINE_TYPE_NAME("nano7")
OBJECT_DECLARE_SIMPLE_TYPE(Nano7MachineState, NANO7_MACHINE)

struct Nano7MachineState {
    MachineState parent_obj;

    ARMCPU *cpu;
    MemoryRegion dram;
    MemoryRegion iram;
    MemoryRegion iram_vec;
    MemoryRegion extra_mr;   /* PA 0x0c000000 test region (extra-dram=) */
    uint64_t extra_dram;     /* 0 none, 1 open bus RAZ/WI, 2 own RAM, 3 alias */
    MemoryRegion vrom;

    char *bootrom;
    char *os_image;
    char *iram_image;
    uint64_t entry;
    uint64_t pcsample_ms;
    uint64_t iram_protect;
    uint64_t warm;          /* preset PMGR+0x04 bit 0x40000 (warm boot) */
    char *ctx_file;
    char *os_zero;          /* "addr:addr:..." u32s zeroed after OS load */
    char *os_zero_file;     /* same, read from a file (any separators) */
    char *os_poke_file;     /* "addr value" u32 stores after OS load */
    char *torn_map;         /* DumpRAM v2 torn-map.bin: watch stale reads */
    char *overlay;          /* DumpRAM v4 overlay.bin: T0-atomic DRAM blocks */
    char *torn_ignore_pc;   /* "lo:hi" guest pc range not counted */
    char *nand_store;       /* NAND page store file (default: temp file) */
    char *nand_miss;        /* never-programmed sector reads: blank|oracle */
    char *nand_disk;        /* 4 KB-LBA disk image for oracle misses */
    char *nand_disk_off;    /* FTL LBA of the image's first sector */
    char *nand_rsrc;        /* resource partition image [@LBA] */
    char *nand_patch;       /* replaced disk blocks (tools/disk_patch.py) */
    char *disp_dump;        /* directory for display-pipe frame dumps */
    char *buttons;          /* timed button script "SEC:BTN[:MS]/..." */
    char *rtc;              /* PMU RTC start: snapshot, host or Unix secs */
    char *state_dir;        /* save-state slots (F2 / F4, F1 reset) */
    char *screens_dir;      /* F12 screenshots */
    char *audio_out;        /* PCM sink for I2S0 (raw / .wav / FIFO) */
    char *aes_gid_key;      /* AES engine hardware keys (32 hex digits) */
    char *aes_uid_key;
    char *bt_hci;           /* BT UART: host socket for the raw H4 stream */
    char *bt_peer;          /* fake controller's peer: gamepad | none */
    char *bt_pad;           /* the peer gamepad's input script */
    uint32_t ctx_regs[16];
    bool ctx_loaded;
    bool ctx_has_cpsr;        /* DumpRAM format: word16 CPSR, word17 'DRAM' */
    uint32_t ctx_cpsr;
    uint64_t cpsr_override;   /* UINT64_MAX = leave reset default */
    uint64_t mmu_ttbr0;       /* 0 = off; else TTBR0 + enable MMU at reset */
    uint64_t resume_hw;       /* ctx resume: replay OS VIC/timer init */
    uint64_t usec_base;       /* start value of the 1 MHz 64-bit counter */
    uint64_t fmss_resume;     /* ctx resume: finish the in-flight FMSS seq */
    QEMUTimer *pc_timer;
    qemu_irq *vic_in;
};

/*
 * A RAM/CPU snapshot carries no peripheral state: on resume the VIC and
 * timer models are still at their reset values, so no IRQ ever fires and
 * every thread sleeps forever. Replay what RetailOS itself programmed
 * at boot (VIC init 0x8006e94 + per-IRQ enables via 0x8001514, scheduler
 * tick setup 0x8000ace..0x8000c6e), through the normal MMIO path.
 */
static void nano7_resume_hw(void)
{
    AddressSpace *as = &address_space_memory;
    int i;

    for (i = 0; i < 64; i++) {
        address_space_stl_le(as, 0x38e00100 + (i / 32) * 0x1000 +
                             (i % 32) * 4, i, MEMTXATTRS_UNSPECIFIED, NULL);
    }
    /* bank0: 2,3,7,8,9,10,11,16,17,21,22,26,29,31; bank1: 32,33,37,52,53,54
     * (54 = FMSS/NAND: handler installed at boot, enabled once NAND runs,
     * which it was on the live device; 37 = display pipe frame done,
     * registered by its init 0x081bbd98, display on at capture; 53 = MBX
     * GPU, enabled by its driver init before T0 - the GL flush waits on
     * it, Session 31) */
    address_space_stl_le(as, 0x38e00010, 0xa4630f8c,
                         MEMTXATTRS_UNSPECIFIED, NULL);
    address_space_stl_le(as, 0x38e01010, 0x00700023,
                         MEMTXATTRS_UNSPECIFIED, NULL);
    /* display pipe irq enable (power-on 0x081bb6f4 set it before T0) */
    address_space_stl_le(as, 0x39000008, 1, MEMTXATTRS_UNSPECIFIED, NULL);
    /* GPIO 0x56 (PMU event line, group 3 bit 14) armed by the button
     * thread at boot (0x083f6f3c(0x56, 1)): enable-style + mask bits */
    address_space_stl_le(as, 0x397000ec, 1u << 14, MEMTXATTRS_UNSPECIFIED,
                         NULL);
    address_space_stl_le(as, 0x397000cc, 1u << 14, MEMTXATTRS_UNSPECIFIED,
                         NULL);
    /* group 1 -> IRQ 32: GPIO 14 (panel TE, bit 6) armed by the display
     * driver (commit callback 0x08060b19), GPIO 0x26 (touch report ready,
     * bit 30) armed by MultitouchTask (0x083f6f3c(0x26, 1)) */
    address_space_stl_le(as, 0x397000c4, 1u << 6 | 1u << 30,
                         MEMTXATTRS_UNSPECIFIED, NULL);
    address_space_stl_le(as, 0x397000e4, 1u << 30, MEMTXATTRS_UNSPECIFIED,
                         NULL);
    /* group 2 -> IRQ 31: volume up/down (GPIO 0x28/0x29, bits 0/1) armed by
     * GPIOButtonManagerTask; its ISR flips +0x88 polarity per edge */
    address_space_stl_le(as, 0x397000c8, 3, MEMTXATTRS_UNSPECIFIED, NULL);
    address_space_stl_le(as, 0x39700088, 3, MEMTXATTRS_UNSPECIFIED, NULL);
    /* LCD window 0 geometry: programmed once at display init, before T0; a
     * commit (0x08080b70) only rewrites enable, +0x10 and the base. Full-
     * screen layer: ctl 0x002107ff (ARGB8888, alpha 0xff), stride 240 px,
     * source 0,0, size 240x432, panel position 0,0 (Session 18 RE). */
    address_space_stl_le(as, 0x38900020, 0x002107ff, MEMTXATTRS_UNSPECIFIED,
                         NULL);
    address_space_stl_le(as, 0x38900028, 240, MEMTXATTRS_UNSPECIFIED, NULL);
    address_space_stl_le(as, 0x3890002c, 0, MEMTXATTRS_UNSPECIFIED, NULL);
    address_space_stl_le(as, 0x38900030, 240u << 16 | 432,
                         MEMTXATTRS_UNSPECIFIED, NULL);
    address_space_stl_le(as, 0x38900034, 0, MEMTXATTRS_UNSPECIFIED, NULL);
    /* audio output: at T0 MeCCAOutputTask had DMA engine 1 channel 2
     * running a self-linked descriptor (0x0b3539c0: 4 KB passes from the
     * IRAM ring 0x22002638 to the I2S0 TX FIFO) and waits for its
     * completion; it never restarts the channel itself (Session 27).
     * Reload the channel from that descriptor and run it. */
    {
        uint32_t d[5], i;

        for (i = 0; i < 5; i++) {
            d[i] = address_space_ldl_le(as, 0x0b3539c0 + 4 * i,
                                        MEMTXATTRS_UNSPECIFIED, NULL);
        }
        if (d[1] == 0x3ca00010) {
            static const hwaddr reg[5] = { 0x00, 0x04, 0x08, 0x0c, 0x14 };

            for (i = 0; i < 5; i++) {
                address_space_stl_le(as, 0x38200140 + reg[i], d[i],
                                     MEMTXATTRS_UNSPECIFIED, NULL);
            }
            /* mem -> periph, destination request 0xc (I2S0 TX), run */
            address_space_stl_le(as, 0x38200150, 0x8b01,
                                 MEMTXATTRS_UNSPECIFIED, NULL);
        }
    }
    /* timer ch1 (B): 3 MHz / 6000 = 500 Hz scheduler tick on IRQ 8 */
    address_space_stl_le(as, 0x3c700030, 0, MEMTXATTRS_UNSPECIFIED, NULL);
    address_space_stl_le(as, 0x3c700028, 6000, MEMTXATTRS_UNSPECIFIED, NULL);
    address_space_stl_le(as, 0x3c700020, 0x1140, MEMTXATTRS_UNSPECIFIED, NULL);
    address_space_stl_le(as, 0x3c700024, 2, MEMTXATTRS_UNSPECIFIED, NULL);
    address_space_stl_le(as, 0x3c700024, 1, MEMTXATTRS_UNSPECIFIED, NULL);
    /* 0x3de00000 engine init (0x800ad94): enable, config, unmask */
    address_space_stl_le(as, 0x3de00004, 1, MEMTXATTRS_UNSPECIFIED, NULL);
    address_space_stl_le(as, 0x3de00000, 0x1703, MEMTXATTRS_UNSPECIFIED, NULL);
    address_space_stl_le(as, 0x3de00010, 0, MEMTXATTRS_UNSPECIFIED, NULL);
    info_report("nano7: resume-hw: VIC enables + 500 Hz tick restored");
}

static void nano7_cpu_reset(void *opaque)
{
    Nano7MachineState *s = opaque;
    CPUState *cs = CPU(s->cpu);
    CPUARMState *env = &s->cpu->env;

    cpu_reset(cs);
    /* The hardware bootrom keeps the live vector table in IRAM. */
    env->cp15.vbar_ns = NANO7_IRAM_BASE;
    env->cp15.vbar_s = NANO7_IRAM_BASE;
    /* Exception-mode stacks in IRAM, as the bootrom would set up;
     * otherwise the first exception handler pushes to address ~0. */
    env->banked_r13[BANK_SVC] = 0x2202c000;
    env->banked_r13[BANK_ABT] = 0x2202d000;
    env->banked_r13[BANK_UND] = 0x2202e000;
    env->banked_r13[BANK_IRQ] = 0x2202f000;
    env->banked_r13[BANK_FIQ] = 0x22030000;
    if (s->cpsr_override != UINT64_MAX) {
        cpsr_write(env, s->cpsr_override, ~0u, CPSRWriteByInstr);
    }
    if (s->mmu_ttbr0) {
        /* Resume with the MMU already live, as on the real device:
         * flat VA=PA via the snapshot's L1 table, all domains manager. */
        env->cp15.ttbr0_ns = s->mmu_ttbr0;
        env->cp15.ttbr0_s = s->mmu_ttbr0;
        env->cp15.tcr_el[1] = 0;   /* TTBCR.N=0: TTBR0 covers everything */
        env->cp15.dacr_ns = 0xffffffffu;
        env->cp15.dacr_s = 0xffffffffu;
        env->cp15.sctlr_ns = (uint32_t)env->cp15.sctlr_ns |
                             SCTLR_M | SCTLR_C | (1U << 12);
        env->cp15.sctlr_s = env->cp15.sctlr_ns;
    }
    if (s->ctx_loaded) {
        /* Live-state resume: r0..r15 from the context snapshot;
         * the snapshot PC overrides the entry property.
         * Also recreate the CPU environment the bootrom/OS would have
         * set up before this thread ran: exception-mode stacks in IRAM
         * and VFP enabled (the OS undef handler uses vmrs). */
        int i;
        if (s->ctx_has_cpsr && s->cpsr_override == UINT64_MAX) {
            /* The snapshot carries its own CPSR (mode + I/F flags).
             * Its T bit is meaningless: MRS reads the execution-state
             * bits as zero, so Thumb comes from pc bit0 below. */
            cpsr_write(env, s->ctx_cpsr & ~CPSR_T, ~0u, CPSRWriteByInstr);
        }
        for (i = 0; i < 15; i++) {
            env->regs[i] = s->ctx_regs[i];
        }
        env->banked_r13[BANK_SVC] = 0x2202c000;
        env->banked_r13[BANK_ABT] = 0x2202d000;
        env->banked_r13[BANK_UND] = 0x2202e000;
        env->banked_r13[BANK_IRQ] = 0x2202f000;
        env->banked_r13[BANK_FIQ] = 0x22030000;
        env->cp15.cpacr_el1 = 0x00f00000;   /* full cp10/cp11 access */
        env->cp15.nsacr = 0x00000c00;
        env->vfp.xregs[ARM_VFP_FPEXC] = 0x40000000; /* FPEXC.EN */
        /* The snapshot PC (r15) is stored word-aligned even for Thumb
         * threads, so cpu_set_pc() would force env->thumb=0 and the CPU
         * would decode Thumb bytes as ARM (first instruction becomes a
         * garbage LDM -> immediate data abort, DFAR 0xffffffff). Set PC
         * directly and keep the Thumb state: honor the T bit from the
         * --cpsr override if given, otherwise infer it from the saved
         * return address (lr bit0) which is preserved for Thumb. */
        env->regs[15] = s->ctx_regs[15] & ~1u;
        /* The snapshot is ground truth: r14/r15 bit0 mark a Thumb thread.
         * An explicit T bit in --cpsr still forces Thumb, but a T=0
         * override never wins over a Thumb snapshot (the old default
         * 0x..1f otherwise re-created the ARM-decode data abort). */
        /* DumpRAM-format ctx: pc bit0 alone marks Thumb (lr is often a
         * stale return address from another function and proves nothing). */
        env->thumb = (s->ctx_regs[15] & 1) |
                     (s->ctx_has_cpsr ? 0 : (s->ctx_regs[14] & 1)) |
                     ((s->cpsr_override != UINT64_MAX) ?
                          (s->cpsr_override >> 5) & 1 : 0);
        if (s->resume_hw) {
            nano7_resume_hw();
        }
        if (s->fmss_resume == 1) {
            nano7_fmc_fmss_resume_done();
            info_report("nano7: fmss-resume: in-flight FMSS sequence "
                        "reported done");
        } else if (s->fmss_resume == 2) {
            nano7_fmc_fmss_resume_kick();
            info_report("nano7: fmss-resume=2: next base-less FMSS kick "
                        "reported complete");
        }
    } else {
        cpu_set_pc(cs, s->entry);
    }
    arm_rebuild_hflags(env);

    info_report("nano7: reset, PC=0x%08" PRIx64, s->entry);
}

static void nano7_pcsample_tick(void *opaque)
{
    Nano7MachineState *s = opaque;
    CPUARMState *env = &s->cpu->env;
    const uint8_t *ir = memory_region_get_ram_ptr(
        s->iram_protect == 2 ? &s->iram_vec : &s->iram);
    int i;

    fprintf(stderr, "[nano7-pc] pc=0x%08x cpsr=0x%08x sp=0x%08x lr=0x%08x\n"
                    "[nano7-iram] ", (uint32_t)env->regs[15],
            (uint32_t)env->uncached_cpsr, (uint32_t)env->regs[13],
            (uint32_t)env->regs[14]);
    if (ir) {
        for (i = 0; i < 0x40; i += 4) {
            uint32_t w;
            memcpy(&w, ir + (s->iram_protect ? NANO7_IRAM_VEC_SIZE : 0) + i, 4);
            fprintf(stderr, "%08x ", w);
        }
    }
    {
        /* VIC driver struct watch: [0x08918220] -> {0x38e00010, ...} */
        uint32_t p = 0, w0 = 0, w1 = 0;
        address_space_read(&address_space_memory, 0x08918220,
                           MEMTXATTRS_UNSPECIFIED, &p, 4);
        if (p) {
            address_space_read(&address_space_memory, p,
                               MEMTXATTRS_UNSPECIFIED, &w0, 4);
            address_space_read(&address_space_memory, p + 4,
                               MEMTXATTRS_UNSPECIFIED, &w1, 4);
        }
        fprintf(stderr, " [vic-struct ptr=%08x w0=%08x w1=%08x]", p, w0, w1);
    }
    {
        /* bootrom state struct watch (ptr at 0x2202fff8) */
        uint32_t sptr = 0, s28 = 0, s29 = 0, sa0 = 0, s24 = 0;
        address_space_read(&address_space_memory, 0x2202fff8,
                           MEMTXATTRS_UNSPECIFIED, &sptr, 4);
        if (sptr) {
            address_space_read(&address_space_memory, sptr + 0x28,
                               MEMTXATTRS_UNSPECIFIED, &s28, 4);
            address_space_read(&address_space_memory, sptr + 0x29,
                               MEMTXATTRS_UNSPECIFIED, &s29, 1);
            address_space_read(&address_space_memory, sptr + 0xa0,
                               MEMTXATTRS_UNSPECIFIED, &sa0, 4);
            address_space_read(&address_space_memory, sptr + 0x24,
                               MEMTXATTRS_UNSPECIFIED, &s24, 4);
        }
        fprintf(stderr, " [brstate st=%p f28=%02x f29=%02x a0=%08x vt=%08x]",
                sptr, s28 & 0xff, s29 & 0xff, sa0, s24);
    }
    {
        /* IRAM trampoline area watch (0x220011c0..0x22001200) */
        int j;
        fprintf(stderr, " [iram@11c0:");
        for (j = 0; j < 0x40; j += 4) {
            uint32_t w = 0;
            address_space_read(&address_space_memory, 0x220011c0 + j,
                               MEMTXATTRS_UNSPECIFIED, &w, 4);
            fprintf(stderr, " %08x", w);
        }
        fprintf(stderr, "]");
    }
    fprintf(stderr, "\n[nano7-stack] ");
    {
        /* 24 words above SP: return addresses give a manual backtrace */
        uint32_t sp = env->regs[13];
        for (i = 0; i < 24; i++) {
            uint32_t w = 0;
            address_space_read(&address_space_memory, sp + 4 * i,
                               MEMTXATTRS_UNSPECIFIED, &w, 4);
            fprintf(stderr, "%08x ", w);
        }
    }
    fprintf(stderr, "\n");
    fflush(stderr);
    timer_mod(s->pc_timer,
              qemu_clock_get_ms(QEMU_CLOCK_HOST) + s->pcsample_ms);
}

static void nano7_load_vrom(Nano7MachineState *s)
{
    uint8_t *buf = g_malloc(NANO7_VROM_SIZE);
    int i;

    /* Default: whole VROM is an infinite-loop stub (smoke-test friendly). */
    for (i = 0; i < NANO7_VROM_SIZE; i += 4) {
        stl_le_p(buf + i, NANO7_STUB_INSN);
    }

    if (s->bootrom) {
        gchar *data = NULL;
        gsize len = 0;

        if (g_file_get_contents(s->bootrom, &data, &len, NULL)) {
            memcpy(buf, data, MIN(len, NANO7_VROM_SIZE));
            if (len < NANO7_VROM_SIZE) {
                warn_report("nano7: bootrom '%s' is only %" G_GSIZE_FORMAT
                            " bytes (< %d KB), tail filled with loop stub",
                            s->bootrom, len, (int)(NANO7_VROM_SIZE / KiB));
            }
            g_free(data);
        } else {
            warn_report("nano7: cannot read bootrom '%s', "
                        "using loop stub", s->bootrom);
        }
    }

    memory_region_init_ram_ptr(&s->vrom, NULL, "nano7.vrom",
                               NANO7_VROM_SIZE, buf);
    memory_region_set_readonly(&s->vrom, true);
    memory_region_add_subregion(get_system_memory(), NANO7_VROM_BASE,
                                &s->vrom);
}

static uint64_t nano7_openbus_read(void *o, hwaddr a, unsigned sz) { return 0; }
static void nano7_openbus_write(void *o, hwaddr a, uint64_t v, unsigned sz) { }
static const MemoryRegionOps nano7_openbus_ops = {
    .read = nano7_openbus_read, .write = nano7_openbus_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void nano7_init(MachineState *machine)
{
    Nano7MachineState *s = NANO7_MACHINE(machine);

    /* Catch-all MMIO loggers at low priority, below any real region. */
    nano7_unimpl_attach(get_system_memory(), 0x00000000, 0x08000000, -1000);
    nano7_unimpl_attach(get_system_memory(), 0x10000000, 0x30000000, -1000);

    /* Known blocks as generic register files (write-store / read-back,
     * never-written reads as all-ones "ready"): */
    nano7_regfile_attach(get_system_memory(), "clkcon", 0x3c500000, 0x1000);
    /* stock clocks instead of the all-ones fill (the OS read-modify-writes
     * these, and nano7_power.c decodes them): CPU = PLL1 684 / 2, the value
     * read on the device (dumps/report.txt); GPU (bits 31:16, domain 20) =
     * gates on, PLL2 266 / 2 = 133 MHz (docs/research/gpu-clock.md; the
     * DRAM half 15:0 keeps the fill) */
    nano7_regfile_poke("clkcon", 0x00, 0x40009001, 0xf);
    nano7_regfile_poke("clkcon", 0x1c, 0xe0010000, 0xc);
    {
        Nano7RegfileOpts o = { .fill = 0x00,
                               .preset_off = s->warm ? 1 : -1,
                               .preset_val = (uint32_t)s->warm,
                               .preset2_off = 0, .preset2_val = 1,
                               .preset3_off = 2, .preset3_val = 0x87400000 };
        nano7_regfile_attach2(get_system_memory(), "pmgr", 0x3d100000,
                              0x1000, &o);
    }
    /* blocks touched by early RetailOS init (generous windows) */
    nano7_regfile_attach(get_system_memory(), "b3e0",  0x3e000000, 0x1000);
    nano7_regfile_attach(get_system_memory(), "b3c8",  0x3c800000, 0x1000);
    /* 0x39700000: power-gate controller (Session 13 RE: 0x08426d18 off,
     * 0x08426f3c/0x08005856 on); polls wait for bits to clear, so
     * never-written reads return 0 here (all domains on). */
    {
        Nano7RegfileOpts o = { .fill = 0x00, .pgate = true,
                               .preset2_off = -1, .preset3_off = -1 };
        nano7_regfile_attach2(get_system_memory(), "vic", 0x39700000,
                              0x1000, &o);
    }
    nano7_regfile_attach(get_system_memory(), "b3cf",  0x3cf00000, 0x1000);
    /* 0x38a00000: FMC (NAND flash controller): stateful PPN model with
     * two PPN 1.0 CEs, see nano7_fmc.c. */
    nano7_regfile_attach(get_system_memory(), "b3c4",  0x3c400000, 0x1000);
    /* 0x38400000: +0x10 is a start/busy register (write 1 -> wait bit0
     * clear + bit31 set). Report "done". */
    {
        Nano7RegfileOpts o = { .fill = 0xff,
                               .preset2_off = -1, .preset3_off = -1,
                               .done_mask = 1ULL << (0x10 / 4) };
        nano7_regfile_attach2(get_system_memory(), "b384", 0x38400000,
                              0x1000, &o);
    }
    memory_region_init_ram(&s->dram, NULL, "nano7.dram",
                           NANO7_DRAM_SIZE, &error_fatal);
    memory_region_add_subregion(get_system_memory(), NANO7_DRAM_BASE,
                                &s->dram);

    switch (s->extra_dram) {
    case 1:
        memory_region_init_io(&s->extra_mr, NULL, &nano7_openbus_ops, NULL,
                              "nano7.extra.openbus", NANO7_DRAM_SIZE);
        break;
    case 2:
        memory_region_init_ram(&s->extra_mr, NULL, "nano7.extra.ram",
                               NANO7_DRAM_SIZE, &error_fatal);
        break;
    case 3:
        memory_region_init_alias(&s->extra_mr, NULL, "nano7.extra.alias",
                                 &s->dram, 0, NANO7_DRAM_SIZE);
        break;
    default:
        break;
    }
    if (s->extra_dram >= 1 && s->extra_dram <= 3) {
        memory_region_add_subregion(get_system_memory(),
                                    NANO7_DRAM_BASE + NANO7_DRAM_SIZE,
                                    &s->extra_mr);
    }

    if (s->iram_protect == 2) {
        /* Whole IRAM read-only: keep the dumped trampoline code intact;
         * the OS's IRAM scratch zeroing is dropped. */
        memory_region_init_ram(&s->iram_vec, NULL, "nano7.iram.vec",
                               NANO7_IRAM_SIZE, &error_fatal);
        memory_region_set_readonly(&s->iram_vec, true);
        memory_region_add_subregion(get_system_memory(), NANO7_IRAM_BASE,
                                    &s->iram_vec);
    } else if (s->iram_protect) {
        /* Keep the vector page pristine: read-only, guest writes dropped
         * (and logged with -d guest_errors). The rest of IRAM stays RAM. */
        memory_region_init_ram(&s->iram_vec, NULL, "nano7.iram.vec",
                               NANO7_IRAM_VEC_SIZE, &error_fatal);
        memory_region_set_readonly(&s->iram_vec, true);
        memory_region_add_subregion(get_system_memory(), NANO7_IRAM_BASE,
                                    &s->iram_vec);
        memory_region_init_ram(&s->iram, NULL, "nano7.iram",
                               NANO7_IRAM_SIZE - NANO7_IRAM_VEC_SIZE,
                               &error_fatal);
        memory_region_add_subregion(get_system_memory(),
                                    NANO7_IRAM_BASE + NANO7_IRAM_VEC_SIZE,
                                    &s->iram);
    } else {
        memory_region_init_ram(&s->iram, NULL, "nano7.iram",
                               NANO7_IRAM_SIZE, &error_fatal);
        memory_region_add_subregion(get_system_memory(), NANO7_IRAM_BASE,
                                    &s->iram);
    }

    nano7_load_vrom(s);

    if (s->iram_image) {
        gchar *data = NULL;
        gsize len = 0;

        if (!g_file_get_contents(s->iram_image, &data, &len, NULL)) {
            error_report("nano7: cannot load iram-image '%s'", s->iram_image);
            exit(1);
        }
        if (s->iram_protect == 2) {
            memcpy(memory_region_get_ram_ptr(&s->iram_vec), data,
                   MIN(len, NANO7_IRAM_SIZE));
        } else if (s->iram_protect) {
            uint8_t *vec_ptr = memory_region_get_ram_ptr(&s->iram_vec);
            uint8_t *rest_ptr = memory_region_get_ram_ptr(&s->iram);
            memcpy(vec_ptr, data, MIN(len, NANO7_IRAM_VEC_SIZE));
            if (len > NANO7_IRAM_VEC_SIZE) {
                memcpy(rest_ptr, data + NANO7_IRAM_VEC_SIZE,
                       MIN(len - NANO7_IRAM_VEC_SIZE,
                           NANO7_IRAM_SIZE - NANO7_IRAM_VEC_SIZE));
            }
        } else {
            memcpy(memory_region_get_ram_ptr(&s->iram), data,
                   MIN(len, NANO7_IRAM_SIZE));
        }
        info_report("nano7: preloaded '%s' (%" G_GSIZE_FORMAT
                    " bytes) at 0x%08x", s->iram_image, len, NANO7_IRAM_BASE);
        g_free(data);
    }

    s->cpu = ARM_CPU(cpu_create(machine->cpu_type));

    /* front-end keys + save states: the RAM regions are saved raw, the
     * devices below register their own vmstate sections */
    nano7_ui_init(s->state_dir, s->screens_dir);
    nano7_ui_add_ram(&s->dram, "dram");
    if (memory_region_size(&s->iram_vec)) {
        nano7_ui_add_ram(&s->iram_vec, "iram-vec");
    }
    if (memory_region_size(&s->iram)) {
        nano7_ui_add_ram(&s->iram, "iram");
    }
    if (memory_region_size(&s->vrom)) {
        nano7_ui_add_ram(&s->vrom, "vrom");
    }

    /* 0x38e00000: dual PL192 VIC (IRQ 0..63) -> CPU IRQ/FIQ.
     * 0x3c700000: timer block (scheduler tick = channel B) on IRQ 8;
     * +0x84/+0xb4 stay free-running counters for busy-wait delays. */
    s->vic_in = nano7_vic_attach(get_system_memory(), 0x38e00000,
                                 qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_IRQ),
                                 qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_FIQ));
    nano7_timer_attach(get_system_memory(), 0x3c700000,
                       s->vic_in[NANO7_IRQ_TIMER], s->usec_base);
    /* 0x3de00000: serial register-command engine, completion on IRQ 52 */
    nano7_spicmd_attach(get_system_memory(), 0x3de00000,
                        s->vic_in[NANO7_IRQ_SPICMD]);
    /* 0x38a00000: FMC (NAND flash controller): stateful PPN model with
     * two PPN 1.0 CEs + FMSS command sequencer (IRQ 54), see nano7_fmc.c */
    nano7_fmc_attach(get_system_memory(), 0x38a00000,
                     s->vic_in[NANO7_IRQ_FMSS]);
    nano7_fmc_nand_config(s->nand_store, s->nand_miss, s->nand_disk,
                          s->nand_disk_off, s->nand_rsrc, s->nand_patch);
    /* 0x39000000: display pipe = 2D blitter, done = IRQ 37 */
    nano7_disp_attach(get_system_memory(), 0x39000000,
                      s->vic_in[NANO7_IRQ_DISP]);
    /* GPIO interrupt groups (0x39700080..) + modeled input levels */
    nano7_gpio_attach(get_system_memory(), s->vic_in);
    /* 0x38900000: LCD controller (2 scan-out windows) + panel TE on GPIO 14;
     * disp-dump writes the scanned-out screen */
    nano7_lcd_attach(get_system_memory(), 0x38900000, s->disp_dump);
    /* 0x3d200000: serial port 2 = multitouch controller (IRQ 11, ready
     * line GPIO 0x26), fed by the host mouse */
    nano7_touch_attach(get_system_memory(), 0x3d200000,
                       s->vic_in[NANO7_IRQ_TOUCH_SPI]);
    /* audio: DMA engines (engine 1 0x38200000 = IRQ 16, engine 2
     * 0x38700000 = IRQ 17), codec on SPI0 (IRQ 9), I2S0 TX -> PCM sink */
    nano7_dma_attach(get_system_memory(), 0x38200000, 1,
                     s->vic_in[NANO7_IRQ_DMA1]);
    nano7_dma_attach(get_system_memory(), 0x38700000, 2,
                     s->vic_in[NANO7_IRQ_DMA2]);
    nano7_codec_attach(get_system_memory(), 0x3c300000,
                       s->vic_in[NANO7_IRQ_SPI0]);
    nano7_i2s_attach(get_system_memory(), 0x3ca00000, s->audio_out);
    /* 0x38c00000: AES engine (polled; hardware keys unknown unless given) */
    nano7_aes_attach(get_system_memory(), 0x38c00000, s->aes_gid_key,
                     s->aes_uid_key);
    /* 0x3db00000: UART port 1 = Bluetooth HCI H4 (IRQ 25, TX DMA request
     * 4), with a fake Broadcom controller or a host socket behind it */
    nano7_btuart_attach(get_system_memory(), 0x3db00000,
                        s->vic_in[NANO7_IRQ_BTUART], s->bt_hci, s->bt_peer,
                        s->bt_pad);
    nano7_hostpad_init();       /* NANO7_HOST_PAD (run.sh --host-pad) */
    /* 0x38500000: media decode co-processor (IRQ 18, ID 0x004e7309, job
     * completion), see nano7_adsp.c */
    nano7_adsp_attach(get_system_memory(), 0x38500000,
                      s->vic_in[NANO7_IRQ_ADSP]);
    /* PowerVR MBX GPU (0x3b000000 core, FIFO apertures, planes 0x391-3,
     * IRQ 53): job completion only, nothing is rendered (nano7_mbx.c) */
    nano7_mbx_attach(get_system_memory(), s->vic_in[NANO7_IRQ_MBX]);
    /* I2C buses: bus 0 slave 0x08; bus 1 PMU 0x73 (buttons) + 0x1a, 0x18 */
    {
        Nano7I2CBus *i2c0 = nano7_i2c_attach(get_system_memory(), 0x3c600000,
                                             s->vic_in[NANO7_IRQ_I2C0], "i2c0");
        Nano7I2CBus *i2c1 = nano7_i2c_attach(get_system_memory(), 0x3c900000,
                                             s->vic_in[NANO7_IRQ_I2C1], "i2c1");

        nano7_i2c_add_regfile_slave(i2c0, 0x08);
        nano7_i2c_add_regfile_slave(i2c1, 0x1a);
        nano7_i2c_add_regfile_slave(i2c1, 0x18);
        nano7_pmu_attach(i2c1, s->buttons, s->rtc);
    }
    nano7_power_init();         /* after CLKCON and the PMU */

    if (s->ctx_file) {
        gchar *data = NULL;
        gsize len = 0;
        int i;

        if (!g_file_get_contents(s->ctx_file, &data, &len, NULL) ||
            len < 64) {
            error_report("nano7: cannot read ctx '%s'", s->ctx_file);
            exit(1);
        }
        for (i = 0; i < 16; i++) {
            s->ctx_regs[i] = ldl_le_p((uint8_t *)data + 4 * i);
        }
        s->ctx_loaded = true;
        /* New DumpRAM layout: [16] = CPSR, [17] = 'DRAM' marker.
         * (The old layout had two IRAM pointers there, ignored.) */
        if (len >= 72 && ldl_le_p((uint8_t *)data + 68) == 0x4d415244u) {
            s->ctx_has_cpsr = true;
            s->ctx_cpsr = ldl_le_p((uint8_t *)data + 64);
        }
        info_report("nano7: ctx '%s': r0=0x%08x sp=0x%08x lr=0x%08x "
                    "pc=0x%08x%s", s->ctx_file, s->ctx_regs[0],
                    s->ctx_regs[13], s->ctx_regs[14], s->ctx_regs[15],
                    s->ctx_has_cpsr ? " (DRAM fmt, cpsr from ctx)" : "");
        g_free(data);
    }

    if (s->os_image) {
        /* Copied straight into DRAM (not a reset-time ROM blob) so the
         * os-zero fixups below are not overwritten at reset. */
        gchar *data = NULL;
        gsize len = 0;

        if (!g_file_get_contents(s->os_image, &data, &len, NULL) ||
            len > NANO7_DRAM_SIZE) {
            error_report("nano7: cannot load os-image '%s'", s->os_image);
            exit(1);
        }
        memcpy(memory_region_get_ram_ptr(&s->dram), data, len);
        g_free(data);
        info_report("nano7: preloaded '%s' (%" G_GSIZE_FORMAT
                    " bytes) at 0x%08x", s->os_image, len, NANO7_DRAM_BASE);
    }
    if (s->overlay) {
        /* DumpRAM v4: blocks copied with IRQs off at T0, i.e. consistent
         * with the ctx. They replace the (streamed, torn) ram-full data and
         * are not watched by the torn-map. Applied before the pokes. */
        gchar *data = NULL;
        gsize len = 0;
        uint8_t *dram = memory_region_get_ram_ptr(&s->dram);
        uint32_t bs, n;

        if (!g_file_get_contents(s->overlay, &data, &len, NULL) || len < 32 ||
            ldl_le_p(data) != 0x594c564fu ||
            (ldl_le_p(data + 4) != 1 && ldl_le_p(data + 4) != 2)) {
            error_report("nano7: bad overlay '%s'", s->overlay);
            exit(1);
        }
        bs = ldl_le_p(data + 8);
        n = ldl_le_p(data + 12);
        if (bs == 0 || bs % 4 || len != 32 + 4ull * n + (uint64_t)bs * n) {
            error_report("nano7: overlay '%s' truncated", s->overlay);
            exit(1);
        }
        for (uint32_t k = 0; k < n; k++) {
            uint32_t a = ldl_le_p(data + 32 + 4 * k);
            const uint8_t *src = (uint8_t *)data + 32 + 4ull * n +
                                 (uint64_t)bs * k;

            if (a < NANO7_DRAM_BASE ||
                (uint64_t)a + bs > NANO7_DRAM_BASE + NANO7_DRAM_SIZE) {
                error_report("nano7: overlay block 0x%08x outside DRAM", a);
                exit(1);
            }
            memcpy(dram + a - NANO7_DRAM_BASE, src, bs);
            nano7_torn_exclude(a, bs);
        }
        /* v1 (DumpRAM v4): word5 = saved irq mask; v2 (v5, IRQs on,
         * verified per-block copies): word5 = copy retries */
        info_report("nano7: overlay '%s' v%u: %u blocks of %u KB at T0 "
                    "(window %u us, %s 0x%x, wanted %u, dropped %u)",
                    s->overlay, ldl_le_p(data + 4), n, bs / 1024,
                    ldl_le_p(data + 16),
                    ldl_le_p(data + 4) == 1 ? "irq mask" : "retries",
                    ldl_le_p(data + 20), ldl_le_p(data + 24),
                    ldl_le_p(data + 28));
        g_free(data);
    }
    if (s->torn_map) {
        /* before the fixups below, so poked words count as fresh */
        nano7_torn_attach(get_system_memory(),
                          memory_region_get_ram_ptr(&s->dram),
                          NANO7_DRAM_BASE, s->torn_map);
        if (s->torn_ignore_pc) {
            unsigned long lo = 0, hi = 0;
            char *colon = strchr(s->torn_ignore_pc, ':');
            const char *end;
            if (!colon || qemu_strtoul(s->torn_ignore_pc, &end, 0, &lo) ||
                end != colon ||
                qemu_strtoul(colon + 1, NULL, 0, &hi)) {
                error_report("nano7: bad torn-ignore-pc '%s'", s->torn_ignore_pc);
                exit(1);
            }
            nano7_torn_set_ignore_pc(lo, hi);
        }
    }

    if (s->os_zero_file) {
        /* Long fixup lists (e.g. C++ static-local guards) live in a file;
         * whitespace/colon/comma separated, '#' starts a comment line. */
        gchar *data = NULL;
        GString *list = g_string_new(s->os_zero);

        if (!g_file_get_contents(s->os_zero_file, &data, NULL, NULL)) {
            error_report("nano7: cannot read os-zero-file '%s'",
                         s->os_zero_file);
            exit(1);
        }
        gchar **lines = g_strsplit(data, "\n", -1);

        for (gchar **p = lines; *p; p++) {
            gchar *line = g_strstrip(*p);
            gchar **tok;

            if (*line == '#') {
                continue;
            }
            tok = g_strsplit_set(line, " \t:,", -1);
            for (gchar **t = tok; *t; t++) {
                if (**t) {
                    g_string_append_printf(list, "%s%s",
                                           list->len ? ":" : "", *t);
                }
            }
            g_strfreev(tok);
        }
        g_strfreev(lines);
        g_free(data);
        g_free(s->os_zero);
        s->os_zero = g_string_free(list, false);
    }

    if (s->os_zero) {
        /* In-memory loader fixup (image file untouched): the image's .data
         * holds pointers to BSS objects that lazy-init only when NULL. */
        gchar **addrs = g_strsplit(s->os_zero, ":", -1);
        uint8_t *dram = memory_region_get_ram_ptr(&s->dram);

        for (gchar **a = addrs; *a; a++) {
            uint64_t addr = g_ascii_strtoull(*a, NULL, 0);

            if (addr < NANO7_DRAM_BASE ||
                addr + 4 > NANO7_DRAM_BASE + NANO7_DRAM_SIZE) {
                error_report("nano7: os-zero address '%s' outside DRAM", *a);
                exit(1);
            }
            info_report("nano7: os-zero 0x%08" PRIx64 " (was 0x%08x)", addr,
                        ldl_le_p(dram + addr - NANO7_DRAM_BASE));
            stl_le_p(dram + addr - NANO7_DRAM_BASE, 0);
            nano7_torn_mark(addr);
        }
        g_strfreev(addrs);
    }

    if (s->os_poke_file) {
        /* Same idea as os-zero, but arbitrary values: one "addr value"
         * pair per line, '#' comments. Used to repair snapshot kernel
         * structures torn by the non-atomic RAM dump. */
        gchar *data = NULL;
        uint8_t *dram = memory_region_get_ram_ptr(&s->dram);

        if (!g_file_get_contents(s->os_poke_file, &data, NULL, NULL)) {
            error_report("nano7: cannot read os-poke-file '%s'",
                         s->os_poke_file);
            exit(1);
        }
        gchar **lines = g_strsplit(data, "\n", -1);

        for (gchar **p = lines; *p; p++) {
            gchar *line = g_strstrip(*p);
            gchar *end;
            uint64_t addr, val;

            if (!*line || *line == '#') {
                continue;
            }
            addr = g_ascii_strtoull(line, &end, 0);
            val = g_ascii_strtoull(end, NULL, 0);
            if (addr < NANO7_DRAM_BASE ||
                addr + 4 > NANO7_DRAM_BASE + NANO7_DRAM_SIZE) {
                error_report("nano7: os-poke address '%s' outside DRAM", line);
                exit(1);
            }
            info_report("nano7: os-poke 0x%08" PRIx64 " = 0x%08" PRIx64
                        " (was 0x%08x)", addr, val,
                        ldl_le_p(dram + addr - NANO7_DRAM_BASE));
            stl_le_p(dram + addr - NANO7_DRAM_BASE, val);
            nano7_torn_mark(addr);
        }
        g_strfreev(lines);
        g_free(data);
    }

    qemu_register_reset(nano7_cpu_reset, s);

    if (s->pcsample_ms) {
        s->pc_timer = timer_new_ms(QEMU_CLOCK_HOST, nano7_pcsample_tick, s);
        timer_mod(s->pc_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_HOST) + s->pcsample_ms);
    }
}

static char *nano7_get_bootrom(Object *obj, Error **errp)
{
    return g_strdup(NANO7_MACHINE(obj)->bootrom);
}

static void nano7_set_bootrom(Object *obj, const char *value, Error **errp)
{
    Nano7MachineState *s = NANO7_MACHINE(obj);

    g_free(s->bootrom);
    s->bootrom = g_strdup(value);
}

static char *nano7_get_os_image(Object *obj, Error **errp)
{
    return g_strdup(NANO7_MACHINE(obj)->os_image);
}

static void nano7_set_os_image(Object *obj, const char *value, Error **errp)
{
    Nano7MachineState *s = NANO7_MACHINE(obj);

    g_free(s->os_image);
    s->os_image = g_strdup(value);
}

static char *nano7_get_iram_image(Object *obj, Error **errp)
{
    return g_strdup(NANO7_MACHINE(obj)->iram_image);
}

static void nano7_set_iram_image(Object *obj, const char *value, Error **errp)
{
    Nano7MachineState *s = NANO7_MACHINE(obj);

    g_free(s->iram_image);
    s->iram_image = g_strdup(value);
}

static void nano7_get_warm(Object *obj, Visitor *v, const char *name,
                           void *opaque, Error **errp)
{
    uint64_t value = NANO7_MACHINE(obj)->warm;

    visit_type_uint64(v, name, &value, errp);
}

static void nano7_get_mmu(Object *obj, Visitor *v, const char *name,
                          void *opaque, Error **errp)
{
    uint64_t value = NANO7_MACHINE(obj)->mmu_ttbr0;

    visit_type_uint64(v, name, &value, errp);
}

static void nano7_set_mmu(Object *obj, Visitor *v, const char *name,
                          void *opaque, Error **errp)
{
    visit_type_uint64(v, name, &NANO7_MACHINE(obj)->mmu_ttbr0, errp);
}

static void nano7_get_resume_hw(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    uint64_t value = NANO7_MACHINE(obj)->resume_hw;

    visit_type_uint64(v, name, &value, errp);
}

static void nano7_set_resume_hw(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    visit_type_uint64(v, name, &NANO7_MACHINE(obj)->resume_hw, errp);
}

static void nano7_get_fmss_resume(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    uint64_t value = NANO7_MACHINE(obj)->fmss_resume;

    visit_type_uint64(v, name, &value, errp);
}

static void nano7_set_fmss_resume(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    visit_type_uint64(v, name, &NANO7_MACHINE(obj)->fmss_resume, errp);
}

static void nano7_get_usec_base(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    uint64_t value = NANO7_MACHINE(obj)->usec_base;

    visit_type_uint64(v, name, &value, errp);
}

static void nano7_set_usec_base(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    visit_type_uint64(v, name, &NANO7_MACHINE(obj)->usec_base, errp);
}

static void nano7_set_warm(Object *obj, Visitor *v, const char *name,
                           void *opaque, Error **errp)
{
    visit_type_uint64(v, name, &NANO7_MACHINE(obj)->warm, errp);
}

/* extra-dram=N: what answers at PA 0x0c000000..0x0fffffff (RAMScope tests).
 * 1 = open bus (reads 0, writes dropped), 2 = independent 64 MB RAM,
 * 3 = alias of the 64 MB DRAM (address wraps). Default 0 = nothing mapped. */
static void nano7_get_extra_dram(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    uint64_t value = NANO7_MACHINE(obj)->extra_dram;
    visit_type_uint64(v, name, &value, errp);
}

static void nano7_set_extra_dram(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    visit_type_uint64(v, name, &NANO7_MACHINE(obj)->extra_dram, errp);
}

static void nano7_get_iram_protect(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    uint64_t value = NANO7_MACHINE(obj)->iram_protect;

    visit_type_uint64(v, name, &value, errp);
}

static void nano7_set_iram_protect(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    visit_type_uint64(v, name, &NANO7_MACHINE(obj)->iram_protect, errp);
}

static char *nano7_get_os_zero(Object *obj, Error **errp)
{
    return g_strdup(NANO7_MACHINE(obj)->os_zero);
}

static void nano7_set_os_zero(Object *obj, const char *value, Error **errp)
{
    Nano7MachineState *s = NANO7_MACHINE(obj);

    g_free(s->os_zero);
    s->os_zero = g_strdup(value);
}

#define NANO7_STR_PROP(field)                                               \
static char *nano7_get_##field(Object *obj, Error **errp)                   \
{                                                                           \
    return g_strdup(NANO7_MACHINE(obj)->field);                             \
}                                                                           \
static void nano7_set_##field(Object *obj, const char *value, Error **errp) \
{                                                                           \
    Nano7MachineState *s = NANO7_MACHINE(obj);                              \
                                                                            \
    g_free(s->field);                                                       \
    s->field = g_strdup(value);                                             \
}
NANO7_STR_PROP(nand_store)
NANO7_STR_PROP(nand_miss)
NANO7_STR_PROP(nand_disk)
NANO7_STR_PROP(nand_disk_off)
NANO7_STR_PROP(nand_rsrc)
NANO7_STR_PROP(nand_patch)
NANO7_STR_PROP(disp_dump)
NANO7_STR_PROP(buttons)
NANO7_STR_PROP(rtc)
NANO7_STR_PROP(state_dir)
NANO7_STR_PROP(screens_dir)
NANO7_STR_PROP(audio_out)
NANO7_STR_PROP(aes_gid_key)
NANO7_STR_PROP(aes_uid_key)
NANO7_STR_PROP(bt_hci)
NANO7_STR_PROP(bt_peer)
NANO7_STR_PROP(bt_pad)

static char *nano7_get_torn_map(Object *obj, Error **errp)
{
    return g_strdup(NANO7_MACHINE(obj)->torn_map);
}

static void nano7_set_torn_map(Object *obj, const char *value, Error **errp)
{
    Nano7MachineState *s = NANO7_MACHINE(obj);

    g_free(s->torn_map);
    s->torn_map = g_strdup(value);
}

static char *nano7_get_overlay(Object *obj, Error **errp)
{
    return g_strdup(NANO7_MACHINE(obj)->overlay);
}

static void nano7_set_overlay(Object *obj, const char *value, Error **errp)
{
    Nano7MachineState *s = NANO7_MACHINE(obj);

    g_free(s->overlay);
    s->overlay = g_strdup(value);
}

static char *nano7_get_torn_ignore_pc(Object *obj, Error **errp)
{
    return g_strdup(NANO7_MACHINE(obj)->torn_ignore_pc);
}

static void nano7_set_torn_ignore_pc(Object *obj, const char *value,
                                     Error **errp)
{
    Nano7MachineState *s = NANO7_MACHINE(obj);

    g_free(s->torn_ignore_pc);
    s->torn_ignore_pc = g_strdup(value);
}

static char *nano7_get_os_poke_file(Object *obj, Error **errp)
{
    return g_strdup(NANO7_MACHINE(obj)->os_poke_file);
}

static void nano7_set_os_poke_file(Object *obj, const char *value,
                                   Error **errp)
{
    Nano7MachineState *s = NANO7_MACHINE(obj);

    g_free(s->os_poke_file);
    s->os_poke_file = g_strdup(value);
}

static char *nano7_get_os_zero_file(Object *obj, Error **errp)
{
    return g_strdup(NANO7_MACHINE(obj)->os_zero_file);
}

static void nano7_set_os_zero_file(Object *obj, const char *value,
                                   Error **errp)
{
    Nano7MachineState *s = NANO7_MACHINE(obj);

    g_free(s->os_zero_file);
    s->os_zero_file = g_strdup(value);
}

static char *nano7_get_ctx(Object *obj, Error **errp)
{
    return g_strdup(NANO7_MACHINE(obj)->ctx_file);
}

static void nano7_set_ctx(Object *obj, const char *value, Error **errp)
{
    Nano7MachineState *s = NANO7_MACHINE(obj);

    g_free(s->ctx_file);
    s->ctx_file = g_strdup(value);
}

static void nano7_get_cpsr(Object *obj, Visitor *v, const char *name,
                           void *opaque, Error **errp)
{
    uint64_t value = NANO7_MACHINE(obj)->cpsr_override;

    visit_type_uint64(v, name, &value, errp);
}

static void nano7_set_cpsr(Object *obj, Visitor *v, const char *name,
                           void *opaque, Error **errp)
{
    visit_type_uint64(v, name, &NANO7_MACHINE(obj)->cpsr_override, errp);
}

static void nano7_get_entry(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp)
{
    uint64_t value = NANO7_MACHINE(obj)->entry;

    visit_type_uint64(v, name, &value, errp);
}

static void nano7_set_entry(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp)
{
    visit_type_uint64(v, name, &NANO7_MACHINE(obj)->entry, errp);
}

static void nano7_get_pcsample(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    uint64_t value = NANO7_MACHINE(obj)->pcsample_ms;

    visit_type_uint64(v, name, &value, errp);
}

static void nano7_set_pcsample(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    visit_type_uint64(v, name, &NANO7_MACHINE(obj)->pcsample_ms, errp);
}

static void nano7_instance_init(Object *obj)
{
    Nano7MachineState *s = NANO7_MACHINE(obj);

    s->bootrom = g_strdup("bootrom-vrom.bin");
    s->os_image = NULL;
    s->iram_image = NULL;
    s->ctx_file = NULL;
    s->ctx_loaded = false;
    s->cpsr_override = UINT64_MAX;
    s->resume_hw = 1;
    s->entry = NANO7_VROM_BASE;
}

static void nano7_instance_finalize(Object *obj)
{
    Nano7MachineState *s = NANO7_MACHINE(obj);

    g_free(s->bootrom);
    g_free(s->os_image);
    g_free(s->iram_image);
    g_free(s->ctx_file);
    g_free(s->os_zero);
    g_free(s->os_zero_file);
    g_free(s->os_poke_file);
    g_free(s->torn_map);
    g_free(s->overlay);
    g_free(s->nand_store);
    g_free(s->nand_miss);
    g_free(s->nand_disk);
    g_free(s->nand_disk_off);
    g_free(s->nand_rsrc);
    g_free(s->nand_patch);
    g_free(s->disp_dump);
    g_free(s->buttons);
    g_free(s->rtc);
    g_free(s->state_dir);
    g_free(s->screens_dir);
    g_free(s->audio_out);
    g_free(s->aes_gid_key);
    g_free(s->aes_uid_key);
    g_free(s->bt_hci);
    g_free(s->bt_peer);
    g_free(s->bt_pad);
}

/* QMP / HMP system_reset: a device reset cannot rebuild a snapshot-resumed
 * OS (RAM, MMIO set-up), so a host-requested reset reloads the boot state,
 * like F1. The start-up reset and resets before the boot state exists are
 * ordinary device resets. */
static void nano7_machine_reset(MachineState *ms, ShutdownCause reason)
{
    if ((reason == SHUTDOWN_CAUSE_HOST_QMP_SYSTEM_RESET ||
         reason == SHUTDOWN_CAUSE_HOST_UI) && nano7_ui_reset()) {
        return;
    }
    qemu_devices_reset(reason);
}

static void nano7_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-a7"), /* stand-in for cortex-a5 */
        NULL
    };

    mc->desc = "Apple iPod nano 7G (S5L8747-class, Cortex-A5)";
    mc->init = nano7_init;
    mc->reset = nano7_machine_reset;
    mc->default_cpus = 1;
    mc->min_cpus = 1;
    mc->max_cpus = 1;
    mc->valid_cpu_types = valid_cpu_types;
    mc->ignore_memory_transaction_failures = true;
    mc->default_ram_size = NANO7_DRAM_SIZE;

    object_class_property_add_str(oc, "bootrom",
                                  nano7_get_bootrom, nano7_set_bootrom);
    object_class_property_set_description(oc, "bootrom",
                                          "VROM/bootrom image file loaded "
                                          "at 0x20000000");
    object_class_property_add_str(oc, "os-image",
                                  nano7_get_os_image, nano7_set_os_image);
    object_class_property_set_description(oc, "os-image",
                                          "OS image preloaded at 0x08000000");
    object_class_property_add_str(oc, "iram-image",
                                  nano7_get_iram_image, nano7_set_iram_image);
    object_class_property_set_description(oc, "iram-image",
                                          "IRAM snapshot (incl. live vector "
                                          "table) preloaded at 0x22000000");
    object_class_property_add(oc, "extra-dram", "uint64",
                              nano7_get_extra_dram, nano7_set_extra_dram,
                              NULL, NULL);
    object_class_property_set_description(oc, "extra-dram",
                                          "PA 0x0c000000 test region: 1 open "
                                          "bus, 2 independent RAM, 3 alias");
    object_class_property_add(oc, "warm", "uint64",
                              nano7_get_warm, nano7_set_warm, NULL, NULL);
    object_class_property_set_description(oc, "warm",
                                          "preset PMGR+0x04 to this value "
                                          "(warm-boot/wake-reason bits)");
    object_class_property_add(oc, "resume-hw", "uint64",
                              nano7_get_resume_hw, nano7_set_resume_hw,
                              NULL, NULL);
    object_class_property_set_description(oc, "resume-hw",
                                          "with ctx=: replay the OS VIC/timer "
                                          "init at reset (default 1)");
    object_class_property_add(oc, "fmss-resume", "uint64",
                              nano7_get_fmss_resume, nano7_set_fmss_resume,
                              NULL, NULL);
    object_class_property_set_description(oc, "fmss-resume",
                                          "with ctx=: 1 = report the FMSS NAND "
                                          "sequence in flight at capture time "
                                          "as finished (IRQ 54); 2 = complete "
                                          "the next kick lacking a sequence "
                                          "base (armed before capture)");
    object_class_property_add(oc, "usec-base", "uint64",
                              nano7_get_usec_base, nano7_set_usec_base,
                              NULL, NULL);
    object_class_property_set_description(oc, "usec-base",
                                          "start value of the 1 MHz 64-bit "
                                          "counter at 0x3c700080/84");
    object_class_property_add(oc, "mmu", "uint64",
                              nano7_get_mmu, nano7_set_mmu, NULL, NULL);
    object_class_property_set_description(oc, "mmu",
                                          "TTBR0 value; nonzero also enables "
                                          "the MMU (flat VA=PA, DACR manager)");
    object_class_property_add(oc, "iram-protect", "uint64",
                              nano7_get_iram_protect, nano7_set_iram_protect,
                              NULL, NULL);
    object_class_property_set_description(oc, "iram-protect",
                                          "IRAM protection: 0=off, 1=vector "
                                          "page RO, 2=whole IRAM RO");
    object_class_property_add_str(oc, "os-zero",
                                  nano7_get_os_zero, nano7_set_os_zero);
    object_class_property_set_description(oc, "os-zero",
                                          "colon-separated DRAM addresses "
                                          "whose u32 is zeroed after the OS "
                                          "image is loaded");
    object_class_property_add_str(oc, "overlay", nano7_get_overlay,
                                  nano7_set_overlay);
    object_class_property_set_description(oc, "overlay",
                                          "DumpRAM v4 overlay.bin: DRAM blocks "
                                          "captured atomically (IRQs off) at "
                                          "T0, laid over the RAM image");
    object_class_property_add_str(oc, "nand-store", nano7_get_nand_store,
                                  nano7_set_nand_store);
    object_class_property_set_description(oc, "nand-store",
                                          "NAND page store file (persists "
                                          "programmed pages; default: temp)");
    object_class_property_add_str(oc, "nand-miss", nano7_get_nand_miss,
                                  nano7_set_nand_miss);
    object_class_property_set_description(oc, "nand-miss",
                                          "reads of never-programmed sectors: "
                                          "blank (0xff) or oracle (meta the "
                                          "sftl expects, data from nand-disk)");
    object_class_property_add_str(oc, "nand-disk-offset",
                                  nano7_get_nand_disk_off,
                                  nano7_set_nand_disk_off);
    object_class_property_set_description(oc, "nand-disk-offset",
                                          "FTL LBA of the disk image's first "
                                          "sector (snap6 device: 49216)");
    object_class_property_add_str(oc, "nand-disk", nano7_get_nand_disk,
                                  nano7_set_nand_disk);
    object_class_property_set_description(oc, "nand-disk",
                                          "disk image (4 KB LBAs) feeding "
                                          "nand-miss=oracle sectors");
    object_class_property_add_str(oc, "disp-dump", nano7_get_disp_dump,
                                  nano7_set_disp_dump);
    object_class_property_set_description(oc, "disp-dump",
                                          "directory: write each changed "
                                          "scanned-out LCD frame as a PPM");
    object_class_property_add_str(oc, "state-dir", nano7_get_state_dir,
                                  nano7_set_state_dir);
    object_class_property_set_description(oc, "state-dir",
                                          "directory for save-state slots "
                                          "(F2 save, F4 load, F1 reset)");
    object_class_property_add_str(oc, "screens-dir", nano7_get_screens_dir,
                                  nano7_set_screens_dir);
    object_class_property_set_description(oc, "screens-dir",
                                          "directory for F12 screenshots");
    object_class_property_add_str(oc, "nand-rsrc", nano7_get_nand_rsrc,
                                  nano7_set_nand_rsrc);
    object_class_property_set_description(oc, "nand-rsrc",
                                          "resource partition image (MSE "
                                          "rsrc img1) for NAND misses, "
                                          "FILE[@LBA], default LBA 0x3f2");
    object_class_property_add_str(oc, "nand-patch", nano7_get_nand_patch,
                                  nano7_set_nand_patch);
    object_class_property_set_description(oc, "nand-patch",
                                          "replaced 4 KB disk blocks for NAND "
                                          "misses (tools/disk_patch.py)");
    object_class_property_add_str(oc, "audio-out", nano7_get_audio_out,
                                  nano7_set_audio_out);
    object_class_property_set_description(oc, "audio-out",
                                          "PCM sink for the audio output: raw "
                                          "s16le stereo, *.wav, or a FIFO");
    object_class_property_add_str(oc, "aes-gid-key", nano7_get_aes_gid_key,
                                  nano7_set_aes_gid_key);
    object_class_property_set_description(oc, "aes-gid-key",
                                          "AES engine key type 1 (32 hex "
                                          "digits); unknown = jobs copy");
    object_class_property_add_str(oc, "aes-uid-key", nano7_get_aes_uid_key,
                                  nano7_set_aes_uid_key);
    object_class_property_set_description(oc, "aes-uid-key",
                                          "AES engine key type 2 (32 hex "
                                          "digits); unknown = jobs copy");
    object_class_property_add_str(oc, "bt-peer", nano7_get_bt_peer,
                                  nano7_set_bt_peer);
    object_class_property_set_description(oc, "bt-peer",
                                          "device in range of the fake BT "
                                          "controller: gamepad (default), "
                                          "gamepad-connect (it also pages "
                                          "the iPod) or none");
    object_class_property_add_str(oc, "bt-pad", nano7_get_bt_pad,
                                  nano7_set_bt_pad);
    object_class_property_set_description(oc, "bt-pad",
                                          "BT gamepad input script "
                                          "SEC:CONTROLS[:MS]/... after HID "
                                          "up (a+b, up, lx=-127, ...; none)");
    object_class_property_add_str(oc, "bt-hci", nano7_get_bt_hci,
                                  nano7_set_bt_hci);
    object_class_property_set_description(oc, "bt-hci",
                                          "Bluetooth UART: UNIX socket path "
                                          "(or tcp:HOST:PORT) to connect for "
                                          "the raw H4 stream instead of the "
                                          "built-in fake controller");
    object_class_property_add_str(oc, "rtc", nano7_get_rtc, nano7_set_rtc);
    object_class_property_set_description(oc, "rtc",
                                          "PMU real-time clock at start: "
                                          "snapshot (default: the OS's "
                                          "cached time), host or Unix "
                                          "seconds");
    object_class_property_add_str(oc, "buttons", nano7_get_buttons,
                                  nano7_set_buttons);
    object_class_property_set_description(oc, "buttons",
                                          "timed button presses "
                                          "SEC:home|power|play|volup|voldown"
                                          "[:HOLD_MS]/...");
    object_class_property_add_str(oc, "torn-map", nano7_get_torn_map,
                                  nano7_set_torn_map);
    object_class_property_set_description(oc, "torn-map",
                                          "DumpRAM v2 torn-map.bin: log guest "
                                          "reads of torn blocks not yet "
                                          "rewritten since resume");
    object_class_property_add_str(oc, "torn-ignore-pc", nano7_get_torn_ignore_pc,
                                  nano7_set_torn_ignore_pc);
    object_class_property_set_description(oc, "torn-ignore-pc",
                                          "lo:hi guest pc range whose torn "
                                          "reads are not counted");
    object_class_property_add_str(oc, "os-poke-file", nano7_get_os_poke_file,
                                  nano7_set_os_poke_file);
    object_class_property_set_description(oc, "os-poke-file",
                                          "file of 'addr value' u32 stores "
                                          "applied to DRAM after OS load");
    object_class_property_add_str(oc, "os-zero-file", nano7_get_os_zero_file,
                                  nano7_set_os_zero_file);
    object_class_property_set_description(oc, "os-zero-file",
                                          "file with more os-zero addresses "
                                          "(merged with os-zero)");
    object_class_property_add_str(oc, "ctx", nano7_get_ctx, nano7_set_ctx);
    object_class_property_set_description(oc, "ctx",
                                          "CPU context snapshot (16 x u32 LE: "
                                          "r0..r15); PC from snapshot");
    object_class_property_add(oc, "cpsr", "uint64",
                              nano7_get_cpsr, nano7_set_cpsr, NULL, NULL);
    object_class_property_set_description(oc, "cpsr",
                                          "CPSR to apply at reset");
    object_class_property_add(oc, "entry", "uint64",
                              nano7_get_entry, nano7_set_entry,
                              NULL, NULL);
    object_class_property_set_description(oc, "entry",
                                          "reset PC (default 0x20000000)");
    object_class_property_add(oc, "pcsample-ms", "uint64",
                              nano7_get_pcsample, nano7_set_pcsample,
                              NULL, NULL);
    object_class_property_set_description(oc, "pcsample-ms",
                                          "periodically log guest PC/CPSR "
                                          "every N ms (0 = off)");
}

static const TypeInfo nano7_machine_typeinfo = {
    .name = TYPE_NANO7_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(Nano7MachineState),
    .instance_init = nano7_instance_init,
    .instance_finalize = nano7_instance_finalize,
    .class_init = nano7_machine_class_init,
};

static void nano7_machine_register_types(void)
{
    type_register_static(&nano7_machine_typeinfo);
}

type_init(nano7_machine_register_types)
