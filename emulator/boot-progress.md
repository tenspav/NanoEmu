# nano7 boot progress (RetailOS direct-boot, entry 0x08000000)

**Canonical resume (since Session 9, snap2):**
`./run.sh --ramfull ../dumps/snap2/ram-full.bin --iram ../dumps/snap2/iram.bin
 --ctx fixups/snap2-ctx.bin --os-poke-file fixups/snap2-timerlist.poke
 --pcsample 1000 -M nano7,mmu=0x08b28000` (run from emulator/; see Session 9).

Config for furthest run: `./run.sh --iram --iram-protect --os-entry`
(9.9MB retailos image @0x08000000, IRAM snapshot @0x22000000,
IRAM vector page 0x1000 bytes read-only, VBAR=0x22000000 at reset).

## Peripherals stubbed (generic regfile: write-store/read-back, dedup log)

- **clkcon** @ 0x3C500000 (4KB, ff-fill): first stall cleared; many gate RMWs.
- **pmgr**   @ 0x3D100000 (4KB, ff-fill): +0x04/+0x08 status reads.
- **tmr**    @ 0x3C700000 (4KB, ff-fill): PLL config at +0x88; **+0x84 is a
  free-running counter** used for busy-wait delays (two reads, wait for
  delta >= N) — implemented as a live counter off QEMU_CLOCK_VIRTUAL.
- **vic**    @ 0x39700000 (4KB, **0-fill**): interrupt-controller-like;
  polls wait for channel bits in +0x14 to clear/match +0x08 — 0-fill passes.
- **b3e0** @ 0x3E000000, **b38e** @ 0x38E00000, **b3c8** @ 0x3C800000,
  **b3cf** @ 0x3CF00000 (4KB, ff-fill): init-time touches.
- **fmc**    @ 0x38A00000 (4KB, ff-fill): NAND controller probe (8 chip
  selects, +0x48 status bit1 = ready) — probe now passes instantly.

## Machine features added this session

- VBAR set to 0x22000000 at reset (hardware bootrom behavior).
- `iram-image` property: preload IRAM snapshot (has live vector table,
  `ldr pc,[pc,#56]` vectors → handlers in the OS image, e.g. SVC→0x082f3828).
- `iram-protect` property: first 0x1000 of IRAM read-only — CRITICAL, the
  OS's early resume-style code memcpy's a 256-byte data struct over
  0x22000000..0x220000FF which otherwise destroys the vectors and the first
  SVC 0x46 (mutex lock syscall, in the clock-enable loop) undef-loops.
- `ctx=<file>` + `cpsr=<val>` properties: restore 16 u32 regs from ctx.bin
  and force CPSR; with `--ramfull` (64MB live DRAM dump) + `--iram` this
  resumes the live system — but ctx.bin layout is guessed and it derails
  (see below). Banked exception SPs + CPACR(VFP on) are set for ctx boots.
- `pcsample-ms=<N>`: host-clock timer logging PC/CPSR/SP/LR + IRAM[0..0x40];
  the essential "where is it stuck" tool.
- run.sh flags: --os/--os-entry/--iram/--iram-protect/--ctx/--cpsr/
  --ramfull/--pcsample/--stub; extra qemu args pass through.

## Boot log progression (unique MMIO accesses per run)

- round 0 (no periphs): 2 lines, stuck after CLKCON bit15 write.
- +clkcon: SVC 0x46 → undef loop (vectors clobbered by OS struct memcpy).
- +iram image + VBAR + iram-protect: 38 lines — clocks, PLL, VIC, PMGR init.
- +tmr counter, vic 0-fill, extra blocks, fmc: 48+ lines — full clock init,
  delay loops work, FMC probes 8 NAND chip selects, syscall machinery runs.

## Current blocker

OS calls its **power-off routine** @0x08417c9c (writes 0x3C800000=0x00104a00,
clkcon+0x50=0xa5, then an intentional infinite NOP loop at 0x08417cae,
CPU in ABT mode). Trigger chain: after the FMC/NAND probe, a delay/syscall
path (call @0x08010bb4, r0=10000) leads via a table dispatch @0x083a3240
into power-off. Prime suspect: NAND FTL/ID init failure (FMC is only a
dumb regfile; ID reads return 0xffffffff) — or a power/battery check.
Next: implement FMC READID semantics (command 0x90 + ID bytes) or find the
power-off predicate via a trace of 0x842aa80 (syscall dispatcher, r0=27).

## Bootrom status

../dumps/bootrom-vrom.bin is now 256KB but **all zeros** — the SCSI peek
currently returns zeros (VROM access is likely clock-gated off via
CLKCON 0x3C500048 at dump time). Bootrom path untested, waiting on a
nonzero dump.

## ctx-resume side path (parked)

`--ramfull --iram --ctx --cpsr 0x3f`: runs real OS code (clkcon+0x4c,
PMGR reads), but after touching 0x3d100004/8 it uses a stack at
0x21fffff8 (below IRAM), then jumps to VA 0 (MMU appears to get enabled;
zero-page NOP sled). ctx.bin layout (18 words, r0..r15 + 2 extra bootrom
pointers 0x20000010/0x20000030) needs ground-truth before this works.

## Open questions

- The struct the OS memcpy's over 0x22000000 contains a thumb pointer
  0x2208ffc2 — suggests a trampoline in **IRAM above 256KB** (real IRAM
  may be 512KB-1MB; our dump only covers the low 256KB). Worth dumping
  0x22040000..0x22100000 on hardware.
- The OS likely switches VBAR to its own trampoline (0x2208ff00-ish) later
  in boot; we never got that far.

## Session 3 (FMC/power-off investigation)

Furthest config: `--iram --os-entry retailos-patched.bin -M nano7,iram-protect=2`

### Fixes this session

1. **VIC struct NULL-store abort (DFAR=0 @ 0x08001514)**: the IRQ-enable
   helper @0x080014fe reads a driver struct via global @0x08918220
   (-> 0x08ae67b8, in BSS). The image's .data has the pointer pre-filled,
   so the lazy-init path (which would fill the struct with the real VIC
   regs 0x38e00010/0x38e01010) is skipped, but BSS is zero -> NULL store
   -> data abort -> OS power-off. Workaround: `retailos-patched.bin` =
   image with u32 @file-offset 0x918220 zeroed, forcing the lazy init.
   After: VIC init runs (writes 0xdeadbeef to 0x38e00f00/0x38e01f00).
2. **Undef @ 0x220011ec (IRAM trampoline)**: the OS zeroes IRAM above the
   vector page early on, wiping the dumped trampoline code that its SVC/
   dispatch table (e.g. `ldr pc,[pc,#-4]` stubs @0x083a3xxx -> IRAM
   0x22001xxx thumb) jumps to. Workaround: `iram-protect=2` = whole 256KB
   IRAM read-only (dump stays intact). After: no aborts/undefs at all;
   ~2300 clean SVC syscalls, all init tasks run to completion.
3. Also: banked exception-mode SPs now initialized at reset (IRQ/FIQ/ABT/
   UND/SVC stacks in high IRAM) for ALL boots, not just --ctx.
4. b38e (VIC hw @ 0x38e00000) widened to 0x3000 (covers +0x1000/+0x2000
   sub-blocks); pmgr fill changed to 0 (no effect either way so far).

### Current blocker (policy, not a crash)

After all init tasks complete, the boot dispatcher (ARM-mode `blx` at
0x083a3240) directly calls the **power-off routine** @0x08417c9c
(preceded by notify call 0x840b86c with r0=3 = shutdown reason code 3?).
Stack shows it was invoked via SVC gateway (svc 0x47 @0x080011be,
syscall nr in r4). This is a deliberate OS decision. Evidence points to
this 9.9MB image being the **osos-style loader / resume image**: it never
attempts a NAND READID or page read (FMC sees only probe/enable writes),
so boot-device selection fails before VFL/FTL engage. Likely missing
input: boot-args / syscfg / boot-reason state that the bootrom normally
provides (registers r0-r2 at entry are all zero in our machine!).

### Next ideas (priority order)

1. Find the shutdown-reason predicate: syscall nr -> dispatch table
   @0x083a3180; check which condition selects power-off vs NAND boot.
   r0=3 at 0x0840b86c may be a readable reason code.
2. Provide boot-args: find where the OS reads the bootrom handoff
   (likely IRAM high area or a fixed DRAM struct; search for known
   Apple boot-args magic 'BootArgs' / atag patterns in the image).
3. Implement real FMC READID/page-read anyway (strings show VFL/FTL
   machinery exists: "[NAND] VFL_Init [Failed]" etc.).
4. Wait for a valid (nonzero) bootrom dump; boot the real chain.

## Session 4 (REAL bootrom boot — dumps/bootrom-vrom.bin is valid now)

Config: plain `./run.sh` (bootrom @0x20000000, entry 0x20000000 = reset
vector `b 0x200000a8`). NOTE: the 256KB dump is the same 64KB repeated 4x —
real VROM is 64KB.

### Bootrom peripheral touch sequence (in order)

1. VIC @0x38e00000: mask/default-vector init (+0x14=ff.., +0xf00/0x1f00,
   +0x2008/0x200c).
2. 0x3c800000 = 0xa5 (block unlock).
3. PMGR @0x3D100004 read, bit 0x40000 = warm-boot flag (clear = cold boot;
   SET would take the warm-resume branch at 0x20000164).
4. CLKCON @0x3C500000: full clock/PLL programming burst (+0x44/+0x48/+0x4c/
   +0x58/+0x68/+0x6c/+0x00/+0x20/+0x30/+0x08..+0x1c/+0x04).
5. Watchdog/timer block @0x39700000: +0x70=0x2582 (kick?), +0x6c, +0x18,
   +0x10=0x3e.
6. tmr @0x3C700000: counters at +0x84 AND +0xb4 (timeout loops use both);
   PLL at +0x88/+0x8c/+0x90; +0xa0/+0xa4/+0xa8/+0xb0 = timer channel ctrl.
7. FMC @0x38A00000 init/reset: +0x00=cs-config, +0x08=0xff (RESET cmd?),
   +0x48 = status (bit1 = cmd-done, write-1-to-clear), +0x04 = command
   (0x2a, 0xe0 seen), waits via 0x20000dd4(mask,timeout) helper.
8. Block @0x3C400000: pulse-init (+0x00/+0x08 strobes, +0x1c=4|6, +0x04=1,
   +0x34=4), status at +0x28 (bit5 must be set; then +0x40/+0x44/+0x100).
9. Block @0x38400000 = the FIRST boot medium driver polls: +0x10 =
   start/busy (write 1 -> wait bit0 clear + bit31 set), +0x14 = status/irq
   flags (bits 2,16,28,29,30 drive the detection state machine), command
   setup +0x08=0x2b, +0x0c=0x1708, +0x18=0x70000006, +0x24=0x218 (536 =
   length?), +0x28=0x01000218, +0x104=0x01000318. Possibly the real NAND
   controller on S5L8747 (FMC@0x38a00000 is the N3G-era block).

### Bootrom architecture (mapped)

- State struct ptr @ 0x2202fff8 (= 0x2202ba3c; matches wInd3x
  statePtrLocation). Key fields: +0x28 = media-detect busy flag,
  +0x29 = boot-ready flag (2 = boot!), +0x47 = USB/DFU active flag,
  +0xa0 = detection state, +0x24 = driver vtable (ROM @0x20000020).
- Main loop @0x20003d30: poll driver, check +0x29==2 -> jump to boot image
  via [[struct+0x3c]+0x20]; if !busy && !usb -> run detection step
  @0x20009f98 (pulses 0x3c400000) and retry.
- Detection state machine @0x2000a8fc: state 0 -> return immediately;
  states 5..11 handled @0x2000a944 (incl. [[vtable]+0x20] driver poll at
  0x2000a98c, retry/reset transitions on 0x38400014 bits).

### Stall

Detection never starts: busy flag (+0x28) is set before polling
0x3c400028 (bit5 — satisfied by ff-fill) but NEVER cleared afterwards;
the only clear site (0x2000ab34, in vtable fn 0x2000aad0) requires the
0x3c400000-block driver struct (RAM) flags bit2 — which is set from
hardware status we don't model. So the bootrom parks in the main loop
exactly as a real device with dead NAND and no USB would.

### Next (priority)

1. RE the 0x3C400000 block: which +0x28/status sequence makes its driver
   clear busy and what the identify result must look like. This block runs
   BEFORE NAND proper and gates everything.
2. Alternatively force the DFU path: find what sets struct+0x47 (USB
   attached) — then watch it init the USB core (likely 0x3A000000).
3. Bootrom warm-boot branch (PMGR+4 bit18): check whether it hands off to
   0x08000000 with boot-args regs — could fix the RetailOS direct-boot
   power-off from session 3.

## Session 5 (2026-10-13): stateful FMC + why NAND init is never reached

Implemented `machine/hw/arm/nano7_fmc.[ch]` (replaces the FMC regfile at 0x38a00000;
added to build.sh + meson.snippet): CE select (+0x00), cmd issued on +0x08 write,
address latch (+0x0c/+0x10, +0x2c=n-1, go +0x04=1), W1C status +0x48 (every go
sets bits 1/2/3/20/23/24), +0x4c=0x40, data-in FIFO +0x80 with byte count +0x28.
Responses per claude-ppn.txt: CE0/CE1 READID(00)="PPN"01 00 00, READID(30)=45..,
cmd 0xEE ctrl ver "NANO7EMU PPN1.00", cmd 0x92 512-B params (caus=8, cau_bits=4,
blocks/cau=0x1000, block_bits=12, ppb=128, ppb_slc=64, pab=7, +2c=1, +30=2,
page=0x4000, timings 20, +e0..ec=0x200, +f0/f4=10, +f8/fc=100: checked against
the 0x812ad5c validator). CE2..7 and all other data read as 0xff.
Verified only on the shutdown-path reset (CMD 0xff to CE0..7, bit1 handshake OK).
**PPN detection code (0x84cce2c / 0x80fb6e0) is never executed in any config**,
so the READID/params path is still untested.

Root cause of the old "~2300 SVC power-off": not NAND. It's a panic, malloc(12)
returns NULL (0x842d444 -> 0x842d554 -> svc 0x47 -> panic 0x83a3180 -> 0x8003e70
save log -> power-off 0x8417c9c). The heap singleton [0x08909134] lazy-inits
only when NULL (0x83fbecc), but the image holds 0x089a6228 (zeroed BSS), so
the free list is empty. **retailos-08000000.bin is a live RAM snapshot**
(metadata.json): every prefilled lazy singleton hits the same trap (VIC
0x08918220, heap 0x08909134, and also [0x08908e04]=0x0b45fd6c, a live heap obj,
causing a prefetch abort via a NULL vtable at 0x80206c6).
New machine prop `os-zero=addr:addr` zeroes u32s in DRAM after load. The OS is
now memcpy'd, not a ROM blob, so fixups survive reset. The image file is untouched.

Best config now (64MB live dump, so heap pointers point at real objects):
`./run.sh --iram --iram-protect 2 --os-entry <ram-full-08000000.bin>
 -M nano7,os-zero=0x08918220:0x08909134`
-> no panic/abort/power-off, 4655 SVCs, user-mode threads run, new MMIO blocks
(0x3d800000, 0x38300000, 0x3de00000, 0x38501000, 0x3c900000 I2C-like); then
parks forever in the idle thread: svc 0x46 ip=9 -> `wfi` @0x0862ade8 (pcsample
59/59 samples). FMC never touched.

Next wall: **no interrupts**. Every thread is blocked on timers/events and nothing
raises an IRQ. Needed: a VIC model (0x38e00000/0x38e01000, the PL192-like regs
the OS programs) plus a periodic timer IRQ source (tmr 0x3c700000 channel at
+0x20..+0x30 / 0x3de00000?) so the scheduler ticks and NAND init can run.

## Session 6 (2026-10-13): VIC + timer IRQs, scheduler ticks
New `nano7_vic.[ch]` (replaces regfile b38e) and `nano7_timer.[ch]` (replaces regfile tmr; +0x84/+0xb4
counters kept). Both added to build.sh + meson.snippet. CPU IRQ/FIQ are wired.
- VIC @0x38e00000 = 2 PL192 banks (+0x1000 = IRQ 32..63) plus glue @0x38e02000. Regs: STATUS +0, FIQSTAT +4,
  RAW +8, SELECT +c, EN W1S +10, ENCLR +14, SOFT +18/+1c, VECTADDR +100 (OS writes n), PRIO +200, ADDRESS +f00.
  ISR 0x862b754 -> 0x800ab78 -> dispatch 0x800d324 reads bank0/bank1 STATUS, then that bank's +f00 = n,
  calls handler table[n] in the VIC object, writes 1<<(n&31) to 0x38e02008/0c (stored only; inputs are
  level), then writes 0x01010101 to +f00 (EOI). Init is 0x8006e94.
- IRQs the OS enables: bank0 2,3,7,8,9,10,11,16,17,21,22,26,29,31; bank1 32,33,52. Only IRQ 8 has a source.
- Timer @0x3c700000 is the S5L8702 8-channel layout: A-D +0x20*n, E-H +0xa0.., CON/CMD/DATA0/DATA1/PRE/CNT,
  CON en bits 12-14 / W1C pend bits 16-18, E-H pend in TSTAT +0x118. Handler 0x8000b38, all channels on
  IRQ 8. The OS ticks on ch1 (B): DATA0=6000, PRE=0, CON=0x1140, 3 MHz => 500 Hz. Modeled as periodic QEMUTimers.
- 0x3de00000 is a command engine: +0x18 data, +0x14 bit0 go/busy (0x8550850). The driver waits on a semaphore
  with a timeout (svc 0x46) after "go", so it probably has an IRQ (52?). Not modeled; it now just times out.
  0x3d800000 / 0x38300000 / 0x38501000 / 0x3c900000: no IRQ path found (init writes only).
TEST (exact task command, 90s): ticks fire and the OS acks them. Blocked threads now wake (previously idle
  forever at wfi 0x0862ade8). SVCs 4655 -> 5107, ~109 IRQs. Then a new panic: a newly started thread (entry
  0x856716c -> 0x81eb4a4) writes through NULL. Data abort -> 0x83a3180 -> power-off loop 0x8417cae (89/89 samples).
  FMC only gets the shutdown CMD 0xff (NAND init is still never reached).
ROOT CAUSE: snapshot inconsistency, not hardware. C++ static-local guards (__cxa_guard_acquire 0x842d3c8)
  live in .data and are still set, but their objects are in BSS (0x08971704..~0x08999020), which boot zeroes.
  Found 864 guard sites / 441 guards / 243 set; 19 guard BSS objects (fixups/bss-guards.txt, from
  fixups/find_guards.py). New `os-zero-file=` machine prop / `--os-zero-file` run.sh flag.
  With bss-guards: 5373 SVCs, 121 IRQs, then a data abort at 0x81d738a (heap object 0x08b27854 has vtable 0).
  Cause: the re-initialized heap (os-zero 0x08909134) hands out memory over live-dump objects that live pointers
  still reference. Resetting all 243 guards (fixups/all-set-guards.txt) gets 184 IRQs / 6251 SVCs, then
  jumps to NULL (vtable slot +0x1c, 0x842a05a). The 9.9MB image is worse: .data holds 2696 pointers into the heap.
NEXT WALL: get a consistent OS image. Options: (a) decrypt/unpack a pristine RetailOS (pristine .data/BSS)
  from the IPSW in files-from-mac/; (b) stop resetting the heap and fix the original free-list problem
  instead; (c) a full ctx/MMU resume of the live snapshot (all IRQ hardware now exists). Then model IRQ 52 / 0x3de00000.
Debug tip: `-d cpu,nochain -dfilter 0x082f3790+0x10` dumps the user LR/SP at abort time (0x82f3794: r4=lr, r0=sp).

## Session 7 (2026-10-13): ctx-resume abort fixed = Thumb-state bug, not timer/NAND

ABORT ROOT CAUSE (task 1): the snapshot-resume data abort was NOT the delay helper or missing
banked state. ctx.bin's thread is Thumb (r14=0x0948b487, bit0 set; code at pc 0x0948b298 is Thumb
`b.w`), but `--cpsr 0x8000001f` has T=0, and more importantly `cpu_set_pc(cs, ctx_regs[15])` with an
even PC (0x0948b298) force-cleared `env->thumb`. The CPU decoded the Thumb bytes `00f05cb8` as the ARM
word 0xb85cf000 (a garbage LDMDALT) -> data abort, DFSR 0x1 / DFAR 0xffffffff, on the very first
instruction. The abort handler then ran the panic path -> power-off loop 0x08417cae. The MMU/ctx
application was fine: L1 @0x08b28000 is a flat VA=PA section map; banked SPs were already set. ctx.bin
is a jmp_buf saved by setjmp-style routine 0x0948b240 (words 16/17 = 0x20000010/0x20000030 are IRAM
pointers in that buffer, not CPSR; machine correctly ignores them).

FIX (nano7.c, ctx_loaded reset path): set env->regs[15]=ctx_regs[15]&~1 directly and derive Thumb
state from the snapshot itself: `env->thumb = (r15&1)|(r14&1)|(cpsr_override T bit)`. The snapshot is
ground truth, so a stale T=0 override no longer re-creates the ARM-decode abort. Rebuilt clean.

TASK 2 (timer helper): moot. The free-running counters at 0x3c700084/0xb4 were never the fault; no
timer-related abort occurs. No change needed to nano7_timer.c.

RESUME TEST (task 3): exact documented command `./run.sh --ramfull --iram --ctx --cpsr 0x8000001f
--pcsample ... -M nano7,mmu=0x08b28000` now runs a full 90s with ZERO aborts and NO power-off. The
resumed thread completes the setjmp/scheduler path and parks in the idle thread (pc=0x0862adec, SVC
mode, 94/94 pcsamples) -- the same healthy idle wfi as Session 5, reached here WITHOUT os-zero/guard
hacks. No further MMIO: LCD 0x38900000, NAND 0x38400000 and FMC 0x38a00000 are NOT touched (resume
lands directly in steady-state idle; all threads block with no new runnable work, consistent with
Session 6: only timer IRQ 8 has a source, 0x3de00000/IRQ 52 still times out). Next wall is unchanged:
give the idle scheduler real work (model IRQ 52 / 0x3de00000, or wake a UI/input thread).

IPSW FEASIBILITY (task 4, investigation only): iPod_1.0.4_37A40005.ipsw -> N31.bootloader.rb3,
Firmware.MSE (184MB), manifest.plist (N31 = nano7G, build 37A40005, v1.0.4). Firmware.MSE is the
classic UNENCRYPTED .MSE updater container (plaintext STOP/ASCII header, "[hi]" directory @0x100). NO
IMG3/IMG4/IM4P and NO KBAG keybag anywhere -> no bundled keys. It genuinely is S5L8747/nano content
(strings "S5L","8747","OSOS","nano","disk"; plaintext asset XML PathString/PathOffset ~0x8a02000). BUT
the OSOS (RetailOS executable) payload is high-entropy (~7.3-8.0 bits/byte, garbage strings, no ARM
code) with no complzss/lzss/gzip/Img2/8900 wrapper -> encrypted, not merely compressed. VERDICT: a
clean directly-loadable RetailOS canNOT be obtained from this IPSW without the S5L8747 AES keys (not
present). The decrypted live RAM snapshot remains the only viable image source.

## Session 8 (2026-10-13): IRQ map, 0x3de00000 = IRQ 52, resume had NO hw state; clock + timer-list fixes
IRQ MAP (VIC obj 0x08ae67b8, handler[n] at +8+4n; dispatch 0x800d324):
  2,3,31,32,33 -> 0x83a33be(4,3,2,1,0): GPIO irq groups @0x39700000 (stat +a0.., mask +c0.., +80..)
  7,8 -> 0x8000b38 timer 0x3c700000 | 9,10,11 -> 0x8000fec(0..2): UARTs 0x3c300000/0x3ce00000/0x3d200000
  16,17 -> IRAM 0x220010ee: DMA 0x38700000/0x38200000 | 21,22 -> IRAM 0x22001214: I2C 0x3c600000/0x3c900000
  25,26 -> 0x8150b8c/0x80009a6: 0x3db00000(+0x3dd00000) | 29 -> IRAM 0x22001136: 0x38800000 (4 ch, +4 stat/+8 clr)
  52 -> 0x8000c9c: 0x3de00000 | 13 -> panic stub | 37,53,54 installed but not enabled at boot.
0x3DE00000 = serial register-command engine (SPI/SWI-like; clock gate 0x35, IRQ 52). Init 0x800ad94: +4=1,
  +0=0x1703, +0x10&=~3 (unmask). ch0 +0x14 ctl (b0 go, b1 16-bit) / +0x18 frame [W|addr7|data8]; ch1 +0x1c/+0x20
  fire-and-forget. Writer 0x855082c: go -> svc 0xa wait(event 0x3c, timeout 0x20); IRQ 52 handler reads +0x0c,
  writes it back (W1C), signals event 0x3c. Model: new nano7_spicmd.[ch], go completes after 20us -> status bit,
  IRQ 52. (Not exercised in resume: nothing writes it after resume.)
KEY FINDING: the ctx resume had ZERO IRQs (3 SVCs then wfi): a RAM snapshot has no MMIO state, VIC enables
  and the tick timer were at reset. New `resume-hw` prop (default 1 with ctx=) replays the OS's own VIC init
  (vect[n]=n, enables 0xa4630f8c/0x00100003), 500 Hz ch1 tick and 0x3de00000 init -> 500 Hz IRQ 8 now ticks.
TORN SNAPSHOT #1: kernel timer list (kern 0x0890b948: +0x1c tick, +0x28 head; node next/pprev/expiry; thread
  table 0x08aad6a0 x60B, thread+8 = node) is a cycle (thr3->30->19->3). At tick 0x82d9 (~35s) the expiry walk
  0x862d5bc livelocks in SVC mode. New `os-poke-file=` / `--os-poke-file` (u32 "addr value" DRAM stores) and
  fixups/fix_timerlist.py -> fixups/timerlist.poke (10 nodes relinked in expiry order).
  With it: threads really run (user mode), new MMIO: I2C1 0x3c900000 (IRAM driver), 0x3cf00000, DMA 0x38200000
  ch3 -> 0x3db00020 len 0xa, 0x3ca0003c; then thread 45 (3 s queue-wait timeout 0x80bf778) deletes sem 0x9f whose
  waiter slot (0x08aad208[0x9f]) still says thread 5 -> panic 0x80a6a8a -> power-off. Poking 0x08aad484=-1
  (experiment, /tmp only) avoids it.
CLOCK BUG: OS system clock 0x83edbc8 = 64-bit counter hi 0x3c700080 / lo 0x3c700084 (hi-lo-hi read), split
  /1000/1000 -> it is a 1 MHz us counter. Model returned 0xffffffff for +0x80 => every timed wait expired
  instantly (thread 76 spun at ~23k SVC/s in a timed cond-wait 0x80191dc). nano7_timer.c now: 64-bit 1 MHz,
  new `usec-base=` prop (start value; default 0).
RESUME TESTS (90 s, -M nano7,mmu=0x08b28000 + --ramfull --iram --ctx --cpsr 0x8000001f):
  exact cmd: 45908 IRQ / 17818 SVC, ticks fine, livelock in timer walk from ~35 s, no MMIO but 0x3de init.
  + --os-poke-file fixups/timerlist.poke: 15657 IRQ / 16192 SVC, MMIO above, then ~30 s thread 76 heap free
    check fails (0x849d18e, heap 0x089a6228, block header kind mismatch) -> panic -> power-off loop 0x08417cae.
  LCD 0x38900000 / framebuffer / FMC / NAND 0x38400000: never touched in any run.
NEXT WALL: the snapshot itself. Three independent torn kernel structures so far (timer list, sem waiter slot,
  heap block headers), consistent with metadata.json: a 300 s non-atomic SCSI read. Fixing them one by one is
  whack-a-mole. Options: (a) a consistency-repair pass over kernel tables (threads/timers/sem slots/heap walk);
  (b) re-dump with the OS quiesced (IRQs off) so the image is atomic; (c) cold-boot path instead of resume.
  Display: 0x38900000 literals in ~30 functions (e.g. 0x80443e4, 0x8049f44, 0x80526cc, 0x8053xxx) not yet
  traced to a thread; the resume never gets there before panicking.

## 2026-10-13 — DumpRAM app built (on-device atomic-ish RAM snapshot)

Built `probe/dumpram/` (sibling of DumpROM; raw-surface HBAppKind-1, SDK
`make -C dumpram` → `probe/dumpram/build/dumpram.hbapp`, 3956 bytes).
Purpose: replace the slow 300 s non-atomic SCSI RAM read (which produced the
internally-inconsistent image above) with a single on-device dump that runs
entirely in one UI frame to minimize time skew. On the "DUMPRAM" tap it, with
no UI redraws in between: (1) captures r0-r15 + CPSR (Thumb bit) of the UI
thread setjmp-style → `ctx.bin` (72 bytes, same layout as the prior snapshot),
(2) streams the full 64 MB DRAM 0x08000000..0x0BFFFFFF → `ram-full.bin` in 1 MB
writes straight from the source address (no staging copy), (3) dumps 256 KB
IRAM 0x22000000..0x2203FFFF → `iram.bin`, (4) writes `report.txt` (bytes/file,
RAM elapsed ms, errors). Files land in `/Apps/Data/probe/`. Confirmation
("OK - REBOOT ME") only after all files are closed. Packaged like DumpROM:
`probe/install/DumpRAM.app` (app_id 0x0dade1e3), `DumpRAM.bin` icon;
`probe/repack_dumpram.py` appends it to the live slot-B pack at install time.

## Session 9 (2026-10-13): snap2 (3.6 s DumpRAM snapshot) wired into resume
PAGE TABLE: L1 still @0x08b28000 (225 identity 1MB sections, VA=PA, entry 0x080 = 0x08085c06); OS literal
  @0x0862ad20 (next to the idle loop) references it. A same-target copy with other attrs sits at 0x0bff4000
  (likely the bootloader's table). `mmu=0x08b28000` unchanged.
CTX: snap2/ctx.bin does NOT match ram-full.bin: its pc 0x09352240 (in the app instance at 0x09351f70) is
  framebuffer pixels in the RAM image. The live app instance is at 0x095f89b0 (the captured stack's return
  address 0x095f8eab); its .bss g_ctx @0x096097f8 holds the matching capture (pc 0x095f8c80, same sp/lr).
  Also: pc=0x0842b47d in the task notes is actually word 14 (lr, stale); word 15 is the pc. The app is Thumb
  but MRS reads T=0, so the cpsr word's T bit means nothing. New fixups/snap_ctx.py extracts the in-RAM
  g_ctx -> fixups/snap2-ctx.bin, with pc = mrs instruction 0x095f8c82|1 (exact resume point).
  nano7.c: a ctx with the 'DRAM' marker -> CPSR from word 16 (T masked) unless --cpsr is given; Thumb = pc
  bit0 only (the lr heuristic is kept for the old format). run.sh already took explicit files.
TEST A (no pokes, 90 s): 776 SVC / 44954 IRQ, then livelock in the timer walk 0x862d454..5c8 from ~2 s.
  snap2's timer list is torn AGAIN (fixups/check_timerlist.py: head 67 -> 30 <-> 19 cycle, duplicate pprevs).
  The same nodes (thr 19/30) are torn in two independent dumps (300 s vs 3.6 s), so it's probably not time
  skew. Hypothesis: dirty D-cache lines missing from the DMA'd dump. Untested.
  fix_timerlist.py now also takes head-reachable nodes (thr 67's node isn't in its slot) and only
  active (+0x14) nodes -> fixups/snap2-timerlist.poke (old timerlist.poke untouched).
TEST B (canonical cmd, 90 s): 46222 SVC / 44953 IRQ / 14 user undefs (VFP lazy-enable, returned OK), 0 aborts,
  NO panic: Session 8's heap-header (~30 s) and sem-waiter (~35 s) panics are gone without fixups; the sem
  poke was not needed. PCs: 89/89 samples in idle wfi 0x0862adec (SVC). User-mode work (~1300 returns to
  0x842ac98/0x8429f04/0x842af80..) continues in ~7/s bursts with spikes at 1 s/11 s/21 s. MMIO: resume-hw
  VIC/tmr/0x3de00000, clkcon +4c/+6c, I2C1 0x3c900000 (IRAM driver), 0x3cf000a4, 0x3ca0003c, DMA
  0x3820014c. LCD 0x38900000 / framebuffer / FMC / NAND 0x38400000: still never touched.
REMAINING WALLS: (1) the resumed DumpRAM thread calls hb_fs_stream_open; the RAM was captured MID-stream
  (app flag @0x09609a70 = 1, fobj 0x0911a000 written 0x01600000 B), so it first does fobj_finish -> OS close
  0x0841b09f/0x0840718d/0x08423be1 and never returns. That's the in-flight FS write whose NAND/DMA hardware
  state the snapshot can't carry. Next: find the FS/NAND thread it waits on and its pending hw op (or poke the
  flag to 0 to replay a fresh open/write = a real NAND workload). (2) Timer-list tear still needs the poke;
  test the cache theory (clean/flush D-cache in DumpRAM before streaming). (3) Display: nothing drives
  0x38900000 yet.


## Session 9b (2026-10-13): replaying the DumpRAM open/write; real NAND driver work starts
POKE: s_stream_live is a BYTE (app .bss +0x110c0); +1 is s_inited -> word 0x09609a70 = 0x00000100.
  fixups/snap2-replay.poke = snap2-timerlist.poke + that store.
RUN 1 (canonical cmd, --os-poke-file fixups/snap2-replay.poke, 90 s): 46181 SVC / 44952 IRQ, no panic, 89/89
  samples idle, MMIO identical to Session 9 (no FMC/NAND/LCD). The app now does open instead of close
  (fobj_begin zeroes its 4 KB buffer, calls OS 0x084137a9) but blocks on kernel mutex 0x109 (svc 0x46 ip=0xb
  -> 0x0865cf74; mutex table 0x08a9cdc8, 32 B/entry, +0 owner thread, +4 recursion count). The close path
  (Session 9) blocks on the same mutex.
ROOT CAUSE: mutex 0x109's owner is thread 36 (0x08aadf10, the app thread itself: its stack holds ctx sp,
  recursion 1, taken by the in-flight dump write). But the kernel current/ready-list head [0x0890b950] is
  thread 34 (list 34 -> 36 -> 0). We run 36's registers while the kernel thinks 34 runs, so the recursive
  lock looks foreign and the thread waits on itself forever. Thread 34 = NAND/PPN driver thread (saved pc
  0x0840ba86, stack -> 0x080fb7c9/0x084cdc11/0x084d09db PPN code); it was mid page I/O when RAM was read.
RUN 2 (EXPERIMENT, fixups/snap2-replay-cur36.poke = replay + ready list reordered 36 -> 34 -> 0): mutex
  0x109 now succeeds and the FS write path runs. Clocks are gated on via IRAM (clkcon +1c/+28/+10/+44/+4c,
  0x3cf0014x/0x150/0x154/0x1e0, 0x3dd00000.. config). Thread 34 resumes its page loop (syscalls from
  0x0840ba86 on 16 KB buffers 0x08c8c000.., r3 = pages 0x464800 +0x400 ...) = REAL NAND driver work.
  Then: (a) 0x08156008 writes 0xd to FMC +0xc0c, polls 100 ms for self-clear -> panic (line 0x150).
  FIXED in nano7_fmc.c (+0xc0c bits 0,2,3 read as 0). (b) Next: the driver arms the FMSS sequencer
  (FMC +0xc00 block: +0xc04 "Sequence base addr", +0xc34 seq ptr, completion = "FMSS__CS_IRQ") and never
  gets completion -> "Timeout!" -> error dump 0x08171290 dereferences FMC +0xc04 (0xffffffff) -> data
  abort (DFAR 0xfffffff3, fault pc 0x081712e0) -> power-off in <1 s. FMC sees only the shutdown CMD 0xff.
  LCD 0x38900000 / framebuffer: still never touched in any variant.
VERDICT: the app replays its open (run 1, blocked) and the write (run 2). The FS -> NAND thread really
  drives the controller, but via the FMSS command sequencer, not the classic FMC cmd/addr regs we model.
NEXT WALL: model FMSS @0x38a00c00: parse the in-DRAM sequence program at [+0xc04], execute PPN ops
  against the existing PPN response model, set the CS_IRQ status and raise the FMC IRQ (find its VIC line).
  Thread 34's stale saved context is a risk; run 2 shows it resumes coherently enough to issue I/O.

## Session 10 (2026-10-13): FMSS command sequencer modeled; real PPN program runs, FTL asserts on skew
FMSS @0x38a00c00 (RE: _performPPNRead/_performPPNProgram 0x084ce4xx/0x084cdb40, wait 0x080d0ce8, kick
  0x080d0ca8, ISR 0x08161e8c): +0xc00 kick (0xfff5), +0xc04 sequence base, +0xc0c CS_IRQ status W1C (bit0
  done, 2/3 err), +0xc10 irq enable (ISR clears), +0xc38 seq ptr, +0xc4c prog length, +0xd00..+0xd7c
  CS_GEN_REG mailbox (args in d04.., result code in d00; NOT the VM regs). IRQ = VIC line 54 (0x36;
  registered at 0x080d0c74). Programs (prebuilt in RAM): 0x0896e920 program ph1, 0x0896f620 program ph2,
  0x0896eb00 read. ISA (8 B: op<<24|d<<16|imm16, w1): 00 halt, 01 MMIO=imm, 02 MMIO=R, 03 R=mem32,
  04 R=MMIO&mask, 05 R=imm, 06 mov, 07 wait FMC event, 0a/0b/0c/0d and/or/add/sub, 13/14 lsl/lsr (ALU:
  imm!=0 -> d=R[s] op imm, imm==0 -> d=R[d] op R[s]), 0e bnez, 17 beqz, 11 mem32=R, 12 R=mbox[8]; r8 = PC.
  The VM drives the classic FMC regs (CE +0x00, go +0x04, cmd +0x08, addr +0x0c, DMA +0x34 addr/+0x30 len).
MODEL (nano7_fmc.c): sequencer runs synchronously on kick, sets done, raises IRQ 54. PPN status +0x4c =
  0x62 after cmd 0x79 (op-status poll wants &0x62==0x62), else 0x40; +0x40 bits0/1 self-clear; ECC words
  +0x60..6c = 0; go&0x100 = DMA flash->DRAM (blank 0xff, no backing store), go&0x200 = DRAM->flash
  (counted, discarded). Accesses at +0x800.. are traced. nano7.c: IRQ 54 wired, enabled in resume-hw
  (bank1 0x00500003); new prop `fmss-resume=1` reports the sequence in flight at capture time done.
RUN (90 s): `./run.sh ... --os-poke-file fixups/snap2-replay-cur36.poke --pcsample 1000
  -M nano7,mmu=0x08b28000,fmss-resume=1`: IRQ 54 -> ISR -> driver accepts phase 1 (status 1) -> fills
  mailbox (d04=0x10000 d08=0x1000 d0c=0x08cd9d60 d10=4 d1c=3 d2c=0x08cd7b00) -> kicks 0x0896f620 ->
  VM: 7678 steps, 197632 B DMA'd out (~12 x 16 KB pages, PPN multi-plane program cmds 0x8a/0x87/0x17,
  status 0x79), d00=0 = success (0x084cdd38 -> exit). NAND WRITE PATH WORKS end to end.
THEN: thread 34 (NAND/FTL) panics: `WMR_ASSERT: vba == s_g_addr_to_vba(wr->sb, wr->nextVbaOfs)` in
  s_btoc_add_data (sftl) line 230: programmed vba 0x2d7ef, FTL write cursor (wr 0x08d0b3f8) expects
  0x8ce40 -> power-off. Snapshot skew: the FTL globals at 0x08d0xxxx were read ~2 s earlier in the 3.6 s
  stream than thread 34's stack (0x0b40xxxx), and the device was writing the dump file itself meanwhile.
  75 SVC / 7 IRQ total; MMIO: clkcon, I2C1 0x3c900000, 0x3cf00000, PMGR, 0x3c800000 (power-off).
  LCD 0x38900000 / framebuffer: NOT touched. The READ program has not run yet (no reads before panic).
NEXT WALL: the self-written snapshot is internally inconsistent for the FS/FTL. Options: (a) don't resume
  the in-flight NAND op: drop thread 34 from the run queue and replay only the app's open (runs FTL from a
  quiescent wr state?); (b) a DumpRAM v2 that dumps to RAM/USB, not NAND, so the FTL is idle during
  capture (best); (c) wr cursor poke 0x08d0b3f8 to match thread 34's request (whack-a-mole).

## Session 11 (2026-10-13): snap3 (DumpRAM v2, 47/1024 torn) + torn-read watcher
SNAP3: L1 still 0x08b28000. ctx.bin == in-RAM g_ctx (app base 0x0936b080, v2 opens the stream before
  capture, so the stream flag already matches: no flag poke). pc 0x0936b3de = the mrs boundary; Thumb
  bit added -> fixups/snap3-ctx.bin. Kernel block 0x08900000 is torn (read after T0): current = thr 34,
  app thr 36 waiting (flag 0x40, waiter slot 0x7f), timer list cyclic again.
  fixups/snap3-replay.poke = fix_timerlist(snap3) + ready list 36->34->0, clear 36's wait flag/slot 0x7f,
  + thr 34 TCB saved sp 0x0b408a40 (see below).
TORN WATCHER: nano7_torn.[ch], `torn-map=` / run.sh --torn-map, `torn-ignore-pc=lo:hi`, env
  NANO7_TORN_LOG=N (per-block log lines). Torn blocks are overlaid by an I/O region over the same DRAM
  with a per-word "written since resume" bitmap; reads of unwritten words = STALE (logged with pc,
  summary every 10 s). Pokes count as fresh. Ignore the app (0x0936b080:0x0936c200): its replayed
  hash pass reads all of RAM.
FIRST RUN (no thr-34 fix): data abort in 2 s. The watcher showed why: the scheduler (0x0862b698)
  restored thr 34 from TCB sp 0x0b408a58 (TCB block 0x08aa0000 read ~0.3 s into the stream) but the
  stack block 0x0b400000 was read ~5 s later -> garbage frame (CPSR with E/IT bits, pc 0x080d0cb3) ->
  DFAR 0x6c0ca038 (byte-swapped FMC +0xc6c). A stale read that matters.
  Fix: self-consistent frame at 0x0b408a40 (cpsr 0x60000010, pc 0x0842aec0 svc stub, lr 0x080d0cb3).
RUN (90 s, `--ctx fixups/snap3-ctx.bin --os-poke-file fixups/snap3-replay.poke --torn-map
  ../dumps/snap3/torn-map.bin -M nano7,mmu=0x08b28000,torn-ignore-pc=0x0936b080:0x0936c200`):
  15 SVC / 121 IRQ, no abort. Thr 34 resumes INSIDE the FMSS kick (0x080d0ca8) and writes +0xc00, but
  +0xc04 and the mailbox were set before capture (MMIO, not in the snapshot) -> sequencer now reports
  "kick with no sequence base" as an error (it used to "run" address 0 and halt with success) ->
  driver error -> panic -> power-off 0x08417cae (89/89 samples). FMC sees only the shutdown CMD 0xff.
  Torn reads: 27/47 blocks hit, 110880 stale / 646637 reads. ~1/3 of the logged ones are before the
  kick (kernel 0x0890b9xx, TCBs 0x08aaxxxx, timer nodes); most bulk ones are pc 0x0800c510 copying
  0x0b33xxxx/0x094bxxxx during the panic/shutdown path.
  MMIO: clkcon, I2C1 0x3c900000, 0x3cf00000, PMGR, 0x3de00000 init, 0x3c800000 (power-off).
  LCD 0x38900000 / panel transport 0x3d800000: NOT touched. FMSS never ran a program (no NAND write).
NEXT WALL: thread 34 is caught between "sequence armed" and "kick". Options: (a) rebuild +0xc04 and the
  mailbox from the caller's frame (r4 0x08ca9b60 = PPN request) the way _performPPN* does; (b) report
  that kick as complete with status 1 (the program already ran on the device); (c) DumpRAM v3: capture
  with NAND idle (e.g. wait until FMSS +0xc0c shows idle before T0).

## Session 11b (2026-10-13): option 2 — complete the pre-capture FMSS kick
`fmss-resume=2` (nano7_fmc.c): the next kick with no sequence base (+0xc04 armed before capture) is
  reported complete: status bit0, mailbox d00 = 0, and +0xc10 = 1 (every program enables it first;
  without it IRQ 54 never fired and the driver sat in its 2000 ms wait). fmss-resume=1 is unchanged.
RUN (90 s, Session 11 cmd with fmss-resume=2): 17 SVC / 121 IRQ, no data/prefetch abort exceptions.
  Kick -> "reported complete" -> IRQ 54 -> ISR 0x08161e8c clears +0xc10 and signals the FMSS event
  (obj 0x0b3bcca0 -> 0x0899c258, torn block 0x08990000) -> thr 34 returns to 0x080d0cde. On the next
  tick it runs 0x0802afc4 (lock helper 0x0800eb7c, svc 0x46 ip=0); that syscall goes straight into the
  prefetch-abort/panic gateway 0x082f38a0 -> 0x083a3180 (r1=3, sp 0x0b408a60) -> power-off 0x08417cae.
  The driver's wait (0x080d0ce8) is never reached, the FMSS never runs a program, there is no NAND write.
  Torn reads: 27/47 blocks, 110880 stale / 646653 (same as Session 11). LCD 0x38900000 / 0x3d800000:
  not touched.
OPEN: why svc 0x46 ip=0 from thr 34 panics. Suspects: thr 34's stack between 0x0b408a40 and its live
  frames is itself torn (the 0x0b400000 block was read while 34 ran), so the code it returns into has
  stale locals (r2=0x0b408a60). The ip=0 handler entry is in torn block 0x08910000 (svc table 0x08917714).

## Session 12 (2026-10-13): snap4 (DumpRAM v3, NAND idle at T0) — vector-smash fixed, FTL skew wall again
SNAP4: L1 still 0x08b28000 (L1[0]=0x2208c402: VA 0 aliases IRAM!). ctx.bin == in-RAM g_ctx (v3 app base
  0x09482a90, pc 0x09482e60 = mrs); snap_ctx.py got a v3 fallback -> fixups/snap4-ctx.bin. Pokes STILL needed:
  kernel block torn exactly as snap3 (cur = thr 34, app thr 36 waiting on 0x7f), timer list cyclic again
  -> fixups/snap4-timerlist.poke, fixups/snap4-replay.poke (= timerlist + ready list 36->34 + thr 34 frame).
  v3 only makes T0 idle; the dump's OWN stream writes still run thr 34 + sftl while blocks are read (54 torn).
CMD: ./run.sh --ramfull ../dumps/snap4/ram-full.bin --iram ../dumps/snap4/iram.bin --ctx fixups/snap4-ctx.bin
  --os-poke-file fixups/snap4-replay.poke --torn-map ../dumps/snap4/torn-map.bin --pcsample 1000
  -M nano7,mmu=0x08b28000,torn-ignore-pc=0x09482a90:0x09483c10,fmss-resume=2
RUN 1 (thr 34 TCB sp 0x0b408a40 -> coherent frame 0x0b408a20, as in snap3): 17 SVC/143 IRQ, panic <1 s,
  identical to 11b. ROOT CAUSE (gdb-RSP watchpoint, VA 0x4): the stack shows thr 34 already PAST the kick;
  its later call 0x0840ba7a overwrote the kick's pushed {r4,lr} at 0x0b408a78 (lr=0x084cdc11). Resuming the
  older frame, kick's pop returns mid-loop with stale r6=2 -> `str r0,[r6,#4]` @0x084cdc26 writes VA 0x6 =
  IRAM vectors (0x22000004/8 -> ae18f038/e59f0003) -> every SVC falls into the PABT vector -> panic. (This was
  also 11b's "svc 0x46 ip=0 panics".) FIX: poke 0x0b408a78=1, 0x0b408a7c=0x084cdb8b (real kick return).
RUN 2 (fixed, 90 s): 58 SVC / 142 IRQ / 1 undef (VFP lazy, OK) / 0 aborts. Kick #1 reported complete ->
  IRQ 54 -> driver fills mailbox, kicks phase-2 program 0x0896f620: VM 7205 steps, 185280 B DMA'd out, d00=0
  = NAND page program SUCCEEDS (past the 11/11b wall, = Session 10 level). Then thr 34 svc 0x47:
  `WMR_ASSERT vba == s_g_addr_to_vba(wr->sb, wr->nextVbaOfs)` s_btoc_add_data:230 — vba 0x000f6b9c vs
  cursor 0x003ae1c4 (wr 0x08d0b3f8) -> CMD 0xff -> power-off 0x08417cae (89/89 samples).
  Torn: 33/54 blocks hit, 118k stale / 655k reads; the PPN loop reads stale 0x08ca9bxx (request) and
  0x08d0d4xx (FTL) words. MMIO: VIC/tmr/0x3de00000 (resume-hw), clkcon, I2C1 0x3c900000, 0x3cf00000, PMGR,
  0x3c800000/FMC CMD 0xff (shutdown). LCD 0x38900000 / panel 0x3d800000: NOT touched; no thread nears display.
VERDICT: the snapshot is still inconsistent for the NAND/FTL thread. The NAND thread's stack, request and FTL
  cursor are each read mid-stream (~1.4 s / ~5.6 s) while that thread writes the dump itself. Poking the
  cursor is whack-a-mole. NEXT: DumpRAM v4 — at T0, inside an IRQ-off section (svc 0x46 ip=0 save / ip=1
  restore = 0x080e04c6/0x080e04dc), memcpy the volatile set (the 54 torn blocks, ~3.4 MB: kernel 0x0890xxxx,
  sync/TCB 0x08a6-0x08ac, PPN/FTL 0x08c0-0x08d2, stacks 0x0b3a-0x0b46) into a buffer allocated before T0;
  write it as overlay.bin and have the emulator overlay it on ram-full.bin. Alternative: synthesize thr 34
  idle in its request wait and drop the in-flight op.

## Session 12b (2026-10-13): DumpRAM v4 built (IRQ-off T0 overlay) + emulator overlay support
APP (probe/dumpram, install/DumpRAM.hbapp, title "V4"): pre-T0 picks the overlay set = built-in union
  of the snap3+snap4 torn maps (60 x 64 KB) + every block torn in the previous torn-map.bin, one 64 KB
  hb_os_alloc each (stops at 96 blocks or 2 MB heap left). T0: nand idle -> svc 0x46 ip=0 (irq-save,
  masks I+F) -> re-check FMC idle (else unmask, wait, retry x3) -> ctx capture (CPSR |= 0xc0) -> copy
  IRAM + all overlay blocks -> svc 0x46 ip=1 restore. Then the stream/torn-map as in v3, plus overlay.bin
  ('OVLY',1,bs,n,copy_us,irq_mask,wanted,missing, n addrs, n blocks). Not yet run on hardware.
EMU: `overlay=` machine prop / `run.sh --overlay FILE`: applied after the RAM image, before the
  pokes; covered blocks are excluded from the torn watcher (nano7_torn_exclude). fixups/overlay_info.py
  shows/compares an overlay (--merge RAM OVL OUT writes the overlaid image for check_timerlist.py etc.,
  --make-test builds an identity overlay). Self-test on snap4 (identity overlay + marker word): marker
  applied before the pokes, 54/54 torn blocks excluded, run identical to Session 12 run 2 (58 SVCs).
NEXT (after a v4 dump): snap_ctx.py on ram-full (v3 fallback works: pc = mrs), merge, check_timerlist on
  the merged image; resume WITHOUT pokes first:
  `./run.sh --ramfull .../ram-full.bin --iram .../iram.bin --ctx fixups/snap5-ctx.bin --overlay
  .../overlay.bin --torn-map .../torn-map.bin --pcsample 1000 -M nano7,mmu=0x08b28000,torn-ignore-pc=<app>`
  (the app resumes inside its IRQ-off section; IRQs come back at its own svc ip=1).

## Session 12c (2026-10-13): v4 rebooted the device on tap -> DumpRAM v5 (verified overlay, IRQs on)
SVC CHECK: svc 0x46 is right: handler 0x082f3828 reads the imm8 from the Thumb svc, 0x46 -> blx 0x8002390
  = table 0x08917714[ip]; ip=0 -> 0x080e04c6 (SPSR |= I|F, returns old bits), ip=1 -> 0x080e04dc ->
  veneer 0x0865cd90 (restores I|F); return via movs pc,lr, so user mode may call it (SDK uses ip=7
  the same way). Emulator test (stub poked at the snap4 app pc): ip=0 -> 0 IRQs, parked in user mode;
  ip=0 then ip=1 -> 2945 IRQs. Not the proven cause.
LIKELIER CAUSE: hb_fs.c keeps its file objects at FIXED, unallocated DRAM (scratch cache 0x09118000,
  stream obj 0x0911a000 + cache 0x0911a200). v4's ~60 x 64 KB heap buffers can cover them, so the T0
  copy overwrites the already-open ram-full stream -> crash on the first write (matches: 0-byte file,
  no report). Unproven: no device log.
V5 (install/DumpRAM.hbapp, "V5"): no svc at all. Buffers overlapping 0x09118000-0x0911b210 are
  rejected (the IRAM copy too); heap largest free block kept >= 1 MB; report.txt "stage=prepared"
  breadcrumb before the capture. At T0 (IRQs on) each block: hash src, copy, hash src + copy, accept if
  all equal (<= 8 tries); only verified blocks go into overlay.bin v2 (word5 = retries, word7 =
  dropped). The rest is the v3 flow. Emulator + overlay_info.py accept v1 and v2 (tested on snap4).

## Session 12d (2026-10-13): snap5 (V5) resumed with the overlay; DumpRAM V6 built
SNAP5: V5 ALL_DONE, overlay 57/60 blocks verified (15 retries, 100.7 ms window), 1 heap buffer rejected
  for the SDK scratch (supports the v4-crash theory). Dropped: 0x08900000 (kernel, ticks every 2 ms),
  0x0b400000 (stacks incl. the app's own), 0x09370000. TCB block now T0-consistent: mutex 0x109 free,
  app thr 36 not waiting. fixups/snap5-ctx.bin (app base 0x093e1ef0, pc 0x093e2589).
RESUME (overlay, no pokes, 90 s): 14 SVC / 88 IRQ, then an illegal svc->svc mode switch at the
  scheduler restore 0x0862b68c, undef, prefetch abort (IFAR 0x57c72a50) -> power-off. Early stale reads
  are in the two dropped blocks (kernel current/timer list, stacks) -> a bad saved frame got restored.
V6 (install/DumpRAM.hbapp, "V6"): kernel + stack blocks allocated first and copied inside one short
  svc 0x46 masked section with the ctx capture + IRAM (FMC idle re-check, CPSR |= I|F); the rest keep
  the V5 verified copy; a block failing verification gets a masked copy instead of being dropped.
  Scratch/heap guards + breadcrumb kept; overlay.bin stays v2 (no emulator change). Not yet on hardware.

## Session 12e (2026-10-13): snap6 (V6) resumes cleanly; real 50 MB NAND write workload; wall = no NAND contents
SNAP6: V6 ALL_DONE, masked section 13.5 ms (kernel + stacks + IRAM), 68/69 blocks (1 fallback-masked),
  FMC idle, ctx CPSR 0x600000d0. Merged image: current = thr 36 (the app) == ctx. Only 2 torn blocks
  uncovered (0x093e0000 app bss, 0x099a0000). fixups/snap6-ctx.bin (app base 0x093d2a80).
RUN 1 (overlay, no pokes): 380 SVC, 0 aborts, then WMR_ASSERT (!boolPowerState) ANDCall:393 -> power-off.
  ROOT CAUSE: NAND power-save flag 0x0895c55c (ANDCall struct 0x0895c554+8; op 4 enter asserts !flag,
  leave asserts flag) = 1 from the stream, but the power manager obj 0x0b40a3c4 (+0x54, masked T0 copy)
  = 0, and thr 24 is blocked at T0 in 0x0819d8a4 about to ENTER power-save. It entered right after T0,
  BEFORE the torn-map h0 pass, so block 0x08950000 was never flagged (h0 blind window ~370 ms).
  The timer list is also inconsistent (verified blocks ~97 ms after the masked ones) but harmless.
FIX: fixups/snap6-power.poke (0x0895c55c = 0). RUN 2 (90 s): 90962 SVC / 45812 IRQ / 1715 undef (VFP),
  no panic, idle wfi. The app really streams its dump again: FOBJ_WRITE 256 KB pieces, 36 MB in 60 s.
  Power-save enter/exit PPN cmds 0x77/0x7d/0xd7/0xef/0xe7 work. nano7_fmc: exit summary line (log cap).
RUN 3 (300 s): 1057 FMSS program runs, 50.7 MB DMA'd out, then the first reads (32 KB in) ->
  WMR_PANIC "sftl: lba mismatch; lba:0xc0d3, meta:0x410f5" s_verify_meta:38 -> power-off (~100 s).
  The FMC model has no backing store (reads DMA 0xff, ECC regs say clean). LCD 0x38900000 / 0x3d800000:
  not touched yet (the app never reaches its final redraw).
NEXT: (1) sparse page store in nano7_fmc (data + meta per CE/page) so read-after-write is real;
  (2) pre-existing flash data: block-level shim fed from a `dd` image of the iPod disk (USB disk mode),
  or a NAND dump; (3) DumpRAM V7: add 0x08950000 to the masked set and move h0 right after the masked
  section to shrink the blind window.
CMD: ./run.sh --ramfull ../dumps/snap6/ram-full.bin --iram ../dumps/snap6/iram.bin --ctx fixups/snap6-ctx.bin
  --overlay ../dumps/snap6/overlay.bin --os-poke-file fixups/snap6-power.poke --torn-map
  ../dumps/snap6/torn-map.bin --pcsample 1000 -M nano7,mmu=0x08b28000,torn-ignore-pc=0x093d2a80:0x093d4100

## Session 13 (2026-10-13): NAND page store + sftl miss oracle; lba-mismatch wall gone
PROTOCOL (env NANO7_FMC_TRACE=file, raw reg trace c=CPU v=sequencer): program = cmd 0x8a/0x87 + 4 addr
  bytes (+0x0c = page addr cau<<19|block<<7|page), per 4 KB sector: meta in +0x60..+0x6c, go 0x34 (meta
  out), +0x34 = DRAM addr written per 1 KB chunk with gos 0x260/0x2e4 x4; cmd 0x17 confirms. 4 sectors
  per 16 KB page, CE0+CE1, 2 planes (bit 19). Read = cmd 0x07/0x0a + 8 addr bytes (+0x0c byte0 = nsect<<4,
  byte2 = first<<4; +0x10 = page) queued per CE, served in order by cmd 0x7a: per sector go 0x32 (meta
  -> +0x60..6c, program copies it to a staging buf 0x08cd7b00+16k), +0x34 = DRAM addr ONCE (bit31 alias),
  gos 0x62/0x1e2. 0xb0/0xb8 + 4-B DMA = page-address list (not erase); no erase cmd seen in 300 s.
  The old model returned the LAST WRITTEN +0x68 on reads -> the bogus "meta:0x410f5". Real DMA out in
  Session 12e was 67 MB, not 50.7 (1 KB-chunk length miscounted).
META (sftl, 16 B/4 KB sector LE): +0 type (1 data, 2 last, 0x1c BTOC), +1 flags (bit1 uECC), +2/+4 seq,
  +8 LBA (4 KB units), +0xc = +4. s_verify_meta 0x0854b9f0 (only caller s_read_real 0x0854a3e4) checks
  only +8 vs the extent walk (G=0x08d0c6a0: exts {u64 start,u64 count} [G+0x2d8], off [G+0x2dc], meta buf
  [G+0x2e0]) and the uECC bit; skipped when flush status &0xc (per-page status byte 0x49 -> 0x8).
STORE (nano7_fmc.c): sparse page store, records = 0x80 hdr (magic N7PG, ce, addr, valid mask, 4 metas) +
  16 KB data, appended on first program, index rebuilt from headers on open. `nand-store=FILE` /
  run.sh --nand-store (default: unlinked temp file). Read-after-write is exact (data + meta).
WALL ANALYSIS: the panicking reads hit pages programmed BEFORE the snapshot (0x89adc.. < this run's
  0x9ae9..); snap6's meta buf already held LBAs 0xc0cf-0xc0d2 = a read stream in flight at T0. So a store
  alone cannot fix it. `nand-miss=oracle` (run.sh --nand-miss; default blank): after an FMSS run, each
  never-programmed sector gets meta {type 1, lba = the verify's extent walk} in its staging slot, entry
  index i = (data DMA addr - lowest data addr of the run)/4 KB; data from `nand-disk=IMG` (lba*4 KB,
  run.sh --nand-disk) or zeros. Firmware-specific (snap6 build addresses), opt-in.
0x39700000 = power-gate controller (RE 0x08426d18 off / 0x08426f3c, 0x08005856 on): +0x10 gate off,
  +0x0c wake, +0x14 status (bit n off, n+10 settled), +0x08 shadow. Regfile opt `pgate` models it.
RUN (300 s, CMD below): 1973 FMSS runs (was 1057), DMA in 14.6 MB / out 73.6 MB; 4494 pages programmed;
  read sectors 382 store hits (read-after-write, verified by the FTL) + 3194 oracle fills; NO panic.
  Furthest: the app's display path programs the display pipe 0x39000000 (layers 240x432 = 0x00f001b0,
  FBs 0x0936d66c / 0x0b82ac00, +0x04 = 1 start) at ~167 s, then power-gates domain 6 (first run without
  pgate spun there forever at 0x08426d78). With pgate: idle wfi 0x0862adec for the last ~217 s.
  LCD 0x38900000 / panel 0x3d800000: still NOT touched. Blank policy (no oracle): same panic as 12e.
NEXT: (1) the display pipe 0x39000000 likely waits for a frame-done IRQ: model it (find its VIC line),
  dump the FB on start for a first screenshot; (2) get a `dd` of the device to feed --nand-disk (check
  LBA 0 alignment vs the FTL's LBA space); (3) blank policy: report per-page status 0x49 for erased pages.
CMD: ./run.sh --ramfull ../dumps/snap6/ram-full.bin --iram ../dumps/snap6/iram.bin --ctx fixups/snap6-ctx.bin
  --overlay ../dumps/snap6/overlay.bin --os-poke-file fixups/snap6-power.poke --torn-map
  ../dumps/snap6/torn-map.bin --pcsample 1000 --nand-miss oracle
  -M nano7,mmu=0x08b28000,torn-ignore-pc=0x093d2a80:0x093d4100

## Session 14 (2026-10-13): display pipe 0x39000000 + frame-done IRQ 37 -> first emulated screenshot
RE (snap6 build): the display driver is a command-ring interpreter 0x081baae0 (ring ctx+4, 2048 words,
  register pointer table 0x0890b788): cmd 1 = layer 0 (+0x10..+0x28), 2 = layer 1 (+0x30..+0x44), 3 =
  +0x280..+0x28c (blend?), 4-7 LUT/matrix blocks, 8 = +0x220.., 10 = kick (+0x04 |= 1, busy=1, stop),
  11/12 = fences (semaphore 0x8427a60). Nothing polls after a kick; the ISR drains the ring.
  IRQ 37 (bank1 bit5), registered by init 0x081bbd98 -> wrapper 0x081bb2e1 -> ISR 0x081bbec6: ack
  +0x0c |= 1 (W1C), re-enter the interpreter. +0x00 bit0 idle (init/power-off spin for it), bit1
  running (submit 0x083bab28 won't kick); +0x08 bit0 irq enable; +0x260 version >= 0x60005. Format 6 =
  ARGB8888 (driver enum 13), stride in pixels, size = w<<16|h (0x00f001b0 = 240x432).
MODEL: nano7_disp.[ch] (new; build.sh + meson.snippet updated): kick -> +0x00 = 2, 16 ms later +0x00 = 1,
  +0x0c bit0, IRQ 37 while +0x0c & +0x08 & 1. `disp-dump=DIR` / run.sh --disp-dump DIR: each frame
  writes the layers whose contents changed as DIR/frame-NNNN-lK.ppm (cap 500). resume-hw now also
  enables IRQ 37 (bank1 0x00500023) and sets +0x08 = 1 (display was on at T0; MMIO not in snapshot).
RUN (300 s, Session 13 CMD + --disp-dump): 3 kicks, 3 frames done and acked, no panic, then idle wfi
  0x0862adec for the last ~177 s. NAND numbers identical to Session 13 (1973 runs, 14.6 MB in, 73.6 MB out).
  frame 1 L0 (FB 0x0936d66c) = DumpRAM V6 "OK - REBOOT ME / FILES SAVED": the resumed app finished its
  dump in emulation. frame 1 L1 / frame 3 L0 (FB 0x0b82ac00) = the older "TAP TO DUMP" screen; frame 2
  = a 16x16 layer at 0x0bf29380. PNGs in emulator/screens/session14/.
  LCD 0x38900000 / panel 0x3d800000: still not touched (the pipe output itself is enough for capture).
OPEN: why frame 3 flips back to the stale buffer (app exit -> launcher redraw not yet happening?);
  what waits after frame 3 (next: a thread/semaphore survey of the idle system).

## Session 15 (2026-10-13): what the idle system waits on after frame 3 = a software-timer deadlock
METHOD: run.sh ... -monitor unix:SOCK,server,nowait (extra args pass through), then monitor `stop`,
  `pmemsave 0x08000000 0x4000000 "FILE"` (QUOTE the path: an unquoted /tmp parses as a division) ~30 s
  after display kick #3. CPU then: kernel idle loop pc 0x0862adec, svc mode.
THREADS (table 0x08aad6a0, 79 x 60 B: w0/w1 links, w2 timer node, w4 saved sp, w7 state, w8 id, w9 prio;
  state 1 susp, 4 sleep, 8 mutex, 0x20 queue, 0x40 event (slots 0x8aad208), 0x80 FIFO, 0x100 dead; tick
  0x0890b964): ready list = idle T0 only. No thread waits on hardware: NAND T34 and run-loop workers
  T5/T7/T58 idle on empty queues, nobody holds a display fence (no stack in 0x081baae0/0x083bab28),
  power mgr T24 on empty queue 4, 5 event waiters with timeouts on a consistent, acyclic timer list.
DEADLOCK: timer thread T3 (loop 0x0800394c) and the app thread T36 (chain 0x08218573 -> 0x0841c435
  = OS "stop timer" -> 0x08404ec9 -> 0x08024869) both sit in 0x080247c8 = cancel-and-wait on timer
  object 0x0b3c77e8: waits on cond 0x0899b724 until +0x24 == 0x73746f70 ('stop'). +0x24 is 'expi'
  (0x65787069 = firing). +0x30 = run-loop state: 'run ' (item queued to run-loop queue 0xb439dec) at
  T0 (overlay-merged start), 'expi' now; the run-loop pop 0x0800d8b8 sets +0x30 = 'stop', the timer
  thread sets +0x24 (writer 0x080039c2, literal 0x08003a7c). The only thread that could finish the
  firing (T3) is itself waiting -> all OS software timers are dead and the app never finishes its stop.
  The snapshot caught this timer mid-dispatch (cf. 12e: thr 24 power-save).
  Also: T30 (backlight/dim poller, entry 0x080cfd19) sleeps 100 ticks on node 0x08a8d368, which is on
  NO timer list already in the start image -> never wakes (second torn-snapshot casualty).
EXPERIMENT: start poke 0x0b3c780c = 'stop' -> identical run (3 frames, then idle): the state is
  re-set legitimately once T3 runs; a start-time poke is not enough.
FRAME 3: the app shows nothing new by design: dumpram.c's S_OK state draws once and then does nothing.
  Frame 3 flips to the stale "TAP TO DUMP" buffer; the resident's ~60 fps heartbeat (an OS timer, see
  NanoApps silver_resident.c) is a casualty of the dead timer service. The device's resident predates
  that source (no 'TIMR' magic at 0x09135c80 in snap6 or the idle RAM).
NEXT: RE the timer-thread fire path (0x0800394c..0x08003a80) to know which state T3 was in for
  0x0b3c77e8 at T0 and make the timer-list fixup restore a consistent dispatch (finish it, or re-queue
  it), plus relink T30's node 0x08a8d368. Or DumpRAM V7: add the timer objects (0x0b3c0000 block) and
  the timer list to the masked T0 set.

## Session 16 (2026-10-13): software-timer fixup -> UI loop resumes, then display-off sequence
RE (OS software timers, g = 0x08817218): obj +0 next, +8 period ms, +0x10 u64 expiry us, +0x18 receiver
  queue, +0x20 callback, +0x24 OS state, +0x28 in-list, +0x30 Qt-layer state, +0x34 owner, +0x38 event
  queue. g+0 armed list (sorted), g+4 expired stack, g+0x14 cond waking T3. Start 0x08405098: stop,
  expiry = now + period, 'run ', sorted insert, in-list 1. T3 (timed wait 0x08003a66) pops the head via
  0x080247c8 (removes only if 'run ' + in-list) -> 'expi' (0x08003992) -> callback ? 'stop' + call :
  post a node to the receiver queue (state stays 'expi'). T5 (loop 0x0800455c) completes it: 0x0800716c
  (+0x30 'expi', 'time' msg to +0x38 queue), 0x08041314 ('stop' + broadcast cond 0x0899b724).
  Periodic timers are re-armed by the app's 'time' handler, not by T3.
ROOT CAUSE: in the T0 overlay g+0 = 0x0b3c77e8 (500 ms, owner 0x0b3c7418), already fired (+0x24 'expi',
  in-list 0, next 0, node queued to 0x0b439dec), while the real armed chain 0x0b3bd724 -> 0x0b3c07f8 ->
  0x0b3bd764 was intact (overlay blocks copied ~97 ms apart). T3 kept re-firing the stale head and then
  waited on it itself -> deadlock (Session 15). Not the heartbeat timer itself: that is a 16 ms timer
  (e.g. 0x0b3c10e4) re-armed by its handler, which died with the service.
FIX: fixups/snap6-swtimer.poke (0x08817218 = 0x0b3bd724). fixups/snap6-resume.poke = snap6-power +
  snap6-timerlist (12e kernel sleep list; relinks thr 30's node 0x08a8d368) + snap6-swtimer.
RUNS (300 s, both): swtimer-only and full resume set behave the same: 9 kicks / 9 frames (was 3), no
  panic, NAND 2088 FMSS runs, 16.7 MB in / 74.1 MB out, 432 store hits, 3637 oracle fills.
  frames 5-6: the app's "OK - REBOOT ME" view redrawn (UI loop alive again); frame 7: new full-screen
  gray layer (FB 0x0b694c00); frame 9: 240x27 gray strip (FB 0x0b68e680). PNGs: screens/session16/.
  Then: IRAM clock reprogramming (0x220008a8.., clkcon 0x3c500000/04/1c/24), 0x38501000 = 0, serial
  engine frame 0x6200 (PMU reg 0x62 = 0?), and the FIRST LCD access: 0x38900008 = 0. CPU idles after.
  Reading: the OS's dim/auto-lock path (screen off after inactivity) - plausible, NOT yet proven.
NEXT: confirm the sleep path (find the idle/auto-lock timer and the 0x6200 PMU write meaning); a wake
  source (button via I2C1 slave 0x73 reg 0x07 per workers/codex-touch.txt, or touch) to bring it back;
  that also starts the CLAUDE.md button/touch checklist.
CMD: ./run.sh --ramfull ../dumps/snap6/ram-full.bin --iram ../dumps/snap6/iram.bin --ctx fixups/snap6-ctx.bin
  --overlay ../dumps/snap6/overlay.bin --os-poke-file fixups/snap6-resume.poke --torn-map
  ../dumps/snap6/torn-map.bin --pcsample 1000 --nand-miss oracle [--disp-dump DIR]
  -M nano7,mmu=0x08b28000,torn-ignore-pc=0x093d2a80:0x093d4100

## Session 17 (2026-10-13): I2C + PMU + GPIO IRQs, Home button wakes the screen -> real iPod UI
RE: I2C (S3C-style) bus0 0x3c600000 IRQ 21, bus1 0x3c900000 IRQ 22: +0 IICCON (bit7 ACK, bit4 pending,
  write 0 = resume), +4 IICSTAT (mode bits7:6, bit5 START/STOP/busy, bit0 NACK), +0xc IICDS, +0x20
  status2 (bit8 byte done, bit13 stop done, W1C); IRAM driver 0x22000522.., ISR 0x22001214. PMU = bus1
  0x73 (also 0x1a, 0x18; bus0 0x08). A PMU reg read = [reg] write + STOP, then a 1-byte read.
  Buttons: PMU regs 1..4 event status (read + W1C, reg3 bit0 Home, bit1 Power, bit3 Play), 5..8 levels
  (reg7 bit4 Home, bit5 Power, reg8 bit1 Play; 0 = pressed), 0x7c..0x7f wake status. GPIO 0x56 = PMU
  event line (active-low, 0x3cf00144 bit 6). GPIO irq groups in 0x39700080..: GPIO n -> b = n>>3,
  g = b ? 1 + ((b-1)>>2) : 0, bit = b ? (n&7) + 8*((b-1)&3) : (n&7)+2; +0x80 cfg, +0xa0 pending W1C,
  +0xc0 mask, +0xe0 enable; g0..g4 -> VIC 33/32/31/3/2. GPIO ISR 0x083a33be: callback, then posts the
  registered semaphore (0x08427a66). Button thread T25 entry 0x08567808, semaphore {cnt, event 0x74}.
MODEL (new files, build.sh + meson.snippet updated): nano7_i2c.[ch] (both controllers, byte done 20 us
  after start, regfile slaves), nano7_pmu.[ch] (slave 0x73: regs, W1C status, GPIO 0x56 low while
  regs 1..4 != 0; buttons via keyboard h/p/space (monitor `sendkey`) and `buttons=` / run.sh --buttons
  "SEC:home|power|play[:MS]/..." ("/" separated: -M splits on commas); PMU accesses traced for 64
  accesses after each button), nano7_gpio.[ch] (groups + level overlay at 0x3cf00144). resume-hw
  re-arms GPIO 0x56 (0x397000ec/cc |= 1<<14). nano7_disp: t= timestamps, skips layers < 64x64 px.
EFFECT OF A REAL PMU (no buttons): PMU reads used to time out (error 0x60); now the app's UI heartbeat
  runs at ~60 fps from t=1.8 s, the OS dims at ~20 s (frames every ~5 s) and turns the screen off at
  t=37.6 s (PMU serial frame 0x6200, first LCD write 0x38900008 = 0). NAND numbers unchanged.
SNAPSHOT TEAR #4: T25 blocked on its semaphore, but count 0 instead of -1 -> the ISR's post (signals
  only when old count == -1) bumped it to 1 and never woke T25. fixups/snap6-buttons.poke (0x0899c0fc =
  -1), now part of fixups/snap6-resume.poke. (A general repair: for every thread in event wait on a
  {cnt, id} semaphore, cnt 0 -> -1.)
RUN (125 s, --buttons "60:home:100/90:home:100"): press -> IRQ 3 -> T25 reads regs 1..4 (reg3 = 1),
  W1C, samples reg7 0x20 (pressed) then 0x30; t=61.0 PMU power-up writes (0x0d, 0x24-0x27); t=61.5 the
  app exits with the springboard icon-zoom animation (112x112 / 118x128 layers ~60 fps, game icon), a
  240x27 status bar "12:52 PM"; second press at 90 s -> animation again at 91.7 s; at t=112 a full
  home-screen layer (icon rows, page dots, "7:07 AM", battery), then dim. PNGs: screens/session17/.
OPEN: the 240x432 home layer at t=112 is sheared diagonally (stride/format of that layer: check +0x3c
  and the format code before blaming the OS); dumps are per layer, no compositing yet; Power/Play
  presses untested; volume buttons (GPIO 0x28/0x29, 0x3cf000a4 bits 0/1, IRQ 31) not modeled.
CMD: add --buttons "60:home:100" (or `sendkey h` on a -monitor) to the Session 16 CMD; fixups/
  snap6-resume.poke now also contains snap6-buttons.

## Session 18 (2026-10-13): real scan-out = LCD 0x38900000 + panel TE; blitter modeled -> correct home screen
FINDING 1: 0x39000000 is a memory-to-memory 2D BLITTER, not a compositor (records: type 1 0x081bb534,
  2 0x081bb3dc, 3 0x081bb8e8, 9 0x081bb83c, 8 0x081baa3c). L0 = source (+0x10 fmt, +0x14 addr, +0x1c
  stride px, +0x20 origin x<<16|y in quarter px, +0x24 size), L1 = destination read-modify-write (+0x30
  fmt, +0x34 base, +0x3c stride, +0x40 size, +0x44 position x<<16|y). +0x04 bits 25:24 mode: 0 copy,
  2 blend (+0x280..+0x28c), 1 fill (+0x290). +0x50/+0x54 scale steps. Blend words: bits 30:29 selector
  (0 = the word's own constant: bytes 0-2 colour, byte 0 alpha), 27:24 op (0 one, 1 src alpha, 7 const
  alpha = low byte of w1/w3), bit 28 = 1 - f; out = S*fs + D*fd. (First guess "selector 0 = factor 0"
  zeroed the alpha of icons blended into tiles -> labels without icons; the constant reading matches
  all 5 driver presets.) Format 6 = ARGB8888 straight alpha. The old model never moved pixels: the
  "sheared home layer" of Session 17 was a stale pre-snapshot scratch buffer, not the screen.
FINDING 2: the screen is LCD controller 0x38900000, 2 windows (+0x20 / +0x40: ctl alpha 7:0 + format
  11:8 (4 RGB565, 7 ARGB8888), base, stride px, src x/y, size, dst x/y; enables +0x04 bits 4/5),
  RE'd from the screenshot capturer 0x08240860 and commit 0x08080b70. The commit only runs from the
  panel TE GPIO callback 0x08060b19 (GPIO 14 = 0x3cf00024 bit 6, group 1 bit 6, VIC IRQ 32): with no
  TE the home composite was rendered but never flipped (drv 0x0998a9c4 slot +0xc0 = 6, +0xc4 = 5).
  A commit rewrites only enable, +0x10 and window 0's base; the geometry was set once before T0.
  LCD IRQ 13 has an assert-stub handler: never raised.
MODEL: nano7_lcd.[ch] (new; build.sh + meson.snippet): registers read back, TE on GPIO 14 every
  16.67 ms (1 ms low), at each TE windows composited (win0 opaque, win1 alpha) into a 240x432 frame;
  disp-dump=DIR now writes DIR/screen-NNNNN.ppm when the SCANNED-OUT frame changes. nano7_disp: real
  blits (copy/blend/fill, nearest scaling, format 6 only), done after 20 us + 5 ns/px (was 16 ms per
  kick), its own screen guessing removed; NANO7_DISP_TRACE=1 logs every kick. nano7_gpio: any pin can
  be modeled (per-bank level overlays), edge logs capped per pin, NANO7_GPIO_TRACE=1 for register
  traces. resume-hw: GPIO 14 mask (0x397000c4 bit 6) + LCD window 0 geometry (ctl 0x002107ff, stride
  240, size 240x432, at 0,0). nano7_unimpl: per-1 MB-region access totals at exit.
RUN (125 s, Session 17 CMD + --disp-dump, Home at 60 s and 90 s): commits flip window 0 between pool
  buffers 0x0bec3f00 / 0x0bf29380 at ~16 fps; dumps: the app ("OK - REBOOT ME"), the app-exit zoom
  animation (t=64.2), and the HOME SCREEN page with all icons (Lines, Nanoracle, Nanosync, Mario Kart,
  DumpROM, DumpRAM), status bar 12:52 PM, battery, page dots (t=65.1). PNGs: screens/session18/.
NEW WALL: after the second press the OS shows "Low Battery / Connect to Power" (t=95.8): the PMU model
  returns 0 for the battery/charger registers it polls every 20 s (0x30 reads 0x14 only because the
  OS wrote it; 0x3c..0x3f, 0x05 = 0). Next: RE the battery gauge reads and give the PMU plausible
  values (charged, on USB power) before the OS decides to shut down.
CMD: Session 17 CMD (fixups/snap6-resume.poke, --nand-miss oracle, --buttons ...) + --disp-dump DIR.

## Session 19 (2026-10-14): PMU battery/charger -> no more "Low Battery"
CAUSE: the battery check is a PMU ADC conversion. The model kept 0x30 bit4 (start/busy) set after the OS
  wrote 0x14, so the OS aborted / read 0 -> 2500 mV -> level 0 -> "Low Battery / Connect to Power".
RE (snap6 build): 0x30 = ADC control (bits 3:0 channel, bit4 start/busy; the OS aborts with 0x4d if busy
  before a start, polls it clear ~1 ms after, 5 retries, averages 5 samples); result 10 bits =
  (0x32 << 2) | 0x31. Channel 4 = battery: mV = 2500 + raw * 2000 / 1023; level 0..15 = count of a
  15-entry mV table (0x0890c490, 4 temperature bands; room temp 3550..4110 mV); power task 0x0801969c
  flags low battery below level 2 (~3666 mV). Channel 1 = charger load %, must stay < 3. 0x3c..0x3f =
  temperature fields (0x0818a4e8). 0x05 bit6 external power + bit3 charger good; 0x29 bit2 + limit
  code (2 = 500 mA); 0x0d bit3 must be 0. Snapshot values: 4024 mV, level 14 (0x0890c078/7c).
MODEL (nano7_pmu.c): ADC completes at once (bit4 reads clear), channel 4 returns 4100 mV (raw 818:
  0x32 = 0xcc, 0x31 = 2), others 0; temperature ~24 C (0x08 0x84 0x7d 0x7d, read-only); reg 0x05 =
  0x48, 0x29 = 0x06. NANO7_PMU_TRACE=1 logs every PMU access.
RUN (160 s, Home at 60/90/130 s, --disp-dump): OS battery state 4099 mV, level 14 (read from RAM at
  ~145 s); no low-battery screen; second press shows home page 1 (Music, Videos, Fitness, Podcasts,
  Photos, Radio), full battery icon; still fine after the third press. PNGs: screens/session19/.
SEEN, NOT FIXED: (1) the status-bar clock jumps from 12:52 PM to 12:18 AM, later 12:17 AM - the PMU
  RTC (not modeled, reads 0) is the likely source; (2) at start the OS polls PMU 0x10 (~700x, ~1.5 s)
  and 0x40..0x43 (~90x) until it gives up - unknown status bits.
CMD: Session 17 CMD + --disp-dump DIR (fixups/snap6-resume.poke, --nand-miss oracle, --buttons ...).

## Session 20 (2026-10-14): PMU real-time clock + blitter fill fix -> correct, clean status-bar clock
RE (PMU 0x73, snap6 build; read 0x0801ed02 = write [reg] then N-byte read, set path 0x08010b1c):
  0x40..0x43 u32 LE free-running 1 Hz counter (never written; the ~90 boot reads of 0x40..0x43 noted in
  Session 19 are its poll), 0x74..0x77 u32 LE persistent offset, Unix time (UTC) = counter + offset;
  snapshot offset 0x6a090928 (2026-05-17 00:17:44, also cached at [0x0890c0a8+0x18]); 0x45..0x49 alarm
  + enable (plain). With the counter at 0 the OS saw exactly the offset -> the "12:17/12:18 AM" clock.
  The OS also keeps a time-of-day cache at 0x08a6b7fc (u16 year, u8 mon, day, u16 yday, s8 tz in 15 min
  (-32), u8 dst (1), h, m, s, flags; snapshot 2026-10-13 07:06:33) that it advances with uptime and
  refreshes from the RTC only now and then; the status bar shows the RTC time without tz conversion.
MODEL (nano7_pmu.c): counter on the virtual clock, read latched at 0x40; offset writable; machine prop
  rtc= / run.sh --rtc: "snapshot" (default; RTC starts at the cache's time, taken as UTC, so both clock
  sources agree), "host", or Unix seconds.
BLITTER FIX (nano7_disp.c): mode 1 (fill) now writes +0x290 as is, alpha included. Applying the blend
  words kept the old alpha -> opaque black boxes where the old clock digits were (minute redraw = fill
  129x26 at (55,1) of the status-bar buffer, CPU draws digits, src-over onto the screen).
RUNS: --rtc 2026-10-14 15:30:00 UTC -> "3:32 PM" at t=121 s (RTC path correct, UTC shown) but "7:08 AM"
  in another frame (cache path) = the jump the snapshot default now avoids. Default run (Home at
  60/90/130 s): "7:08 AM" at t=91.7 = 07:06:33 + 92 s, digits clean. The "12:52 PM" right after the wake
  is the status bar as last rendered on the device before the snapshot, until the next minute redraw.
OPEN: whether the cache's 07:06:33 is UTC or local (tz -8 h, DST) is not settled; use --rtc host for
  wall-clock time (expect one jump when the OS next refreshes its cache from the RTC).
CMD: Session 17 CMD + --disp-dump DIR [--rtc snapshot|host|SECONDS].

## Session 21 (2026-10-14): live display over VNC, mouse = touchscreen, keys = buttons
DISPLAY: the build already has VNC (pixman yes; SDL/GTK disabled), so no reconfigure. nano7_lcd.c now owns
  a QEMU graphic console: its 240x432 XRGB composite (per TE pulse, bulk row reads instead of per-pixel
  loads) IS the DisplaySurface; dpy_gfx_update_full only when the frame hash changes. run.sh --vnc [N]
  adds -vnc :N (port 5900+N; -nographic kept). tools/vnc_probe.py = minimal RFB 3.8 client (shot, tap,
  key, watch) since no VNC client is installed here.
TOUCH (RE re-verified on snap6, Session 21): serial port 2 0x3d200000, IRQ 11, handler 0x08000fec:
  +0x00 bits 2/3 FIFO flush, +0x04 read-back + bit8 START, +0x08 status bit0/bit22 done, bit1 TX
  request, bit5 RX data (W1C) + live levels 10:6 TX / 15:11 RX, +0x0c bit1 CS (clear = selected),
  +0x10 TX, +0x20 RX pop, +0x34/+0x4c length. MultitouchTask = thread 10 (id 0xa), blocked on event 0x43
  from ready callback 0x08163ab1 (GPIO 0x26 = group 1 bit 30, IRQ 32); per report: 16-byte length query
  (tx[2] = 0; reply LE16 L at 1, LE16 sum of 0..13 at 14) then fetch of L+5 bytes (tx[2] = 1; EA 01 L
  hdr-checksum, payload, LE16 sum), then re-arm (mask bit set, pending cleared). Real gate 0x089144d4[0,1]
  = 1,1 in snap6 (0x08909068 is not it). nano7_touch.[ch] (new): one-contact reports (0x44 payload,
  record off 0x12 stride 0x10, id 1, type 1, state 3 down / 4 move / 5 up, raw = inverse of x =
  (rawX+75)*239/2845, y = 431-(rawY+75)*431/5250); ready line low only while a report is queued AND the
  OS armed the line, high again on CS release after the serving fetch. Host mouse via an absolute
  QemuInputHandler (VNC/GTK/SDL); moves coalesced. resume-hw: 0x397000c4 = 0x40000040 (TE + touch),
  0x397000e4 |= 1<<30. nano7_gpio: nano7_gpio_irq_armed(). Keys h/p/space: the PMU input handler (same
  nano7_pmu_button path as --buttons) works over VNC.
VERIFY (canonical snap6 CMD + --vnc 5, --buttons "60:home:100/70:home:100"): VNC frames of the app and
  home screen; tap at (64,86) on home page 1 -> touch down/up fetched -> Music app opens (Genius Mixes,
  Playlists, Artists, ...); 7 frame updates in 4 s streamed during the transition; VNC key h -> PMU Home
  -> back to the home screen (clock 7:07 AM). PNGs: screens/session21/.
OPEN: tapping the homebrew "Lines" icon only shows the pressed highlight (launch stalls; the app's files
  are pre-snapshot NAND data = oracle zeros without a disk image); touch after screen-off may need the
  controller firmware-upload path (0x0801f9dc) that is not modeled; multi-touch, volume buttons not done.
CMD: ./run.sh --ramfull ../dumps/snap6/ram-full.bin --iram ../dumps/snap6/iram.bin --ctx fixups/snap6-ctx.bin
  --overlay ../dumps/snap6/overlay.bin --os-poke-file fixups/snap6-resume.poke --torn-map
  ../dumps/snap6/torn-map.bin --nand-miss oracle --vnc 5 [--buttons "60:home:100"]
  -M nano7,mmu=0x08b28000,torn-ignore-pc=0x093d2a80:0x093d4100    (then: vncviewer HOST:5905)

## Session 22 (2026-10-14): volume buttons; DSI controller; Lines launch traced to a FAT-cache anomaly
VOLUME (RE snap6): GPIO 0x28 up / 0x29 down (0x3cf000a4 bits 0/1, active-low), group 2 bits 0/1 -> IRQ
  31; GPIOButtonManagerTask (idx 19, entry 0x080de2cc) waits on event 0x48 (callback 0x081541f4), the
  GPIO ISR flips +0x88 polarity per edge -> needs press AND release interrupts; 10 ms debounce, emits
  0x08408570(8 up / 7 down). No snapshot tear. MODEL: nano7_gpio_model_pin_both_edges (rising edges
  latch too); buttons volup/voldown (--buttons) and keys Up/=/keypad+ and Down/-/keypad- via the PMU
  front-end; resume-hw: 0x397000c8 |= 3, 0x39700088 |= 3. Verified: press+release edges reach IRQ 31.
DSI (nano7_lcd.c): MIPI DSIM 0x3d800000 modeled: payload FIFO +0x38, packet header +0x34 (0x29 DCS long
  write: 0x2a/0x2b window, 0x2c/0x3c RGB888 pixels into panel GRAM; 0x05 short writes ignored), FIFO
  control +0x44 always empty/not full. The OS sends 0x2a/0x2b + short 0x2c every frame and used to spin
  on the unmodeled FIFO status (10.9 M unimpl accesses per run). GRAM is now the displayed surface;
  window composites are copied in only when they change, so direct DSI drawing would persist.
DISK IMAGE: dumps/ipod-disk.dd (15.8 GB, dd of the device) = MBR + FAT32 partition at block 63 (4 KB
  sectors, 4/cluster, rsv 32, 2 FATs x 942, root clus 2, data at partition block 1916 = image 1979).
  FTL LBA = image block + 8661 (verified: Lines.hbapp cluster 9672 = image 40659 = FTL 0xc0a8, read by
  the OS on launch). New option nand-disk-offset= / run.sh --nand-disk-offset 8661. Without the offset
  every oracle fill was 8661 blocks off. The image was taken hours after snap6 (dump files moved), so it
  is not fully consistent with the snapshot.
LINES LAUNCH (RE + gdbstub traces; tools/rsp_trace.py = tiny GDB-remote client: breakpoints, step-over,
  return values, memory at r0/r1/r2): resident at 0x09880000..: tap -> launch 0x098868F4 -> loader
  0x09887C88: size wrapper 0x09886EA4 returns 111012 (correct), 24-byte read 0x09886E60 returns 24 but the
  bytes are garbage -> magic check fails -> silent failure (tap consumed, icon stays pressed). Inside
  the read: FAT get 0x084F510A(vol 0x08AE77BC, cluster 9672) returns 0x32013346 every time (image FAT
  says 9673); the only NAND read in that window is FTL 0xc0a8 (the file's own first block), and
  0x32013346 is exactly that block's bytes at offset 1824 = where FAT[9672] sits in a FAT block (real
  FAT block 104 / FTL 0x223d has c9250000 there, never read). The bad chain then reads FTL 0x15f13.
  So a FAT-sector request is satisfied with the data block's bytes - most likely a block read cache in
  the FS/FTL layer (all FTL reads land in buffer 0x8b40a440) whose tag state from the snapshot no longer
  matches the buffer contents. Not yet located. Snapshot FAT cache (vol+0x8c, 32 slots) matches the
  image; nothing dirty (subagent RE: FAT cache tags 0x08AE7874.., 512 B sector cache 0x08AE7FAC x256).
ALSO: Music hangs with --nand-disk (even with the offset) but opens without it - same mixed-state issue.
NEXT: find the block-read cache (who maps FAT LBA 0x223d onto the 0xc0a8 buffer; break on the callers of
  wrapper 0x0854db60 with the FAT-get on the stack) and invalidate its tags at resume; or take a fresh
  DumpRAM snapshot + dd pair with no writes in between.
CMD (with disk): Session 21 CMD + --nand-disk ../dumps/ipod-disk.dd --nand-disk-offset 8661.

## Session 23 (2026-10-14): the "read cache" was a wrong disk offset -> Lines (GBA emulator) and Music run off the real disk
TRACE (tools/rsp_trace.py, new RSP_STACK=lo:hi lists return addresses in another thread's stack):
  requester stack at the bad FTL read: FAT get 0x084F510A <- FAT lookup 0x084f51cd -> block read
  0x083d4e98(dev, 512 B sector, buf, n) -> 0x08543f04 -> 0x084df730 -> (FTL thread) s_read_real.
  Arguments: FAT refill = sector 0x14b (FAT1 sector 75, volume-relative, 512 B units) into FAT-cache
  slot 11 (0x08b16cac) -> FTL LBA 0xc0a8; the data read = sector 0x4f4a0 x8 -> FTL 0x15f13.
  (sector + 504 partition sectors) / 8 = image block 104 resp. 40659, so BOTH reads are image block +
  49216. There is no faulty cache: Session 22's offset 8661 was inferred from 0xc0a8 = "the file's
  first block", but 0xc0a8 was the FAT read. With 8661 the FAT refill got the Lines data block
  (0x32013346 = its bytes at offset 1824) and the chain went astray.
FIX: --nand-disk-offset 49216 (0xc040): FTL LBA = image block + 49216 (the dd = MBR + FAT32 partition at
  block 63; 49216 x 4 KB = 192 MB of FTL space before it). Snapshot check: the read in flight at T0
  (LBA 0xc0cf) is image block 143 = FAT1, i.e. DumpRAM allocating clusters - consistent.
RESULTS (snap6 CMD + --nand-disk ../dumps/ipod-disk.dd --nand-disk-offset 49216 --vnc 5):
  tap Lines -> the homebrew GBA emulator starts, loads a ROM from disk (Pokemon FireRed), 62 FPS, game
  advancing (Pallet Town at +3 s, Gary's dialogue at +20 s). Music opens and Songs lists the real
  library ("Maladie"). No panics. PNGs: screens/session23/. The image is still hours newer than snap6;
  no inconsistency visible yet, but writes from the emulated OS land on the NAND store only.
CMD: ./run.sh --ramfull ../dumps/snap6/ram-full.bin --iram ../dumps/snap6/iram.bin --ctx fixups/snap6-ctx.bin
  --overlay ../dumps/snap6/overlay.bin --os-poke-file fixups/snap6-resume.poke --torn-map
  ../dumps/snap6/torn-map.bin --nand-miss oracle --nand-disk ../dumps/ipod-disk.dd --nand-disk-offset
  49216 --vnc 5 [--buttons "60:home:100"] -M nano7,mmu=0x08b28000,torn-ignore-pc=0x093d2a80:0x093d4100

## Session 24 (2026-10-14): save state (F2 / F4) and reset (F1)
UI: new nano7_ui.[ch] owns the single QEMU keyboard handler (QEMU delivers keys to one active handler):
  button keys go to nano7_pmu_key() (the PMU's own handler is gone), hot keys run from a main-loop BH.
SAVE STATE: slot directory under state-dir (machine prop; run.sh passes emulator/state): ram-<region>.bin
  (DRAM, IRAM, VROM raw, from memory_region_get_ram_ptr), devices.vmstate (every non-RAM vmstate section
  via qmp_xen_save_devices_state(live=false): CPU, virtual clock, all nano7 devices), nand.bin (copy of
  the NAND page store). Load: VM stopped, RAM copied back, store copied back + page index rebuilt,
  device state through qemu_loadvm_state() directly (the Xen load helper's migration teardown asserts
  in yank outside a real migration), then qemu_loadvm_state_cleanup() (else the 2nd load asserts on
  the RAM receive bitmap), migration "send-configuration" set false (the device-only stream has no
  configuration section), tb_flush, vm_start. ~40-180 ms per save/load.
  New vmstate descriptions + post_load IRQ re-drive for every device: VIC (banks, glue), timer (4
  channels incl. QEMUTimers), spicmd, regfile (written-bytes table converted from GHashTable to a byte
  array), FMC + NAND store cursor (store file copied separately; nano7_fmc_store_save/load), disp
  (blitter), LCD + DSI (incl. GRAM, TE timer), GPIO (groups, bank levels), I2C buses (active slave as
  an index) + regfile slaves, PMU (regs, RTC base, script/release timers), touch (FIFOs, queue).
  Not saved: torn-map watcher state (debug aid), pcsample timer, log dedup tables.
RESET: the "boot" slot is saved automatically when the VM first runs (after resume set-up / at the
  bootrom); F1 reloads it - same path for snapshot resume and bootrom boots.
VERIFY (snap6 + disk image + --vnc 5): F2 on the home screen -> Lines (GBA game) -> F4 back to the saved
  home screen -> tap Lines again starts the game (restored FS/NAND/touch all live) -> F1 back to the
  boot state (DumpRAM "OK - REBOOT ME"). F1, F4, F1 in a row in a fresh process also fine; F4 there
  loaded slot0 written by the previous process (cross-run loading works with the same command line).
  Sheet: screens/session24/.

## Session 25 (2026-10-14): rotate (F7), integer zoom (F9), screenshot (F12)
DISPLAY PATH: nano7_lcd.c no longer owns the QEMU console; at a TE after a GRAM change it calls
  nano7_ui_frame(GRAM). nano7_ui.c owns the graphic console and renders the 240x432 frame rotated
  (F7 cycles 0/90/180/270 clockwise) and integer-zoomed (F9 cycles 1x/2x/3x, nearest neighbour) into a
  surface it resizes when the output size changes (VNC clients get the new size).
TOUCH: nano7_touch.c keeps the raw absolute pointer position and maps it through
  nano7_ui_map_touch() (undo zoom, then the inverse rotation) to panel pixels.
SCREENSHOT: F12 writes the panel frame (unrotated, 240x432 RGB) as PNG (zlib, own minimal encoder)
  to screens-dir (run.sh: emulator/screens) as nano7-YYYYmmdd-HHMMSS.png.
VERIFY (snap6 + disk + --vnc 5, home page 1): F7 -> VNC frame 432x240, rotated home screen; tap at the
  rotated Music position (345,64) -> mapped to panel (63,87) -> Music opens (rotated); F9 -> 864x480,
  crisp; F12 -> valid 240x432 PNG of the Music menu. PNGs: screens/session25/. (Only 90 deg and 2x
  exercised; 180/270 and 3x use the same code paths.)

## Session 26 (2026-10-14): pause (F5 / Pause), fast-forward (F6, Tab), save-state hardening
GUEST CLOCK: new nano7_clock.[ch]. Guest time = QEMU_CLOCK_VIRTUAL x speed, piecewise linear
  (re-anchored on every change, continuous). The time-giving devices use it: timer block (500 Hz tick,
  1 MHz counters), LCD TE pulse, PMU RTC + button script (nano7_now_ns, nano7_timer_mod). Every device
  timer is registered (nano7_clock_track); guest-time timers are re-armed on each speed change.
  Short completion delays (I2C, blitter, touch, spicmd) stay on virtual time. Saved in save states.
FAST-FORWARD: F6 cycles 1x/2x/4x, Tab held = 4x. Measured with the guest-clocked TE pulse over 20 s of
  real time: 4x requested -> ~3.1x achieved (host-CPU bound; 183 TE/s vs 60); the OS's work scales
  along (blits 1770 -> 4391). The RTC/clock runs fast while fast-forwarding (as accepted).
PAUSE: F5 / Pause = soft pause: pause_all_vcpus() + guest clock speed 0, run state stays "running".
  Pitfalls found: (1) QEMU drops all input while the VM is stopped (vm_stop -> no key could resume);
  (2) VNC delivers keys through a QEMU_CLOCK_VIRTUAL key-delay queue and pause_all_vcpus() disables
  virtual-clock timers -> re-enabled right after pausing (guest time is frozen instead); (3) periodic
  guest timers re-armed during the freeze got an infinite deadline -> the clock re-arms all of them on
  resume. Verified in the GBA game: 0 frame updates while paused, game continues after resume (FPS 61;
  low update counts afterwards = the game waiting at a dialogue box, not a slowdown).
SAVE-STATE FIX: QEMU's device stream restores the saved QEMU_CLOCK_VIRTUAL; loading an older state left
  QEMU's own virtual timers (VNC key queue) stranded up to minutes in the future (keys ignored after
  F1). Now ui_load_state keeps the virtual clock monotonic: after the load it moves
  timers_state.cpu_clock_offset forward by delta = (virtual before load) - (restored), and
  nano7_clock_after_load(delta) shifts every registered device timer and the guest clock anchor.
  Save/load also keep a soft pause active.
VERIFY (VNC, keys via vnc_probe.py; new `hold KEYSYM SECONDS`): F6 2x -> F2 -> F1 (reset, 40 s back) ->
  F4 -> frame identical pixel-for-pixel to the saved one -> F5 -> F2 while paused -> F4 while paused ->
  F5: all keys handled, no asserts. Sheets: screens/session26/.
KEYS (all over VNC / display / sendkey): h p space, Up/=/+ Down/-, F1 reset, F2 save, F4 load, F5/Pause
  pause, F6 speed, Tab hold 4x, F7 rotate, F9 zoom, F12 screenshot. Headless works (keys only matter with
  a display or the monitor's sendkey; save states also work from -monitor via sendkey f2/f4).

## Session 27 (2026-10-14): audio output - DMA, codec, I2S, decode co-processor, resource partition, MBX stub
NEW DEVICES: nano7_dma.[ch] (two 8-ch engines; ch regs base+0x100+0x20n SRC/DST/NEXT/CFG/CTRL/COUNT,
  COUNT in elements of 1<<CFG[23:21] bytes, bit26/27 src/dst increment, CTRL bit0 run, +0x04 status /
  +0x08 ack; NEXT reload, self-linked rings; passes to a paced FIFO take bytes/rate of guest time).
  IRQ MAP CORRECTED: engine 1 0x38200000 = IRQ 16, engine 2 0x38700000 = IRQ 17 (IRAM thunks 0x22001168 /
  0x2200116c pass id 1/2 to ISR 0x220010ee). The spec, HARDWARE.md and the Session 8 note had it reversed;
  docs fixed. I2S0 TX may only use engine 1 (endpoint mask table 0x890c1f4).
  nano7_codec.[ch]: SPI0 0x3c300000 IRQ 9, same SSP as touch; codec [0x6c,msb,lsb,0,d] write, lsb|0x80
  multi, [0x6d,..] read -> rx[4]; status 0x002f reads bit7 set (stop sequence spins on it).
  nano7_i2s.[ch]: I2S0 0x3ca00000; +0x00 bit1 reads 1, +0x3c reads 0, +0x40 divisor -> rate (12 MHz/div,
  snapped) -> DMA pacing; TX FIFO bytes -> PCM sink: run.sh --audio-out FILE (raw s16le / *.wav) or FIFO.
  nano7_adsp.[ch]: decode co-processor 0x38500000 IRQ 18 (was a regfile stub): ID +0xc80 = 0x004e7309
  (else panic -> power-off), +0x114 reads 7, irq group +0xa8c set / +0xa90 clear / +0xa94 mask / +0xa98
  pending / +0xc48 ack. Every job (+0x984, +0x98c=1) sets pending bit 2 after 20 us. Result protocol (parser
  0x082ad500): status 0x2203ff28 h0 bit0 = valid, h1 0 = ok (2 = need input); PCM descriptor 0x22038000
  (h[1] mode, h[2] capacity, entry0 ready +0xa / len +0xc), samples 0x22038100; 7 empty results stop the
  track. The model returns one MP3 frame of SILENCE (1152 x 2 x s16) per +0x98c doorbell.
BLOCKERS FOUND ON THE WAY (play = Music > Songs > Maladie, or space on Home):
  1. Now Playing asserted (0x083dc9a8 AnisotropicBitmapGeneratorTask, rect of UI image 0x0dad06ab = 0):
     UI images live in Resources/UI/SilverImagesDB.LE.bin on the RESOURCE partition, not in ipod-disk.dd
     (data volume only) -> the oracle served zeros. Stock 1.1.2 (39A10023) IPSW Firmware.MSE rsrc image
     (plaintext FAT16 "iPodResources") matches the snapshot byte for byte. New tools/extract_rsrc.py IPSW
     OUT.img1 + run.sh --nand-rsrc FILE[@LBA] (nano7_fmc oracle). Base FTL LBA 0x3f2 = firmware area at
     LBA 64 + MSE dev_offset 0x3b1000/4K + 1 (calibrated: 9/9 resource loads byte-identical; the first
     guess 0x3d7 from miss-range fitting was 27 blocks off -> garbage IDs -> data abort). emulator/rsrc/
     is gitignored (copyrighted firmware, 179 MB).
  2. Then a spin on MBX GPU 0x3b00012c bit 6: integrated the draft nano7_mbx.c (IRQ 53, completion only,
     nothing rendered) + +0x838 transfer job -> status bit 6. Album art area is black (no GPU render).
  3. Then idle with no output: MeCCAOutputTask had engine 1 ch 2 running at T0 (self-linked descriptor
     0x0b3539c0, IRAM ring 0x22002638 -> 0x3ca00010) and only waits for completions; it never restarts
     the channel. resume_hw now reloads the channel from that descriptor (guarded on dst = I2S0 FIFO).
RESULT (snap6 CMD + --nand-disk ... 49216 --nand-rsrc rsrc/rsrc-1.1.2.img1 --audio-out x.wav --vnc 5):
  Now Playing "Maladie" (Mauvais djo), status-bar play icon, pause button, no panic, Home still works.
  DMA engine 1 ch 2: one 4 KB pass per 23.2 ms (44.1 kHz); decoder 1205 frames (31 s of track) in the
  run; WAV 105 s 44.1 kHz stereo, peak 0 (silence, nothing decoded). FIFO sink: 24.0 s streamed in a
  25 s run. Codec saw no register traffic (already configured at T0). Screens: screens/session27/.
NEXT (real sound): host-side MP3 decode in nano7_adsp - compressed bytes come from the type-4 input job
  (block-internal DMA, port regs 0x38500100.., layout to trace), chunks up to 0x8020 bytes, not frame
  aligned -> buffer + minimp3 (no decoder lib installed here), h1 = 2 until a full frame is ready.
TOOLS: rsp_trace.py RSP_REGMEM=reg:len dumps memory at registers, importable (main guarded).

## 2026-10-14 — RAMScope v3 validated in the emulator (bare-metal harness) + extra-dram= property
nano7.c: new machine property `extra-dram=N` (what answers at PA 0x0c000000+): 1 open bus
(reads 0 / writes dropped, matches the device's read behaviour), 2 independent 64 MB RAM, 3 alias of
DRAM. Default 0 = unchanged. `tools/ramscope_harness/` runs probe/ramscope/ramscope.c unmodified
(user mode, MMU on, L1 at 0x08b28000, svc #70 stubbed, fake DMC in RAM) via semihosting output:
modes 0/1 -> NO STORAGE, 2 -> INDEPENDENT MEMORY, 3 -> ALIASED. Found+fixed a real bug this way:
write offsets beyond the single mapped 1 MB section would have translation-faulted.
Not modelled: the hardware's bus hang on writes to a rank-less PA 0x0c000000 (device evidence).

## Session 28 (2026-10-14): real sound - host-side MP3 decode in the co-processor model
WHY NO INPUT BEFORE: the Session 27 stub read +0x114 as a constant 7. It is the port state (+0x110 command
  4 = stop -> 7, polled by setup 0x083f404a; 0x10/0x20/0x40/0x60 = run -> 0), and the frame feed
  0x0814959c / 0x084d47a4 only queues input while it reads 0 -> with 7 only doorbells were written.
INPUT PATH (RE + gdbstub): DecodeFrame feeds one MP3 frame per call, 0x082ad470(obj, src, len) (src = the
  file buffer in DRAM via the 0x80000000 alias, starts with the fffb sync; 1044/1045 bytes at 320 kbps).
  0x084d4680 builds a 24-byte descriptor at [codec obj + 0x40e8] = {next, len << 16 | 0xf80, src, -, out,
  out}; 0x084d47a4 writes its address | 1 (1 = system memory) to +0x100, then +0x98c = 1. No DMA engine,
  no IRAM copy; 0x38501000 = DRAM controller (periodic zero writes; now inside the 0x2000 adsp window).
MODEL (nano7_adsp.c): +0x110/+0x114 port state machine; at a doorbell the descriptor's bytes are read
  from guest RAM into a 16 KB buffer and decoded with minimp3 (emulator/machine/hw/arm/minimp3.h, CC0,
  lieff/minimp3 @ ea99364f, MINIMP3_ONLY_MP3; copied by build.sh). PCM -> 0x22038100 + descriptor + ok
  status; no samples yet -> status h1 = 2 (need more input, benign). Decoder reset on config job (+0x984),
  port stop and state load. NANO7_ADSP_SILENT=1 = old silent frames; NANO7_ADSP_TRACE=1 = register log.
RESULT (snap6 CMD + --nand-disk ... 49216 --nand-rsrc rsrc/rsrc-1.1.2.img1, play = space on Home, or
  Music > Songs > Maladie over VNC): "MP3 44100 Hz, 2 ch, 320 kbps"; 1205 frames decoded in ~31 s of
  playback, 0 need-more-input; I2S peak 31539/32767. VERIFIED BIT-EXACT: 6/6 16-sample probes (every 5 s)
  of the emulator WAV found exactly in ffmpeg's decode of Maladie.mp3 at one constant offset -> no dropped
  or repeated frames; playback starts at track position 0.
LIVE SOUND: run.sh --audio-play = FIFO emulator/state/audio.fifo + host player (ffplay -nodisp -autoexit,
  else aplay), 44.1 kHz s16 stereo; tested with SDL_AUDIODRIVER=dummy (no sound card on this server):
  0 bytes dropped, player exits with the emulator. --audio-out FILE(.wav) still records.
LIMITS: MP3 only (AAC/ALAC tracks still get the old protocol: AAC would need a decoder, ALAC is decoded
  in software by the OS itself); the raw FIFO assumes 44.1 kHz (a 48 kHz track plays slightly slow via
  --audio-play; the WAV header follows the I2S rate); fast-forward overruns the player (drops counted).

## Session 29 (2026-10-14): Now Playing album art - it is the 2D blitter, not the MBX GPU
FINDING: with NANO7_MBX_TRACE=1 (new: logs MBX core / FIFO writes) Now Playing makes exactly ONE MBX job
  (register pairs +0x824/+0x828/+0x82c/+0x838/+0x83c/+0x6d8, the transfer from Session 27) and no 3D
  draws or FIFO streams. The art square is blitter work (NANO7_DISP_TRACE): a 240x240 layer at y = 87
  copied from 0x0b66c780 / 0x0b634300, which are filled by a black fill + a copy from the decoded art
  bitmap (e.g. 0x09487b10). That source is RGB565 = blitter L0 format 4, which nano7_disp.c rejected
  ("format L0 4 / L1 6 not modeled") -> the copy never ran -> black square. A gdbstub write watch on the
  art buffers saw no CPU writes (all device-side), confirming the path.
FIX (nano7_disp.c): source format 4 = RGB565 (R in 15:11, expanded to opaque ARGB8888, stride in pixels
  of 2 bytes) next to format 6; unmodeled formats are now reported once per format.
RESULT: Now Playing shows the art: here the stock "no artwork" note, because the user's library has no
  iPod_Control/Artwork at all (synced by iOpenPod without art) - the same picture the real device shows.
  Real cover art goes through the same RGB565 bitmap -> blitter copy, but cannot be tested with this
  disk. Screens: screens/session29/ (now-playing-art.png, art-source-rgb565.png = the dumped source).
GPU: the MBX still renders nothing; no screen seen so far depends on it (Cover Flow / games would).

## Session 30 (2026-10-14): Cover Flow on the MBX - not reachable on nano 7G 1.1.2 (investigation)
RE (static, snap6): the Cover Flow strings are leftovers. Main-menu 'HandleCoverflowHilited' (0x0843ed90,
  matched at 0x0843ea6c) pushes "EmptyMusicHilited"; the transition table maps controller.CoverFlowSelected
  (0x0888b80b) to Touch_ComingSoon_Screen; nothing references the CoverFlow_Screen / _Default strings
  (0x0888db34/48); Now Playing's 'HandleSwapToCoverflow' case (dispatcher 0x0829c704) is a no-op and its
  real handler 0x0829c97c only navigates (mode 3). Orientation handlers (video/photo) never mention it;
  the nano 7G has no accelerometer anyway. The GK log table still lists a CoverFlow domain next to TGL /
  GK2D (so it was a GL ES on MBX feature on the predecessors).
CHECKED LIVE (VNC): Music menu = Genius Mixes .. Composers, Compilations, Audiobooks, Podcasts, iTunes U -
  no Cover Flow row; Compilations opens a normal (empty) list. NANO7_MBX_TRACE: 0 MBX writes in Music,
  Compilations, Songs; Now Playing makes only the one +0x824..+0x83c transfer job. No stock screen seen so
  far draws with the MBX.
TOOLS: vnc_probe.py drag X0 Y0 X1 Y1 [SECONDS] (touch scroll). Screens: screens/session30/.
NEXT: an MBX renderer needs a consumer to verify against: a homebrew GL ES 1.x test (or the reVC port,
  docs/research/revc-gles1-*.md) driving the OS's GL stack, then RE of the TA/ISP command stream from
  the FIFO apertures 0x3b800000/0x3ba00000/0x3bc00000 (writers 0x081805dc / 0x081806ac / 0x08198e5a).

## Session 31 (2026-10-14): GL ES test app + MBX completion model - GL frames flow (nothing drawn yet)
TEST APP: probe/gltest (gltest.c, GL_SURFACE kind, 3.4 KB .hbapp): every frame a pure function of the
  frame counter - clear (0.05,0.08,0.25), one perspective quad, 64x64 RGBA checker texture (yellow
  1-texel border) modulated by red/green/blue/white vertex colours, depth test, 2 deg/frame about Z,
  -30 deg about X. Built against nanobtfix-untether/nanoapps: the device's installed Blocks.hbapp is
  byte-identical to that tree's build, so its resident/SDK is the one on the snap6 disk.
GETTING IT ONTO THE EMULATED DISK (dumps/ untouched): tools/disk_patch.py IMG /FAT/PATH=LOCAL -o PATCH
  lays a file over an installed file's clusters (FAT32 walk incl. long names; self-test: patching
  Blocks with itself reproduces its 6 blocks); run.sh --nand-patch PATCH (nand-patch=, N7PATCH1 records
  {u64 FTL LBA, 4 KB}) is served before the rsrc/disk images. The resident's loader (hb_reloc_file.c)
  demands file size == header + image + relocs, so HRL1 blobs are padded INSIDE the format (zeros
  appended to the image, image_size grown; loader zero-fills to span anyway). Launch = Blocks icon
  (home page 2, (64,215)); the resident re-reads the executable on every launch (hb_ensure_app_
  executable after hb_unload_other_apps). Patch used: rsrc/gltest-as-blocks.patch (gitignored dir).
MBX FRAME PROTOCOL (RE + traces; NANO7_MBX_TRACE=1 now logs every MBX write with the guest PC):
  - IRQ 53 was never enabled at resume (VIC bank1 now 0x00700023) and the core's +0x130 enable reset
    to 0; it is 0x8775 after the driver init that preceded T0 -> now the reset value.
  - slot job (GL flush 0x0813175c): +0x810 = slot, +0x81c = 1 (pair-list variant +0x818) -> status bit
    8 + slot (ISR sets the waited word; 0.5 s timeout -> the Blocks assert seen first).
  - scene: +0x800 = 1 (TA start, after register pairs +0x840.. incl. 240x432), TA words into FIFO
    0x3b800000 in batches, each committed by a doorbell 1 to 0x3bc00000, end-of-list word 0xf0000000.
    The hardware then renders by itself and reports bit 4 (render done: slot state 3, wakes ctx+0x2c);
    the same word is the fence of 0x08192194. NOT bit 5: that is the TA parameter-overflow / partial
    render event - its handler leads to bridge call 0x1b (allocate a depth store, 0x080b14f8), which
    1.1.2 never supports (unconditional assert -> power-off). Raising bit 5 caused exactly that.
  - render job: the scheduler 0x0813645a then writes the job's register pairs (+0x60c..+0x6dc: param
    base, target 0x00335180, 240/432, stride) and +0x6ec -> completion = bits 2 AND 6 (0x08148c48 frees
    the render slot; tested: 0x40, 0x0c, 0x04, 0x08, 0x48 alone leave the allocator spinning).
  - software events: writes to +0x12c OR into the status (the allocator pokes 1 as a wake-up).
  Model (nano7_mbx.c): end-of-list -> +50 us -> +500 us bit 4; +0x6ec -> +100 us bits 2|6; +0x81c/0x818
  -> bit 8+slot; +0x838 -> bit 6 (Session 27); IRQ 53 = status & enable; timers on the guest clock,
  saved in save states; exit summary "N scenes, N renders done, N render jobs".
RESULT: original Blocks: 562 scenes in ~25 s, no panic. gltest: 1080 scenes (one per frame), ~164 TA
  words per frame, no panic; the GL view is presented but shows stale pixels (the old DumpRAM surface)
  because nothing is rasterized yet. Regression: Music > Maladie plays (audio, art), Lines (GBA) 60 FPS.
  Screens: screens/session31/. One full gltest frame (496 writes): docs/research/mbx-gltest-frame.txt.
NEXT (the renderer): decode the TA stream (control words 0x1/0x2/0x7/0x9/0xb/0xc/0xe/0xf.., vertex
  formats; the driver sends screen-space floats), the texture words (GPU VA through the MBX MMU roots
  +0x1000..+0x101c) and rasterize into the render target named in the +0x6xx job, using gltest's known
  scene as ground truth.
