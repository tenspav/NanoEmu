# iPod nano 7th Generation: Diagnostic Mode & Dual-Boot Research Report

## Executive Summary
This document investigates the iPod nano 7th generation (N31 platform / Samsung S5L8740 SoC, ARM Cortex-A5, 64 MB DRAM) diagnostic mode for a homebrew OS dual-boot project (e.g., Linux or Rockbox). All research is derived from non-destructive static analysis, live memory dumps (`snap6/ram-full.bin`), flash partition dumps (`dumps/boot-area/reserved.bin`), and reverse engineering notes in the repository.

Directly flashing a custom homebrew payload into the diagnostic boot slot (`NAND:diag`) is **infeasible** due to hardware-enforced RSA-1024 signature checks in the pre-OS bootloader. However, a non-destructive dual-boot setup is **fully feasible** via an **OS-level warm chainloader (kexec takeover)** launched from the existing untethered jailbreak, leaving stock RetailOS 100% intact with zero brick risk. Alternatively, stock DiagOS can be decrypted via an on-device AES-GID hardware oracle to audit it for coldboot chainload vulnerabilities.

---

## 1. How to Enter and Exit Diagnostic Mode

### 1.1 Entering Diagnostic Mode
* **Hardware Button Sequence:**
  1. Force reset the device: Press and hold **Sleep/Wake (Power) + Home** simultaneously for approximately 8–10 seconds until the Apple logo disappears and reappears (or the display blanks).
  2. Immediately upon reboot/Apple logo display, press and hold the diagnostic chord: **Sleep/Wake + Home + Volume Down**.
* **Pre-OS Selector Mechanism (`BdsN31.dll`):**
  * Diagnostic mode is **not** selected in BootROM (BootROM only checks warm boot status and USB DFU pin).
  * Selection occurs in the second-stage UEFI bootloader (`BdsN31.dll`, located at DRAM base `0x0bc2d000`).
  * The boot-selection routine at `0x0bc2ea20` calls `0x0bc2e56c`, which queries `AppleButtonProtocol` (`GUID` at `0x0bc31ff0`):
    * Button 0: **Home**
    * Button 1: **Sleep/Wake**
    * Button 2: **Volume Down**
  * When all three buttons report pressed (`Button 0 == 1`, `Button 1 == 1`, `Button 2 == 1`), execution branches to `0x0bc2e5ec` to dispatch diagnostic boot.
* **Software / OS-Level Reference:**
  * RetailOS contains a `DiagnosticMode` preference flag (at `0x08016abc` / `/var/mobile/Library/Preferences`), but the hardware button chord evaluated by BDS is the true pre-OS gateway.

### 1.2 Exiting Diagnostic Mode
* **Hardware Forced Reset:** Press and hold **Sleep/Wake + Home** for 8–10 seconds. The PMU / PMGR resets the SoC into a clean cold boot back to standard RetailOS.
* **DiagOS Menu Reset:** If the stock DiagOS menu is loaded, selecting "Reset" or "Reboot" issues a watchdog or PMGR reboot command.
* **Power Drain / Recovery:** If the device freezes in diagnostic mode, allowing the battery to drain or performing a Sleep+Home hard reset restores normal boot.

---

## 2. NAND Partition and Boot Path

### 2.1 Boot Hierarchy
The S5L8740 follows Apple's standard secure boot chain:
```
+-------------------------------------------------------------------+
| 1. SecureROM (BootROM, 64 KB @ 0x20000000)                        |
|    - Validates RSA-1024 + AES-GID decrypts LLB from Raw NAND      |
+-------------------------------------------------------------------+
                                  |
                                  v
+-------------------------------------------------------------------+
| 2. Low-Level Bootloader (LLB / N31.bootloader.rb3, 149 KB @ IRAM) |
|    - Initializes DRAM controller (64 MB @ 0x08000000)             |
|    - Validates RSA-1024 + AES-GID decrypts NAND:disk (UEFI BDS)   |
+-------------------------------------------------------------------+
                                  |
                                  v
+-------------------------------------------------------------------+
| 3. UEFI Boot Device Selection (BdsN31.dll @ DRAM 0x0bc2d000)      |
|    - Evaluates Sleep + Home + VolDown chord (0x0bc2e56c)          |
+-------------------------------------------------------------------+
             |                                              |
      [Chord Detected]                              [Normal Boot]
             v                                              v
+-----------------------------+               +---------------------+
| 4a. NAND:diag (2.00 MB)     |               | 4b. NAND:osos (OS)  |
|     - Loads to 0x08000000   |               |     - Loads to      |
|     - Validated by          |               |       0x08000000    |
|       ROMBootValidator.dll  |               |     - Boots         |
|     - Executes DiagOS       |               |       RetailOS      |
+-----------------------------+               +---------------------+
```

### 2.2 NAND Flash Organization
The physical NAND flash is controlled by the FMC controller (MMIO `0x38a00000`) using the Pure PPN (Pixel Pipeline NAND) protocol with 16 KB pages and 128 pages per block across 2 Chip Enables (CEs). Flash storage is logically partitioned into three distinct zones:

1. **Zone 0: Raw Physical Boot Blocks (Outside FTL / MBR):**
   * CE0 Block 0 rows 0..49 (`0x00..0x31`) and Block 1 backup rows 256..305 (`0x100..0x131`).
   * Houses LLB (`N31.bootloader.rb3`, ~149 KB) loaded directly by BootROM into IRAM (`0x22000000`).
2. **Zone 1: Reserved Boot Area / Partition 2 ("Firmware.MSE", LBAs 0..49215 / 192.25 MB):**
   * Precedes the user data FAT32 volume (4096-byte sectors).
   * **LBA 69 (offset `0x00045000`):** MSE Partition Directory (11 image records).
   * **LBA 71 (offset `0x00047000`):** `NAND:disk` (UEFI firmware volume / `BdsN31.dll`, 674 KB).
   * **LBA 240..753 (offset `0x000f0000..0x002f1063`):** `NAND:diag` (Apple DiagOS image, 2.10 MB).
   * **LBA 44789 (offset `0x0aef5000`):** `NAND:osos` (RetailOS kernel image, 9.46 MB).
3. **Zone 2: User Data Partition / Partition 3 (LBAs 49216..3857034 / ~14.7 GB):**
   * Standard FAT32 filesystem (`/dev/sdb1`), containing user applications, media, and homebrew files (`/Apps/`).

### 2.3 Diagnostic Load Target
* `NAND:diag` is configured with physical load address **`0x08000000`** (the base of DRAM).
* This is identical to `NAND:osos` (RetailOS). In diagnostic mode, DiagOS has exclusive access to the full **64 MB DRAM** address space (`0x08000000..0x0bffffff`).

---

## 3. Known Files and Structures Involved

### 3.1 MSE Partition Directory Entry (`DNANgaid`)
Located at offset `0x00045028` in `reserved.bin` (Entry 1, 40 bytes):
* **Volume Tag:** `DNANgaid` (b'DNANgaid' -> 'diag' on NAND)
* **Partition Offset:** `0x000b0000` (relative to Partition 2 base `0x40000`, absolute byte offset `0x000f0000`, LBA 240)
* **Partition Size:** `0x00201063` (2,101,347 bytes / 2.00 MB)
* **Load Address:** `0x08000000` (Physical DRAM base)
* **Flags:** `0x0001e000`
* **End Marker:** `0xffffffff`

### 3.2 Apple "8740" Container Structure (LBA 241, Offset `0x000f1000`)
The `NAND:diag` payload is encapsulated in Apple's proprietary "8740" container:
* **Header (`0x400` bytes):**
  * `+0x00`: Magic `ASCII "8740"` (`0x38373430`)
  * `+0x04`: Version `ASCII "2.0\0"` (`0x322e3000`)
  * `+0x07`: Image Type `0x03` (AES-128-CBC GID hardware encrypted)
  * `+0x08`: Entry Offset `0x00000000`
  * `+0x0c`: Payload Length $P = \text{0x00200000}$ (2,097,152 bytes = 2.00 MB)
  * `+0x10`: Body Length `0x00200c63` (2,100,323 bytes)
  * `+0x14`: Certificate Offset `0x00200080`
  * `+0x18`: Certificate Length `0x00000be3` (3,043 bytes)
  * `+0x40`: Auth Tag (16-byte AES-CBC MAC over header bytes `0..0x40`: `3deceea1643df986138ff52b3d836e01`)
* **Ciphertext Payload:** Bytes `0x000f1400..0x002f1400` (2.00 MB AES-128-CBC encrypted code).
* **RSA-1024 Signature:** Offset `0x002f1400` (128 bytes, PKCS#1 v1.5 signature over `header + payload`).
* **Certificate Chain:** Offset `0x002f1480..0x002f2063` (3 X.509 certificates: S5L8740 Secure Boot CA -> iPod CA -> Apple Root CA).

### 3.3 Key UEFI Modules Involved
* `BdsN31.dll` (DRAM `0x0bc2d000`): Performs button query (`0x0bc2e56c`), builds diagnostic candidate list (`0x0bc31e94`: `FV/FVDIAG.Fv`, CYGWIN path, `diag`), and invokes `gBS->LoadImage` / `gBS->StartImage` (`0x0bc2fc98`).
* `Image1FSReadOnly.dll` (DRAM `0x0bc71000`): Locates MSE directory records and reads the container from flash into memory.
* `ROMBootValidator.dll` (DRAM `0x0bc77000`):
  * `0x0bc789a4`: Validates container header and checks Auth Tag.
  * `0x0bc78658`: Body validator; computes SHA-1 over `header + payload`.
  * `0x0bc78714`: Enforces **RSA-1024 signature verification** against the certificate chain.
  * `0x0bc7878a`: Hardware AES-128-CBC decryption via silicon engine MMIO `0x38c00000`.

---

## 4. Homebrew Bootloader Chainloading & Stock OS Preservation

### 4.1 Infeasibility of Flashing Custom Payloads to `NAND:diag`
Analysis of `ROMBootValidator.dll` disproves the hypothesis that `NAND:diag` can be used as an open coldboot slot:
1. **Verification Order:** In `ROMBootValidator.dll` (`0x0bc78658`), the RSA-1024 signature verification (`0x0bc78714`) is executed **before** checking container type (`0x0bc7876c`).
2. **Type 4 (Plaintext) Myth:** While Type 4 containers skip AES decryption (`0x0bc7878a`), they **still require a valid Apple RSA-1024 signature**. Flashing an unsigned homebrew Type 4 image triggers an immediate verification abort (`0x8000001a`).
3. **No Signature Bypass in Diag Path:** The diagnostic candidate loop in BDS calls the exact same `gBS->LoadImage` and `ROMBootValidator.dll` entry points as RetailOS (`osos`).

### 4.2 Potential Diag-Based Chainloader (Stock DiagOS Exploitation)
If a coldboot path through diagnostic mode is strictly required, it cannot be done by overwriting the partition. Instead, it requires:
1. **Hardware AES Decryption Oracle (`DiagCrypt`):** RetailOS userland homebrew uses kernel AES driver `0x0841140c` / MMIO `0x38c00000` with Key Type 1 (hardware GID key) to decrypt `NAND:diag` into plaintext `diag-decrypted.bin` on Partition 3 FAT32.
2. **Vulnerability Audit:** Audit the decrypted 2.00 MB DiagOS binary for parsers accessible via Lightning UART serial console, USB diagnostic packets, or FAT32 staging.
3. **Weaponized Chainload:** On boot chord, UEFI authenticates and boots stock DiagOS; the exploit triggers inside DiagOS and warm-chainloads the homebrew payload.

### 4.3 Recommended Architecture: OS-Level Chainloader (Warm Kexec Takeover)
The verified, safest, and most practical approach for dual-booting without modifying pre-OS firmware partitions is the **OS-Level Chainloader**:

```
+--------------------------------------------------------------------------+
| 1. Stock Boot Chain (BootROM -> LLB -> UEFI -> RetailOS)                |
|    - 100% factory signed, unmodified NAND partitions                     |
+--------------------------------------------------------------------------+
                                     |
                                     v
+--------------------------------------------------------------------------+
| 2. Dual-Boot Selector (Resident Daemon /Apps/Startup/resident-B.hbapp)    |
|    - Cold boot trap: Polls Volume Down button before SpringBoard loads    |
|    - If held (or selected in Chainloader.hbapp): initiates Takeover      |
|    - If released: boots standard stock RetailOS UI immediately           |
+--------------------------------------------------------------------------+
                                     |
                                     v
+--------------------------------------------------------------------------+
| 3. Chainloader 6-Phase Execution Pipeline                                |
|    - Phase 1: Stage target payload (/Apps/DualBoot/payload.bin) in high  |
|               DRAM (0x09500000..0x0b500000) and verify checksum.         |
|    - Phase 2: Escalate privilege from User (CPSR 0x10) to SVC (CPSR 0x13)|
|               via kernel SVC table hook at 0x08917714 (svc #0x46).       |
|    - Phase 3: Quiesce hardware DMA & peripherals:                        |
|               * FMC/FMSS flash controller (0x38a00000): poll idle,       |
|                 disable IRQs (+0xc10=0), ack status, deselect bus.       |
|               * DMA engines (0x38200000 / 0x38700000): clear run bit,   |
|                 drain FIFOs, zero NEXT link.                             |
|               * LCD (0x38900000), GPU (0x3b000000), ADSP (0x38500000).  |
|               * Timers (0x3c700000): disable all 8 channels (CMD=0).     |
|               * VIC0/VIC1 (0x38e00000/1000): mask all lines (INTENCLEAR).|
|    - Phase 4: Switch execution to private high IRAM (0x22008400), set    |
|               VBAR = 0x22008000, set SP = 0x22010000.                   |
|    - Phase 5: MMU & Cache Teardown (CP15 from IRAM):                     |
|               * Clean & invalidate D-Cache by Set/Way (CCSIDR geometry). |
|               * Clear SCTLR M, C, I, Z, V bits (disable MMU and caches). |
|               * Invalidate TLB, I-cache, and branch predictor (DSB/ISB). |
|    - Phase 6: Relocate payload from staging buffer to physical DRAM base |
|               0x08000000, clear BSS, setup ARM register ABI (r0=0,       |
|               r1=0x1097, r2=Handoff Block @ 0x22009000), and branch.     |
+--------------------------------------------------------------------------+
                                     |
                                     v
+--------------------------------------------------------------------------+
| 4. Homebrew OS (Linux / Rockbox)                                         |
|    - Owns full 64 MB DRAM (0x08000000..0x0bffffff) with MMU identity.   |
+--------------------------------------------------------------------------+
```

---

## 5. Risks and Open Questions

### 5.1 Risk Analysis
| Risk | Severity | Mechanism | Mitigation |
|---|---|---|---|
| **Hard Brick** | **Critical** | Writing unsigned payloads to raw NAND blocks (LLB) or Partition 2 (`NAND:disk`/`osos`). BootROM/LLB signature verification failure leaves device permanently in DFU mode with no public exploit. | **Strictly avoid writing Zones 0 & 1.** All dualboot files remain exclusively on Partition 3 FAT32 (`/Apps/`). |
| **FTL / Partition Corruption** | **High** | Direct FMC writes without PPN OOB/ECC metadata calculation will corrupt Whimory block translation tables. | Do not use raw FMC writes on system partition boundaries; use standard FAT32 file APIs. |
| **AXI Bus Master Hang** | **Medium** | Relocating memory while FMC, DMA, or LCD controllers have pending bus transactions corrupts DRAM. | Explicitly wait for idle status, clear run bits, clear descriptor NEXT links, and acknowledge status registers before IRAM switch. |
| **Watchdog Panic** | **Medium** | Watchdog timer at `0x3c800000` expires during kernel takeover, resetting the device. | Disable watchdog or reload with maximum timeout (30s) and pass register base in Handoff Block. |

### 5.2 Open Questions
1. **Stock DiagOS Decryption & Attack Surface:** Has `DiagCrypt` been run on physical hardware to decrypt `NAND:diag`? Auditing `diag-decrypted.bin` will reveal whether DiagOS can serve as a coldboot chainloader via Lightning UART or USB commands.
2. **Watchdog Disable Semantics:** The exact register sequence for a permanent hardware disable of the `0x3c800000` watchdog (vs feeding it periodically) requires final validation in emulator traces.
3. **Hardware Driver Ports for Homebrew OS:**
   * **PowerVR MBX Lite GPU:** Command stream format and clock gating (`CLKCON 0x3c500000`).
   * **Bluetooth / FM Radio:** Broadcom BCM2078 initialization via UART (`0x081920ac`).
   * **Audio Output:** SPI codec protocol (IRQ 9) and ADSP co-processor mailbox interface.
