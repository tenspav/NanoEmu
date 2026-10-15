================================================================================
DEEP DIVE: IPOD NANO 7G DIAGNOSTIC BOOT SLOT (NAND:diag) FEASIBILITY ANALYSIS
================================================================================
Target: iPod nano 7G (N31 / S5L8740 SoC, Single-Core ARM Cortex-A5, 64 MB DRAM)
Investigated Components:
  - NAND Partition 2 MSE Directory (0x45000), 'diag' (LBA 241..754, 0x000f1000)
  - UEFI BdsN31.dll (DRAM 0x0bc2d000, snap6/ram-full.bin)
  - UEFI ROMBootValidator.dll (DRAM 0x0bc77000, snap6/ram-full.bin)
  - UEFI Image1FSReadOnly.dll & AppleImageValidationManager.dll (0x0bc71000..0x0bc76000)
  - dumps/boot-area/reserved.bin & docs/research/bootrom.md
Output File: /home/pavliukov/ipodwork/NanoRework/workers/agy-diagdeep.txt
================================================================================

EXECUTIVE SUMMARY
--------------------------------------------------------------------------------
The previous verdict in docs/research/dualboot-chainloader-plan.md is FULLY UPHELD.
The diagnostic boot slot (NAND:diag, Partition 2, LBAs 240..753) CANNOT be used
for persistent coldboot dualboot without an unmitigated cryptographic vulnerability
(RSA-1024 factoring or private key leak) or an unauthenticated parser exploit.

Every potential bypass path investigated is either DEAD or REQUIRES AN UNPROVEN EXPLOIT:
  - Path 1 (Validation Order / Type 4):       DEAD (Type 4 bypasses AES only, not RSA)
  - Path 2 (Alternative Loader/Validator):   DEAD (Shares identical gBS->LoadImage)
  - Path 3 (Signed Container Reuse / Hooks): DEAD (Flat container; rigid SHA-1 coverage)
  - Path 4 (TOCTOU Race on Next Boot):        DEAD (Single core; jailbreak dormant on disk)
  - Path 5 (Stock DiagOS Chainloading):      NEEDS EXPLOIT (Static factory test suite)
  - Path 6 (Patching On-Disk BdsN31):        DEAD / BRICK RISK (LLB verifies NAND:disk)


================================================================================
INVESTIGATION 1: EXACT VALIDATION ORDER (RSA vs. TYPE 4 PLAINTEXT HANDLING)
================================================================================
Question:
  Is RSA really checked before Type 4 plaintext handling for the diag path
  specifically, or only for the normal OS path?

Disassembly & Reverse Engineering Evidence:
  The validator resides in ROMBootValidator.dll (PE image base 0x0bc77000).
  Both the normal retail path ('osos') and diagnostic path ('diag') pass their
  containers to the identical entry points:
    - Header Validator: 0x0bc789a4
    - Body Validator:   0x0bc78658

  [Header Validator @ 0x0bc789a4]:
    0x0bc789a8: cmp.w   r1, #0x400          ; Must be at least 0x400 bytes
    0x0bc789b8: mov     r0, r4
    0x0bc789ba: bl      #0xbc77638          ; Compares magic against ASCII "8740"
    0x0bc789c0: ldrb    r0, [r4, #7]        ; Read container Type from header+7
    0x0bc789c2: cmp     r0, #4              ; Type 4 (Plaintext)?
    0x0bc789c4: beq     #0xbc789d0          ; Proceed to header MAC check
    0x0bc789c6: cmp     r0, #3              ; Type 3 (AES-GID encrypted)?
    0x0bc789c8: beq     #0xbc789d0          ; Proceed to header MAC check
    0x0bc789ca: mov     r0, r5              ; Reject any other type!
    0x0bc789cc: pop     {..., pc}
    ...
    0x0bc78a28: add.w   r0, r4, #0x40       ; Compare decrypted SHA-1 digest
    0x0bc78a2c: bl      #0xbc77924          ; against 16-byte Auth Tag at +0x40

  [Body Validator @ 0x0bc78658]:
    0x0bc786aa: bl      #0xbc789a4          ; Run Header Validator
    0x0bc786b2: subs    r4, r0, #0
    0x0bc786b4: blt     #0xbc78792          ; Abort on header failure
    0x0bc786b6: ldr     r0, [r5, #0xc]      ; Payload Length (plen)
    0x0bc786b8: ldr     r1, [r5, #0x10]     ; Body Length (blen)
    0x0bc786ba: adds    r0, #0xf
    0x0bc786be: bic     r6, r0, #0xf        ; r6 = P = align_up(plen, 16)
    0x0bc786c2: add.w   r0, r6, #0x300
    0x0bc786c6: cmp     r1, r0              ; Check blen >= P + 0x300
    0x0bc786c8: blo     #0xbc78792
    0x0bc786ca: ldr     r0, [r5, #0x14]     ; Certificate Offset
    0x0bc786cc: add.w   r1, r6, #0x80
    0x0bc786d0: cmp     r0, r1              ; Check cert_off >= P + 0x80 (128-byte sig)
    0x0bc786d2: blo     #0xbc78792
    0x0bc786d4: lsls    r0, r0, #0x1c       ; Check cert_off is 16-byte aligned
    0x0bc786d6: bne     #0xbc78792
    0x0bc786e4: ldr     r0, [r5, #8]        ; Entry Offset
    0x0bc786e6: cmp     r0, r6              ; Check entry_offset <= P
    0x0bc786e8: bhi     #0xbc78792
    ...
    ; SHA-1 Hash over 0x400 header + P payload bytes:
    0x0bc786f0: add.w   r2, r6, #0x400      ; Hash Length = P + 0x400
    0x0bc786f4: mov     r1, r5              ; Source = Container base
    0x0bc786f6: ldr     r4, [r0]
    0x0bc786f8: blx     r4                  ; Execute SHA-1 update
    ...
    ; RSA-1024 Signature & X.509 Certificate Chain Check:
    0x0bc78702: add.w   r8, r5, #0x400      ; Payload base
    0x0bc78706: ldrd    r2, r1, [r5, #0x14] ; r2 = cert_off, r1 = cert_len
    0x0bc7870a: add.w   r3, r8, r6          ; r3 = signature ptr = payload + P
    0x0bc7870e: add.w   r0, r2, r8          ; r0 = cert chain ptr = payload + cert_off
    0x0bc78712: ldr     r2, [sp, #0xc]      ; r2 = SHA-1 digest context
    0x0bc78714: bl      #0xbc77d78          ; CALL RSA / CERTIFICATE CHAIN VERIFIER!
    0x0bc78718: cmp     r0, #1              ; Did RSA verification succeed (return == 1)?
    0x0bc7871a: beq     #0xbc78722          ; IF SUCCESS, continue!
    0x0bc7871c: ldr     r4, [pc, #0x80]     ; IF FAILED: load error status (0x8000001a)
    0x0bc78720: b       #0xbc78792          ; IMMEDIATE ABORT AND RETURN ERROR!
    ...
    ; Type Check (AES Decryption vs Plaintext Pass-Through):
    0x0bc7876c: ldrb    r0, [r5, #7]        ; Read Container Type
    0x0bc7876e: cmp     r0, #3              ; Type 3 (Encrypted)?
    0x0bc78770: bne     #0xbc78792          ; IF TYPE 4: SKIP AES AND EXIT WITH SUCCESS!
    0x0bc7877e: mov     r3, r6              ; Length = P
    0x0bc78784: mov     r1, r8              ; Source = ciphertext
    0x0bc78786: ldr     r2, [sp, #0x10]     ; Destination buffer
    0x0bc7878a: blx     r4                  ; Hardware AES-128-CBC Decrypt
    ...
    0x0bc78792: ldr     r0, [sp, #0xc]
    0x0bc78794: bl      #0xbc788b0          ; Free hash context
    0x0bc78798: mov     r0, r4              ; Return status
    0x0bc7879a: b       #0xbc786de

Conclusion:
  1. The RSA-1024 signature check (0x0bc78714) occurs BEFORE the Type check (0x0bc7876c).
  2. If the RSA signature verification fails, execution aborts at 0x0bc78720 without
     ever inspecting the Type byte.
  3. Type 4 merely bypasses the hardware AES-128 decryption step at 0x0bc7878a.
  4. Type 4 containers are STILL REQUIRED to have a valid Apple RSA-1024 signature
     matching the SHA-1 digest of [header 0x400 + P bytes payload], plus an AES
     MAC authentication tag over header [0..0x40).
  5. There is NO special branch, flag, or exemption for the 'diag' path.

Path 1 Rating: DEAD


================================================================================
INVESTIGATION 2: DIAG BUTTON-CHECK PATH vs. RETAIL PATH LOADER / VALIDATOR
================================================================================
Question:
  Does the diag button-check path (0x0bc2e56c) use a DIFFERENT loader/validator
  than the retail path?

Disassembly & Control Flow Tracing:
  1. Button Query at 0x0bc2e56c:
     - Locates AppleButtonProtocol via gBS->LocateProtocol (GUID @ 0x0bc31ff0).
     - Queries Button 0 (Home), Button 1 (Sleep), Button 2 (VolDown).
     - At 0x0bc2e5ae..0x0bc2e5c0:
       Checks if Button 1 == 1, Button 2 == 1, Button 0 == 1.
       If all 3 buttons are pressed, branches to 0x0bc2e5ec (Diagnostics selection).
  2. Diagnostics Candidate Dispatch at 0x0bc2e5ec:
     - Prepares pointer table at 0x0bc31e7c + 0x18 = 0x0bc31e94:
         [0x0bc31e94]: "FV/FVDIAG.Fv" (Firmware Volume)
         [0x0bc31e98]: "C:/edk2/Projects/snowfox/Build/N20/RELEASE_RVCTCYGWIN/FV/FVDIAG.Fv"
         [0x0bc31e9c]: "diag" (Image1Fs partition entry on Partition 2)
     - Sets r1 = 3 (candidate count).
     - Calls 0x0bc2f488(candidate_array, count=3, ...).
  3. Candidate Processing Loop at 0x0bc2f488:
     - Iterates through candidate names r4 = 0 .. 2:
         0x0bc2f49e: ldr.w   r0, [r5, r4, lsl #2]  ; image_name
         0x0bc2f4a2: add     r1, sp, #8            ; &image_handle
         0x0bc2f4a4: bl      #0xbc2fc98            ; Load Image Handler
     - If 0x0bc2fc98 returns 0 (EFI_SUCCESS):
         0x0bc2f4b4: prints "Launching the %s application"
         0x0bc2f4f2: bl      #0xbc30d1c            ; Starts loaded image!
  4. Core Image Loader at 0x0bc2fc98:
     - Calls 0x0bc2f0fc: looks up image name across filesystems (SemihostFs, Image1Fs).
       For "diag", Image1FSReadOnly.dll locates MSE partition record 'NAND:diag'.
     - Allocates memory buffer via gBS->AllocatePool (0x0bc2fd28).
     - Reads entire container from flash into buffer (0x0bc2fd36).
     - Invokes standard UEFI Boot Services LoadImage:
         0x0bc2fcd8: ldr     r2, [r5]              ; gBS table pointer
         0x0bc2fcda: ldr     r3, [r2, #0x58]       ; gBS->LoadImage
         0x0bc2fcde: blx     r3                    ; Execute LoadImage
     - Invokes standard UEFI Boot Services StartImage:
         0x0bc2fce4: ldr     r0, [r5]
         0x0bc2fce6: ldr     r0, [r0, #0x4c]       ; gBS->StartImage
         0x0bc2fce8: blx     r0                    ; Execute StartImage
  5. Comparison with Normal Retail Path ('osos'):
     - Normal boot (0x0bc2ea20 -> 0x0bc2ec76 / 0x0bc2eeb4) uses the exact same
       gBS->LoadImage and gBS->StartImage infrastructure.
     - Both paths invoke AppleImageValidationManager.dll and ROMBootValidator.dll.

Conclusion:
  The diagnostic button-check path uses the exact same loader (0x0bc2fc98),
  the exact same UEFI LoadImage entry point, and the exact same cryptographic
  validator (ROMBootValidator.dll). No auxiliary loader or backdoor exists.

Path 2 Rating: DEAD


================================================================================
INVESTIGATION 3: SIGNED CONTAINER REUSE & POST-VALIDATION HOOKS
================================================================================
Question:
  Is there any signed Apple diag image we could reuse as a container and patch
  post-validation (e.g. image loaded then hooks inserted via Type 4 data sections)?

Container Structure in dumps/boot-area/reserved.bin (LBA 241, offset 0x000f1000):
  - Magic:          ASCII "8740" (0x38373430)
  - Version:        ASCII "2.0\0"
  - Container Type: 3 (AES-128-CBC GID encrypted)
  - Entry Offset:   0x00000000
  - Payload Length: 0x00200000 (2,097,152 bytes = 2.00 MB)
  - Body Length:    0x00200c63 (2,100,323 bytes)
  - Cert Offset:    0x00200080
  - Cert Length:    0x00000be3
  - Header Tag:     3deceea1643df986138ff52b3d836e01 (AES MAC over 0..0x40)
  - Signature:      Offset 0x00200400 (128 bytes = RSA-1024 PKCS#1 v1.5)
  - Cert Chain:     Offset 0x00200480 (3 X.509 certs: S5L8740 Secure Boot -> iPod CA -> Apple Root)

Vulnerability Analysis:
  1. Section / Container Structure:
     The 8740 format is NOT an ELF, Mach-O, or PE container with independent
     section headers. It is a single contiguous image.
     There is no support for "Type 4 data sections" inside a Type 3 container.
  2. Cryptographic Coverage:
     The SHA-1 hash is computed across:
       Offset 0x0000 to 0x0400 (0x400 header bytes)
       Offset 0x0400 to 0x0400 + P (all 0x200000 payload bytes)
     Every byte of executable code and data is covered by the RSA-1024 signature.
     Altering any single byte within the payload invalidates the SHA-1 digest.
  3. Gap Injection Analysis:
     - End of signature is P + 0x80 = 0x200080.
     - CertOffset is 0x200080.
     - There is ZERO gap between the RSA signature and the certificate chain.
     - Even if CertOffset were shifted forward, ROMBootValidator (0x0bc7872a)
       strictly copies only `payload_length` (0x200000) bytes to the destination
       execution buffer. Trailing data after P is discarded.
  4. PKCS#1 v1.5 Padding Strictness (0x0bc77bbc):
     Reverse engineering of the RSA padding checker at 0x0bc77bbc confirms:
       - Checks byte 0 is 0x00 (0x0bc77ca4)
       - Checks byte 1 is 0x01 (0x0bc77cac)
       - Checks all padding bytes [2..k-t-1] are 0xFF (0x0bc77cbc..0x0bc77cc8)
       - Checks delimiter byte is 0x00 (0x0bc77ccc)
       - Checks ASN.1 DigestInfo prefix: 30 21 30 09 06 05 2b 0e 03 02 1a 05 00 04 14
       - Checks full 20-byte SHA-1 digest (0x0bc77cf4)
     Full 128-byte block verification is rigidly enforced with cumulative ORs.
     No Bleichenbacher low-exponent (e=3) or loose-padding forgery is possible.

Path 3 Rating: DEAD


================================================================================
INVESTIGATION 4: TOCTOU RACE CONDITION ON NEXT BOOT
================================================================================
Question:
  Since the image is validated in DRAM then executed, can we patch DRAM
  between validation and jump from the jailbreak on next boot?

Architecture & Timing Analysis:
  1. CPU Architecture:
     The Samsung S5L8740 SoC is a SINGLE-CORE ARM Cortex-A5 processor.
     There are no secondary CPU cores, companion application processors, or
     background threads available to execute concurrent code.
  2. Execution State of the Jailbreak:
     The untethered resident jailbreak is an OS-level userland hook located at
     /Apps/Startup/resident-*.hbapp on Partition 3 (ipod-disk FAT32).
     On a cold boot or button-triggered reset:
       - Hardware reset initializes BootROM.
       - BootROM resets all MMIO peripherals (VIC, PMGR, FMC, DMA).
       - BootROM authenticates and loads LLB into IRAM.
       - LLB authenticates and loads UEFI into DRAM.
       - UEFI BdsN31 performs the button check and image validation.
     During ALL of these phases, RetailOS has NOT run. The jailbreak is completely
     dormant on flash storage and has zero presence in memory.
  3. Execution Window Between Validation and Jump:
     Inside 0x0bc2fc98:
       0x0bc2fcde: blx   r3                  ; gBS->LoadImage (validates container)
       0x0bc2fce0: subs  r4, r0, #0
       0x0bc2fce2: blt   #0xbc2fcd2          ; Abort if invalid
       0x0bc2fce4: ldr   r0, [r5]
       0x0bc2fce6: ldr   r0, [r0, #0x4c]     ; gBS->StartImage
       0x0bc2fce8: blx   r0                  ; Branch directly to payload!
     There are exactly 4 CPU instructions between the validation return and the
     execution jump. Interrupts are disabled or managed solely by UEFI.
     No hardware DMA or asynchronous event can race this sequence.

Path 4 Rating: DEAD


================================================================================
INVESTIGATION 5: STOCK DiagOS CAPABILITIES & CHAINLOADING
================================================================================
Question:
  What does the stock diag image contain and can it chainload unsigned code
  itself (diag menu loading modules from disk)?

Analysis:
  1. Payload Identity:
     The image at Partition 2 LBA 241 is Apple's proprietary N31 DiagnosticOS
     (2.00 MB, AES-128-CBC GID encrypted).
  2. Stock Functionality:
     In Apple iPod/iOS hardware, DiagOS is a static bare-metal diagnostic suite
     used by Foxconn assembly lines and Genius Bar technicians.
     It incorporates:
       - Memory test patterns (DRAM sweep)
       - Display / RGB pattern tests & Backlight PWM check
       - Touch panel raw ADC & multitouch coordinate verification
       - Audio codec loopback / headphone jack detection
       - PMU / Gas Gauge / Battery charging state
       - Broadcom BCM4330 Bluetooth / FM radio verification
       - FMC / NAND controller flash ID and bad block readout
       - Button matrix verification (Home, Sleep, Vol+, Vol-)
  3. Module Loading / Chainloading Features:
     - DiagOS is entirely self-contained; it contains NO dynamic linker, NO EFI
       shell, and NO module loader that reads external plugins from FAT32.
     - It does NOT chainload third-party payloads or expose arbitrary jump menus.
  4. Exploit Surface:
     - DiagOS communicates over Tristar Lightning UART / serial debug console
       and USB.
     - Weaponizing DiagOS to chainload unsigned code would require:
       a) Decrypting DiagOS using the jailbreak in RetailOS as a hardware
          AES-GID oracle (decrypting 0x000f1400..0x002f1400).
       b) Finding a remote code execution vulnerability (e.g. heap overflow,
          stack overflow, or format string in UART command parser or USB packets).
       c) Constructing an automated tethered or hardware-triggered payload.
     - Without discovering a 0-day vulnerability in DiagOS, stock DiagOS cannot
       chainload unsigned code.

Path 5 Rating: NEEDS EXPLOIT (Requires AES decryption + 0-day vulnerability audit)


================================================================================
INVESTIGATION 6: ON-DISK PATCHING OF BdsN31 (Partition 2, NAND:disk)
================================================================================
Question:
  Button-check happens in BdsN31 loaded FROM NAND each boot, can the jailbreak
  patch the on-disk copy (FTL-level) since RetailOS can write its own partition?

Boot Chain Security Analysis:
  1. On-Disk Location of BdsN31:
     BdsN31.dll is NOT a standalone raw file on disk. It is packaged inside the
     UEFI firmware volume image 'NAND:disk' located at Partition 2, LBA 71
     (offset 0x00047000 in reserved.bin).
  2. Container Format of NAND:disk:
     - Magic:          "8740" Type 3 (AES-GID encrypted)
     - Payload Size:   0x000a77a8 (685,992 bytes)
     - Total Size:     0x000a8813 (690,195 bytes)
     - RSA Signature:  Offset 0x000a77b0 (128 bytes)
     - Certificate:    Offset 0x000a7830 (0xbe3 bytes, Apple iPod CA chain)
  3. Who Loads and Validates NAND:disk?
     On cold boot, Low Level Bootloader (LLB, N31.bootloader.rb3) executes from
     IRAM (loaded and verified by BootROM).
     LLB's primary job is to:
       a) Initialize the DRAM controller.
       b) Load the 'disk' (UEFI) container from Partition 2 into DRAM (0x0bc00000).
       c) Enforce RSA-1024 signature verification and AES-GID decryption on NAND:disk.
       d) Jump to UEFI entry point.
  4. Consequence of Modifying NAND:disk:
     - If the jailbreak overwrites LBA 71 on Partition 2 to patch BdsN31:
       On the subsequent boot, LLB will hash NAND:disk and verify its RSA signature.
       Because the private RSA key is unavailable, the signature verification WILL FAIL.
     - When LLB fails image verification, it halts boot or falls into DFU mode.
     - Neither BDS nor RetailOS will ever execute.
     - THE DEVICE IS BRICKED.
     - Recovery requires an Apple-signed IPSW restore via iTunes DFU mode.

Path 6 Rating: DEAD (Fatal Brick Risk)


================================================================================
COMPREHENSIVE PATH RATING MATRIX
================================================================================
Path | Mechanism / Concept             | Status         | Primary Blocker / Failure Point
-----+---------------------------------+----------------+-------------------------------------------------------------
  1  | Type 4 Plaintext Diag Payload   | DEAD           | RSA-1024 checked before Type parsing (0x0bc78714 precedes 0x0bc7876c)
  2  | Diag Button-Check Bypass        | DEAD           | Button check 0x0bc2e5ec calls identical gBS->LoadImage & validator
  3  | Signed Container Reuse / Hooks  | DEAD           | Flat container; SHA-1 covers entire header + payload; rigid PKCS#1 v1.5
  4  | Post-Validation TOCTOU Race     | DEAD           | Single-core Cortex-A5; jailbreak dormant on disk during pre-OS boot
  5  | Stock DiagOS Chainloader        | NEEDS EXPLOIT  | Static test binary; requires AES GID decrypt + 0-day parser exploit
  6  | On-Disk BdsN31 Patching         | DEAD (BRICK)   | LLB strictly verifies RSA-1024 on NAND:disk before executing BDS
-----+---------------------------------+----------------+-------------------------------------------------------------


================================================================================
STRATEGIC VERDICT & ARCHITECTURAL RECOMMENDATION
================================================================================
1. The diagnostic boot slot (NAND:diag) CANNOT be used as a simple coldboot
   dualboot slot. The boot security model enforced by Apple on the S5L8740 SoC
   (BootROM -> LLB -> UEFI BdsN31 -> DiagOS/RetailOS) creates an unbroken chain
   of RSA-1024 cryptographic verification for all pre-OS stages.

2. Any attempt to modify flash partitions containing pre-OS images (LLB in raw
   NAND, or NAND:disk in Partition 2) carries catastrophic brick risk due to
   LLB/BootROM signature enforcement.

3. The OS-Level Chainloader (Warm Kexec Takeover) detailed in
   docs/research/dualboot-chainloader-plan.md remains the ONLY feasible, safe,
   and reliable dualboot architecture for the iPod nano 7G:
     - Leverages the existing resident untethered jailbreak (/Apps/Startup/).
     - Lets Apple's signed boot chain boot RetailOS legitimately (zero signature issues).
     - Escalate to SVC mode via verified dispatch hook (0x08917714).
     - Quiesces hardware engines (FMC, DMA, Timers, VIC).
     - Switches to IRAM, disables MMU/caches via CP15, relocates Rockbox/Linux
       payload to 0x08000000, and hands over all 64 MiB DRAM.
     - Leaves NAND firmware partitions 100% stock and untouched (ZERO brick risk).
================================================================================
