# nano7-emu

QEMU 9.1.2 with a new machine `nano7`: iPod nano 7G (2012), S5L8747-class
SoC, single Cortex-A5 (QEMU lacks cortex-a5; cortex-a7 is used as stand-in).
Memory map: DRAM 64MB @0x08000000, VROM 256KB @0x20000000, IRAM 256KB
@0x22000000, plus a low-priority catch-all MMIO logger (0x0-0x07ffffff and
0x10000000-0x3fffffff) that logs each unimplemented access once per unique
(guest PC, address) to stderr.

Build (downloads qemu-9.1.2.tar.xz, applies machine, builds): `./build.sh`
Our QEMU files also live in `machine/` so they survive vendor/ deletion.

Run: `./run.sh` (boots bootrom-vrom.bin at 0x20000000; if absent/short the
VROM is padded with an ARM `b .` loop). Flags: `--stub`, `--os [file]`
(preload RetailOS @0x08000000), `--os-entry [file]` (preload + jump there),
`--iram` (IRAM snapshot + vectors), `--iram-protect` (vector page read-only -
needed!), `--ramfull` (64MB live DRAM dump), `--ctx`/`--cpsr`, `--pcsample N`.

Works: RetailOS direct-boot runs the FULL init-task sequence cleanly
(session 3), AND the real bootrom (../dumps/bootrom-vrom.bin, 64KB valid
ARM code) boots: VIC/CLKCON/PLL/watchdog/FMC-init all run, then it parks
in the boot-media detection loop, see boot-progress.md "Session 4".
Blocked at: media engine @0x3C400000/0x38400000 needs semantic status
sequences (busy flag in bootrom state struct never clears).
Next: RE 0x3C400000 identify semantics; or force USB/DFU path; or take
the warm-boot branch (PMGR+4 bit18) for the RetailOS handoff.
