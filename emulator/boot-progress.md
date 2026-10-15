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
  delta >= N), implemented as a live counter off QEMU_CLOCK_VIRTUAL.
- **vic**    @ 0x39700000 (4KB, **0-fill**): interrupt-controller-like;
  polls wait for channel bits in +0x14 to clear/match +0x08, 0-fill passes.
- **b3e0** @ 0x3E000000, **b38e** @ 0x38E00000, **b3c8** @ 0x3C800000,
  **b3cf** @ 0x3CF00000 (4KB, ff-fill): init-time touches.
- **fmc**    @ 0x38A00000 (4KB, ff-fill): NAND controller probe (8 chip
  selects, +0x48 status bit1 = ready), probe now passes instantly.

## Machine features added this session

- VBAR set to 0x22000000 at reset (hardware bootrom behavior).
- `iram-image` property: preload IRAM snapshot (has live vector table,
  `ldr pc,[pc,#56]` vectors → handlers in the OS image, e.g. SVC→0x082f3828).
- `iram-protect` property: first 0x1000 of IRAM read-only, CRITICAL, the
  OS's early resume-style code memcpy's a 256-byte data struct over
  0x22000000..0x220000FF which otherwise destroys the vectors and the first
  SVC 0x46 (mutex lock syscall, in the clock-enable loop) undef-loops.
- `ctx=<file>` + `cpsr=<val>` properties: restore 16 u32 regs from ctx.bin
  and force CPSR; with `--ramfull` (64MB live DRAM dump) + `--iram` this
  resumes the live system, but ctx.bin layout is guessed and it derails
  (see below). Banked exception SPs + CPACR(VFP on) are set for ctx boots.
- `pcsample-ms=<N>`: host-clock timer logging PC/CPSR/SP/LR + IRAM[0..0x40];
  the essential "where is it stuck" tool.
- run.sh flags: --os/--os-entry/--iram/--iram-protect/--ctx/--cpsr/
  --ramfull/--pcsample/--stub; extra qemu args pass through.

## Boot log progression (unique MMIO accesses per run)

- round 0 (no periphs): 2 lines, stuck after CLKCON bit15 write.
- +clkcon: SVC 0x46 → undef loop (vectors clobbered by OS struct memcpy).
- +iram image + VBAR + iram-protect: 38 lines, clocks, PLL, VIC, PMGR init.
- +tmr counter, vic 0-fill, extra blocks, fmc: 48+ lines, full clock init,
  delay loops work, FMC probes 8 NAND chip selects, syscall machinery runs.

## Current blocker

OS calls its **power-off routine** @0x08417c9c (writes 0x3C800000=0x00104a00,
clkcon+0x50=0xa5, then an intentional infinite NOP loop at 0x08417cae,
CPU in ABT mode). Trigger chain: after the FMC/NAND probe, a delay/syscall
path (call @0x08010bb4, r0=10000) leads via a table dispatch @0x083a3240
into power-off. Prime suspect: NAND FTL/ID init failure (FMC is only a
dumb regfile; ID reads return 0xffffffff), or a power/battery check.
Next: implement FMC READID semantics (command 0x90 + ID bytes) or find the
power-off predicate via a trace of 0x842aa80 (syscall dispatcher, r0=27).

## Bootrom status

../dumps/bootrom-vrom.bin is now 256KB but **all zeros**, the SCSI peek
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
  0x2208ffc2, suggests a trampoline in **IRAM above 256KB** (real IRAM
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

## Session 4 (REAL bootrom boot, dumps/bootrom-vrom.bin is valid now)

Config: plain `./run.sh` (bootrom @0x20000000, entry 0x20000000 = reset
vector `b 0x200000a8`). NOTE: the 256KB dump is the same 64KB repeated 4x -
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
0x3c400028 (bit5, satisfied by ff-fill) but NEVER cleared afterwards;
the only clear site (0x2000ab34, in vtable fn 0x2000aad0) requires the
0x3c400000-block driver struct (RAM) flags bit2, which is set from
hardware status we don't model. So the bootrom parks in the main loop
exactly as a real device with dead NAND and no USB would.

### Next (priority)

1. RE the 0x3C400000 block: which +0x28/status sequence makes its driver
   clear busy and what the identify result must look like. This block runs
   BEFORE NAND proper and gates everything.
2. Alternatively force the DFU path: find what sets struct+0x47 (USB
   attached), then watch it init the USB core (likely 0x3A000000).
3. Bootrom warm-boot branch (PMGR+4 bit18): check whether it hands off to
   0x08000000 with boot-args regs, could fix the RetailOS direct-boot
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

## 2026-10-13, DumpRAM app built (on-device atomic-ish RAM snapshot)

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

## Session 11b (2026-10-13): option 2, complete the pre-capture FMSS kick
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

## Session 12 (2026-10-13): snap4 (DumpRAM v3, NAND idle at T0), vector-smash fixed, FTL skew wall again
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
  `WMR_ASSERT vba == s_g_addr_to_vba(wr->sb, wr->nextVbaOfs)` s_btoc_add_data:230, vba 0x000f6b9c vs
  cursor 0x003ae1c4 (wr 0x08d0b3f8) -> CMD 0xff -> power-off 0x08417cae (89/89 samples).
  Torn: 33/54 blocks hit, 118k stale / 655k reads; the PPN loop reads stale 0x08ca9bxx (request) and
  0x08d0d4xx (FTL) words. MMIO: VIC/tmr/0x3de00000 (resume-hw), clkcon, I2C1 0x3c900000, 0x3cf00000, PMGR,
  0x3c800000/FMC CMD 0xff (shutdown). LCD 0x38900000 / panel 0x3d800000: NOT touched; no thread nears display.
VERDICT: the snapshot is still inconsistent for the NAND/FTL thread. The NAND thread's stack, request and FTL
  cursor are each read mid-stream (~1.4 s / ~5.6 s) while that thread writes the dump itself. Poking the
  cursor is whack-a-mole. NEXT: DumpRAM v4, at T0, inside an IRQ-off section (svc 0x46 ip=0 save / ip=1
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

## 2026-10-14, RAMScope v3 validated in the emulator (bare-metal harness) + extra-dram= property
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

## Session 32 (2026-10-14): Bluetooth comes up - BT UART + fake BCM2076B1, AES engine, host socket
STARTING POINT: BT is OFF in snap6 (Settings > Bluetooth "Off"); 60 s idle never touches 0x3db00000. Tapping
  the toggle first runs the AES engine 0x38c00000 (unmodeled -> poll on +0x0c forever -> spinner forever).
AES ENGINE (new nano7_aes.c, driver 0x0841140c aes(enc,keytype,key,src,dst,len,iv)): +0 enable, +4 go,
  +8 reset (poll 0), +0xc status (W 7 clears, bit0 done), +0x14 0xe|enc, +0x18 len, +0x20/+0x28 src/dst
  (+0x24/+0x2c/+0x34 lengths, +0x30 dst again), +0x5c..+0x68 custom key, +0x6c key type (0 custom,
  1/2 hardware, 3 keep) +0x88 = ~type, +0x70 = 1, +0x74.. IV. Model: AES-128-CBC done at GO; key type 0
  real; hardware keys only via aes-gid-key= / aes-uid-key= (run.sh --aes-gid-key/--aes-uid-key HEX32),
  else the job copies (logged with caller + first bytes). Consumers seen: "8740" images (0x08025154: header
  check = AES_hw(SHA1(hdr[0:0x40])) vs hdr[0x40:0x50]; payload decrypt 0x0801f058); BT toggle decrypts
  800 B with key 1 (caller 0x0842f509) and later ENCRYPTS two XML plists with key 1 (caller 0x0842e695) -
  with the copy model these round-trip consistently inside the emulator. The GID-keyed decrypts here are
  the same oracle Mission 3 needs (DecryptIPSW / S5Late).
BT UART (RE, port table 0x089180fc: port 1 = 0x3db00000, IRQ 25, DMA req TX 4 / RX 5; 0x3dd00000 = port 3
  = debug console, NOT BT): Samsung-style. +0 line 3, +4 ctl 0x405 (|0xc TX DMA, |3 RX DMA, |0x15880),
  +8 FIFO 7, +0xc modem 1 / 0x11 auto-flow, +0x10 status: bit0 RX data, W1C pending bits 3/4 RX, 5 TX,
  6 error (+0x14), 8 autobaud; decoder 0x08001868 loops while +0x10 & 0x178 BEFORE the ISR 0x08150b8c
  drains +0x24 into the 32 KB ring and signals event 0x45 (RxLoop 0x081920ac). +0x18 bit9 TX full, +0x28
  divisor. TX goes by DMA engine 1 into +0x20 (the DMA model's address-space writes feed the UART).
  Bootstrap (0x0854f56c) reads replies by polling (0x08550d10); IRQ 25 is enabled by the OS (0x08001514).
  Pitfall hit: re-raising the RX bit on W1C while bytes remain livelocks the decoder -> fixed.
FAKE CONTROLLER (new nano7_btuart.c): H4 framing, replies after 200 us + 4.2 us/byte, Command Complete
  (ncmd = 1, status 0, real return-param layouts: version HCI/LMP 6, mfr 0x000f; features bf fe cf fe db
  ff 7b 87 (byte4 bit6 = LE, bit5 = 0 required); buffers ACL 1021x8; LE 27x8; BD_ADDR = what the OS writes
  with 0xfc01), Command Status + Inquiry Complete after Inquiry_Length x 1.28 s, page timeout for
  Create_Connection / Remote_Name. Patchram records counted, not logged. NANO7_BT_TRACE=1 = every packet
  (default first 2000 compact). Save-state section nano7-btuart.
SOCKET BACKEND: machine prop bt-hci=PATH|tcp:[HOST]:PORT, run.sh --bt-hci-socket: QEMU listens
  (server, wait=off) and pipes the raw H4 stream (fake controller off). tools/bt_hci_host.py = host
  companion (minimal controller; base for a real-adapter / vhci / HID bridge). NOTE: AF_UNIX paths must be
  < 108 chars (use emulator/state/bt-hci.sock, gitignored).
RESULT (snap6 CMD + --nand-disk ... 49216 --nand-rsrc ...; Home > Settings > Bluetooth > toggle):
  bootstrap Reset, Version, BD_ADDR, fc18 (2.4 Mbaud), fc2e + 151 Write_RAM (30039 B) + fc4e, fc01 (device
  address f0:d1:a9:51:07:74), fc27, 2nd fc18; upper-layer table 0c03 1001 1002 1009 1003 0c01 0c56 0c6d
  2002 201c 200f 0c33 0c31 1005 2007; static settings (080f, 0c16/18/1c/1e/26/2a/2c/2f/3c/43/45/47/49),
  local name "iPod nano", EIR, class of device 1c 04 08, LE random address, Write_Scan_Enable 3
  (discoverable + connectable), then continuous HCI_Inquiry (GIAC, 5.12 s, gap ~1.5 s). Screen: Bluetooth
  ON, BT icon in the status bar, "Devices / Searching" (screens/session32/). Socket run with
  bt_hci_host.py: identical, 238 commands through the socket, 38 inquiries in ~60 s (instant replies).
  No panics, no BT error strings. Idle runs (BT off) are unchanged.
NEXT GAP: nothing is ever in range. Next: inquiry results from a fake peer (Extended Inquiry Result with a
  name + class) -> Devices list -> pairing (SSP IO-capability / user-confirm flow, link key -> plist) ->
  ACL/L2CAP: a fake HID gamepad (PSM 0x11/0x13) for Mission 4, or bridge bt_hci_host.py to a real adapter.

## Session 33 (2026-10-14): MBX renderer - alpha blending, depth func, texture combine, 16/8-bit textures
(Session 32 = the renderer itself, nano7_mbxr.c, commit a00406a; it has no journal entry of its own.)
METHOD: new gltest variants 8-19 (probe/gltest/gltest.c header; make VARIANT=n), each overlaid on Blocks
  with tools/disk_patch.py (/Apps/Executables/Blocks.hbapp) and run 4 at a time with NANO7_MBXR_STATS=1
  (distinct draw states + raw texels at frame 300) and NANO7_MBXR_DUMP=<isp> (one segment).
RE RESULTS (ISP/TSP word = draw block 0):
  - bits 31:29 / 28:26 = src / dst blend factor in the PowerVR2 TSP encoding (0 zero, 1 one, 2 other
    colour, 3 1-other, 4 srcA, 5 1-srcA, 6 dstA, 7 1-dstA); opaque = ONE/ZERO (0x22......). Seen:
    SRC_ALPHA/ONE_MINUS_SRC_ALPHA 0x966....., ONE/ONE 0x266.....; bit 13 set with GL_BLEND.
  - bits 11:9 depth compare in GL order (LESS 1 = default 0x200, LEQUAL 3 = 0x600, test off -> 7 + bit 7
    = no depth write); bit 17 = textured. Culling (glEnable(GL_CULL_FACE)) changes nothing here - not found.
  - glColor without a colour array: the driver's program loads constant c15 = ubyte/255 * 256/255
    (instruction 07c1000f); Blocks' HUD rects use it (fmt 0002).
  - texture layer word 2 = texture combine: MODULATE 0x86084610, REPLACE 0xd6087610, ALPHA texture
    0xc6084610, LUMINANCE texture 0x86086610. Model: bit 12/28 colour/alpha replace; else modulate, an
    8-bit texture feeding colour only with bit 13 and alpha only with bit 30.
  - formats (word 0 bits 16:12): 0x09 ARGB4444, 0x01 ARGB1555, 0x11 AL88 (alpha high byte), 0x15 8-bit
    (GL_ALPHA and GL_LUMINANCE share it). Same twiddling as 32/16 bpp. Raw texels confirmed the layouts.
RENDERER (nano7_mbxr.c): blend per pixel against the host frame (incl. dst alpha), depth compare + write
  mask, c15 constant colour, combine(), texel decode for the new formats. Still missing: culling, alpha test,
  stencil, fog, lighting programs, filtering/mipmaps.
RESULT: Blocks landscape HUD (d-pad, jump, hotbar backing, rotate button) is now translucent over the 3D
  world with visible white arrows (before: opaque white squares - no constant colour, no blend). gltest
  variants 0/3/6/7/8-19 all render plausibly; 1555 dark cells (alpha 0) fully transparent, 4444/AL88 half.
  Numeric check v19 (glColor 1,.5,.25,.5 over clear): MBX output 0xbf864a40 = exact expectation (134,74,64,
  a 0.75). On screen it is x0.875 darker: the display pipe's layer blend (nano7_disp.c) uses that surface
  alpha - a compositor question, not the GPU (worth checking against the real device some day).
  Screens: screens/session33/ (blocks-hud-before-opaque.png vs blocks-hud-blended.png, variant montage).
NOTE: commit 209f5bf (cold-boot plan) also picked up this session's in-progress gltest.c and a VARIANT=19
  build of probe/gltest/build/*; the working tree holds the VARIANT=0 build again.

## Session 34 (2026-10-14): MBX renderer - back-face culling
RE: full-segment diff of gltest v0 vs v12 (glEnable(GL_CULL_FACE) + glCullFace(GL_BACK)) differs in ONE
  word - the primitive word: 0x40020000 -> 0x40060000 (the ISP word does not change, which is why Session 33
  missed it). New variants 20 = GL_FRONT -> 0x400a0000, 21 = GL_FRONT_AND_BACK -> the driver sends no quad
  draw at all. So primitive-word bits 19:18 = cull: 0 none, 1 back, 2 front (3 treated as all). Blocks'
  world draws 0x70060000 = list + cull back. probe/gltest has no glFrontFace (SDK lacks it), so only CCW-
  front was observable.
RENDERER (nano7_mbxr.c): PRIM_CULL() latched per draw; emit_tri drops a triangle by the sign of its
  y-down screen-space area (front = CCW in GL window space = negative area here; strips already keep a
  consistent winding).
RESULT: v12 keeps the camera-facing quad, v20 removes it (screens/session34/), v0 unchanged; Blocks'
  world + translucent HUD look as in Session 33.

## Session 35 (2026-10-14): MBX renderer - alpha test (and an SDK glAlphaFunc bug)
RE (subagent + own reading of 0x08102784, the OS ISP/TSP state build, called from the dirty flush 0x0810bb38
  when gc+0x5d4 & 0x2003): draw block 1 = alpha test word: bits 31:29 compare (GL order), 28:21 reference
  (ref * 256, clamped to 255), from gc+0x3d0 (init 0xe0000000). glEnable(GL_ALPHA_TEST) (handler 0x08484428)
  sets gc+0x140 bit 1; while it is clear, or the test is trivially true, the word is forced to 0xe0000000
  (always) - which is why every capture so far showed 0xe0000000. An active test also ORs ISP bit 14; a test
  that can never pass is sent as 0 (never) + ISP bits 8:7.
SDK BUG (nanobtfix-untether/nanoapps/sdk/gl.c, NOT fixed here - outside this repo): its gl_current_gc()
  (tls+12 -> ctx+0x24, = 0x098e1240 in gltest) is NOT the context the OS GL methods use - they ignore their
  gc argument and fetch it with the getter 0x083c2df0 (= 0x098df98c, 0x18b4 lower; seen at 0x08480d6a in
  glBlendFunc). So the SDK's C glAlphaFunc (and likely glNormal3f, same pattern) writes into the wrong object:
  no effect on rendering, and it scribbles over whatever lives at 0x098e1240+0x3d0/+0x5d4. Proven with the
  gdbstub: a write watch on 0x098df98c+0x3d0 never fires while the app calls glAlphaFunc every frame, and a
  breakpoint on the state build shows gc+0x140 = 0x2a (enable set) but gc+0x3d0 = 0xe0000000. Fix for the
  SDK: get the gc via ((void *(*)(void))(0x083c2df0 | 1))() for direct field writes.
TEST: gltest variants 22 (GREATER 0.5) / 23 (same, no glEnable) / 24 (LESS 0.5), RGBA8888 texture with alpha
  64 on the dark cells; they use a local alpha_func() on the real context (gltest.c). Stream: v22 block 1
  0x90000000 + ISP 0x22624200, v24 0x30000000, v23 0xe0000000 (forced always). Variants 25/26 = probes
  (glEnable(GL_ALPHA_TEST) raises no GL error; the SDK-gc fields read back as written).
RENDERER (nano7_mbxr.c): per-triangle block 1; after texture combine the 8-bit alpha is compared with the
  reference; a failed fragment writes neither colour nor depth (depth write moved after the test).
RESULT: v22 drops the dark cells (background shows through), v24 keeps only them, v23 unchanged, Blocks
  unchanged (screens/session35/).

## Session 36 (2026-10-14): MBX renderer - bilinear texture filtering
RE: gltest variants 27 (MIN + MAG GL_LINEAR), 28 (MAG only), 29 (MIN only) vs v0 (both NEAREST): the only
  change in the three texture layer words is word 1 bit 31 (0x12005ba0 -> 0x92005ba0), identical for all
  three - the driver has ONE filter bit, set when either filter is GL_LINEAR. (Mipmapped min filters not
  probed: gltest uploads no mip levels.)
RENDERER (nano7_mbxr.c): HostTex.linear from word 1 bit 31; sample() = nearest, or the 2x2 bilinear blend
  around (u * w - 0.5, v * h - 0.5) with the existing repeat wrap, per ARGB channel, before the combine.
RESULT: v27's checker edges are smooth gradients, v0 keeps hard texel steps (screens/session36/, zoom);
  Blocks (NEAREST) unchanged.

## Session 37 (2026-10-14): a fake Bluetooth gamepad in the inquiry results
PEER (nano7_btuart.c, fake-controller mode; bt-peer=gamepad (default) | none, run.sh --bt-peer): HID
  gamepad "NanoPad", BD_ADDR 02:4e:50:41:44:01, class 0x002508 (Peripheral / Gamepad), RSSI -45, clock
  offset valid, EIR = Complete Local Name + 16-bit UUID list {0x1124 HID}. Reported 0.4 s into every
  HCI_Inquiry in the format Write_Inquiry_Mode chose (the OS picks 2 -> Extended Inquiry Result 0x2f;
  1 -> 0x22, 0 -> 0x02); Inquiry_Cancel / Reset drop it; Remote_Name_Request for its address now
  succeeds (others still page timeout). Paging it (Create_Connection) still times out. Debug knob:
  NANO7_BT_PEER_COD=0xXXXXXX overrides the class. Save state: peer timer + inquiry mode.
RESULT (Home > Settings > Bluetooth > On, screens/session37/): the OS gets the 0x2f result on every
  inquiry (GIAC, 5.12 s, every ~6.7 s) but the stock Devices list does NOT show a gamepad:
  CoD 0x002508 hidden; 0x202508 (gamepad + Audio service bit) hidden; 0x240418 (Audio/Video headphones)
  -> "NanoPad" is listed (name taken from the EIR, no Remote_Name_Request). So RetailOS filters inquiry
  results by MAJOR class Audio/Video - it only pairs headphones/speakers.
FILTER (static RE + tests): stack level, not UI. DAgent::getRequiredInfoForDiscovery 0x08438974 ->
  ServiceManager::getRequiredInfoForDiscovery 0x082c9998 polls every registered service's vote (vtable
  +0x24; A2DPService, SensorService, FmRadioService...) and drops a device nobody claims. The only vote that
  claims headsets is BT::A2DPService::getRequiredInfoForDiscovery 0x082b4dfc: CoD service bits 0x40000
  (Rendering) AND 0x200000 (Audio) set, then major 0x400 (any minor), or major 0x100 minor 0x18, or major
  0x700 minor 4/0xc/0x10/0x14. The code also looks up EIR UUID 0x110b, but adding it to the gamepad's EIR
  (NANO7_BT_PEER_A2DP=1) did NOT make it visible -> the class decides. There is no HID service in 1.1.2.
CONSEQUENCE for Mission 4: the stock UI can never list or pair a gamepad as a gamepad. Homebrew must do
  its own discovery/pairing below the service layer (raw HCI via 0x080f731e / the RX hook), or register a
  new service whose discovery vote claims CoD major 0x500 minor 0x08. The emulator peer keeps the true
  gamepad class by default (correct for that work); NANO7_BT_PEER_COD=0x240418 shows it in Settings.
NEXT: page/connection for the peer (Create_Connection -> Connection Complete, remote features, SSP
  IO-capability + user-confirm -> link key), then L2CAP PSM 0x11/0x13 + HID input reports.

## Session 37 (2026-10-14): MBX renderer - mipmaps
TEST: gltest variants 30-33: 7 levels (64..1) uploaded by hand with glTexImage2D(level), each a solid colour
  (red, green, blue, yellow, magenta, cyan, white), drawn on a steep receding plane (uv x 8, no colour array;
  z kept in front of the eye since there is no near clipping). Min filter 30 NEAREST_MIPMAP_NEAREST,
  31 LINEAR_MIPMAP_LINEAR, 32 LINEAR_MIPMAP_NEAREST, 33 NEAREST_MIPMAP_LINEAR.
RE: texture word 1 top byte (with v0 / v27): NEAREST 0x12, LINEAR 0x92, N_M_N 0x32, L_M_N 0xb2, N_M_L 0x4e,
  L_M_L 0xce -> bit 31 bilinear within a level, bit 29 mipmapped / nearest level, bit 30 mipmapped / blend of
  two levels (bits 27:26 come with it). Words 0 and 2 unchanged. Layout (NANO7_MBXR_TEXSCAN=1, a stats-only
  scan of colour runs around the texture address): levels contiguous after level 0, largest first, each
  twiddled on its own, no padding: +0 red x 4096, +16384 green x 1024, +20480 blue x 256, +21504 yellow x 64,
  +21760 magenta x 16, +21824 cyan x 4, +21840 white.
RENDERER (nano7_mbxr.c): HostTex holds the level chain (level offsets / sizes); per pixel the LOD lambda =
  log2 of the larger screen-space texel step (uv of the +1 x / +1 y neighbours, perspective-correct); GL rules:
  lambda <= 0 -> level 0, nearest-mip -> level ceil(lambda + 0.5) - 1, linear-mip -> blend of floor(lambda)
  and the next. Bit 31 filters within a level. Assumptions: no LOD bias, 16 / 8 bpp small levels unpadded
  (only 32 bpp was observed).
RESULT: v30 / v32 show hard level bands (red -> green -> blue -> yellow into the distance), v31 / v33 smooth
  gradients between levels; v0, v27 and Blocks unchanged (screens/session37/).

## Session 38 (2026-10-14): MBX renderer - near-plane clipping
BEFORE: a triangle with any vertex behind the eye (w <= 0) was dropped whole.
CHECK: the driver's viewport z row is 0.5 + 0.5 * ndc (Blocks and gltest, NANO7_MBXR_STATS now prints the
  viewport), i.e. the GL convention: near plane = ndc -1 -> clip z >= -w.
RENDERER (nano7_mbxr.c): draw() keeps clip-space vertices (ClipVtx: position before the divide, plain colour /
  uv); emit_tri() clips each triangle against z >= -w (Sutherland-Hodgman, vertex order kept so the cull
  winding is unchanged), fans the 3-4 vertices, then project() divides / applies the viewport and store_tri()
  culls and stores as before. Only the near plane is clipped: x / y are bounded by the raster box and far
  fragments fail the depth test against the cleared 1.0. Batches without the vertex program (pixel coords)
  are not clipped.
TEST: gltest variant 34 = the checker quad as a floor from 5 units behind the eye to 11 in front.
RESULT: before, the floor was invisible (both triangles straddle the eye plane); now it runs from the screen
  bottom to the horizon, perspective-correct. Blocks: faces next to the camera that cross the near plane
  (ceiling / wall of the tunnel the save stands in) now render where sky showed through before. v0 and v31
  unchanged. Screens: screens/session38/.

## Session 39 (2026-10-14): stencil - the OS GL driver has no stencil path (nothing for the renderer to model)
PROBE: gltest variant 35 = glClearStencil(0x55) + glClear(... | GL_STENCIL_BUFFER_BIT) + glEnable(
  GL_STENCIL_TEST): its GPU command stream is word-for-word identical to v0's except two stack pointers.
RE (subagent, OS GL vtable at 0x087e777c): glStencilFunc / glStencilOp / glStencilMask do not exist - vtable
  slots 0x1f0..0x1f8 (between glShadeModel 0x1ec and the 0x1fc texcoord method) are null, the flat stubs
  skip from glShadeModel (0x08058374) to the next one at 0x08058394, and neither GL_INVERT 0x150a nor GL_DECR
  0x1e03 occurs as an immediate or literal anywhere in 0x08000000..0x08900000. glEnable(GL_STENCIL_TEST) is
  accepted (0x08484674: gc+0x14c |= 8, no dirty bit) but nothing reads that bit; glClearStencil (slot 0x3c,
  0x08481d7c) stores gc+0x360 and nothing reads it; glClear (0x084842ee) accepts mask bit 0x400 but only acts on
  0x4000 / 0x100. No ISP / block-6 / block-8 stencil bits. Conclusion: stencil is a silent no-op on this
  device (MBX Lite as driven by 1.1.2) - the emulator matches the real hardware by not modeling it.
SIDE FINDINGS (for later renderer work):
  - glColorMask = slot 0x58, 0x08480e5a: gc+0x3e0 bits 0x80 R, 0x40 G, 0x20 B, 0x100 A (1 = masked), dirty
    0x1001; the ISP build sets ISP bit 14 when any channel is masked and bit 8 when all four are (0x1e0).
    Where the per-channel mask reaches the GPU is not known yet; the renderer ignores colour masks.
  - glDepthFunc 0x08480eb6 (slot 0x7c) -> gc+0x354 bits 11:9; glDepthMask 0x08480efe (slot 0x80) -> gc+0x354
    bit 7 (= ISP no-depth-write, matches Session 33). gc+0x140 bit 6 = GL_POLYGON_OFFSET_FILL, bit 7 = GL_FOG.
  - BUG FOUND (not fixed yet): glShadeModel (0x0848141f) stores gc+0x2bc = 0x400000 for GL_SMOOTH (0x1d01),
    0 for GL_FLAT, and the ISP build ORs gc+0x2bc into the ISP word -> ISP bit 22 = Gouraud shading. The
    renderer (Session 32) reads bit 22 as "batch goes through the vertex program"; it only works because
    every app so far uses the default GL_SMOOTH and the clears are flat. A glShadeModel(GL_FLAT) app would
    have its vertices treated as pixel coordinates (no transform, no depth test). Needs a real VGP / pass-
    through criterion and flat shading (provoking-vertex colour).

## Session 40 (2026-10-14): MBX renderer - fix the "vertex program" criterion, flat shading
BUG (Session 39): the renderer keyed "run through the vertex program" on ISP bit 22, which is GL_SMOOTH
  (glShadeModel 0x0848141f -> gc+0x2bc 0x400000), so a GL_FLAT draw would have been taken as pixel coordinates.
RE: the word after the primitive word (the "control word") separates the two kinds of draw: the pass-through
  clears send 0xb00b0800, every transformed draw sends 0xf002x004 (bit 12 set when textured). Checked with
  NANO7_MBXR_STATS (the state key now includes it) on gltest v0 / v31 / v34 / v36 and Blocks (world, HUD rects,
  hotbar icons): no exception. Criterion now: control word bit 30 (DRAW_VGP).
FLAT SHADING: gltest variant 36 = glShadeModel(GL_FLAT), untextured strip with red / green / blue / white
  vertices -> ISP 0x22200200 (bit 22 clear), control word 0xf0020004. Renderer: MbxTri.flat = !ISP bit 22,
  colour = the provoking vertex's (GL: the last vertex of each triangle; strips / fans / lists keep it last in
  emit order, captured before near clipping). Assumption: the MBX follows the GL provoking-vertex rule (the
  driver could also reorder) - unverified on the real device.
RESULT: v36 = a transformed, rotating quad in two solid triangles, blue and white (screens/session40/); v0, v31,
  v34, Blocks unchanged.

## Session 41 (2026-10-14): MBX renderer - glColorMask
TEST: gltest variants 37 (1,0,1,1) / 38 (1,1,1,0) / 39 (0,0,0,0) / 40 (0,0,1,1) around the quad (mask restored
  at frame end: glClear honours it).
RE: a full-stream diff (NANO7_MBX_TRACE) of a late frame only shows the ISP word: 0x22624200 (bit 14) for every
  partial mask, 0x22620300 (bit 8) for all-masked - the per-channel bits travel elsewhere: the dirty flush
  ORs gc+0x3e0 into texture layer word 0 (0x0810bef4; layer words are only re-sent on change, hence absent from
  late frames), and for an untextured draw with a partial mask emits a dummy layer 0 (word 1 0xae000000 | ...)
  to carry it (0x0810bf44). Layer word 0: v37 0x03319640 (G), v38 0x03319700 (A), v39 0x033197e0, v40
  0x033196c0 (R + G); bits 8:5 = A R G B, 1 = masked.
RENDERER (nano7_mbxr.c): ISP bit 8 -> no colour write (depth only); ISP bit 14 -> keep the channels flagged in
  the current TA layer-0 word (applied after blending, per pixel). Depth is written as usual.
RESULT (quad area pixels, clear = 12,20,64 on screen): v37 G = 20 everywhere (R / B vary), v40 R = 12 and G = 20
  everywhere, v39 pure background, v38 identical to v0 (alpha is not visible); Blocks unchanged.
  Screens: screens/session41/.

## Session 42 (2026-10-14): the fake BT gamepad connects, pairs, and speaks HID (stock OS has no HID host)
(BT thread; follows the BT "Session 37" entry - that number is used twice, the other one is mipmaps.)
PEER LINK (nano7_btuart.c, fake-controller mode): Create_Connection to NanoPad -> Connection Complete
  (handle 0x0040); remote features (SSP), ext features page 1, version, name, role / link policy /
  supervision / sniff / packet type / RSSI answered. bt-peer=gamepad-connect: the pad pages the iPod
  3 s after page scan (Connection_Request -> Accept), then opens HID control and leaves security to the
  host. Pairing, host-initiated: Link_Key_Request -> stored key (fixed per pad, so it survives reboots)
  or negative -> SSP (IO_Capability_Request / Response NoInputNoOutput + general bonding / User_Confirm
  123456 / Simple_Pairing_Complete / Link_Key_Notification type 4 / Auth Complete) or legacy PIN
  (any PIN, combination key); Set_Connection_Encryption -> Encryption_Change; Disconnect; 30 s LMP-style
  timeout on any unanswered pairing step. L2CAP signalling: info (ext features 0, fixed channels),
  echo, connect / config / disconnect both directions. SDP server (PSM 1): ServiceSearch,
  ServiceAttribute, ServiceSearchAttribute with continuation; records HID (0x1124, gamepad subclass
  0x08, report descriptor 16 buttons + X/Y/Z/Rz int8, virtual cable, PSMs 0x11/0x13) and Device ID
  (VID 0x1209 PID 0x4e50); NANO7_BT_PEER_A2DP=1 adds an Audio Sink record (+ accepts AVDTP PSM 0x19).
HID (new): once the link is encrypted the pad opens PSM 0x11, then 0x13; with interrupt up it sends
  HIDP DATA|Input (a1 + buttons lo/hi + X Y Z Rz) from bt-pad=SEC:CONTROLS[:MS]/... (run.sh --bt-pad;
  relative to HID up; a b x y l r select start home b1-b16 up down left right lx= ly= rx= ry=; default
  demo "1:a/2:b/3:right:300/4:a+b:200/5:start"; none). Control channel: GET/SET_REPORT, GET/SET_PROTOCOL,
  GET/SET_IDLE, VIRTUAL_CABLE_UNPLUG; output reports logged. Save state covers link / channels / pad.
RESULTS (stock RetailOS, Settings > Bluetooth; screens/session42/):
  - honest gamepad (class 0x002508): never listed (Session 37); incoming page -> accepted, but the pad's
    PSM 0x11 is answered "PSM not supported" -> disconnect.
  - class 0x240418, HID-only SDP: tap -> connect -> SDP browse reads HID + Device ID -> "Connection
    Failed" (no supported service, ServiceManager::handleQueryComplete 0x082c8fa4 code 0x6d).
  - class 0x240418 + NANO7_BT_PEER_A2DP=1: tap -> SDP -> reconnect -> Authentication_Requested -> SSP
    (iPod IO cap 1 DisplayYesNo, auth 0x04, auto-confirm) -> PAIRED, link key saved (3 AES-copied XML
    plist writes), encryption on, AVDTP PSM 0x19 opened + AVDTP Discover (unanswered -> closes after
    6.5 s). Devices list shows NanoPad as a paired device. Second tap: Link_Key_Request_Reply with the
    stored key -> accepted, no re-pairing. The pad's own PSM 0x11 request on that link: refused.
WHY (static RE, Sonnet agent): ServiceManager (ctor 0x080363c8) registers only AVRCP 0x08, A2DP 0x10,
  Sensor/Nike+ 0x40, FM 0x200 - masks HID 0x20 / Gaming 0x800 exist in the name table 0x082c8e58 but no
  service object -> no L2CAP PSM 0x11/0x13 listener. SECMGR (state 0x0895c0b8: 1 idle, 5 authenticating,
  target 0x08b302e0) answers IO_Capability_Request (handler 0x080f5178, reply via 0x080f94fc) only while
  it authenticates itself -> remote-initiated pairing is silently dropped. Security level from DB
  callback 0x0854c424: SDP UUIDs level 0, everything else level 2.
HID REPORTS: implemented and script-parsed, but no report was delivered - stock RetailOS never accepts
  the HID channels. NEXT: the consumer = a homebrew HID host on the iPod that registers PSM 0x11/0x13
  with the OS L2CAP layer (or drives raw HCI/ACL itself); the emulator pad is ready for it.

## Session 42 (2026-10-14): MBX renderer - fog
RE (subagent + stream diffs): the OS driver has NO glFog* (vtable slots 0xa4..0xd4 empty, no code compares
  GL_FOG_* pnames); glEnable(GL_FOG) (0x084845a4) sets gc+0x140 bit 7 -> ISP bit 24. Context fields (glGet
  0x08191020): mode gc+0x32c, colour +0x334..0x340 (floats), density +0x344, start +0x348, end +0x34c, +0x350 =
  1 / (end - start); defaults EXP, density 1, 0..1. The vertex program always carries a fog stage (descriptor
  0x4000, chosen by mode, IDs 72/73/74 at 0x08102c84); with fog on the driver also uploads the modelview rows
  c8..c11, and c91 = (end, 1/(end-start), density*log2(e), density*sqrt(log2(e))). The control word grows
  (0xf0021004 -> 0xf002500c: an extra vertex output) and ISP bits 24:23 are set.
TEST: gltest 41 = glEnable(GL_FOG) with defaults; 42-44 write the fog fields of the real context (as for the
  alpha test) with a red colour: 42 LINEAR 2.5..5, 43 EXP 0.3, 44 EXP2 0.3. Fog stage last instruction word:
  LINEAR 0x0a09005b, EXP 0x0a020005, EXP2 0x0a020006 (EXP2 has 4 instructions). The red fog colour appears
  NOWHERE in the FIFO or the MBX register writes (all register values identical across 41-44) - the driver
  never programs it, so the hardware keeps its own (reset) fog colour.
RENDERER (nano7_mbxr.c): fog mode from the last uploaded 0x4000 stage; per vertex d = |c10 . (x,y,z,1)| and
  f = LINEAR (end-d)/(end-start), EXP 2^(-c91.z d), EXP2 2^(-(c91.w d)^2), clamped; interpolated perspective-
  correct (kept through near clipping); per pixel rgb = f * colour + (1 - f) * FOG_RGB after texturing.
  FOG_RGB = black (GL default; the real register value is unknown - an assumption to verify on the device).
RESULT (quad-centre pixel, eye depth 3.5, unfogged 24,60,60): v41 f = e^-3.5 = 0.03 -> (1,2,2); v42 f = 0.6 ->
  (15,36,36); v43 f = 0.35 -> (9,21,21); v44 f = 0.33 (sample on a light cell this frame: 0.33 x 255/96 = 0.88
  -> 22,53,53, matches). v0, v34, Blocks unchanged. Screens: screens/session42/.

## Session 43 (2026-10-14): homebrew BT HID host app - groundwork (RE + build loop), app NOT written
(BT thread, follows Session 42 "fake BT gamepad"; that number is used twice, the other one is fog.)
GOAL: an iPod app that is a Bluetooth HID host for gamepads (Mission 4), tested against the emulator's
  fake NanoPad (Sessions 37/42). Status: groundwork only - the Bluetooth layer and UI were not written
  and nothing was tested on BT; the code drafts were stopped mid-write and removed.
BUILD / TEST LOOP (works): probe/hidhost/ (Makefile against ../../../nanobtfix-untether/nanoapps,
  LV_SURFACE=1). Disk slot: /Apps/Executables/Nanosync.hbapp (456280 B, kind 1 in AllApps-B.pack, home
  page 3 at (64,215)); NanoBtFix's slot (268848 B) has no headroom. tools/disk_patch.py ... -o
  rsrc/hidhost-as-nanosync.patch + run.sh --nand-patch: a NanoBtFix clone built here launches from the
  Nanosync icon and runs ("PATCHED"). Probe/packtool.py lists AllApps(-B).pack kinds.
DONE: probe/hidhost/hid_parse.[ch] - report descriptor parser (Usage Page/Usage/Min/Max/Logical
  Min/Max/Size/Count/Report ID/Input/Push/Pop) + decoder to buttons(32)/axes(X..dial)/hat; host unit test:
  the NanoPad descriptor and a report-ID + 16-bit-axis + hat layout decode correctly.
  probe/hidhost/hh_bt.h - the intended interface (scan / connect / state / pad), no implementation.
RE (Sonnet agents, static; verified spots marked *):
  - L2CAP (Open Interface): Register 0x080f7764(psm, connInd(psm, bd, cid)) [* PSM table *0x0896d92c,
    {u16 psm; fn} stride 8, count *0x08417308(47); NULL while BT is off]; Accept 0x083d0fb4(cfm, disc, data,
    cid, params, accept, secdesc); Connect 0x083d8a60(cfm, disc, data, psm, bd, params, &cid, secdesc)
    [* AVDTP call 0x080efb74]; Write 0x080f77e4(wcfm, cid, bd|NULL, buf, len) - one write per CID, buffer
    owned until wcfm; Disconnect 0x08410eb8(cid) [* checks]. Callbacks cfm(cid, a, b, status) [* AVCTP
    0x080e35c4], disc(cid, reason), data(cid, buf, len), wcfm(cid, buf, len, status). Security descriptor
    {0x00040003, id, 0..} [* AVDTP 0x0877eb98 id 0x19, AVCTP 0x08782800 id 0x110e].
  - Threading: stack mutex lock 0x08404e74 / unlock 0x08404e94, no args [* RxLoop wraps 0x080f8640 in
    them]; callbacks run on the stack thread with it held; calls from other tasks lock/call/unlock.
  - HCI commands by message id: 0x0841acf4(id, done_cb, params...) via table 0x0877209c (20 B: opcode at
    +2, format string at +8) [*]: id 0 = Inquiry "3l1" (LAP, length, count), 0x1e = IO_Capability_Reply.
  - Security: unknown PSM -> level 2 -> the OS authenticates (host-initiated, SSP auto-accepted for
    outgoing); link keys stored/served independent of the service filter (agent, partly uncertain).
  - Discovery without hooks: inquiry results are visible in the BT UART RX ring [* ISR 0x08150b8c:
    ring ptr *(0x08cdc608+4), write index 0x0896d8e0+8, wraps 0x8000; RxLoop read index at +12].
  - App lifetime [* resident source]: LVGL apps get no quit callback; the arena is freed when ANOTHER app
    launches (hb_unload_other_apps), .bss re-zeroed per launch -> OS-held callbacks must not point into
    the app (persistent trampolines needed), or the app must be foreground-only.
NEXT: write the Bluetooth layer (foreground-only first: outgoing SDP -> PSM 0x11 -> 0x13, inquiry from the
  ring) + UI, then test in the emulator (bt-peer=gamepad; gamepad-connect needs PSM registration).

## Session 43 (2026-10-14): MBX renderer - fixed-function lighting
RE (subagent): glLightf/fv (slots 0x140/0x144), glMaterialf/fv (0x168/0x16c) exist; no glLightModel*, no x/xv
  variants. Enables in gc+0x144: bits 0-7 LIGHT0-7, 8 LIGHTING, 9 RESCALE_NORMAL, 10 COLOR_MATERIAL, 11 NORMALIZE.
  Lights: gc+0x324 -> 8 x 0x88 bytes; material gc+0x2d4.. ; model ambient gc+0x2c0 (no setter, 0.2). Program
  stages: 8 directional loop (ID 47 spec / 48), 9 positional loop (49-54: point / spot / both, spec or not), 10
  "any light" (56 / 57); loop count - 1 patched in (bits 19:16 of the stage descriptor; 0 = 0 or 1 light).
CONSTANTS (raw; the program forms the products; confirmed by gltest 45-51 stream captures): c8..c11 modelview,
  c12..c14 normal matrix, c18 / c19 colour-material weights, c20 scene colour (emission + mat ambient * model
  ambient, x 256/255, alpha = mat diffuse alpha), c22..c25 material ambient / diffuse / specular / emission, c26.x
  shininess, c27 + 7k per ENABLED light in enable order, directional first: +0 half vector (w 0) / eye position
  (w 1), +1 direction to light / spot direction, +2 ambient, +3 diffuse, +4 specular, +5 (exponent, cos cutoff,
  1 = spot), +6 attenuation. E.g. v46 light (1,1,1,0): +1 = 0.577 x 3, +0 = normalize(+1 + (0,0,1)) = (.325,
  .325, .888); v48 cutoff 15 -> cos 0.9659; v51 (only LIGHT1 on) -> LIGHT1 lands in c27. Normals = vertex slot 1
  (format 0x33 with a normal array).
RENDERER (nano7_mbxr.c): program stage descriptors tracked per stage bit; a draw is lit when its format has slot
  1 and stage 10's descriptor is the lit one (top byte 0x05 vs 0x01); lights walked from c27 (directional count
  from stage 8, positional from stage 9, each block accepted only if its +0 w matches the kind - resolves the
  0-vs-1 ambiguity); per vertex GL ES 1.1 lighting: eye position / normal (renormalised - assumption), ambient +
  diffuse + Blinn specular (infinite viewer, specular only when N.L > 0), attenuation, spot cone, colour material
  via c18 / c19, clamp; the lit colour replaces the vertex colour (then texture / fog / blending as before).
TESTS gltest 45-51 (per-vertex normals tilted outwards): 45 defaults, 46 directional orange + specular, 47 point
  at the eye + linear attenuation, 48 spot cutoff 15, 49 textured, 50 LIGHT0 + LIGHT1 (red, from the left), 51 only
  LIGHT1.
RESULT: v45 centre = (135,135,135) = 255 x (0.04 + 0.8 x 0.7071 cos 30) exactly (centre = mean of the diagonal's
  two vertices, independent of the spin); v47 blue 18 = 0.04 + att 0.49 x 0.3 x 0.2 as expected; v48 dark between
  the corners (every vertex outside the 15 deg cone - correct per-vertex GL behaviour); v50 adds red to v45, v51 red
  only. v0, Blocks unchanged. Screens: screens/session43/.
LIMITS: stale light blocks could be picked up if an app drops from positional to none in the same scene and the
  kinds happen to match; two-sided lighting / local viewer have no setter in this driver; byte / short normals
  (c92 scale / bias) not handled: such a draw stays unlit, with a one-time warning (-> Session 44).

## Session 44 (2026-10-14): MBX renderer - GL_BYTE / GL_SHORT normals
TEST: gltest 52 / 53 = variant 45 with GL_BYTE (x127, 4-byte stride) / GL_SHORT (x32767) normals.
RE (stream): the driver converts both to SHORT4: vertex-format nibble 1 = 8 (2 words: x | y << 16, z | 1 << 16;
  e.g. byte -64 -> 0xffc0, 90 -> 0x005a; short 16384 -> 0x4000), and uploads c92 = (scale, bias) = GL's signed-
  normalised conversion (2c + 1) / (2^b - 1): (2/255, 1/255) for bytes, (2/65535, 1/65535) for shorts, (1, 0) for
  float normals. Before this the renderer took nibble 8 as 8 words, so such draws had a wrong vertex stride.
RENDERER (nano7_mbxr.c): attr_words(8) = 2; a lit draw's short4 normal = int16 * c92.x + c92.y (any other normal
  nibble stays unlit with a one-time warning).
RESULT: v52 and v53 centre = (135,135,135), identical to v45 (float normals); v0, Blocks unchanged.
  Screens: screens/session44/.

## Session 45 (2026-10-14): two-sided lighting - the driver never sends it (nothing for the renderer to model)
RE: no glLightModel* exists (Session 43). The two-side flag gc+0x2d0 (init 0 at 0x080e09e4) is read in exactly one
  place of the GL state code: 0x0815a080 builds a "needed vertex data" mask (lighting 1 / 0x31 with lights, +2 when
  gc+0x2d0 != 0, +0xa0 fog, texture units ...) into gc+0x148c (and a companion word gc+0x1494, returned as
  "changed"). The program-stage selection (0x08102984) never reads it.
TEST: gltest 54 / 55 = variant 45 flipped 180 deg about x (back face towards the camera); 55 writes gc+0x2d0 = 1 on
  the real context every frame before glEnable(GL_LIGHTING). gdbstub at 0x0815a0b0: the flag is seen (gc+0x2d0 = 1,
  mask 0x33 vs 0x31 in 54). Yet the GPU command streams of 54 and 55 are identical over 200 frames: same program
  uploads, constants, vertex format, control and draw words.
CONCLUSION: two-sided lighting has no effect on the hardware with this driver - back faces are lit with the front
  normal (one-sided). The renderer already does exactly that: v54 and v55 both show the scene colour (10,10,10) =
  0.04 x 255 (N.L < 0 for every vertex). No renderer change.

## Session 46 (2026-10-14): MBX renderer - GL_POINTS and point sprites
RE (vtable + subagent): no glPointSize / glPointParameter* / glPointSizePointerOES (slots 0x1a4..0x1c0 null);
  point size gc+0x260 (1.0), GL_POINTS half sizes gc+0x258 / +0x25c (0.5, chosen by POINT_SMOOTH via gc+0x254),
  min / max {0, 32}, attenuation (1,0,0) - nothing writes them after init, so apps get size 1 unless they write
  the context. GL_POINT_SPRITE_OES = gc+0x144 bit 20; coord replace per unit (gc+0x440 + 0x44 n).
STREAM (gltest 56-59):
  - GL_POINTS (0x0813f8cc): primitive word 0xe0020000, then *(gc+0x254) = half size in px (0x3f000000; 12.0 =
    0x41400000 after writing 12), then the control word, then vertices.
  - sprites (CPU expansion 0x080d87a8 / 0x081674ec): each point sent 4 times with a slot-7 corner word (format
    nibble 7, one word, u16 x | y << 16; table {1,0,0x10001,0x10000}, Y-flipped {0,1,0x10000,0x10001} when
    primitive bit 19) as a strip, and after each point a DOORBELL WRITE OF 0 (primitive restart: the next
    segment carries only vertices of the same draw) - before this the renderer dropped all but the first point
    ("unknown TA command word 0x3f800000"). Size via constants: c99 = attenuation (a, b, c, 0.5), c100 = (min,
    max, size) (c100.z 1 -> 24 after writing gc+0x260 = 24 + gc+0x1500 bit 25). The vertex program copies the
    corner to the texcoord.
RENDERER (nano7_mbxr.c, nano7_mbx.c, nano7_mbxr.h): mbxr_doorbell() gets the doorbell value; doorbell 0 keeps the
  draw open and the next segment is parsed as its vertices. Primitive 0xe: a square of side 2 * half size per
  vertex. Sprites: per 4 vertices one square, size = clamp(c100.z * sqrt(1 / (a + b d + c d^2)), min, max) (d =
  eye distance), each vertex placed by its corner word ((2 corner - 1) * size / 2) with uv = the corner. Points
  are never culled; dropped in front of the near plane. Format nibble 7 = 1 word.
RESULT: v56 / v57 size 1 -> single pixels (4 / 4 points, 2 on screen this frame); v58 sprites size 24 -> 24 x 24
  squares with the whole checker texture (modulated by the vertex colour), v59 points size 24 -> solid 24 x 24
  squares: exactly 2 x 576 px each. v0, Blocks unchanged. Screens: screens/session46/.

## Session 47 (2026-10-14): polygon offset - not implemented by the driver (nothing for the renderer to model)
RE: glPolygonOffset / glPolygonOffsetx have no method (vtable slots 0x1bc / 0x1c0 null, Session 46 slot map);
  no glGet case compares GL_POLYGON_OFFSET_FACTOR 0x8038 / UNITS 0x2A00 (no immediate in the glGet / state code);
  glEnable(GL_POLYGON_OFFSET_FILL) (0x08484590) sets gc+0x140 bit 6, and no code in 0x080d0000..0x08200000 /
  0x08470000..0x084a0000 tests that bit after loading gc+0x140 (scan for lsls #25 / tst #0x40 on the loaded
  word: none; the ISP build 0x08102784 tests bits 0, 1, 2, 5, 7 only).
TEST: gltest variant 60 = v0 + glEnable(GL_POLYGON_OFFSET_FILL): over 200 frames the GPU stream is identical to
  v0 (program uploads, draw state words, all constants apart from the per-frame matrices).
CONCLUSION: polygon offset is a no-op on this device's GL; the hardware gets no depth bias. Nothing to model.

## Session 48 (2026-10-14): MBX renderer - multitexturing (2 units) and a real texture-combine decode
TEST: gltest 61-64: unit 0 = the checker (MODULATE), unit 1 = a 32x32 texture (left half orange opaque, right
  half blue alpha 128) with its own texcoords (x 2) via glActiveTexture / glClientActiveTexture; unit 1 env 61
  MODULATE, 62 ADD, 63 DECAL, 64 REPLACE.
STREAM: draw header 0x100001ef (blocks 0-3, 5-8): layer 1 = (0x02219600 = 32x32 ARGB8888, address, combine);
  vertex format 0x22503 -> unit-1 texcoords in slot 4 (2 floats); control word 0xf0023004 (bit 13 = unit 1, bit
  12 = unit 0); vertex-program stage 0x2000 copies input 4 to texcoord output 1; ISP 0x22650200.
COMBINE WORD (layer word 2) = two 16-bit combiners, colour in 31:16, alpha in 15:0 (an ALPHA texture changes only
  the colour half, a LUMINANCE one only the alpha half - this replaces Session 33's bit guesses):
  colour 0x8608 (unit 1: 0x8e08) modulate, 0xd608 replace, 0xc608 pass the input, 0xce80 add, 0xb384 decal;
  alpha 0x4610 (0x4e10) modulate, 0x7610 replace, 0x6610 (0x6e10) pass. Bit 11 of a half = "previous stage"
  input instead of the vertex colour (equal for unit 0). GL_BLEND / GL_COMBINE codes not seen: warned once,
  modulate instead.
RENDERER (nano7_mbxr.c): per-vertex u1 / v1 (perspective-correct, kept through near clipping), MbxTri.tex1 from
  layer 1 when control bit 13 + slot 4; stage chain vertex colour -> unit 0 -> unit 1, each with its own LOD /
  mip / filter (tex_fetch); combine() is now a table on the two halves; host_tex() shares texture loading.
RESULT: v64 orange stripes exactly (255,128,0) (blue ones dimmed by their 0.5 alpha in the display pipe, as in
  Session 33); v63 decal = solid orange over the opaque half, mix over the half-transparent one; v61 blue zeroed
  under orange; v62 brightened / saturated. No combine code unrecognised. v0, v8 (replace), v17 (ALPHA texture),
  v18 (LUMINANCE), v49 (lit + texture), v58 (sprites), Blocks unchanged. Screens: screens/session48/.

## Session 49 (2026-10-15): BT HID host app works - emulated gamepad moves a square on the iPod screen
(BT thread; follows Session 43 "HID host groundwork". Session 48 is the MBX multitexturing entry.)
APP (probe/hidhost: hidhost.c UI, hh_bt.c HID host, hid_parse.c): GL_SURFACE app (10.6 KB) in the Blocks slot
  (make; tools/disk_patch.py dumps/ipod-disk.dd /Apps/Executables/Blocks.hbapp=probe/hidhost/build/
  hidhost.hbapp -o probe/hidhost/build/hidhost-as-blocks.patch; run.sh --nand-patch that). On launch it
  switches Bluetooth ON itself, registers L2CAP PSMs 0x11/0x13 as soon as the PSM table exists, accepts the
  pad's channels, decodes HIDP input (0xa1 + report) and draws: status line with a live seconds counter
  (BUSY / LISTENING / HID CONNECTED + report count), axes, buttons, raw report, peer address, a square
  the left stick drives (velocity, ~1 s across), A = red / B = green, 16 button lamps.
WHY GL, NOT LVGL: the resident in the snap6 image predates the LVGL-surface heartbeat (Session 15): an
  LV_SURFACE app's frame callback ran exactly twice (counter stuck), GL surface apps get gl_app_frame()
  every frame (~60 fps in the emulator).
RE (Sonnet agents + checks; V = seen in disassembly):
  - L2CAP: Register 0x080f7764(psm, fn) 0 / 0x192 dup / 0x1e2 bad PSM / 0x68 full, no mutex (V); Unregister
    0x080f7580(psm) (V); dispatcher 0x080f7528 calls connInd(psm, bd6, cid), return ignored (V); Accept
    0x083d0fb4(cfm, disc, data, cid, [sp] params|NULL, [sp+4] accept 0/1, [sp+8] secdesc) - callbacks and
    secdesc must be non-NULL, secdesc kept by pointer (V); data(cid, buf, len) buf = L2CAP payload, owned
    by the stack (V); cfm(cid, a, mtu, status 0 = open) (V).
  - Security descriptor = SDP UUID element {u8 type 3, u8 0, u16 size 4, u32 uuid} (0x080f29e8, V). The
    security hook *(*(0x0895c0b8+0xc)+4) = 0x0842e4f0 (read live by the app): UUID 0x1000..0x1002 -> level
    0, else level 2 (V). With level 2 an unbonded pad that connects IN is dropped at once (HCI Disconnect
    reason 0x05, no Link_Key_Request) - seen with {3/4, 0x11} and {0, ..}. The app uses UUID 0x1000 = no
    security (build flag -DHH_SEC_UUID=0x1124 for the authenticated variant, needs a bonded pad).
  - Power: manager getter 0x084127a0, async power-on 0x08004f64(mgr, 1 = Bluetooth) (off 0x08005014),
    returns 0 = queued; state byte *(*0x08970234 + 0x25) 0/1/2 (V; Settings switch link inferred).
TRAMPOLINES: every OS-held callback (connInd, cfm, disc, data) points at a 48-byte ARM stub in an OS-heap
  block (survives the app): if hb_shared->aux_block == this launch's token -> tail-jump to the app, else
  return 0. The resident frees/clears aux_block on every launch, so a callback after an app switch is a
  no-op. A relaunch finds its block through the PSM table entry (magic 'HHB1') and re-arms it.
TIMING (bt-peer=gamepad-connect): the pad pages ~3 s after page scan and opens PSM 0x11 ~1 s later, once
  per run -> the PSMs must be registered before BT comes up; hence the app powers BT on itself (toggling
  in Settings first leaves no time). Settings > Bluetooth page needs >6 s to open in the emulator.
RESULT (snap6 CMD + --nand-disk ... --nand-rsrc ... --bt-peer gamepad-connect --bt-pad "2:lx=127:1200/
  3.5:ly=127:1200/5:lx=-90+ly=-90+a:1500/7:b:500/8:lx=60+ly=-127:800/9.5:x+y+start:600"; Home, page 2,
  Blocks icon): BT on -> LISTENING -> page -> PSM 0x11 accepted + configured -> PSM 0x13 accepted -> "HID
  link up" -> the square crosses right, down, diagonal up-left (red with A), green with B; lamps and
  axes track every report. Screens: probe/hidhost/screens/session49-demo.png.
KNOWN GAPS: the pad's first (neutral) report, sent as the interrupt channel opens, never reaches the app
  (count is one short; harmless). No SDP client: the descriptor is picked by report length (6 B = NanoPad
  16 buttons + X/Y/Z/Rz, 3 B = 8 buttons + X/Y). No security / bonding (UUID 0x1000), no outgoing connect,
  relaunch / app-switch paths not exercised, nothing on the real iPod yet.
NEXT: bond once via an outgoing L2CAP Connect 0x083d8a60 (host-initiated SSP is auto-accepted) and then use
  HID security; SDP client for the real descriptor; keep the PSMs after an app switch (resident-side).

## Session 49 (2026-10-15): MBX renderer - GL_BLEND and GL_COMBINE: the texture combiner decoded
CAPTURE: gltest 65 / 66 / 67 draw a 4 x 7 grid of quads per frame, each with a different texture env (unit 0):
  65 = GL_BLEND + one-parameter changes from the GL_COMBINE defaults (every RGB function incl. DOT3, sources,
  operands, scales, alpha functions), 66 = sources / operands inside ADD / SUBTRACT / INTERPOLATE / REPLACE /
  ADD_SIGNED, 67 = alpha-only table (every alpha source / operand reset per quad - 66's alpha rows first leaked
  state because SRC1/2_ALPHA / OPERAND1/2_ALPHA were not reset). ~80 layer-word-2 codes in draw order.
DECODE (layer word 2): two combiners each computing X1 * Y1 + X2 * Y2:
  colour h = w2 >> 16: X1 15:14 (0 K, 1 C, 2 T, 3 one), Y1 12:11 (0 P, 1 C, 2 T, 3 K) + alpha-replicate 13,
  X2 10:9 (0 P, 1 C, 2 T, 3 one), Y2 7:6 (as Y1) + alpha-replicate 8, complement X1 5 / Y1 4 / Y2 2, drop
  product 2 bit 3, -0.5 bias bit 1; alpha a = w2 & 0x7fff: X1 14:13 (0 P), Y1 12:11, X2 10:9, Y2 8:7,
  complement 6 / 5 / 3, drop 4, bias 2; scale (alpha 1:0, colour = h bit 0 << 1 | w2 bit 15): 0 x1, 1 x2, 2 x4,
  3 = 1 - result (GL_SUBTRACT is built as 1 - ((1 - a) + b)). T texel, P primary (vertex) colour, C previous
  stage, K env colour. E.g. GL_BLEND 0x84e0 = (1 - T) * P + T * K; DECAL 0xb384 = T * T.a + C * (1 - T.a);
  MODULATE with operand SRC_ALPHA moves the texel to product 2 (X1 = one complemented = 0). DOT3 = layer word 0
  bit 13 (+ colour Y2 field 1 for DOT3_RGBA) - which also showed that the texture-format field is bits 16:12
  WITHOUT bit 13 (0x19 read as 0x1b before: TEX_FMT() now masks it).
ENV COLOUR: glTexEnvfv stores it (texture unit +0x10..+0x1c, packed at +0x0c by 0x081538e4), but it reaches
  neither the FIFO, nor the MBX registers, nor GPU memory (trace search in every byte order + a RAM snapshot
  via F2 into a scratch state-dir); the hardware keeps its own constant -> modeled as (0,0,0,0) (GL default),
  unverified, like the fog colour.
RENDERER (nano7_mbxr.c): combine() is now this evaluator (replaces Session 48's code table), stage inputs P / C
  explicit; new debug NANO7_MBXR_DUMPFB=file writes the GPU frame 300 as RGBA PAM (before the display pipe -
  which scales the colour by the surface alpha, so screenshots cannot verify alpha).
VALIDATION: a GL ES 1.1 reference (Python, K = 0) for all 84 quads vs the dumped GPU frame at a point inside a
  dark texel: 28 / 28, 28 / 28, 28 / 28 match in RGBA (+-3). Regression: v0, v8, v14, v16, v17, v18, v49, v61-64,
  Blocks unchanged (v64 still exactly 255,128,0). Screens: screens/session49/.

## Session 50 (2026-10-15): MBX renderer - texture wrap modes
TEST: gltest 68 = 6 quads (uv -0.5 .. 1.5, checker with a yellow 1-texel border, GL_REPLACE), glTexParameteri
  wrap S / T per quad: REPEAT / REPEAT, CLAMP_TO_EDGE / REPEAT, REPEAT / CLAMP, CLAMP / CLAMP, then
  GL_MIRRORED_REPEAT_OES (0x8370) on S, and on S + T.
STREAM: texture layer word 0 bit 31 = clamp S, bit 29 = clamp T (0x03319600 -> 0x83.. / 0x23.. / 0xa3..).
  GL_MIRRORED_REPEAT_OES is rejected by the driver (the state stays as before: quad 5 = quad 4's clamp-S / repeat-
  T word, quad 6 sends no new layer block) - ES 1.1 here has only REPEAT and CLAMP_TO_EDGE. Bits 30 / 28 (maybe
  hardware mirror) are never set; not modeled.
RENDERER (nano7_mbxr.c): HostTex.clamp_s / clamp_t; sample_level() wraps or clamps each axis (tex_wrap()), for
  nearest and for every bilinear neighbour.
RESULT: GPU frame (NANO7_MBXR_DUMPFB) vs a GL reference at 192 points (32 per quad incl. outside 0..1): 192 / 192.
  Repeat tiles, clamped axes smear the yellow edge texels. v0, v27 (bilinear), v31 (mipmaps), Blocks unchanged.
  Screens: screens/session50/.

## Session 51 (2026-10-15): MBX renderer - texture matrix
TEST: gltest 69 = 6 quads (uv 0..1, GL_REPLACE checker), GL_TEXTURE matrix per quad: identity, translate (.25, .5),
  scale (2, 3), rotate 30, projective (glLoadMatrixf: q = 1 + .5 s), and unit 1 (61-64 texture, MODULATE) with
  translate (.5, 0).
STREAM: unit n's texture matrix = constants c85 + 3n .. c87 + 3n = the rows producing s, t, q (e.g. translate ->
  c85 (1,0,0,.25), c86 (0,1,0,.5); projective -> c87 (.5,0,0,1)); identity is re-uploaded whenever it returns.
  Vertex-program stage bit 12 + n: 1 instruction (copy, 0x00806969 / 0x00806e6e) for identity, 2 (0x05506a69 /
  0x05506f6e) for an affine matrix, 5 (0x05506d69: 3 dots, reciprocal of q, multiply) for a projective one ->
  the hardware divides by q PER VERTEX (GL wants per pixel).
RENDERER (nano7_mbxr.c): tex_matrix(): when the unit's stage is more than a copy (descriptor last > first),
  (s, t) <- (c85 . v, c86 . v) / (c87 . v) per vertex, v = (s, t, 0, 1); unit 1 with c88..c90.
RESULT: GPU frame (NANO7_MBXR_DUMPFB) vs a reference (per-vertex transform + divide, then screen-linear
  interpolation per strip triangle) at 192 points: 192 / 192. The projective quad shows the per-vertex-divide
  kink along the triangle diagonal - the hardware behaviour as modeled, not GL's per-pixel result (unverified
  on the device). v0, v61, v31, Blocks unchanged. Screens: screens/session51/.

## Session 52 (2026-10-15): user clip planes - no API in the driver; left unmodeled (findings only)
RE: glClipPlanef / glClipPlanex have no method (vtable slots 0x4c / 0x50 null, between glClientActiveTexture 0x48
  and glColor4ub 0x54), so GL apps cannot set a plane equation; only glEnable(GL_CLIP_PLANE0..5) exists (gc+0x144
  bits 13-18). With GL's default equation (0,0,0,0) an enabled plane clips nothing.
STREAM (gltest 70 = v0 + CLIP_PLANE0/1 enabled): control word 0xf0021004 -> 0xf012102c (bit 20 + 0x28; identical for
  one or two planes), c100.w / c101.zw -> 0 (the eye-space pattern also seen with fog / lighting / sprites), no
  program-stage change. The constant uploader (0x081559b6: if gc+0x1500 & 0x3f0000) sends 6 vec4 from gc+0xcd4 as
  c93..c98 every frame (normally (0,0,0,0) except c95 = (0, 0, -2.55e38, 1)).
PROBES (writing the context): 71 plane 0 = (1,0,0,0) at gc+0xcd4 -> c93 = (1,0,0,0); 72 + plane 1 at +0x10 -> c94
  stays 0; 73 planes 1 / 3 = (0,1,0,.25) / (0,0,1,5) -> the uploader (gdbstub at 0x081559c0, source = gc+0xcd4)
  reads (0,0,0,.25) / (0,0,0,0): the array is rewritten by the driver (probably derived per frame from equations
  kept elsewhere, e.g. object-space x inverse modelview), so gc+0xcd4 is not the input. Where the hardware applies
  the distances (fixed program code + control bit 20?) is not established.
DECISION: no renderer change. Every state a GL app can reach (enable with the default equation) clips nothing, which
  the renderer already reproduces (control bit 20 is ignored). Modeling clipping would need the plane-storage RE
  (the real input fields and the transform) and the hardware's use of c93..c98 - documented here as open.

## Session 51 (2026-10-15): HID host outgoing pairing, encrypted HID and SDP descriptor
(Bluetooth worker; independent of the renderer's Session 51.)
APP: probe/hidhost now starts an outgoing SDP channel to NanoPad, requests only
  HIDDescriptorList (0x0206) via ServiceSearchAttribute UUID 0x1124, accumulates bounded
  continuation responses, walks the SDP data elements and feeds the returned report
  descriptor to hid_parse. Removed both hardcoded report descriptors and report-length
  selection. HID decoding waits for a successfully parsed descriptor. Then opens outgoing
  control PSM 0x11 and interrupt PSM 0x13; incoming reconnect listeners remain registered.
  HH_SEC_UUID defaults to 0x1124 for both HID channels; only SDP uses public UUID 0x1000.
  hh_bt_connect(addr) selects a known address before the stack powers up; default NanoPad
  is 02:4e:50:41:44:01 in OS display order. Connect reverses it into HCI wire order.
RE / PAIRING: Connect 0x083d8a60 and Write 0x080f77e4 use Session 43's signatures;
  calls from the UI task hold stack mutex 0x08404e74/0x08404e94. A nonzero immediate
  Connect result is retried (PSM table exists before controller initialization finishes).
  HID security triggers Authentication_Requested, Link_Key_Request and (fresh peer)
  Negative_Reply, IO_Capability exchange, User_Confirmation_Request. The stock audio
  agent leaves this HID confirmation unanswered (auth request 0x00). RetailOS's pending
  confirmation holds the OS-order peer at 0x08b302e0, numeric value at +12. The app
  confirms only its selected peer during outgoing HID setup using SECMGR's own reply
  API 0x084135e4(accept, bd), retaining its normal completion callback 0x0856a4e1.
  This follows the iPod's existing Just Works policy for a pad with NoInputNoOutput;
  no raw HCI injection, fake pairing state, or security-level override is used.
UART AUDIT: nano7_btuart.c already implements Authentication_Requested, positive/negative
  Link_Key_Request replies, SSP IO capability / user confirmation, legacy PIN replies,
  Link_Key_Notification, Authentication_Complete and Set_Connection_Encryption /
  Encryption_Change. The exercised command path needs no model extension. Model untouched.
VALIDATION: make -C probe/hidhost (no warnings), disk_patch.py replacing Blocks.hbapp,
  then python3 probe/hidhost/test_pairing.py from repo root. Uses snap6 + disk/resources,
  --bt-peer gamepad (no incoming paging), QMP stdio, Home -> previous home page -> Blocks;
  no sockets required. Asserts fresh negative key reply, confirmation reply, paired event,
  encryption on, SDP response, both HID PSMs open and all seven scripted input reports.
  Result: PASS. HCI: SDP 62 bytes -> Authentication_Requested -> SSP confirmation reply
  -> Link_Key_Notification type 4 -> Authentication_Complete -> encryption on -> both HID
  channels. Emulator also tries incoming control after encryption; duplicate is correctly
  rejected while outgoing control completes. UI: HID CONNECTED, 7 REPORTS, HID SEC 1124
  SDP 62; square moves right then upward, A/B states decoded from the received descriptor.
  Evidence: probe/hidhost/screens/session51/{right,a-up,secure-connected}.png and pairing.log.
LIMITS: RetailOS 1.1.2 addresses, known-peer selection (no inquiry UI), SDP accumulator
  1024 bytes / nesting 8 / continuation 16 bytes. Fresh SSP and encrypted data verified;
  persistence of the OS bond across a cold reboot, legacy PIN, and app-switch/relaunch
  cleanup were not exercised. No real-iPod test. No changes to nano7_mbxr.c or probe/gltest.

## Session 53 (2026-10-15): MBX renderer - line primitives
TEST: gltest 74 = one frame, 2D ortho, untextured: GL_LINES (2 segments, an X) width 1, GL_LINE_STRIP (4 points)
  width 1, GL_LINE_LOOP (4 points) width 1, GL_LINES width 4, GL_LINES with per-vertex colours width 2.
STREAM: primitive nibble 6 = a LINE STRIP (0x60020000), then one float = half the line width in px (glLineWidth
  1 / 4 / 2 -> 0x3f000000 / 0x40000000 / 0x3f800000, like the points' half size), the control word, vertices.
  GL_LINES = a 2-vertex strip per segment separated by doorbell-0 restarts (Session 46); GL_LINE_STRIP = one
  strip; GL_LINE_LOOP = one strip with the first vertex repeated at the end (5 vertices). Before this the
  renderer warned "primitive word 0x60020000 not modeled" and drew nothing.
RENDERER (nano7_mbxr.c): draw_cmd() reads the half-width word for nibble 6 as for points; emit_line(): each
  adjacent pair, near-clipped in clip space, projected, drawn as GL's wide-line rectangle (+-half width in y for
  an x-major segment, in x for a y-major one) as two triangles; colour / uv / depth interpolated along it, flat
  colour = the second vertex (GL's provoking vertex for lines); never culled.
RESULT: GPU frame: 79 / 79 coverage / absence checks (+-1 px) - the strip adds the right edge, the loop also the
  left one; measured widths 1 / 4 / 2 exactly; the coloured pair shades red -> green. v0, v56 points, v58 sprites
  (still 1152 px), Blocks unchanged. Screens: screens/session53/.

## Session 54 (2026-10-15): glDrawElements - de-indexed on the CPU by the OS driver (works as is)
TEST: gltest 75 = 4 quads via glDrawElements (GL_TRIANGLES ubyte, ushort, GL_TRIANGLE_STRIP ubyte, a permuted
  ubyte list), 76 = one 96-index ushort GL_TRIANGLES draw of a 5 x 5 vertex grid.
  HOST SLOT NOTE: with glDrawElements linked, the SDK's initialised `s_glde_native` gives the app a .data section
  at a 64 KB-aligned address, so the flattened .hbapp grows to ~68 KB - too big for the Blocks slot (20 KB);
  Lines (111 KB) highlights but does not launch a GL surface app; "Mario Kart.hbapp" (1.1 MB, a GL app) works:
  tools/disk_patch.py over /Apps/Executables/Mario Kart.hbapp, launch = tap (176, 215) on the home page Home
  returns to.
STREAM: the GPU never sees indices - the OS resolves them on the CPU and even re-forms the primitives: the 6-index
  list of v75 arrives as a 4-vertex STRIP (0x40020000), the permuted list as the strip 3, 2, 1, 0; ubyte and
  ushort give identical streams; the v76 grid arrives as strips with doorbell-0 restarts (19 doorbells). No new
  command or index fetch exists, so no renderer change is needed.
RESULT: v75 all 4 quads correct; v76 GPU frame vs a reference: 1120 / 1120 sample points (texel-boundary ties
  skipped). Screens: screens/session54/.

## Session 55 (2026-10-15): MBX renderer - nearest-sampling tie artefact fixed
SYMPTOM (Session 54): axis-aligned textured quads showed serrated checker edges where a texel boundary falls on
  a pixel centre: the per-pixel float barycentrics rounded differently from row to row, so floor(u * w) flipped
  between the two texels along a straight edge (v75 quad 0: 706 of 20592 pixels in the wrong checker cell vs
  exact arithmetic with ties to the upper texel).
FIX (nano7_mbxr.c): uv_at() interpolates in double precision (own double area / barycentrics / 1/w), and
  sample_level()'s nearest path floors u * w + TEX_TIE_BIAS (1e-4 texel, above the float spacing of u * w) so
  exact ties resolve to the upper texel consistently - what a fixed-point texture unit does. Bilinear / mip LOD
  use the same uv_at.
RESULT: v75 0 / 20592 wrong; v76 grid now checked densely WITHOUT skipping ties: 9585 / 9585; v68 wrap 192 / 192,
  v69 texture matrix 192 / 192; Blocks unchanged. Screens: screens/session55/ (before / after crop, 2x).

## Session 56 (2026-10-15): MBX renderer - vertex program (VGP) microcode interpreter
TEMPLATES (subagent): the OS never assembles VGP code - fixed-function fragments are pre-encoded 8-byte
  instruction pairs in .data 0x0874f680..0x08750590, indexed by fragment ID via {ptr, start, count} entries at
  0x08750738 + 12 * ID and descriptors at 0x087505f8 + 4 * ID; the emitter 0x0812e83c only ORs (lights - 1) << 16
  into the light-loop descriptors. Full dump with a disassembly: docs/research/mbx-vgp-templates.txt.
ISA (decoded from the templates; semantics fixed by matching the HLE):
  word A: 31:24 / 23:16 / 15:8 swizzle of src2 / src1 / src0 (e4 = xyzw), bits 5 / 6 / 7 negate src0 / 1 / 2,
    4:0 opcode: 01 MOV 02 ADD 03 MAD 04 MUL 05 RCP 06 RSQ 07 DP3 08 DP4 09 MIN 0a MAX 0c SGE 0f LIT 10 DST
    12 ARL (a0) 13 EX2; scalar ops read the LAST swizzle component (e4 -> .w; reading .x broke the point
    light's homogeneous divide).
  word B: 31:26 dest (0 position, 1 colour BGRA, 2 fog .w, 3 / 4 texcoord 0 / 1, 5 clip distances (near / far
    / user planes), 16 + n temp rn), 25:22 write mask, src k: file 17:16 + 2k (input / constant / temp), index
    3:0 + 4k; constant = c[base + (idx8 & 0x7f) + (idx8 & 0x80 ? a0 : 0)], idx8 = B[15:12] << 4 | index.
  descriptor: 7:0 / 15:8 first / last instruction, 23:20 constant base / 16, 19:16 loop count - 1 (light
    stages 8 / 9: the loop does r12 += c84.x (7) and ARL a0 = r12 -> 7 constants per light).
  The control word's bits 15:0 are the STAGE-ENABLE mask (0x1004 = position + texcoord, 0x500c + eye + fog,
  0x05c4 lighting, 0x101c sprites, 0x0800 = the pass-through stage the clears run through); stages run in bit
  order. Inputs: format slots (ubyte4 = (b0..b3) / 255, u16 x2 corner, s16 x4 normals), default (0,0,0,1).
  The colour output is BGRA x 256/255 (the TA floors to 8 bits). Point sprites are expanded BY THE PROGRAM
  (corner offsets in clip space from c99-c101) - with the interpreter they are ordinary 4-vertex strips.
RENDERER (nano7_mbxr.c): program memory kept from the uploads (r->prog), vgp_inputs() / vgp_run() per vertex
  (default); outputs -> clip position, colour (BGRA -> RGBA, floored), uv, uv1, fog. Stages not uploaded yet
  after a snapshot resume (the OS loaded them before T0) -> those first draws keep the HLE values. The HLE paths
  stay as NANO7_MBXR_VGP=hle; NANO7_MBXR_VGPCHECK=1 compares both per vertex, NANO7_MBXR_VPDUMP=file dumps the
  per-draw ground truth (program, constants, raw vertices, HLE outputs) used for the Python model
  (docs/research/mbx-vgp-interp.py).
VALIDATION: (1) Python model vs VPDUMP of gltest 0..74 + Blocks: positions to 1e-7, texcoords / fog exact, colour
  <= 0.85 / 255 (HLE lighting rounding). (2) VGPCHECK over 44 variants: positions exact in all, colour <= 1.02 /
  255. (3) GPU frames, interpreter vs HLE, 46 variants + Blocks: 0 differing pixels each (sprites too). Blocks
  ~7 % slower (594 vs 637 scenes in 40 s).

## Session 57 (2026-10-15): MBX renderer - user clip plane via the VGP clip distance (resolves Session 52)
FINDING (with the Session 56 ISA): the clip-plane stage is template ID 39 (control-word stage bit 5, descriptor
  0x06501e1e: base c80): DP4 oext.z = r0 . c93 - eye position (from the eye-space stage) . plane equation. It is
  the ONLY template writing a user distance (all other dest-5 writes are .x / .y = the near / far distances
  z + w, -z + w), so the driver supports exactly one user plane, GL_CLIP_PLANE0 - which explains Session 52's
  plane 1 / 3 writes vanishing. Control-word bit 20 = clip enable (set with the stage).
RENDERER (nano7_mbxr.c): ClipVtx.ud = the interpreter's oext.z when the draw has stage bit 5 + bit 20 (r->uclip);
  emit_tri() now clips with a generic Sutherland-Hodgman (clip_poly(): plane 0 near, plane 1 user distance,
  <= 5 vertices -> fan, order kept); lines clip both planes, points are dropped when ud < 0. HLE mode: no user
  clipping (unchanged).
TEST (Session 52 probes, plane 0 written into the context): v71 plane0 = (1, 0, 0, 0) (keep eye x >= 0): GPU frame
  has 0 quad pixels left of the screen centre and exactly v0's 26037 on the right (clean cut at eye x = 0); v70
  (planes enabled, default equation 0) identical to v0. Screens: screens/session57/.

## Session 60 (2026-10-15): save states - GL round trip, timer/display/touch fixes, QMP reset, renderer state
STARTING POINT: save / load / reset already existed (Session 24: F2 / F4 / F1, raw RAM + every device's vmstate).
  Audit (all nano7_*.c vs their vmstate): gaps only in the MBX renderer (nano7_mbxr.c has no vmstate; mbx
  post_load called mbxr_reset), touch (finger-down state), adsp (MP3 bit reservoir reset on load, by design),
  btuart peer_results (log counter), fmc per-FMSS-run scratch (reset each run). torn / unimpl / pcsample are
  debug only. extra-dram=2 RAM is not in the save (test mode).
TEST (tools/savestate_roundtrip.py OUT; snap6 + disk + rsrc + rsrc/gltest-as-blocks.patch, QMP stdio): launch
  gltest, F5 pause, screendump A, F2, resume, Home (screen C), pause, F4 -> screendump D, resume, frames.
BUGS FOUND + FIXED (committed in 114a5fc):
  - nano7_clock.c: the guest deadline of each tracked timer was not saved; after_load re-derived it from the
    restored QEMUTimer, impossible for a state saved WHILE PAUSED (speed 0 = all guest timers at the frozen
    deadline) -> the current process's deadlines were reused -> every guest-time timer (TE, tick, MBX) fired
    ~4 s late (the guest time between save and load): the screen stayed on C for 4 s after F4. Now a
    subsection "nano7-clock/deadlines" carries them (registration order; older states fall back).
  - nano7_lcd_refresh(): the UI shows the restored frame right after the load (paused too); before, only at
    the next TE.
  - touch subsection "nano7-touch/finger": a press the guest saw without its release is released at load
    (log "state load: finger was down ... releasing it"); the host pointer is not state.
  - QMP / HMP system_reset (and HOST_UI resets) -> nano7_machine_reset -> reloads the boot slot like F1 (a
    device reset cannot rebuild a snapshot-resumed OS). Start-up reset unchanged. QMP: send-key f2 / f4 = save /
    load. QEMU's own savevm / loadvm still need a qcow2 drive (none) - F2/F4 is the save-state path.
RESULT (same process): D vs A 0 differing pixels; the app animates from the first frame after resume; QMP
  system_reset -> boot frame (DumpRAM "OK - REBOOT ME") identical to the start frame.
GPU RENDERER STATE (fresh-process load, tools/savestate_xload.py OUT SLOT REF.png): the restored frame is exact,
  but then gltest draws only its clear colour forever: the OS driver does not resend draw state / VGP program /
  constants / texture layers each frame, and a new process's MbxR is empty (same-process loads only worked by
  luck - the stale renderer state matched). FIX = machine/patches/mbxr-vmstate.patch, APPLIED after the HUD
  session's commit 853a658 (kept as a record): vmstate_mbxr (persistent TA state, prog, stage, cst, vp, layers,
  in-flight segment + scene as sized buffers with a size validator), saved as subsection "nano7-mbx/renderer";
  mbx_post_load resets the renderer only for older states without it. Tested in an isolated build tree:
  fresh-process load -> 6 / 6 distinct correctly rendered frames after resume; a pre-patch state still loads
  (renderer reset, as before). Re-verified on the main build after applying: same-process D vs A 0 px,
  fresh-process load 0 px + 6 / 6 distinct animated frames, no load failures.
LEFT OUT: adsp decoder reservoir (one "need input" frame), btuart peer_results, torn-map watcher, extra-dram=2.
  Real-iPod: n/a. Screens: screens/session58/.

## Session 58 (2026-10-15): usage HUD with the GPU model (new nano7_hud.c)
HUD: a 40-row strip under the display (x zoom, follows F7 rotation's width), toggled with F10, run.sh --hud
  (NANO7_HUD=1) starts with it shown; the sampler only runs while shown. Console = panel + strip; touch maps over
  the whole console and clamps to the panel (a press on the strip lands on the bottom row). F12 screenshots stay
  panel-only. Text line "CPU n%  GPU n% nFPS  RAM N/A" in a built-in 3x5 font; CPU (green) and GPU (orange)
  history graphs, 120 points (~30 s), grid at 0 / 50 / 100 %.
METRICS (windows of 250 x 1 ms virtual-clock samples):
  CPU busy = 1 - (samples with first_cpu->halted) / samples - the RetailOS idle thread waits in WFI (0x0862adec).
  GPU busy = (rasterized fragments + executed VGP instructions) / (133 MHz x guest seconds) - one per clock each
    (PowerVR ISP / TSP, VGP); the clock is the STOCK profile's GPU clock (an assumption until the clock model
    exposes it). Counters: MbxrStats in nano7_mbxr.c (frames, verts, vgp_ins, frags), nano7_mbx_stats().
  FPS = render jobs per guest second.
  RAM = N/A: the OS heap statistics (singleton 0x08909134) are not decoded yet - the remaining HUD item.
RESULT (screens/session58/, 2x): home screen CPU 5 % GPU 0 % 0 fps; Blocks CPU 94 % GPU 5 % 13 fps (CPU-bound);
  gltest v0 CPU 35 % GPU 7 % 36 fps (~250k fragments / frame x 36 / 133 MHz = 6.8 %, consistent). F10 toggles the
  console 240x432 <-> 240x472 live; app launch taps worked with the strip shown.
FILES: nano7_hud.[ch] (new; build.sh copy list + meson.snippet), nano7_ui.[ch] (strip render, F10,
  nano7_ui_redraw(), touch mapping), nano7_mbx.[ch] / nano7_mbxr.[ch] (stats), run.sh (--hud).

## Session 59 (2026-10-15): usage HUD - RAM (OS heap) usage
RE: the homebrew SDK's hb_os_heap_free() calls the OS free-size query 0x08417824 -> 0x0800e2f4(heap) ->
  0x0841402a(heap, stats): free = stats[6] - stats[7], where 0x0801c7be copies heap[+0xa8] (total) and
  heap[+0xc0] (used) and, when heap[+0x1684] is set, used -= p[+8] * (p[+12] - p[+0x1c]) for each of the 6 sub-pool
  caches p = heap[+0x1668 + 4i]. Heap object = *0x08909134 (snap6: 0x089a6228, one region 0x08d188ac, 40.3 MB;
  region table at +4 / count at +0xa4 as in nanoapps system/heap_diagnostic).
HUD (nano7_hud.c): each window reads those fields (address_space_ldl_le, read-only) -> "RAM used/totalM" (blue)
  and a third graph line (used fraction); N/A when the heap object is not valid. NANO7_HUD_LOG=1 prints every
  window's numbers to stderr.
VALIDATION: gltest 77 calls hb_os_heap_free() (the OS's own query) each frame and puts free / 16 into the clear
  colour; GPU frame (DUMPFB) -> 7213568..7213583 bytes, the HUD's figure 7213576: exact. Over that run the HUD free
  went 2.78 MB (DumpRAM resident at resume) -> 7.2-7.6 MB. Blocks: "RAM 33.9/40.3M". Screens: screens/session59/.
  The HUD checklist item (CPU %, RAM, GPU %) is now complete.

## Session 60 (2026-10-15): clock profiles, thermal model + governor (new nano7_power.c)
CLOCKS are decoded from what the guest programs in CLKCON (0x3c500000):
  - CPU: +0x00, bits 13:12 select the source (1 = PLL1 684, 2 = PLL2 266), bits 3:0 = div - 1.
  - GPU: +0x1c bits 29:16 (source, div1, div2; docs/research/gpu-clock.md). The gate bits are ignored.
  The register file used to start all-ones, and the OS read-modify-writes these words: the GPU decoded as
  486/256 = 1.9 MHz. nano7.c now seeds the stock values: +0x00 = 0x40009001 (as read on the device,
  dumps/report.txt) and +0x1c[31:16] = 0xe001 (PLL2 / 2 = 133 MHz). The DRAM half keeps the fill.
  New regfile API: nano7_regfile_peek() returns a written-bytes mask; nano7_regfile_poke() writes bytes.
FINDING: RetailOS runs its own DFS on +0x00 (IRAM 0x2200098a..): 0x40009001 = 342 MHz busy, 0x4000a001 =
  PLL2 / 2 = 133 MHz idle. A profile's CPU clock therefore holds only until the OS's next switch. The GPU
  clock persists (the OS only flips the gate bits 31:30). A game OS must own DFS for the CPU part.
PROFILES: STOCK 342/133, SAFE 684/171 (PLL1/4), MAX 684/266 (PLL2/1). They are applied by writing CLKCON as
  the guest would. F11 cycles them; run.sh --clock-profile stock|safe|max (NANO7_CLOCK_PROFILE) starts
  with one. With no selection the guest owns its clocks ("GUEST") and no governor runs.
  GOVERNOR: when a profile is selected, a model temperature above 65 C for 2 s drops one profile.
THERMAL MODEL (illustrative, NOT calibrated):
  - The temperature relaxes towards 24 C + 35 C * cpu_busy * fCPU/684 + 12 C * gpu_busy * fGPU/266.
  - Time constant 45 s, stepped every 100 ms of guest time.
  - Published via nano7_pmu_set_temp() into PMU regs 0x3c..0x3f, the registers the OS decodes at
    0x0818a4e8 in 4 C steps. 24 C reproduces the snapshot bytes 08 84 7d 7d exactly.
  - run.sh --temp C (NANO7_TEMP) pins the temperature.
  NOTE: CLAUDE.md says "PMU reg 0x08" for temperature. The OS actually reads 0x3c..0x3f.
CPU MHz cannot change the TCG execution rate: it only feeds the HUD and the thermal model. The GPU clock
  does time the MBX model:
  - Render-job completion = max(100 us, work / fGPU), capped at 50 ms.
  - work = fragments + VGP instructions since the previous job.
  - GPU % in the HUD uses the programmed clock (was a 133 MHz constant).
HUD: a second line "CLK cpu/gpuMHZ PROFILE tempC"; the strip is now 47 rows. NANO7_HUD_LOG also prints the raw CLKCON words.
TESTS (Blocks, snap6):
  - Decode: SAFE -> 684/171, MAX -> 684/266. Under MAX the OS's DFS later returned the CPU to 342; the GPU stayed at 266.
  - Governor: --clock-profile max --temp 70 -> MAX -> SAFE -> STOCK, 2 s apart.
  - F11 cycles STOCK -> SAFE -> MAX -> STOCK.
  - GUEST: 342/133, Blocks 74 % CPU, 5 % GPU, 12 fps, model at 32 C after 25 s.
  - Blocks renders unchanged; no aborts. Screen: screens/session60/blocks-hud-clock.png.

## Session 60, piece 2 (2026-10-15): chainloader hardware shutdown hw_shutdown_all() (probe/chainloader/)
CODE: probe/chainloader/shutdown.[ch]. Order: display -> FMC -> DMA -> timers -> VIC. CPSID if at entry and at return.
  The code makes no OS, SDK or libc call. Waits are bounded by the 1 MHz free-running counter 0x3c700084, with a
  loop-count backstop. It returns HW_SD_* flags and fills the hw_shutdown_last log. No NAND command, address or
  data cycle is ever issued. PIECE2.md lists every register written with its source, plus the open questions.
CORRECTIONS TO THE PLAN (docs/research/dualboot-chainloader-plan.md), taken from the device models:
  - DMA: completion status is read-only at +0x04; the W1C ack is at +0x08.
  - Timers: channels A..D keep their pending bits in CON 18:16 (W1C), so CMD=0 does not clear them.
  - FMC: +0x48 & 0x0181000e are phase-DONE flags, not busy. There is no known busy bit, so the caller waits for
    FMC idle while the OS is still alive.
  - Display: the panel is command-mode with its own GRAM. The routine stops the pushes (OS quiesce flag
    [0x08ae642c]+0x772, TE mask in GPIO group 1) and leaves the LCD registers alone.
  - TE GPIO addresses: lcd.md (0x39700084/a4) conflicts with nano7_gpio.c (pending 0x397000a4, mask 0x397000c4).
    The code follows the model.
EMULATOR GAP: nano7_lcd.c re-composites from DRAM every 60 Hz tick. After relocation the emulated screen will
  change, while a real panel keeps its last frame (PIECE2.md, open question 4).
BUILD: make -C probe/chainloader (target shutdown-check, appended to piece 1's Makefile). Result: 1020 B text,
  no undefined symbols with -nostdlib --no-undefined, -Werror clean. Not run in NanoEmu yet (integration piece).

## Session 61 (2026-10-15): fake BT HID gamepad - live control from the host keyboard
STATE BEFORE: the fake gamepad already existed (Sessions 37 / 42 / 49 / 51, nano7_btuart.c):
  - It shows up in inquiry as "NanoPad", pairs (SSP or legacy), has SDP with a HID descriptor, and opens
    HID control and interrupt.
  - Its input came only from the timed bt-pad script.
  - The consumer is probe/hidhost (an iPod-side HID host app).
NEW (nano7_btuart.c, nano7_ui.c, nano7_hud.c, run.sh): pad mode.
  - F8 toggles it; run.sh --pad-keys (NANO7_PAD_KEYS=1) starts with it on.
  - In pad mode the keyboard drives the pad:
    - WASD = left stick (X / Y), arrows = right stick (Z / Rz), full deflection; opposite keys cancel.
    - J K U I = A B X Y, Q / E = L / R, Enter = start, Backspace = select, Esc = home (buttons 1-9).
  - Every change sends one HIDP input report at once (state = keys held). Auto-repeat is ignored.
  - Leaving pad mode releases all held keys (one neutral report).
  - Unmapped keys still reach the iPod (h / p / space, = / - volume). The bt-pad script still runs alongside.
  - If the link is not up yet, a one-time log line points at the HID host app.
  - HUD: "PAD OK" (green, link up) / "PAD --" (orange, no link) on the second line.
  - API: nano7_btuart_pad_key / _pad_mode / _pad_state.
TEST (snap6, hidhost-as-blocks.patch, --bt-peer gamepad --bt-pad none, keys sent over VNC):
  - Outgoing pairing, then HID up. F8, then W / J / D / Right / K -> reports #2-#11, each matching its key.
  - The app shows "HID CONNECTED 11 REPORTS"; the square moved up and right.
  - F8 off -> W goes back to the iPod. --pad-keys -> A gives X -127 with no F8 needed.
  - Screen: screens/session61/hidhost-live-keys.png.
NOT DONE: a real host gamepad (QEMU has no joystick input; it would need SDL / evdev); analog values
  between full deflection; any HID host other than probe/hidhost (stock RetailOS has none).

## Session 62 (2026-10-15): fake BT gamepad - a real host gamepad drives it (new nano7_hostpad.c)
NEW: run.sh --host-pad auto|PATH (NANO7_HOST_PAD). It reads a real gamepad through Linux evdev.
  - QEMU here is built with --disable-sdl --disable-gtk, and SDL2 dev files are not installed on this host.
  - auto = the first /dev/input/event* with gamepad buttons and ABS_X / ABS_Y. It is rescanned every 2 s,
    so the pad can be plugged in later or re-plugged.
  - PATH = an evdev node, or a FIFO of struct input_event records. A FIFO is opened read-write, so a
    bridge can reconnect.
  - Non-Linux builds compile a stub that only warns. A macOS backend (GameController / IOKit) is still open.
MAPPING (Linux gamepad API, by position, Xbox naming; NanoPad buttons 1-13):
  - SOUTH / EAST / WEST / NORTH = A B X Y. TL / TR = L / R. SELECT, START, MODE = home.
  - THUMBL / THUMBR, TL2 / TR2.
  - Left stick = ABS_X / Y.
  - Right stick = ABS_RX / RY if the pad has them; ABS_Z / RZ are then analog triggers, which press TL2 /
    TR2 past half way. Otherwise the right stick is Z / RZ.
  - D-pad (HAT0 or BTN_DPAD_*) is added to the left stick at full deflection.
  - Axes are scaled from EVIOCGABS min / max with the device's flat zone (a FIFO gets +-32768, flat 4096).
  - One report per SYN_REPORT that changed something.
  - nano7_btuart_pad_host() merges this with the F8 keyboard state (buttons OR, axes summed and clamped).
TEST: no gamepad on this host, no input-group access, /dev/uinput is root-only. So the test fed a FIFO
  with 10 frames of real input_event records (emulator/tools/padfeed.py) into probe/hidhost, after
  outgoing pairing:
  - Reports #2-#11: X 127, flat zone -> 0, A, hat up -> Y -127, RX -> Z -127, trigger -> 0x0800 (TL2),
    MODE -> 0x0100, half up -> Y -63 (analog), each followed by its release.
  - The app counted 11 reports.
  - auto mode without permission prints the "input" group hint once.
  - Screen: screens/session62/hidhost-host-gamepad.png.

## Session 63 (2026-10-15): NanoGL - PVRTC 4bpp / 2bpp and paletted 4bpp / 8bpp textures (docs/tasks/pvrtc-spec.md)
DRIVER (Sonnet disassembly of glCompressedTexImage2D 0x08483794): it accepts all 14 tokens.
  - Paletted textures: the driver expands them on the CPU to 8888 (RGB8 / RGBA8), 565, 4444 or 5551 and
    uploads them like glTexImage2D. The GPU never sees indices or a palette.
  - PVRTC: copied verbatim (an 8-byte block loop). Storage is max(2, w / 4 or w / 8) x max(2, h / 4) words.
  - The format table is at 0x0874f14c. The research notes had its PVRTC entries mislabelled: 0x0874f188
    is 2bpp RGB.
  - imageSize must be exact (0x501 otherwise).
CAPTURE (gltest 78-87: one 64x64 texture via glCompressedTexImage2D, drawn 3x as a 192x192 2D quad,
  NEAREST, REPLACE; clear colour green = no GL error, which held for all ten):
  - Layer word 0: PVRTC 4bpp 0x03339600, PVRTC 2bpp 0x03321600. So bit 17 = PVRTC; bit 15 set = 4bpp
    (the field reads 0x19), clear = 2bpp (field 0x01).
  - RGB vs RGBA: same word 0. Only the combiner word differs (RGB: alpha from the vertex).
  - Raw texture memory = the app's bytes in order. The hardware reads standard PVRTC v1: modulation word,
    then colour word, in twiddled word order.
  - Paletted variants arrive as format 0x19 (RGB8 / RGBA8) or as the 16-bit formats, all already modelled.
RENDERER (nano7_mbxr.c): load_tex_pvrtc() / pvrtc_decode(), a port of Imagination's reference
  algorithm.
  - Colour A 554 / 3443 and colour B 555 / 3444.
  - Both colours are upscaled bilinearly from the word centres with wrap, using integer weights.
  - 4bpp modulation: weights 0 3 5 8, or 0 4 4+punch-through 8.
  - 2bpp: 1 bit per texel, or the checkerboard with H+V / H / V interpolation.
  - Small levels are padded to 2 x 2 words.
  - Mip levels are assumed contiguous (unverified).
VALIDATION (docs/research/mbx-ctex-ref.py):
  - The reference rebuilds the app's LCG data. It decodes PVRTC with texture2ddecoder (AssetStudio's
    decoder, independent of this code) and expands palettes per GL_OES_compressed_paletted_texture.
  - It is compared with the NANO7_MBXR_DUMPFB frame over all 36864 quad pixels in RGBA.
  - Results: v78 / v79 / v80 / v81 (PVRTC 4/2 bpp RGBA/RGB) and v82-v87 (P4 RGBA8, P8 RGBA8, P4 565,
    P8 RGBA4, P8 RGB8, P4 5551): 36864 / 36864 each, max difference 0.
REGRESSION: GPU frames byte-identical to the earlier dumps, under both VGP interpreter and HLE, for v0, v27
  (bilinear), v31 (mipmaps), v34 (near clip), v68 (wrap) and Blocks. Blocks needs RUN=40 to reach frame 300
  at ~12 fps.
Screens: screens/session63/. docs/NanoGL.md feature matrix updated.

## Session 64 (2026-10-15): Chainloader round-trip in NanoEmu - RetailOS -> bare-metal green payload -> PMGR reset -> RetailOS
PIECE 5 GOAL: prove the full chainloader round trip on NanoEmu: RetailOS running (snap6) -> launch chainloader app -> orderly hardware shutdown -> SRAM takeover -> MMU/cache disable -> bare-metal hello payload fills screen 100 % green -> QMP Home button press -> PMGR reset back into RetailOS.
APP INJECTION (same flow as gltest / hidhost in Sessions 31/49):
  - Target slot: /Apps/Executables/Blocks.hbapp (20708 B, Kind 2 / GL surface in AllApps-B.pack, home page 2 at (64,215)).
  - tools/disk_patch.py pads chainloader.hbapp (2468 B) inside its HRL1 image section to 20708 B, matching the directory entry and FAT cluster chain without modifying ipod-disk.dd.
  - Output: probe/chainloader/build/chainloader-as-blocks.patch (6 blocks, served via run.sh --nand-patch).
TAKEOVER & PAYLOAD EXECUTION:
  - Launch = tap (8700, 17500) normalized on home page 2. Resident relocates and executes chainloader.hbapp entry payload_entry.
  - chainloader_main(): falls back to statically embedded default_payload_bin (304 B payload.bin), calls hw_shutdown_all() (Piece 2: stops LCD commits, masks TE IRQ 32, settles & deselects FMC NAND, drains/aborts DMA engines 1 & 2, stops all 8 timer channels and clears pending IRQs, masks VIC0/VIC1).
  - handoff_to_payload() (Piece 3): copies position-independent trampoline to internal SRAM 0x22000000, warm-reset trap to 0x22008100, populates struct n7_handoff at 0x22009000.
  - Supervisor escalation: hooks RetailOS kernel dispatch slot 0 (*(uint32_t *)0x08917714 = 0x22000000) and executes svc #70 (r12=0). Dispatcher at 0x08002390 jumps to SRAM 0x22000000 via bx r3 in ARM state, SVC mode.
  - Trampoline: CPSID aif, L1 D-Cache clean and invalidate by Set/Way (CCSIDR geometry: 4 ways, 128 sets), DSB, clears SCTLR bits M, C, Z, I, V (MMU and caches off), ICIALLU / BPIALL / TLBIALL, DSB/ISB.
  - Relocates payload from DRAM staging to physical 0x08000000.
  - ABI setup: r0=0, r1=0x1097, r2=0x22009000, sp=0x22010000, lr=0x22008100; bx 0x08000000.
  - Bare-metal payload (start.S -> payload_hello.c): sets private stack, clears BSS, configures LCD Window 0 (240x432, ARGB8888, stride 240) and fills framebuffer with solid bright green (0x0000ff00).
  - Screen capture 04_payload_green.png: all 103,680 pixels verified exactly RGB (0, 255, 0).
RESET & RETAILOS RESTORATION:
  - Payload polls GPIO 0x56 (PMU IRQ line, 0x3cf00144 bit 6, active-low).
  - QMP `sendkey h` asserts PMU event register 3 bit 0, pulling GPIO 0x56 low.
  - Payload detects press, arms watchdog (0x3c800000 = 0x00104a00) and triggers warm reset by writing 0xa5 to PMGR reset (0x3c500050).
  - NanoEmu model (nano7_regfile.c): clkcon register +0x50 write of 0xa5 hooks nano7_ui_reset() (reloads boot state from emulator/state/boot).
  - Boot snapshot reloads instantly; subsequent Home press confirms interactive RetailOS home screen (pixel-identical to pre-launch state).
AUTOMATION:
  - Standalone regression test: probe/chainloader/test_roundtrip.py.
  - Evidence screenshots: emulator/screens/session64/
    - 01_retailos_boot.png: snap6 resume state
    - 02_retailos_home_p3.png: home screen page 3
    - 03_retailos_home_p2.png: home screen page 2 (Blocks slot)
    - 04_payload_green.png: bare-metal green payload (100 % solid RGB 0, 255, 0)
    - 05_retailos_reset.png: RetailOS reloaded immediately following PMGR reset
    - 06_retailos_restored.png: restored interactive RetailOS home screen
RESULT: PASS. Zero CPU crashes, clean handoff, 100 % pixel-accurate green payload, flawless warm reset back to RetailOS.


## Session 64 (2026-10-15): host gamepad - macOS backend (IOKit HID)
NEW (nano7_hostpad.c, __APPLE__ part): IOKit HID Manager in plain C.
  - Its own thread runs a CFRunLoop. QEMU's main loop is not a run loop, and with the Cocoa display
    AppKit owns the main thread.
  - Callbacks snapshot the state under a QemuMutex and schedule a bottom half; the report is sent from the
    main loop.
  - Hot-plug and removal come from the manager.
  - Device matching: Generic Desktop joystick, gamepad or multi-axis controller.
  - --host-pad auto takes the first pad; any other value picks a pad by product-name substring.
  - Gamepads need no Input Monitoring permission.
WHY NOT GameController.framework: it needs Objective-C, and its notifications arrive on the main dispatch
  queue, which QEMU only services with the Cocoa display. IOKit works with any display (VNC included).
MAPPING (HID button numbers differ per maker; tables from the controllers' public descriptors as SDL
  documents them; NOT tested on hardware):
  - Sony 0x054c (DS4 / DualSense), Xbox Wireless over Bluetooth 0x045e, Switch Pro 0x057e. All are mapped
    by position like Linux (south = A ...).
  - Any other pad: HID button n = NanoPad button n.
  - NANO7_HOST_PAD_BUTTONS="HID:PAD,..." overrides entries. NANO7_HOST_PAD_TRACE=1 logs every element
    change, to work out a new pad's numbers.
  - Axes: left = X / Y. Right = Z / Rz when the pad has Rz, else Rx / Ry.
  - Sony Rx / Ry and Xbox Brake / Accelerator are the analog triggers: past half way they press L2 / R2.
  - Hat switch (8 directions) and D-pad usages 0x90-0x93 add to the left stick. Dead zone 10 %.
SHARED: hp_emit() / hp_release() (state -> report, change detection) are now common to both backends.
  meson.snippet adds QEMU's own coref / iokit dependencies, which are empty lists off macOS.
TESTS:
  - Linux build and the FIFO test (padfeed.py into probe/hidhost): the same 11 reports as Session 62.
  - The macOS part cannot be built here. It compiles with gcc -Wall -Wextra against stub IOKit /
    CoreFoundation headers (signatures written from the SDK API), which checks syntax and types only.
  - It still needs a real build and a pad on the M2 Mac.

## Session 65 (2026-10-15): NanoEmu.app - a real macOS desktop app (docs/MACOS.md)
GOAL: replace "raw SDL window from a CLI command" on the user's MacBook Air M2 with a double-clickable app.
BUNDLE (emulator/macos/): make-app.sh builds emulator/dist/NanoEmu.app.
  - Info.plist (io.github.tenspav.nanoemu) and NanoEmu.icns, made by sips + iconutil from assets/icon-256.png
    (icon-512.png for the large sizes).
  - Launcher Contents/MacOS/NanoEmu (launcher.sh, bash 3.2) execs Contents/MacOS/qemu-system-arm, so macOS
    attributes the process to the bundle: Dock icon, "NanoEmu" app menu, bundle id all verified with NSWorkspace.
  - Device data from ~/Library/Application Support/NanoEmu: snapshot/ is required (or bundled with
    --snapshot); ipod-disk.dd, rsrc.img1, bootrom-vrom.bin and nanoemu.conf are optional. Save states go to
    state/, screenshots to ~/Pictures/NanoEmu, the log to ~/Library/Logs/NanoEmu.log (capped at 20 MB).
    A missing snapshot shows an alert with "Open Data Folder".
  - Self-contained: 22 Homebrew dylibs are copied into Frameworks/, rewired with install_name_tool and ad-hoc
    re-signed. Homebrew SDL2 is sdl2-compat, which dlopen()s SDL3 (invisible to otool), so libSDL3 is copied
    in as Frameworks/libSDL3.dylib, where sdl2-compat looks first (@loader_path). otool shows no
    /opt/homebrew references left. 49 MB.
WINDOW:
  - build.sh patches QEMU's SDL front end: the title is the plain -name, and SDL_RenderSetIntegerScale is on.
  - nano7_ui.c implements the console ui_info hook: after a window resize (debounced 1 s by the console),
    zoom = the largest whole factor that fits (1-6) and the window snaps to it.
  - NANO7_ZOOM / run.sh --zoom set the start zoom. The launcher's ZOOM=auto computes it from
    NSScreen.visibleFrame (JXA) minus the title bar, which is 32 pt on macOS 26.
  - Measured on the MBA (visible area 1470x839 with the Dock): 1x gives a 240x464 window. Cmd-2 asks for
    480x864 + title; macOS clamps it to 807 content, ui_info falls back to 1x and the window snaps back.
    HUD on/off: 464 <-> 511.
KEYS:
  - Esc and Home are now also the iPod Home button.
  - Cmd shortcuts in nano7_ui.c mirror the F keys, because MacBook F keys are media keys: Cmd-S save, Cmd-L/O
    load, Cmd-R reset, Cmd-P pause, Cmd-1..4 zoom, Cmd-T rotate, Cmd-U HUD, Cmd-K clock profile,
    Cmd-F fast-forward, Cmd-G gamepad keys, Cmd-E screenshot.
  - While Cmd is down no key-down reaches the iPod or the gamepad, and the release of a shortcut key is
    dropped. The drop flag is cleared on the next real press, because Cocoa sends no key-up for keys
    released while Cmd is held.
BUG FIXED (pre-existing): pause (F5) deadlocked when the key came from QMP send-key with more than one key,
  or from any queued input. Queued keys are delivered from a QEMU_CLOCK_VIRTUAL timer, and pause_all_vcpus()
  -> qemu_clock_enable(VIRTUAL, false) waits for that clock's timers to finish. Pause now runs in the UI
  bottom half (UI_PAUSE).
build.sh: the macOS `sed -i${SED_I}` left `file''` backup copies in vendor/ (word splitting). Replaced by
  a sedi() helper.
TESTS:
  - Linux headless (QMP send-key, scratch state dir): Cmd-S / E / 3 / P x2, F5 x2, Esc, Home, Cmd-U / K,
    F10 / F11, Cmd-L / R, then plain p still presses Power.
  - Mac: built, packaged and launched with `open dist/NanoEmu.app`. Window owned by NanoEmu, snapshot
    resumed; Esc = home, Cmd-S/E/L, F10, F11 checked over QMP.
  - Not checked: the window title text and real Cmd keypresses (ssh has no Screen Recording / Accessibility
    permission).
SEEN, NOT CHANGED: a few seconds after load / profile changes, RetailOS sometimes writes 0xa5 to PMGR +0x50
  (pc 0x08417cac), a guest-initiated reboot. The Session 64 hook turns it into a boot-state reload.

## Session 66 (2026-10-15): NanoEmu.app usable on the Mac - iPod frame, clickable buttons, menu bar, sound, always charged
WINDOW: nano7_skin.c draws a nano 7G body (366 x 659 units) around the panel with clickable Sleep/Wake, Volume
  Up / Down, Play/Pause and Home; nano7_ui.c routes a press that starts on a drawn button to the PMU, anything
  on the glass is a touch. sdl-hidpi.patch: the SDL window at native Retina density, so sizes are points
  (100 % = pixel zoom 2 on Retina). FRAME=0 in nanoemu.conf gives the bare screen.
MENU (nano7_macmenu.m, Cocoa, installed into SDL's menu bar): File (Save / Load State, Reset, Screenshot),
  Emulation (Pause, Mute, Speed, Clock Profile, Gamepad Keys), View (Size, Rotate, Device Frame, HUD, Full
  Screen), Device (Home, Sleep/Wake, Play/Pause, Volume). Mute = AUD_set_volume_out on the I2S voice.
SOUND: I2S0 plays through a QEMU -audiodev (coreaudio, machine prop audiodev=); no ffplay needed.
TIMERS: darwin-poll-ns.patch - macOS has no ppoll, g_poll rounds sub-ms timers up to 1 ms and device models
  with us delays (I2C, NAND, DMA) ran up to 50x slow; select() with a us timeout for short waits.
BATTERY: always charged by default (USB power 0x05 = 0x48, 4190 mV = level 15); --battery-drain [MIN] /
  BATTERY_DRAIN=MIN for power testing.
SLEEP: PMU reg 0x10 (display rails: 0x68 on, 0x00 asleep, 0x08 / 0x28 waking) now blanks the screen.
START: snap6 resumes inside DumpRAM ("OK - REBOOT ME"), where taps do nothing; the launcher presses Home 1 s
  in (HOME_AT_START=1), so the app opens on the home screen. A homebrew app "not launching" in earlier runs
  was this: the tap landed on the DumpRAM screen.
ALSO: LIS3DH accelerometer on I2C1 0x18 (nano7_accel.c, Left / Right keys or --tilt), guest-reboot log names
  the next-boot mode, tools/guest_smoke.py (QMP driver: taps, keys, hmp:, screendumps, log scan).
TESTS (headless, snap6 + disk + rsrc, tools/guest_smoke.py): Home at 1 s -> home screen; Settings and Music
  open; Music > Songs > Maladie plays 100 s (1768 MP3 frames, I2S peak 31539), no guest reboot; Lines (GBA,
  61 FPS), Mario Kart, Nanoracle, NanoSync, Game Boy launch; Cmd-R reloads to the home screen; Power sleeps
  (black) and wakes; Volume Up shows the OS volume HUD; with --frame the drawn Home and Volume Up buttons and
  taps through the frame work. App: built, launched, menu bar logged by NANO7_MENU_SELFTEST, CoreAudio voice
  opened. QEMU's PPM screendump pads rows to 4 bytes (366 px wide = 1100-byte rows): read with that stride.
NOT DONE: cold boot from the IPSW; jailbreak probe apps (DumpROM / DumpRAM) inside the emulated iPod.

## Session 67 (2026-10-15): v1.2.0 - side panel with live numbers and buttons, bigger window, .dmg
LAYOUT: the device is now compact (body 268 x 508 units, thin bezels; canvas 320 x 536) and the side panel
  (NANO7_PANEL_W = 200 units) sits on its right: 520 x 536 units, so the launcher's auto zoom picks pixel zoom
  3 on a 13" Retina MacBook = 780 x 804 points (was 366 x 659 at 1x). Rotation turns only the device; the
  panel stays upright.
PANEL (nano7_skin.c): 5x7 font drawn at any size (4x4 supersampled): FPS (big), CPU / GPU MHz and load, RAM,
  temperature, rotation; buttons Stock / Safe / Max (active = blue), Home, Sleep, Vol -/+, Play / Pause,
  Pause / Resume, Reset, Mute / Unmute, Rotate, Screenshot. Cached and redrawn only when its content or the
  pressed button changes. Panel = HUD on (Cmd-U); FPS = max(GL render jobs, panel updates) per guest second.
SPEED: a frame with nothing else changed only rescales the 240 x 432 screen into its rectangle and updates that
  rectangle (dpy_gfx_update), instead of re-copying and re-uploading the whole 1560 x 1608 window. Before it,
  the main loop was busy enough that PMU I2C transfers ran 100-300 ms late and the OS missed button events.
BUTTONS: every press reaches the guest for at least 200 ms (PMU_MIN_HOLD_MS): a mouse click is ~70 ms, and the
  OS missed short wake presses after its 1 minute auto-lock (display power PMU reg 0x10 = 0, now shown black).
START: Home at 3, 6 and 9 s (300 ms): out of DumpRAM, then the first home page (Music, Videos, Fitness).
SDL: no pointer grab while the guest pointer is absolute (build.sh patch of sdl_grab_start): no "exit grab"
  title and the cursor stays free.
PACKAGING: make-app.sh takes the version from CHANGELOG.md (1.2.0); packaging/macos/make-dmg.sh builds
  NanoEmu-1.2.0.dmg (app + Applications link + README, UDZO), refusing an app with a bundled snapshot.
TESTS: real window (computer use): opens on home page 1, tap opens apps, panel Home / Sleep / Safe work,
  wake from auto-lock; headless: frame + panel at 0 / 90 degrees, Settings > Bluetooth ON, Music plays.
