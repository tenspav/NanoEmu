t4k, 0)` returns `0` on success; it reaches locked reader `0x081d83d8`, including cache maintenance.
4. Find `obj` dynamically: `dev = *(u32 *)(0x08971660+4)`; `obj = **(u32 **)(dev+4)`. In snap6: `dev=0x0b206f2c`, `obj=0x0b3bc9bc`.
5. Its backend is `[obj+4]=0x0b3bc988`; `[backend+0x20]=0`, so this path supplies **unshifted logical LBAs**, including candidates below the data volume.
6. The mounted data backend is `0x0b412484`; `[0x0b4124a4]=0xc040`. Reader `0x0848ab58` adds that offset; ordinary filesystem reads cannot address the hidden prefix.
7. **Boundary confirmed:** hidden logical range `[0,0xc040)` = **49,216 × 4096 = 201,588,736 bytes = 192.25 MiB**.
8. FAT32 itself starts at global LBA `0xc07f`: disk-image block `63` plus `0xc040`. For literally everything before FAT32, dump `[0,0xc07f)` = **192.496 MiB**.
9. This confirms the logical boundary, **not that `0..0xc03f` contains the entire boot chain or diagnostics**; raw bootstrap/FPart allocations require separate capture.
10. FTL LBAs have no physical ordering guarantee: the physical mapping of data LBA zero cannot define “all physical pages before it.”
11. **Raw path:** FMC is `0x38a00000`; CE/config `+0x00`, command `+0x08`, address `+0x0c/+0x10`, FIFO `+0x80`, completion `+0x48`, device status `+0x4c`.
12. Correction to the notes: runtime boot reader `0x084ed600(ce, boot_address, buf, ecc_enable)` sends `0x0a`, four address bytes, `0x37`, waits, then `0x7a`; finishes with `0x77`.
13. ROM reader `0x20009388` uses `0x00/0x30`. PPN runtime data reads use `0x07/0x0a` and `0x7a`; these interfaces are distinct.
14. Direct FMC is harder: establish exclusive NAND-worker/controller ownership, wait for DMA/FMSS completion, capture into RAM, restore/release ownership, **then** perform filesystem output.
15. FMC-idle observations alone cannot prevent races. Controller ownership and raw spare acquisition remain prerequisites; do not call the low-level reader concurrently with OS storage.
16. **Bootstrap coverage:** capture CE0–3 candidate rows `0..0x3f` and `0x100..0x13f`; this conservatively covers the documented ROM loader bound. Record absent CEs and expand from geometry/image headers.
17. **Logical metadata:** record LBA, return status, retries, validity and checksum. This OS wrapper supplies corrected data, **not OOB or physical mapping**.
18. **PPN metadata:** where exposed, preserve all **16 bytes per 4 KiB sector**—64 bytes per 16 KiB page—plus CE/address/sector, device status and transfer errors; metadata byte 1 bit 1 indicates uECC.
19. **Raw metadata:** preserve complete uncorrected data/spare, bad-block markers and parity; boot BCH uses nominal **52 parity bytes per 1024 bytes**, with three useful sectors (`0xc00`).
20. Do not hardcode raw records to 3228 bytes: `0x084ed600` programs parity-transfer length register `+0x30=0x34`; resolve its transfer/padding semantics and actual spare geometry.
21. Preserve ECC result `0x38a0080c`, completion `0x38a00810`, history `0x38a00818` **per sector**; result bit 0 means uncorrectable, bit 1 corrected. Save raw and corrected copies separately.
22. **Output:** streaming to `/Apps/Data/probe/DumpBoot/` through the existing filesystem is practical, but **writes NAND**, potentially including FTL maintenance. It satisfies “do not write reserved contents,” not literal “never write NAND.”
23. Literal zero-write acquisition needs an external transport; 192.25 MiB cannot fit in 64 MiB RAM. PPN internal read maintenance also cannot be ruled out.
24. **App initialization:** show BUSY immediately; verify firmware fingerprints, resolve live pointers, require backend offset zero and block size `0x1000`; reuse initialized storage—do not reset NAND or repeat SET FEATURES.
25. Allocate a 32-byte-aligned 256 KiB buffer using Thumb `0x0842b441(size,2)`; free via `0x0842d86d`. Avoid SDK scratch `0x09118000..0x0911a400`.
26. **Probe first:** read global LBAs `0xc040` and `0xc07f`, compare with disk-image MBR/FAT boot sector; then test LBA zero and `0xc03f`. Stop on unexpected ABI/status behavior.
27. **Read loop:** request 64 × 4 KiB sectors, finish each synchronous read before writing its chunk; update MB/percentage, support cancellation between chunks, retry failures individually without hiding errors.
28. **Files:** `manifest.json` with firmware/geometry/ranges; `reserved.bin` with fixed LBA offsets; `status.bin` with per-sector validity/status; `boot-raw.bin` with length-tagged CE/row/data/spare/ECC records.
29. **Verification:** sentinel-fill buffers, check every return/write length, hash chunks and final files, reread source chunks for comparison, retain failures and incomplete markers; search image directories for `diag`/`osos` and follow all extents/replicas.
30. **Duration:** benchmark the first MiB; at an assumed 1–5 MiB/s, one logical pass takes **38–192 seconds**, plus output/verification/raw capture. Snapshot file output measured 64 MiB in 4.112 s; reserved-read speed remains unmeasured.
