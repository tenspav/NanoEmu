# S5L8747 Dualboot Architecture & Chainloader Implementation Plan
# Target: iPod nano 7G (Cortex-A5, 64MB DRAM @ 0x08000000, 256KB IRAM @ 0x22000000)
# Output: workers/agy-dualboot.txt

================================================================================
1. EXECUTIVE SUMMARY & ARCHITECTURE VERDICT
================================================================================
We have:
  1. Untethered jailbreak running persistently via /Apps/Startup/resident-B.hbapp
     (survives reboots, auto-registers on-disk AllApps-B.pack).
  2. Full 192.25 MiB boot-area dump (dumps/boot-area/reserved.bin, LBAs 0..49215).
  3. Validated UEFI diagnostic boot selector in BdsN31.dll (DRAM 0x0bc2d000).
  4. Working NanoEmu emulation environment with snap6 resume, overlay, torn-map,
     FMC NAND store, and dynamic disk patching (disk_patch.py).

VERDICT ON DUALBOOT MECHANISMS:
  - Coldboot Diag Slot (Writing NAND:diag): NOT FEASIBLE WITHOUT RSA EXPLOIT.
    UEFI BdsN31 button check (0x0bc2e56c) queries Sleep+Home+VolDown and resolves
    Partition 2 container 'diag' (LBA 240..753). However, the UEFI image validator
    (0x0bc78658 / 0x0bc77d78) strictly verifies RSA-1024 signatures before parsing
    container type. Type 4 payloads ONLY bypass AES decryption, NOT RSA signature
    checks. Flashing an unsigned custom payload to NAND:diag will fail validation.
    Furthermore, modifying Partition 2 carries catastrophic brick risks.
  - OS-Level Chainloader (Warm Kexec Takeover): OPTIMAL & RECOMMENDED.
    Executes from the untethered resident jailbreak in RetailOS. Escalates to SVC
    privilege via verified dispatch table hook (0x08917714), cleanly quiesces all
    hardware engines (FMC, DMA, Timers, VIC), switches to private IRAM execution,
    disables Cortex-A5 MMU and caches via CP15, relocates the target payload to
    0x08000000, and hands over all 64 MiB DRAM.
    Stock OS remains 100% untouched, intact, and bootable. Zero brick risk.

================================================================================
2. NAND OFFSETS & PARTITION MAP: WHAT TO WRITE VS AVOID
================================================================================
Physical & logical layout derived from reserved.bin and docs/research/:

--------------------------------------------------------------------------------
Zone 0: Raw Physical NAND Boot Blocks (OUTSIDE FTL / LOGICAL BLOCKS)
--------------------------------------------------------------------------------
- CE0 Block 0 rows 0..49 (0x00..0x31) and Block 1 backup rows 256..305 (0x100..0x131):
  Contains LLB (N31.bootloader.rb3, 149 KB).
- AVOID AT ALL COSTS: Read-only to BootROM (0x2000336c). Flashing raw LLB with
  unsigned code triggers BootROM RSA failure, resulting in an unrecoverable HARD BRICK
  (no USB DFU exploit exists for S5L8747). NEVER write raw FMC blocks 0 and 1.

--------------------------------------------------------------------------------
Zone 1: Reserved Boot Area / Partition 2 (GLOBAL LBAs 0..49215 / 192.25 MiB)
--------------------------------------------------------------------------------
- LBA 0..63 (0x00000..0x40000): MBR / Volume header padding.
- LBA 64..49215 (0x40000..0xc040000): Partition 2 ("Firmware.MSE", 192.0 MiB).
  * LBA 69 (0x45000): MSE Partition Table (11 container records).
  * LBA 71 (0x47000): 'disk' container (UEFI BDS bootloader BdsN31.dll, 674 KB).
  * LBA 240..753 (0xb0000..0x2b1063): 'diag' container (DiagOS, 2.10 MB).
  * LBA 756 (0x2f4000): 'fv00' container (70 KB).
  * LBA 1010 (0x3f2000): 'rsrc' container (Plaintext FAT16 UI resources, 171 MB).
  * LBA 44789 (0xaef5000): 'osos' container (RetailOS kernel & binaries, 9.46 MB).
- AVOID AT ALL COSTS:
  * Do NOT write to LBA 71 (disk) or LBA 44789 (osos): Corrupts UEFI or RetailOS,
    destroying the untethered jailbreak and rendering the device unbootable.
  * Do NOT flash LBA 240..753 (diag): Fails UEFI RSA-1024 verification. Any FTL
    page corruption in this region risks mounting failures in UEFI Image1Fs.
  * Do NOT write to MSE Partition Table (LBA 69).

--------------------------------------------------------------------------------
Zone 2: User Data Volume / Partition 3 (GLOBAL LBAs 49216+ / 0x0c040000+)
--------------------------------------------------------------------------------
- LBA 49216..3857034 (15.8 GB): Standard FAT32 partition mounted as /dev/sdb1.
- THIS IS THE ONLY AREA TO WRITE.
  All chainloader components, second-stage kernels, devicetrees, and user payloads
  must reside strictly within the FAT32 filesystem:
  * /Apps/Executables/Chainloader.hbapp (the homebrew loader application)
  * /Apps/Icons/Chainloader.bin (icon asset)
  * /Apps/AllApps-B.pack (repacked bundle containing Chainloader launch record)
  * /Apps/DualBoot/payload.bin (or /boot/zImage, /boot/rockbox.bin, /boot/dtb.bin)
  * /Apps/DualBoot/boot.cfg (boot selection preferences, timeouts, kernel args)

================================================================================
3. HOW TO KEEP STOCK OS UNTOUCHED & SELECT OS AT BOOT
================================================================================
Dualboot is managed non-destructively through the existing untethered jailbreak.

A. STOCK OS PRESERVATION:
   1. NAND boot chain (BootROM -> LLB -> UEFI BDS -> RetailOS) is 100% factory stock.
   2. All Apple cryptographic certificates, signatures, and AES-CBC containers are
      preserved byte-for-byte on NAND.
   3. If the iPod is reset normally or USB connected, it boots pristine RetailOS.

B. OS SELECTION ARCHITECTURE (TWO-TIER SELECTION):
   Tier 1: Pre-UI Boot Trap in Resident (/Apps/Startup/resident-B.hbapp)
   - On cold boot, RetailOS initializes drivers and launches the resident daemon
     before rendering the Springboard/Home screen.
   - Resident queries hardware buttons via PMU / GPIO MMIO:
     * Sleep/Wake button: PMU reg 3 bit 1 (I2C1 0x3c900000, slave 0x73)
     * Home button: PMU reg 3 bit 0
     * Volume Down button: GPIO 0x29 (0x3cf00144 / VIC IRQ 32)
   - Boot Trap Rule:
     * If Volume Down is held during boot: Resident intercepts startup, skips
       Home UI, and directly executes Chainloader.
     * Optional 3-second countdown splash: Resident blits a simple splash screen
       ("Hold VolDown for Dualboot, or tap screen"). If no input within 3s,
       resident yields and normal RetailOS Home screen appears.
     * Default boot behavior (no buttons pressed): Boots RetailOS immediately.

   Tier 2: Interactive Home Screen App (Chainloader.hbapp)
   - A dedicated app icon on the RetailOS Home screen.
   - When opened, provides a clean raw-surface graphical UI:
     * Menu: [1] Boot Linux, [2] Boot Rockbox, [3] Baremetal Diag, [4] Reboot Stock.
     * Configuration: Set default boot OS, configure countdown timeout (0-10s).
     * System Status: Displays DRAM map, payload size, battery mV, checksum status.
     * Tap "BOOT" to initiate teardown and warm takeover.

================================================================================
4. STEP-BY-STEP CHAINLOADER IMPLEMENTATION & EXECUTION FLOW
================================================================================

--------------------------------------------------------------------------------
Phase 1: File Loading & Staging (User Space / RetailOS API)
--------------------------------------------------------------------------------
1. File Staging:
   - Read payload file (/Apps/DualBoot/payload.bin) using SDK filesystem APIs.
   - Parse Handoff Header (first 32 bytes):
     * Magic: 0x4c43374e ('N7CL')
     * Entry Offset: e.g. +0x00000000 (or Linux zImage entry +0x00000040)
     * Image Size: <= 60 MiB
     * BSS Size, Checksum (CRC32/FNV-1a)
2. Staging Allocation:
   - Allocate contiguous DRAM staging buffer via hb_os_alloc (Thumb 0x0842b441(size, 2)).
   - Staging memory MUST be allocated above the destination payload range.
     Target location is physical 0x08000000; staging buffer will typically reside
     in the shared heap at 0x09500000..0x0b500000.
3. In-Memory Validation & Handle Cleanup:
   - Verify payload checksum in RAM.
   - Close all open file descriptors via hb_fs_close.
   - Ensure no pending filesystem writes exist on the FAT32 volume.

--------------------------------------------------------------------------------
Phase 2: Privilege Escalation via SVC Dispatcher Hook
--------------------------------------------------------------------------------
RetailOS apps run in User mode (CPSR=0x10) but have write access to kernel DRAM.
1. Target Dispatcher:
   - SVC dispatcher is located at 0x08002390, handling `svc #0x46` (svc #70).
   - Dispatch table is at 0x08917714:
     * Index 0: 0x080e04c7 (irq_save: masks I|F in SPSR, returns old mask)
     * Index 1: 0x080e04dd (irq_restore: restores I|F mask)
2. Cache Synchronization:
   - Prepare privilege trampoline function in writable memory.
   - Invalidate instruction cache using SDK svc #70 r12=7 (hb_icache_invalidate).
3. Interrupt Masking & Atomic Hook:
   - Call original selector 0 (`svc #70`, r12=0) to save and mask interrupts.
   - Write trampoline address (with Thumb bit 0 set: `(uint32_t)trampoline | 1`)
     to table slot 0 at 0x08917714.
   - Execute `DSB`.
4. Trigger Escalation:
   - Execute `svc #70` with r12=0.
   - Trampoline executes in SVC mode (CPSR=0x13).
5. Immediate Kernel Cleanup:
   - Inside trampoline: immediately execute `cpsid aif` (mask Asynchronous aborts,
     IRQs, and FIQs).
   - Immediately restore original selector 0 pointer (0x080e04c7) to 0x08917714.
   - Save original SVC stack pointer and LR for reversible testing.

--------------------------------------------------------------------------------
Phase 3: Hardware Quiescence & Engine Teardown
--------------------------------------------------------------------------------
VIC interrupt masking alone does NOT stop bus-mastering DMA engines. Outstanding
AXI bus transactions will corrupt DRAM once payload relocation starts.

1. Storage & Flash Controller (FMC / FMSS @ 0x38a00000):
   - Check pending status: wait until `(FMC_STATUS & 0x0181000e) == 0` and
     `FMSS_CS_STATUS (+0xc0c) == 0`.
   - Disable FMSS sequencer interrupt generation: write `0` to 0x38a00c10.
   - Acknowledge all pending completion flags: W1C write to 0x38a00048 and 0x38a00c0c.
   - Deselect NAND bus: write `0x20` to +0x04, write `1` to +0x00.
   - NEVER abort an in-flight NAND program/erase; always wait for idle.
2. General DMA Engines (DMA1 @ 0x38200000, DMA2 @ 0x38700000):
   - For both engines, channels n = 0..7:
     * Clear bit 0 (run) at `base + 0x110 + 0x20*n`.
     * Read back register to force pipeline drain.
     * Clear NEXT descriptor link at `base + 0x108 + 0x20*n = 0`.
     * Acknowledge completion flags at `base + 0x08`.
3. Display, Blitter & GPU:
   - Quiesce LCD scanout controller at 0x38900000 and 2D blitter at 0x3c100000.
   - Quiesce MBX GPU at 0x3b000000 (ensure transfer job bit 6 is idle).
   - Quiesce Audio ADSP co-processor at 0x38500000 and USB DWC2 OTG at 0x38400000.
4. Hardware Timers (@ 0x3c700000):
   - Channel offsets: {0, 0x20, 0x40, 0x60, 0xa0, 0xc0, 0xe0, 0x100}.
   - For each channel: write `CMD (+0x04) = 0`, clear CON interrupt enables `0x7000`.
   - Clear pending status bits at +0x118 (write 0x07070707).
5. Vectored Interrupt Controller (VIC0 @ 0x38e00000, VIC1 @ 0x38e01000):
   - Mask all interrupt lines: write `0xffffffff` to INTENCLEAR (0x38e00014 and 0x38e01014).
   - Clear software interrupts at +0x1c.
   - Clear VIC glue latches at 0x38e02008 and 0x38e0200c.
   - Acknowledge bank VICADDRESS (+0xf00).
6. Watchdog Protocol (@ 0x3c800000):
   - Disable watchdog or reload with maximum timeout (e.g. 30 seconds).
   - Pass watchdog MMIO base and status in Handoff Block so target kernel can kick it.

--------------------------------------------------------------------------------
Phase 4: Transition to IRAM Execution
--------------------------------------------------------------------------------
Stock IRAM interrupt vectors (0x22000000..0x22002000) point into DRAM kernel code.
Once DRAM is overwritten, any vector execution triggers fatal corruption.
Takeover execution MUST move to dedicated high IRAM.

1. Layout in IRAM (0x22008000..0x22010000):
   - 0x22008000: Private takeover exception vectors (hang/trap stubs)
   - 0x22008400: Position-independent takeover stub (`iram_takeover_stub`)
   - 0x22009000: Handoff Block data structure
   - 0x22010000: Top of private takeover stack (grows downwards)
2. Setup & Jump:
   - Copy vectors, handoff data, and takeover stub to IRAM.
   - Synchronize copied code: `DSB; ISB`.
   - Install takeover exception vectors: `MCR p15, 0, r0, c12, c0, 0` (VBAR = 0x22008000).
   - Set private SP: `mov sp, #0x22010000`.
   - Branch to `0x22008400` in IRAM.

--------------------------------------------------------------------------------
Phase 5: MMU & Cache Teardown (Cortex-A5 CP15 from IRAM)
--------------------------------------------------------------------------------
Must be executed entirely from register-only assembly in IRAM:
1. Data Cache Clean & Invalidate:
   - `DSB`
   - Read CCSIDR (c0, c0, 0) to obtain D-Cache geometry: 128 sets, 4 ways, 32-byte line.
   - Loop sets (0..127) and ways (0..3):
     `MCR p15, 0, rSetWay, c7, c14, 2` (Clean and Invalidate D-Cache line by Set/Way).
   - `DSB`
2. Disable MMU, Caches & Branch Prediction:
   - Read SCTLR: `MRC p15, 0, r0, c1, c0, 0`
   - Clear bits:
     * Bit 0: M (MMU disable)
     * Bit 2: C (Data and unified cache disable)
     * Bit 11: Z (Branch prediction disable)
     * Bit 12: I (Instruction cache disable)
     * Bit 13: V (Low vectors selected: 0x00000000 / VBAR)
   - Write SCTLR: `MCR p15, 0, r0, c1, c0, 0`
   - `ISB`
3. Pipeline & TLB Invalidation:
   - Invalidate Instruction Cache: `MCR p15, 0, r0, c7, c5, 0`
   - Invalidate Branch Target Cache: `MCR p15, 0, r0, c7, c5, 6`
   - Invalidate Entire Unified TLB: `MCR p15, 0, r0, c8, c7, 0`
   - `DSB; ISB`

--------------------------------------------------------------------------------
Phase 6: Relocation & Handoff to Target OS
--------------------------------------------------------------------------------
Physical memory is now 100% identity-mapped.
1. Memory Relocation:
   - Run overlap-safe `memmove_words` from IRAM.
   - Copy staged payload from high DRAM staging buffer down to 0x08000000.
   - Clear target BSS region: zero fill `[bss_start, bss_end)`.
   - `DSB; ISB`
2. Handoff ABI Structure (@ 0x22009000):
   ```c
   struct n7_handoff {
       uint32_t magic;           // 0x4c43374e ("N7CL")
       uint32_t version;         // 1
       uint32_t dram_base;       // 0x08000000
       uint32_t dram_size;       // 0x04000000 (64 MiB)
       uint32_t entry_point;     // 0x08000000 + entry_offset
       uint32_t initrd_start;    // Physical address or 0
       uint32_t initrd_size;     // Size in bytes or 0
       uint32_t cmdline_ptr;     // Pointer to ASCII cmdline string
       uint32_t watchdog_mmio;   // 0x3c800000
       uint32_t uart_mmio;       // 0x3ce00000 (UART0 console)
   };
   ```
3. Register ABI at Entry:
   - `r0 = 0x00000000` (Standard ARM Linux boot convention)
   - `r1 = 0x00001097` (Machine architecture ID)
   - `r2 = 0x22009000` (Pointer to Handoff Block / ATAGS / DTB in IRAM)
   - `r3..r12 = 0`
   - `SP = 0x22010000` (Private IRAM stack)
   - `LR = 0x22008100` (Trap handler; triggers warm reset on return)
   - `CPSR = 0x000001d3` (ARM state, SVC mode, A/I/F interrupts disabled)
4. Execution Jump:
   - `bx rEntry` (Jump to 0x08000000 + entry_offset). Target payload owns all 64 MiB.

================================================================================
5. RISK ANALYSIS PER STEP & MITIGATION STRATEGY
================================================================================
| Step | Action | Risk Level | Failure Mode | Mitigation & Containment |
|---|---|---|---|---|
| 1 | Read payload from FAT32 | LOW | Corrupt file, I/O error, heap exhaustion | Check return codes, verify CRC32/FNV hash, abort cleanly back to RetailOS UI. |
| 2 | Stage payload in DRAM | LOW | Insufficient heap memory | Use `hb_os_heap_largest()` check (> payload size + 2MB); abort if allocation fails. |
| 3 | Hook SVC table at 0x08917714 | MED-LOW | Race with unhandled interrupt; Thumb/ARM state mismatch | Call IRQ save (`svc #70 r12=0`) before patching; synchronize I-cache; restore table entry immediately inside trampoline. |
| 4 | Quiesce FMC & DMA engines | MEDIUM | Hanging bus master; stuck DMA transfer; corrupted write | Poll status with 100ms timeout; clear channel bit 0 then NEXT ptr; acknowledge status W1C; never write DRAM if FMC busy. |
| 5 | Mask Timers & VIC | LOW | Unmasked timer fires after IRAM transition | Write CMD=0 to all 8 channels; mask VIC0/VIC1 INTENCLEAR; clear glue latches. |
| 6 | Transition to IRAM (0x22008400) | LOW | Overwriting kernel IRAM variables | Use safe upper range (0x22008000..0x22010000) well clear of lower kernel thunks (0x22000000..0x22002000). |
| 7 | Disable MMU & Caches (CP15) | MEDIUM | Cache incoherency; dirty line evict over new payload | Full CCSIDR set/way clean+invalidate; strict DSB/ISB barriers; invalidate TLB, BPI, ICI; run strictly from IRAM. |
| 8 | Relocate payload to 0x08000000 | LOW | Staging and destination overlap corruption | Overlap-safe word copy (`memmove`); verify destination bounds <= 0x0c000000. |
| 9 | Target Kernel Execution | LOW (to HW) | Kernel crash, invalid instruction, hang | Hardware is unaffected. Watchdog or manual Sleep+Home reset cold-boots stock RetailOS. |

================================================================================
6. VERIFICATION IN NANOEMU BEFORE TOUCHING HARDWARE
================================================================================
EVERY step can and must be tested in the NanoEmu emulator using the existing
snap6 snapshot and disk patching tools.

--------------------------------------------------------------------------------
Phase A: Build the Test Harness
--------------------------------------------------------------------------------
1. Minimal Baremetal Test Payload (`ramcheck.bin`):
   - Small binary linked at 0x08000000:
     * Validates r0, r1, r2 register ABI and reads handoff block at 0x22009000.
     * Writes alternating test patterns (0x55aa55aa, 0xa55aa55a) across entire
       DRAM (0x08000000..0x0c000000) to prove 100% exclusive 64 MiB ownership.
     * Writes solid fill color to LCD blitter MMIO (0x3c100000) or sends character
       to UART0 (0x3ce00000) to signal SUCCESS.
     * Enters WFI loop.
2. Build Chainloader:
   - Compile `probe/chainloader` -> `build/chainloader.hbapp`.

--------------------------------------------------------------------------------
Phase B: Emulated Disk Patching (Zero Image Modification)
--------------------------------------------------------------------------------
Inject the Chainloader app into the emulated disk using `disk_patch.py`:
```bash
cd /home/pavliukov/ipodwork/NanoRework/emulator
python3 tools/disk_patch.py ../dumps/ipod-disk.dd \
    /Apps/Executables/Nanosync.hbapp=../probe/chainloader/build/chainloader.hbapp \
    -o rsrc/chainloader-as-nanosync.patch
```
(Using the Nanosync slot: 456 KB capacity, ample headroom for Chainloader).

--------------------------------------------------------------------------------
Phase C: Launch & Test Sequence in NanoEmu
--------------------------------------------------------------------------------
Launch NanoEmu resuming snap6 with the patch:
```bash
./run.sh \
    --ramfull ../dumps/snap6/ram-full.bin \
    --iram ../dumps/snap6/iram.bin \
    --ctx fixups/snap6-ctx.bin \
    --overlay ../dumps/snap6/overlay.bin \
    --os-poke-file fixups/snap6-power.poke \
    --torn-map ../dumps/snap6/torn-map.bin \
    --nand-disk ../dumps/ipod-disk.dd \
    --nand-disk-offset 49216 \
    --nand-rsrc rsrc/rsrc-1.1.2.img1 \
    --nand-patch rsrc/chainloader-as-nanosync.patch \
    --vnc 0
```

1. Verification Stage 1: Reversible Privilege Hook
   - Configure Chainloader in "Probe Mode":
     * Executes `svc #70`, enters trampoline, checks CPSR == 0x13.
     * Restores table entry 0x08917714.
     * Returns to user mode via saved SVC continuation.
     * Verifies user app resumes cleanly without kernel panic.
2. Verification Stage 2: Hardware Quiescence Assertion
   - Enable QEMU MMIO tracing: verify all DMA engines report run bit 0, FMC is idle,
     and VIC0/VIC1 INTENABLE reads 0.
3. Verification Stage 3: CP15 & MMU Teardown
   - Attach GDB stub (`qemu-system-arm -s -S`):
     * Break at 0x22008400 (IRAM stub).
     * Step through CP15 instructions: verify SCTLR bit 0 becomes 0.
     * Assert CPU reads flat physical memory at 0x08000000.
4. Verification Stage 4: RAM Ownership & Handoff
   - Continue execution into `ramcheck.bin`.
   - In GDB or memory monitor, inspect DRAM 0x08000000..0x0c000000:
     * Confirm pattern matches, zero memory aborts.
     * Check screenshot via `--disp-dump` or VNC: confirm test pattern rendered.

--------------------------------------------------------------------------------
Phase D: Fault Injection & Robustness Testing
--------------------------------------------------------------------------------
1. Stuck DMA Injection:
   - Mock a DMA channel that fails to clear run bit 0; verify Chainloader times
     out (100ms) and aborts cleanly to RetailOS without crashing.
2. Corrupt Payload Header:
   - Provide zeroed payload file; verify Chainloader displays "BAD MAGIC" on UI
     and preserves RetailOS stability.
3. Watchdog Expiry Test:
   - Intentionally hang in IRAM without kicking watchdog; verify watchdog resets
     system cleanly into stock RetailOS.

================================================================================
7. ROLLBACK & DISASTER RECOVERY PROCEDURES
================================================================================

--------------------------------------------------------------------------------
A. Runtime Hang / Crash Rollback (Device Unresponsive)
--------------------------------------------------------------------------------
Because Chainloader runs strictly in volatile RAM:
1. Hold **Sleep/Wake + Home** buttons simultaneously for 10 seconds.
2. The PMIC and SoC undergo a hardware power-on reset (POR).
3. BootROM executes, verifies stock LLB, loads stock UEFI BDS, and launches
   pristine RetailOS.
4. Zero NAND corruption; system returns to standard Home screen.

--------------------------------------------------------------------------------
B. Startup Bootloop Recovery (If Startup Resident was Modified)
--------------------------------------------------------------------------------
If an early boot trap in `/Apps/Startup/resident-B.hbapp` causes an infinite loop:
1. Force reset: Hold **Sleep/Wake + Home** for 10 seconds.
2. As soon as the Apple logo appears, immediately hold **Volume Up**.
   (The resident includes a built-in safe-mode bypass: holding VolUp aborts
   startup scripts and loads standard RetailOS).
3. Alternatively, connect Lightning USB cable while holding Home to enter
   standard Apple USB Disk Mode (Mass Storage).

--------------------------------------------------------------------------------
C. Complete Host-Side Filesystem Rollback
--------------------------------------------------------------------------------
To completely remove the Chainloader app and restore the exact prior disk state:
1. Connect iPod to Linux host and mount user partition:
   ```bash
   sudo mkdir -p /mnt/ipod
   sudo mount /dev/sdb1 /mnt/ipod
   ```
2. Restore original app pack from backup:
   ```bash
   sudo cp /home/pavliukov/ipodwork/NanoRework/dumps/apps-backup-20261013T032913Z/AllApps-B.pack \
           /mnt/ipod/Apps/AllApps-B.pack
   ```
3. Remove Chainloader files:
   ```bash
   sudo rm -f "/mnt/ipod/Apps/Executables/Chainloader.hbapp"
   sudo rm -f "/mnt/ipod/Apps/Icons/Chainloader.bin"
   sudo rm -rf "/mnt/ipod/Apps/DualBoot"
   ```
4. Verify checksums against manifest:
   ```bash
   cd /mnt/ipod/Apps
   sha256sum -c /home/pavliukov/ipodwork/NanoRework/dumps/apps-backup-20261013T032913Z/MANIFEST.sha256
   ```
5. Clean unmount and eject:
   ```bash
   sync
   sudo umount /mnt/ipod
   sudo eject /dev/sdb
   ```

================================================================================
8. SOURCE CODE & REPO FILE STRUCTURE
================================================================================
Recommended layout for the Chainloader probe app:

```
probe/chainloader/
├── Makefile                # Builds Chainloader.hbapp using NanoApps SDK hb_app.mk
├── Info.plist              # Bundle metadata (hb.chainloader, HBAppKind 1)
├── chainloader.c           # UI, button checks, file loader, state machine
├── privilege.S             # Assembly trampoline for svc #70 privilege escalation
├── teardown.c              # Hardware quiescence (FMC, DMA, Timers, VIC, Watchdog)
├── iram_takeover.S         # Position-independent IRAM code: CP15, MMU off, memmove
├── iram.ld                 # Linker script placing takeover code at 0x22008400
├── handoff.h               # ABI v1 structure definitions
└── repack_chainloader.py   # Script to append Chainloader bundle to AllApps-B.pack

payload/test_payload/
├── Makefile                # Builds bare-metal flat binary
├── link.ld                 # Linked at 0x08000000
├── start.S                 # ABI entry point, saves r0-r2, sets SP
└── ramcheck.c              # 64MB memory test pattern + LCD solid fill
```

Build & Test Commands:
```bash
# 1. Build test payload
make -C payload/test_payload

# 2. Build chainloader app
make -C probe/chainloader

# 3. Patch into emulated disk and test in NanoEmu
python3 emulator/tools/disk_patch.py dumps/ipod-disk.dd \
    /Apps/Executables/Nanosync.hbapp=probe/chainloader/build/chainloader.hbapp \
    -o emulator/rsrc/chainloader.patch
./emulator/run.sh ... --nand-patch emulator/rsrc/chainloader.patch
```
================================================================================
END OF IMPLEMENTATION PLAN
