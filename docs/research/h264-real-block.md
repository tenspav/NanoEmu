 start code `0x1b0`; `0x081958d6` recognizes VOP code `0x1b6`.
4. H.263 evidence: `0x08226ad2` calls header parser `0x0814875c`; shared submission `0x08226c8e` calls `0x0812eacc`.
5. MPEG-4/H.263 aperture: `0x39600000–0x3966ffff`; command `2`/`0x102` → `0x39630110`; command `3` → `0x3966000c`.
6. Its initializer `0x082268da` selects IRQ 45; handler `0x080ec8cc` acknowledges child status and top `0x39600000`.
7. Completion: `0x39660000` bit 1, top `0x39600000` bit 6; semaphore `0x08811d50`.
8. `0x3966001c` is input **end address**, `input+length+0x103`, not byte count (`0x0812ebc4–0x0812ebe2`).
9. H.264 identity: strings `H264_MPD`/`H264_DecoderDriver` at `0x0871b274–0x0871bcd4`.
10. `AppleM2H264MPDDriver` diagnostic at `0x0871b570`, referenced by `0x083e34a0`; no other `AppleM2*` names found.
11. `avc1`/`avcC`/`mp4v` are container identifiers; purported H.264 vtable `0x0879e9d0` actually belongs to a file/stream wrapper.
12. H.264 initializer `0x08232d8e` loads `0x38f00000` from `0x08232f24`; `0x08496b94` stores it at `0x088117fc`.
13. H.264 accesses include `0x38f00000–0x38f02b7f`; map at least `0x3000` bytes. Full hardware aperture is unproven.
14. IRQ registration: `0x08232d34` selects 35 (`0x23`), handler `0x080df2d9`; completion semaphore `0x08811d44`.
15. Decode start: write `1` → `0x38f01000` at `0x08494a3c`; preceding write clears `0x38f01078`.
16. Reset: `0x38f01004 |= 0x7ff`; wait bit 0 clear (`0x08495834`); restore path waits the entire register zero.
17. IRQ acknowledgement: write `0` → `0x38f010c0` (`0x0849716c`); `0x38f010b8` bit 4 is checked and cleared for malformed data.
18. Decode result: read/write-back `0x38f01074` (`0x08495b30`); bits 1–7 report decode errors.
19. Parser start: `0x38f01804 |= 1`, then `0x38f01600[11:0]=0x801` (`0x08493b10`).
20. Parser status: `0x38f01628` bit 8 ready, bits 0–4 NAL type, bits 5–6 header error.
21. Exp-Golomb request: `0x38f01078=1`; wait `0x38f0107c` bit 7 (`0x083c3560`).
22. Input registers: `0x38f01200=input>>10`; `0x38f0180c=input&0x3ff`; `0x38f01810=offset+NAL_length`.
23. No descriptor-pointer doorbell found: H.264 submits through programmed registers and software surface records.
24. Surface record (`0x08232f9a`), seven u32: `+00 surface*`, `+04 Y>>10`, `+08 chroma>>10`, `+0c aux>>10`.
25. Its `+10/+14/+18` contain Y/chroma/aux physical addresses with bit 31 cleared.
26. Slots `i=0..19`: `0x38f0120c+4i=Y>>10`, `0x38f0125c+4i=chroma>>10`, `0x38f012ac+4i=aux>>10`.
27. IRAM `0x22000000–0x2203ffff` checked: no codec strings, relevant aligned MMIO literal-load candidates, or direct branches into identified codec routines.
28. Exact correction: retain/rename the existing model as MPEG-4/H.263 at `0x39600000`/IRQ 45; add a separate H.264 device at `0x38f00000`/IRQ 35 with registers above.
29. Merely changing base/IRQ is insufficient: H.264 requires parser readiness/results, register tables, reset and distinct completion semantics; chroma order/tiling remain unverified.
