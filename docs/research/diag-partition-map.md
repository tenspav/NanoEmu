[TASK 1] Firmware.MSE Partition Table (Offset 0x5000, 40-byte records, 11 entries):
- NAND:disk: off=0x00006000, size=0x000a8813 (690,195 B / 674 KB), load=0x08000000, flags=0x1e000
- NAND:diag: off=0x000b0000, size=0x00201063 (2,101,347 B / 2.00 MB), load=0x08000000, flags=0x1e000
- NAND:osos: off=0x0a6b4000, size=0x00974e03 (9,915,907 B / 9.46 MB), load=0x08000000, flags=0x1e000
  (Others: fv00 @ 0x2b3000, appl @ 0x2c6000, lbat @ 0x2cb000, bdsw @ 0x2f8000, bdhw @ 0x346000, chrg @ 0x373000, gpfw @ 0x3a0000, rsrc @ 0x3b1000)
  Payload format: Apple 87402.0 Type 3 container starts at +0x1000 within each image (diag @ 0xb1000, body 0x200c63 + 0x400 hdr).
[TASK 2] Whimory / FTL / UEFI Structures in ram-full.bin:
- WhimorySignature at 0x08d09340 (magic 0x776d7278 'wmrx', build 1.0 37A00230g); PPN params at 0x08d0d3e0 (2 CEs, 16KB page, 128 pgs/blk).
- Kernel storage globals: _fpart=0x08d0a18c, _vfl=0x08d0a608, _ftl=0x0896ff58, _fil=0x0896ded4; active signature validated in DRAM.
- UEFI Image1Fs volume (IM1V) at 0x0bc72d10 mounts Partition 2 BlockIO (handle 0x0bc887a0, Media 0x0bc887c0) at table offset 0x5000.
- BlockIO partition map: Part 2 (Firmware) = LBAs 0x40..0xc040 (64..49,216 = 192.0 MB); Part 3 (Disk) = LBAs 0xc040..0x3ada8a (ipod-disk.dd).
- Live osos entry in Image1Fs (0x0bc1e858): tag 'DNANsoso', offset 0x0aeb4000, size 0x009751c3 (firmware 1.0 layout within Part 2).
[TASK 3] Exact Dump Ranges & Safest Read Path:
- NAND:diag logical range: LBA 0x00f0..0x02f1 (LBAs 240..753, 514 × 4KB sectors = 2,105,344 B); within Part 2 offset 0x000b0000..0x002b1063.
- LLB raw physical range: CE0 Block 0 rows 0..49 (0x00..0x31) and Block 1 backup rows 256..305 (0x100..0x131) (0xc00 useful bytes/page).
- Safest read path (diag & reserved area [0, 0xc040)): Reuse OS unshifted logical block reader Thumb 0x081d85bb(obj, lba4k, buf, count, 0)
  where dev=*(u32*)(0x08971660+4), obj=**(u32**)(dev+4) (in snap6 obj=0x0b3bc9bc). Supplies kernel-managed ECC & caching with zero FMC conflict.
- Safest read path (LLB): Runtime boot reader 0x084ed600(ce, addr, buf, ecc) or FMC MMIO (0x38a00000) while storage worker task is quiesced.
[TASK 4] Sanity-Check vs docs/research/diag-boot-agy.md:
- CONFIRMED: Partition table @ 0x5000, diag size 0x201063 (2.1MB), and load target 0x08000000.
- CORRECTION: diag is not an raw unmapped FPart slice; it is indexed by the MSE partition table in Partition 2 (LBAs 64..49,216).
- CORRECTION: ipod-disk.dd starts at LBA 49,216 (Part 3); the entire 192.25MB firmware area [0, 49,216) is absent from ipod-disk.dd.
- Dual-boot verdict: HIGHLY FEASIBLE. Diag payload loads to 0x08000000; can be booted via BDS button check (0x0bc2e56c) or replaced in Part 2.
