t initial work: obtain pristine plaintext, supply Type-4 derived containers, and bypass authentication in **ROM, LLB’s disk-loading path, and UEFI’s osos-loading path**; ROM-only patches are insufficient.
21. ROM patch targets are header verifier `0x200006ac`, body verifier `0x200007d4`, and Type-3 branch `0x20000914`; preserve metadata writes `0x200008ec–0x2000090c`, especially entry offset.
22. Skipping AES while retaining ciphertext cannot boot. Most faithful: unchanged signed Type-3 images plus real crypto MMIO; GID operations require a device oracle or explicitly limited recorded transformations.
23. **Reset → ROM:** PC `0x20000000`, writable IRAM, cold flag `0x3d100004.bit18=0`; retain clocks `0x3c500000`, power gates/GPIO `0x39700000`, timers `0x3c700000`.
24. Model USB absence (`0x3c400028.bit0=0`) and boot straps correctly: ROM’s `0x200037f4–0x2000381c` can select USB before NAND. Current generic USB/PHY register files can falsely indicate attachment.
25. **ROM → LLB:** ROM masks VIC banks through `0x38e00014/0x38e01014`, sets CPSR `0xd3`, disables MMU/I-cache and invalidates instructions; header remains at `0x22025000`, boot-source word `0x2202fff4` becomes 24.
26. **LLB → disk:** implement whatever pristine LLB programs for PLLs, DRAM initialization/training and storage; DRAM is already allocated at `0x08000000–0x0bffffff`, but controller `0x38501000` currently only stores register values.
27. Exact LLB exit registers, boot arguments, DRAM-ready responses and VIC configuration are **not established**; derive them from decrypted LLB and trace execution rather than inventing handoff constants.
28. Firmware directory is reserved offset `0x45000`; `disk` container `0x47000`, payload `0x47400`, executable length `0xa77a8`; declared load `0x08000000`, followed by UEFI relocation.
29. **UEFI → BDS:** preserve accumulated clocks/DRAM/storage state; snapshot BdsN31 checks AppleButton IDs 0,1,2 at `0x0bc2e56c`; all pressed selects diagnostics, normal selection uses `osos` at `0x0bc2ec6c`.
30. Those UEFI addresses are snapshot relocation addresses, not guaranteed cold-boot addresses; locate modules/functions in the chosen decrypted build.
31. **BDS → osos:** reserved container `0x0aef5000`, payload `0x0aef5400`, executable `0x974160`, load/entry `0x08000000`; UEFI header/body validators are snapshot `0x0bc789a4/0x0bc78658`.
32. **RetailOS → Home:** VIC, timer IRQ8, FMC/FMSS IRQ54, PMU, LCD/blitter, GPIO/buttons and touch already support snapshot operation; their reset-time initialization remains unvalidated.
33. Storage blocker: current disk oracle reads live RetailOS FTL structures at `0x08d0c6a0`; it does **not** reconstruct FTL from reset. Provide physical PPN metadata/maps or implement a logical storage service usable by LLB/UEFI/OS.
34. Logical layout: reserved `[0,0xc040)` in 4-KiB LBAs; append `ipod-disk.dd` starting at global LBA `0xc040`. Existing sparse pages alone do not supply the missing cold FTL state.
35. **M1—existing files:** synthesize raw LLB rows, implement ROM NAND/ECC/DMA, fix attachment/straps; verify reconstructed container bytes before crypto.
36. **M2—new device acquisition required:** decrypt exact LLB, disk, osos and any boot-requested firmware blobs; obtain pristine executable bytes, not surviving RAM. No GID key extraction is required.
37. **M3—implementation after decryption:** trace LLB DRAM/storage setup; reach UEFI BDS and osos entry; preserve firmware-created state and disable snapshot resume/fixup options.
38. **M4—conditional new dumps:** acquire missing syscfg/identity sectors and physical FTL metadata if required; for faithful raw preservation capture CE0–3 rows `0..0x3f`, `0x100..0x13f`, parity and per-sector status.
39. **M5—acceptance:** repeatedly reset from ROM with blank RAM, initialize storage, load pristine RetailOS, render Home and accept input; raw bootstrap dumps are optional for synthesis, essential for proving the original physical layout.
