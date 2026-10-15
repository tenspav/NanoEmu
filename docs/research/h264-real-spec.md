[22:0]=(input_phys&0x3ff)+NAL_length`-end offset within the aligned input window.
12. Parser (`0x08493b10`): `+1004=(old&~0xf)|0xc`, poll bit0 clear; clear `+1628[8:0]`; `+1814=(old&0xc0000000)|0x20`, `+1818=(old&0xc0000000)|8`; `+1804|=1`; `+1600[11:0]=0x801`.
13. Parser status **correction to existing notes**: `+1628` bit8=ready, bits4:0=NAL type, bits6:5 behave as **nal_ref_idc**, not generic errors; IDR requires nonzero, SEI/AUD types require zero (`0x08493bfc..3c6c`).
14. Syntax bank: reads `+1400+4*(n&31)` consume fixed-width bits; repeated `+1404` reads consume successive single bits; `+1408/+140c/+1410/+1414/+1420/+1440` are used; `+1480` supplies lookahead.
15. Exp-Golomb: write `+1078=1` for unsigned (`0x083c3560`), `=3` for signed (`0x083c287c`); wait `+107c` bit7; short unsigned result is bits20:8, short signed result is arithmetic `word>>8`.
16. Long Exp-Golomb results use `+107c` bit6, prefix length bits5:0, then syntax-bank reads; `+1508` bit13 participates in PPS extension detection; post-completion writes `+1508=0`.
17. Software surface record (`0x08232f9a`), seven little-endian u32: `+00 surface*`, `+04 Y_phys>>10`, `+08 chroma_phys>>10`, `+0c aux_phys>>10`, `+10 Y_phys`, `+14 chroma_phys`, `+18 aux_phys`; physical addresses clear bit31.
18. Surface slots (`0x084964dc`), `i=0..19`: `+120c+4*i=Y>>10`, `+125c+4*i=chroma>>10`, `+12ac+4*i=aux>>10`; firmware reads these tables back when releasing/outputting pictures.
19. Optional dual-surface mode also programs `+12fc+4*i` and `+134c+4*i` with a second surface’s shifted Y/chroma addresses (`0x08496550..6576`).
20. Job format: **register-programmed submission, no RAM descriptor-pointer doorbell**; decode object `+0c=input*`, `+10=sample_length`, `+18=NAL-length-field width`; samples contain 1-4-byte big-endian NAL lengths.
21. IRQ35 ISR `0x080df2d8→0x080df2c0`: calls `0x0849716c` (`+10c0=0`), then `0x084974e0` (tests `+10b8` bit4, clears it by RMW); stores malformed flag at `0x088117dc`.
22. ISR always posts semaphore `0x08811d44` via `0x08427a66`; decoder waits via `0x08406172` with argument `0x84` (`0x08232ab8`).
23. After wake, `0x08495b64→0x08495b30` reads `+1074`, saves result, writes that same word back-implement result W1C-then writes `+10c0=0`; **IRQ acknowledgement itself is zero-write, not W1C**.
24. Result `+1074`: values `0/1` pass the driver’s result check; bits1-7 diagnose VLC, macroblock, submotion, pattern, IDF, delta-Q and timeout; malformed flag must also remain zero.
25. MPEG-4/H.263 instead uses `0x39600000`/IRQ45, doorbell `+30110=2/0x102`, start `+6000c=3`, completion `+60000` bit1 and top `+00000` bit6, semaphore `0x08811d50`; `+6001c` is an end address.
26. **Base+IRQ retargeting is insufficient:** retain/rename the draft as MPEG-4/H.263; H.264 needs different doorbell, reset, parser/consuming reads, shifted-address tables, result W1C and zero-write IRQ acknowledgement.
27. QEMU init checklist: cover the `+3400+4*i` bank (a `0x3000` window is insufficient); retain ordinary registers/tables across reads, clear reset requests, and support firmware context copies; `0x4000` is a practical initial window, physical aperture unproven.
28. Parser checklist: DMA each bounded NAL; handle header/RBSP emulation-prevention bytes, consuming bit reads, unsigned/signed Exp-Golomb and readiness; parse valid SPS/PPS, then an IDR with nonzero nal_ref_idc. Constant ready/zero responses cannot initialize the driver.
29. One-frame checklist: accept surface/work registers and slice state; on `+1000=1`, finish asynchronously with `+1074=1`, `+10b8.bit4=0`, assert IRQ35; `+10c0=0` lowers IRQ, semaphore wakes, result write-back clears status, and decodeFrame should return `0`.
30. Limits: this establishes the firmware-facing protocol, not every hardware bit or IRQ-enable rule; output chroma order/tiling remains unproven, and the proposed happy path was not executed in QEMU-valid displayed pixels additionally require decode and guest-plane writes.
