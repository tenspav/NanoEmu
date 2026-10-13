# probe — one-shot N7G hardware probe (boot ROM dumper), staged writes

Runs as `/Apps/Startup/ab.hbapp` at Home screen render. Each stage writes and
closes its file before the next runs, so a fault leaves partial progress:
1. `report.txt` — CPSR + regs 0x3C500000/0x3C500048/0x38900000/0x22000000.
2. Ungate VROM clock (write 0 to 0x3C500048) → `gate.txt` (before/after).
3. Dump 256 KB from 0x20000000 as `vrom0.bin`..`vrom3.bin` (4 x 64 KB).
4. Rewrite `report.txt` with `ALL_DONE` appended, then spin forever
   (the watchdog reboots the iPod back to disk mode).

Build: `make` (needs arm-none-eabi-gcc + pyelftools; SDK at ../../NanoApps).
Output: `probe.hbapp` (relocatable HRL1 Thumb blob, entry `payload_entry`).
Install: copy `probe.hbapp` to the iPod FAT disk as `/Apps/Startup/ab.hbapp`.
Breadcrumbs: hb_trace ring tags `PROBE*` (NanoApps `tools/dump_trace.sh`).
