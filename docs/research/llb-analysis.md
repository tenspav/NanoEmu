[TASK 1: CARVE RESERVED.BIN & 8740 CONTAINERS]
- Part 2 (Firmware) = LBAs 64..49215 (0x40000..0xc040000). MSE ptable at 0x45000 (LBA 69, 11 entries).
- Exact 87402.0 containers carved (all have 0x400 header, Type 3, AES-encrypted with GID key):
  * disk: off=0x00047000 (LBA 71), body=0x0a8413, total=0x0a8813, load=0x08000000, encrypted (UEFI/BDS)
  * diag: off=0x000f1000 (LBA 241), body=0x0200c63, total=0x0201063, load=0x08000000, encrypted (DiagOS)
  * fv00: off=0x002f4000 (LBA 756), body=0x011263, total=0x011663, load=0x08000000, encrypted
  * appl: 0x307000 (body 0x2f13); lbat: 0x30c000 (0x2afa3); bdsw: 0x339000 (0x4cba3); bdhw: 0x387000 (0x2afa3)
  * chrg: 0x3b4000 (body 0x2afa3); gpfw: 0x3e1000 (0xf5c3) - all Type 3 encrypted HW firmware blobs
  * rsrc: off=0x003f2000 (LBA 1010), body=0xab00c63 (171MB), Type 3 header, but payload is PLAINTEXT FAT16 @ 0x3f2400
  * osos: off=0x0aef5000 (LBA 44789), body=0x974dc3, total=0x9751c3, load=0x08000000, encrypted (RetailOS)
- LLB IS ABSENT: BootROM loads LLB from raw NAND (CE0 Blk 0/1 rows 0..49), outside FTL logical blocks.

[TASK 2: LLB & SELECTOR DISASSEMBLY / SIGNATURE ENFORCEMENT]
- Prior Claim Refuted: LLB does NOT perform button checks or OS/diag selection.
- Actual Selector: Evaluated in UEFI BdsN31.dll (DRAM 0x0bc2d000). At 0x0bc2eaee, it calls button check 0x0bc2e56c.
- Button Logic: Queries AppleButton IDs 0,1,2. If IDs 0,1,2 all pressed (1), 0x0bc2e5ec selects Diagnostics.
- Image Resolution (0x0bc2f488->0x0bc2fc98): Searches FV/FVDIAG.Fv, FVDIAG.Fv, then Part 2 'diag' (LBA 240). Normal boot loads 'osos' (0x0bc2ec6c).
- Type 4 vs RSA: UEFI validator (0x0bc789a4) accepts Type 3 and Type 4 (cmp r0, #4 @ 0x0bc789c2), BUT body validator (0x0bc78658) strictly enforces RSA-1024 cert chain check (0x0bc77d78) BEFORE checking Type (0x0bc7876c).
- Verdict: Type 4 does NOT bypass signature checks; unsigned plaintext payloads are strictly rejected.

[TASK 3: DUALBOOT OPTIONS RANKED BY RISK]
1. Low Risk: OS-level kexec/chainloader via resident homebrew (/Apps/Startup/resident-*.hbapp). Boots RetailOS, shuts down kernel/MMU, and branches to bare-metal payload in DRAM. Zero brick risk.
2. Medium Risk: In-memory pre-BDS hook. Hook UEFI validator in DRAM to force RSA success (0x0bc78714 -> 1), loading custom Type-4 payload from NAND diag partition without modifying bootloader flash.
3. High Risk: Flashing modified diag partition (LBAs 241..754). Requires exploiting 8740 header/RSA parser or AES GID oracle. Recovery possible via RetailOS if boot buttons left untouched.
4. Very High/Fatal: Flashing raw LLB (CE0 Blk 0/1) or Part 2 'disk'. Fails BootROM RSA signature; hard brick.

[TASK 4: ENCRYPTED VS READABLE NOW]
- Readable Now: Partition table (0x45000), all 11 container 0x400 headers, RSA cert chains/signatures (diag sig @ 0x2f1400, cert @ 0x2f1480), rsrc FAT16 filesystem (0x3f2400), and live DRAM UEFI/RetailOS code.
- Encrypted (Requires device AES oracle with GID key): Executable payloads of LLB (N31.bootloader.rb3), disk (UEFI), diag (DiagOS), osos (RetailOS), and HW firmware blobs (appl, lbat, bdsw, bdhw, chrg, gpfw).
