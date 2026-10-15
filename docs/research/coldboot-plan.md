# S5L8747 Cold-Boot Firmware Analysis & Architecture Plan (Nano 7G)
## 1. BootROM NAND Read Layout & LLB Geometry
- Probing: ROM 0x2000336c scans row bases B=0 (Blk 0), B=0x100 (Blk 1) across CE0..3 (0x200034d4); LLB resides on CE0.
- Strapping: 0x3c400028 bit 0 must be 0 (no USB cable) at 0x200037f4 to bypass DFU and enter NAND boot path.
- Raw Reader (0x20009388): Issues cmd 0x00, 5-byte addr (00 00 r_lo r_mid r_hi at 0x20009318), 0x30 confirm.
  Page read loops 3 sectors (0x20009464): 52B parity (+0x30=0x34, go 0x32) + 1024B data (+0x30=0x3ff, go 0x22).
  ECC decode (+0x804=0x040001e1, poll +0x810 bit 1, check +0x80c bit 0); DMAs 1024B to RAM (+0x34, go 0x1a0).
  Total raw page: 3 x (52 + 1024) = 3228 B raw, delivering 3072 B (0xc00) user data (per docs/research/ecc.md).
- LLB Layout: ROM reads rows B, B+1, B+2 into 0x22025000 (triple redundant 0xc00 header page; 0x200033c4-e0).
  Header page holds 0x400 header + first 0x800 body bytes; copied to IRAM entry 0x22000000 (0x20003440).
  Continuation: Rows B+3..B+50 (48 pages) stream remaining body (0x23c63 B) to 0x22000800 (0x20003464-84). Total 51 rows.
- IPSW rb3: N31.bootloader.rb3 (0x24863 B, body 0x24463 <= 0x24c00 limit at 0x20003428) is the EXACT LLB image.
  NAND Store: Map rows B..B+2 = rb3[0:0xc00]; rows B+3+j = rb3[0xc00+j*0xc00 : 0x1800+j*0xc00] (j=0..47).
  Emulator can bypass BCH decode by auto-ACKing +0x810 bit 1 / +0x80c=0 and DMA-ing plain 0xc00 pages.
## 2. Crypto Analysis: Patching vs Type 4 vs Faithful Emulation
- Validation Pipeline: BootROM and UEFI (0x0bc78658) enforce: (1) Header AES-CBC tag over [0:0x40) (0x200006ac);
  (2) SHA-1 over header + aligned ciphertext; (3) RSA-1024 cert chain (0x200008d4 / 0x0bc78714); (4) Type-3 AES decrypt.
- Type 4 Verdict: Type 4 ONLY skips payload AES decryption (0x20000914); header AES tag & RSA checks STILL FAIL.
- Patching: Least initial work: patch ROM 0x200006ac->1, 0x200007d4->1, 0x20000914 (skip AES); BUT requires patching
  all downstream validators too (LLB disk loader + UEFI osos loader 0x0bc78714).
- Most Faithful: Retain pristine signed Type 3 images; emulate MMIO SHA (0x38000000), AES (0x38c00000), RSA (0x3d000000).
  Requires device GID oracle to obtain decrypted payloads once, or GID keystore in QEMU AES engine.
## 3. Boot-Chain Checklist & MMIO Handoff State
- Cold -> BootROM: 0x3d100004 bit 18=0 (cold reset); 0x3c400028 bit 0=0 (no USB); IRAM/VROM mapped; clocks 0x3c500000.
  Status: VROM runs, generic regfiles exist; LACK: USB PHY bit 0 strapping, raw FMC ECC registers (+0x800..+0x820).
- BootROM -> LLB: Entry 0x22000000 (IRAM), CPSR=0xd3 (SVC), MMU/caches off, VIC0/1 masked (0x38e00014=0xffffffff),
  boot args at 0x2202fff4=0x18 (NAND), header at 0x22025000. LLB inits PLLs, storage, and DRAM controller 0x38501000.
  Status: DRAM region 0x08000000 (64MB) mapped; LACK: DRAM training MMIO handshake at 0x38501000, decrypted LLB.
- LLB -> UEFI (disk @ LBA 71, 0x47000): Relocates to high DRAM (0x0bc00000); reads rsrc FAT16 (0x3f2000); runs BDS.
  BdsN31 button check (0x0bc2e56c): AppleButton IDs 0,1,2 all 1 -> DiagOS (0x0bc2e5ec); unpressed -> osos (0x0bc2ec6c).
  Status: GPIO/buttons modeled; LACK: decrypted disk/UEFI payload, cold MSE partition table mounting in LLB.
- UEFI -> RetailOS (osos @ LBA 44789, 0xaef5000): Entry 0x08000000; sets TTBR0 0x08b28000, enables MMU/caches.
  Handoff: PMGR, VIC0/1/2 (IRQ 8 timer, 54 FMSS), timers 0x3c700000, LCD blitter 0x3c100000, touch, PPN NAND FTL.
  Status: Snap6 peripherals run; LACK: cold FTL bringup (current disk oracle relies on live DRAM FTL @ 0x08d0c6a0).
## 4. Milestone Plan to Reach Home Screen (Cold Boot)
- M1 (Existing Data): Populate raw NAND store with rb3 rows 0..50; add raw FMC ECC/DMA hooks; fix USB strapping.
- M2 (New Device Dump): Run device AES oracle on real iPod (via homebrew) to decrypt LLB, disk (UEFI), and osos.
- M3 (LLB & DRAM Bringup): Trace decrypted LLB; implement DRAM controller handshake at 0x38501000; reach UEFI entry.
- M4 (UEFI BDS & osos Launch): UEFI mounts MSE table, verifies button state (normal boot), loads & enters osos @ 0x08000000.
- M5 (Cold RetailOS -> Home): Boot RetailOS kernel, mount clean PPN FTL from reserved.bin+disk.dd, render Home on LCD.
