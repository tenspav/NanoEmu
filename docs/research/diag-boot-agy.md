1. DIAG BOOT DECISION & BOOTROM NAND BOOT PATH
- Check location: NOT in Boot ROM. Boot ROM only checks warm boot (PMGR 0x3D100004 bit 18) and DFU (GPIO bank 11 pin 3 low via 0x20001e10 + USB PHY 0x3C400028 bit 0). Button checks (Sleep+Home reset -> hold VolDown/VolDown+Sleep for diag) are evaluated in LLB (2nd bootloader).
- Disassembly (0x2000336c): Loops CE0-3 (r8), Block 0 (sb=0) and Block 1 backup (sb=1, page 256). Reads 3 pages into IRAM 0x22025000 via FMC 0x38A00000 (0x20009388).
- Checks: Header auth tag at +0x40 (0x200006ac); Magic "8740" (0x38373430); Type 3 (AES encrypted) or 4 (plain) at 0x20003418; Body length <= 0x24c00 (147KB) at 0x20003428.
- Payload read: Pages 3..N read in 0xc00-byte chunks into IRAM 0x22000800.
- Crypto: SHA-1 (0x38000000) over header+payload; RSA-1024 (0x3D000000) sig verified at payload+P (0x200007d4) against certs at payload+cert_offset; AES-128 (0x38C00000) decrypts type 3.
- Handoff: Jumps to IRAM 0x22000000 + entry_offset (0x200034cc -> 0x20003b14). Boot ROM loads ONLY LLB (N31.bootloader.rb3, 149KB).

2. DIAGNOSTICS PAYLOAD ON NAND
- Separate payload: YES. Firmware.MSE partition table (0x5000) defines dedicated partition "NAND:diag" (size 0x201063, 2.1MB), distinct from "NAND:osos" (RetailOS, 9.9MB) and "NAND:disk" (670KB).
- Load target: Load address 0x08000000 (DRAM base, identical to RetailOS osos!). Full 64MB RAM available.
- Layout: Lives in raw NAND Whimory FPart space preceding FTL user space (ipod-disk.dd starts at FTL LBA 49216 / 192MB offset; sectors 1-40 on dd are MBR/FAT padding).

3. SIGNATURE & ENCRYPTION
- Format: Apple "8740" container (0x400-byte header, AES auth tag, RSA-1024 PKCS#1 v1.5 sig, SHA-1).
- Type: Official image is Type 3 (AES-128-CBC encrypted via hardware GID key).
- Patchability: Untethered jailbreak currently runs via OS-level persistence (/Apps/Startup/resident-*.hbapp). If LLB is unpatched, custom payloads must either be loaded by a patched LLB or exploit LLB's parser; if LLB permits Type 4 (plaintext) or lacks strict cert pinning on diag, unsigned code executes directly.

4. NAND PLACEMENT & WRITE PATH
- Location: Whimory "NAND:diag" slice on raw flash blocks (referenced in Whimory FPart / signature).
- Write path: Written from homebrew using raw FMC MMIO (0x38A00000) / FMSS sequencer (0x38A00C00) or OS Whimory driver primitives (m2fmssWrite2ndBootLoader / _EmulateWriteBLPage at 0x0812B320).

5. VERDICT & NEXT STEPS
- Verdict: HIGHLY FEASIBLE. Diag mode is an independent 2MB bare-metal payload loading to 0x08000000 with full 64MB DRAM, leaving RetailOS intact for clean dual boot.
- Steps:
  1. Dump "NAND:diag" and LLB from NAND using an FMC/Whimory dumper homebrew app.
  2. Disassemble LLB button handler & diag load routine to verify signature enforcement / Type 4 support.
  3. Package bare-metal payload with 8740 header targeting 0x08000000, write to NAND:diag, and boot holding VolDown.
