# S5L8747 Cold Boot & Type-4 Firmware Analysis (Zero-Device Preservation)
1. IMAGE RECONSTRUCTION & CONTAINER FORMAT:
- 8740 Container Spec (0x400 header): +0x00 magic "87402.0\0"; +0x04 epoch; +0x07 Type=4 (plaintext);
  +0x08 entry_off (<=P); +0x0c payload_len L; +0x10 body_len (>=P+0x300); +0x14 cert_off (>=P+0x80, 16B-align);
  +0x18 cert_len (0 if patched); +0x40 AES auth tag (16B). Body: payload @ +0x400 (P=align16(L)), RSA sig @ +0x400+P
  (128B), cert @ +0x400+cert_off. With validator bypass, auth tag, RSA sig, and cert bytes can be all zeros.
- RetailOS ('osos'): FULLY SOURCED. Carve first 0x974160 bytes from retailos-08000000.bin (total 0x980000);
  maps to reserved.bin @ 0x0aef5000 (payload @ 0x0aef5400, load/entry @ 0x08000000). Contains live RTXC text/data.
- UEFI ('disk'): PARTIALLY SOURCED. Original monolithic SEC/PEI load image @ 0x08000000 (L=0xa77a8, reserved
  0x47000) was overwritten by RetailOS. High DRAM (0x0bc00000..0x0bc90000 in snap6/ram-full.bin) preserves
  relocated runtime PE modules (BdsN31 @ 0x0bc2d000) and FV @ 0x0bc5d010, but not the un-relocated cold startup.
- LLB ('rb3'): NOT RECOVERABLE from RAM dumps. In snap6/iram.bin & bt-research iram-22000000.bin, entry
  0x22000000..0x22003fff is completely overwritten by RetailOS vectors (0x082f39c8 etc.) & Mountainbones banner.
2. VALIDATOR BYPASS & EXACT CHECK SITES (MINIMAL PATCHES):
- Emulated BootROM (vrom0.bin / 0x20000000, ARM32; file offset = addr - 0x20000000):
  * Header tag bypass @ 0x200006ac (off 0x6ac): 01 00 a0 e3 1e ff 2f e1 ('mov r0, #1; bx lr').
    (Or off 0x778: 05 00 00 ea ('b 0x20000794') to skip AES tag while keeping '8740' magic check).
  * Body RSA bypass @ 0x20000860 (off 0x860): 21 00 00 ea ('b 0x200008ec'). Skips SHA-1/RSA, preserves
    entry offset/metadata writes @ 0x200008ec..0x2000090c; Type 4 falls through to skip AES decrypt @ 0x20000918.
- Emulated UEFI Validator (snap6 ram-full.bin @ 0x0bc77000 PE, Thumb-2; RAM offset = addr - 0x08000000):
  * Header tag bypass @ 0x0bc789d0 (off 0x3c789d0): 00 20 fb e7 ('movs r0, #0; b 0x0bc789cc').
  * Body RSA bypass @ 0x0bc78714 (off 0x3c78714): 01 20 00 bf ('movs r0, #1; nop').
  * Type-4 memcpy fix @ 0x0bc7876c: Type 4 branches to cleanup without copying. Replace 0x0bc7876c with
    04 98 41 46 32 46 00 2a 05 d0 11 f8 01 3b 00 f8 01 3b 01 3a f9 d1 00 24 05 e0 (copies P bytes r8->[sp+0x10]).
3. MINIMAL DEVICE NEED & LLB NECESSITY EVALUATION:
- Minimal device need if requiring original ROM->LLB boot: Run homebrew AES-GID oracle on device once to
  decrypt N31.bootloader.rb3 (146KB). GID key readout is unnecessary; only decrypted plaintext is required.
- Honest check on LLB: LLB IS NOT NEEDED in an emulator. Hardware DRAM calibration (0x38501000), PLL locks,
  and raw NAND BCH ECC are hardware-only workarounds; QEMU maps flat DRAM directly at 0x08000000.
  A 5-instruction synthetic reset stub in IRAM setting CPSR 0xd3 and branching to DRAM completely replaces LLB.
4. CONCRETE FILE SET & ZERO-DEVICE COLD BOOT ROUTE:
- File Set: (1) retailos-08000000.bin (repackaged into Type 4 'osos' @ reserved.bin 0x0aef5000);
  (2) snap6/ram-full.bin (UEFI high-DRAM runtime 0x0bc00000..0x0c000000); (3) reserved.bin (MSE table LBA 69,
  rsrc FAT16 LBA 1010); (4) ipod-disk.dd (user data FAT32 @ global LBA 0xc040); (5) vrom0.bin (patched ROM).
- Emulator Changes & Checkpoints:
  Stage 1 (Reset): ROM bypasses tag/RSA; synthetic stub or reset vector enters UEFI BDS @ 0x0bc2d000 (or OS directly).
  Stage 2 (BDS): Button check (0x0bc2e56c=0) selects 'osos'. Validator bypass verifies Type 4 container, copies to 0x08000000.
  Stage 3 (RetailOS): BDS branches to 0x0800002c. RetailOS initializes RTXC, LCD blitter (0x3c100000), PMU/timers.
  Stage 4 (Storage & UI): Mounts FAT32 from reserved.bin+disk.dd; cold boots directly to Home Screen with zero device access.
