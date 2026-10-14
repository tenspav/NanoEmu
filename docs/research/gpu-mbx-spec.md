84890+0x104`; IRQ registration follows the `"MBX1"` path at `0x08105e18`.
3. `0x39100000`: image/scaling engine; `0x0825327c` programs source ranges at `0x39100028/2c/30/34`, then writes `1` to `0x39100008`.
4. `0x39200000`: two image planes; addresses `0x39200024/44`, format/control `0x39200020/40`, geometry `0x39200028/30/34` and `0x39200048/50/54`.
5. `0x39300000`: video-output configuration; init `0x08253004`; control `0x39300000`, configuration `0x39300008`, status test `0x39300040&1`.
6. Shutdown `0x0825281c` clears bit 0 at `0x39300000`, `0x39200000`, `0x39100000` and waits for bit 1 at each address.
7. **GPU IRQ = 53 (`0x35`):** installed by `0x080df780`; VIC slot `0x08ae6894=0x0814e321`.
8. IRQ dispatcher `0x0814e321` walks list rooted at `0x089098a8`; node `0x09984a3c` calls `0x08191de5` with argument `0x09984890`.
9. GPU interrupt registers: status `0x3b00012c`, enable `0x3b000130`, W1C acknowledgement `0x3b000134`.
10. Init `0x0812b90c` enables mask `0x8775`; handler `0x08191de4` temporarily disables interrupts, processes status, then restores enables.
11. Status `0x20`: handler `0x0814fb10` advances tiled-job state; `0x10`: `0x08191e64` advances render-completion state.
12. Status `0x0c` and `0x40` set paired completion flags; `0x100`, `0x200` set further flags; `0x400` invokes callback through `0x08198f1e`.
13. **Init:** `0x08105d50` → `0x0812b600`; GPU reset/control `0x3b000080` receives `0x111`, then `0`; `0x3b000134=0xffff`; `0x3b00085c=0x80`.
14. Identification `0x0813f574` reads `0x3b000ff0/ff4/ff8/ffc` for assembled `0xb105f00d`, otherwise checks `0x3b000f00/f10`.
15. Synthetic accepted ID fallback: `0x3b000f00=0x01020003`, `0x3b000f10=0x00010500`; this matches saved device fields but is not a measured hardware ID.
16. **Submission is mixed:** FIFO word streams plus RAM lists of MMIO `(offset,value)` pairs; no hardware RAM ring was established.
17. Mapped FIFO windows: `0x3b800000`, `0x3ba00000`, `0x3bc00000`, each `0x200000` bytes; these are MMIO mappings, not RAM allocations.
18. FIFO writers `0x081805dc` and `0x081806ac` repeatedly write successive RAM words to the same MMIO address.
19. Compositor submission `0x08198e5a` streams to `0x3ba00000`; flush `0x0847ffb8` sends seven words from `0x08752d34`: `0xf0000000`, then six `0x70000000`.
20. FIFO capacity calculation at `0x0817b508`: available bytes = `(0x7b−((0x3b00012c>>16)&0xff))*4`, reduced by software reservations.
21. 3D scheduler `0x0813645a`: job `+0x20` contains pair count; pairs begin at `+0x24`; `0x083f1a88` writes each value to `0x3b000000+offset`.
22. Scheduler writes job address to `0x3b0006ec`; special render path `0x08195e50` handles offsets `0x608/610/674` and writes `0x3b000808=1`.
23. **GPU address translation:** object `0x099655f4` holds eight tables at `0x0995c000/5d000/5e000/5f000/60000/61000/62000/63000`, each `0x1000` bytes.
24. `0x084850e4` installs their addresses at `0x3b001000/1004/1008/100c/1010/1014/1018/101c`; table entries contain physical page addresses.
25. Translation control `0x3b001020`: bit 0 toggled; polling `0x08484e94/0x08484df4` waits for bit 16 transitions; invalidate uses `0x3b001024/1028` and bit 8.
26. **Flush thread:** entry `0x081dd021`, body `0x081dcba0`, object `0x099821fc`; queue descriptor `0x09982210`, capacity `0x10`, allocation `0x09981fec`, records `0x20` bytes.
27. Flush records: words at `+0x00..+0x14`, operation byte `+0x18`, completion semaphore `+0x1c`; actual active 3D command-buffer addresses/sizes were not recovered.
28. **Flip thread:** entry `0x08254ea3`, body `0x08254e34`, object `0x09984808`; producer/consumer indices `0x09984808/0c`.
29. Flip callback ring `0x09984814`, eight `0x0c`-byte records `{object,argument,synchronization}`; enqueue `0x08254e0b`, callback uses object-vtable `+0x28`.
30. **App/compositor interface:** getter `0x08402075` returns GKMBX2D object `0x09989344`; vtable `0x087e7704`; worker entry `0x084806e3`, body `0x08480218`.
31. Vtable `+0x10→0x08480605` queues operation `0`; `+0x1c→0x08480681` queues operation `1`; `+0x24→0x0847fff5` queues synchronization operation `6`.
32. GK queue `0x09989374`: capacity `0x20`, records `0x110` bytes; wrappers copy surface/drawing descriptors and signal semaphore `0x09989358`.
33. **Clear+flip ABI remains unresolved:** `0x08480681` is a drawing wrapper and `0x0847ffb8` a flush barrier; neither establishes a callable `glClear`/`eglSwapBuffers` sequence.
34. **QEMU init checklist:** map `0x3b000000..0x3b003fff` and the three FIFO windows; implement ID reads, reset storage, enable/W1C registers, and IRQ 53.
35. **One-frame checklist:** consume FIFO words, update occupancy, execute register-pair jobs, implement translation/poll transitions, and generate completion status before asserting IRQ 53.
36. **Limit:** these addresses were statically traced in snap6; rendering packet semantics, framebuffer resolution, and the clear+flip ABI need further tracing before this is a complete homebrew-3D specification.
