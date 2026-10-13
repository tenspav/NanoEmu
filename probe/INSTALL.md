# DumpROM install — what was done and how to use it

## The app

`dumprom/` — a raw-surface homebrew app (HBAppKind 1, no LVGL, like
NanoApps `apps/memprobe`). One big "DUMP ROM" button; on tap it runs the
staged dump from the headless `probe.c`, one stage per UI frame:
`/Apps/Data/probe/report.txt` (CPSR + regs 0x3C500000/0x3C500048/0x38900000/
0x22000000) → ungate VROM (0x3C500048 = 0) + `gate.txt` → `vrom0..3.bin`
(4 x 64 KB from 0x20000000) → `ALL_DONE` appended to report.txt.
Then it shows "OK - REBOOT ME" (or "FAIL <stage>"). No spin, no watchdog.

Build: `make -C dumprom` (SDK path is absolute in the Makefile).
Output: `dumprom/build/dumprom.hbapp` (HRL1 relocatable blob, 3188 bytes).

## Install (disk-only — the c6 96 SCSI exec channel is dead)

`./start install` was NOT usable: its step 5 always re-execs the resident via
`sg_raw c6 96` (dead), and its disk step wipes `/Apps/AllApps.pack`,
`Executables/`, `Icons/` and copies a freshly built pack — which would drop
the user's apps (MK64, BLE Music's exe, etc.) that have no built .hbapp in
this repo. So the install was done by hand, incrementally.

Key discovery: the ACTIVE pack is **`/Apps/AllApps-B.pack`** (slot B is active —
`/Apps/Startup/resident-B.active` exists; the B resident reads the `-B` pack,
see mk64-untether `scripts/install-games.sh`). `/Apps/AllApps.pack` (slot A,
3 apps) was left untouched as the fallback. Note: `BLE Music.hbapp` is on disk
but NOT in the active pack (pre-existing condition, not changed).

Steps performed (precedent: `mk64-untether/ipod-app/install-mk64.sh`):

1. Mounted `/dev/sdb1` at `/mnt/ipod`.
2. Backup BEFORE any write to `../dumps/apps-backup-20261013T032913Z/`:
   both packs, all of Executables/, Icons/, Startup/, plus
   `MANIFEST.sha256` — sha256 of every file under /Apps (1443 files).
3. Built the launch record with NanoApps' own `tools/mkapp.py bundle`:
   name `DumpROM`, sbid `hb.dumprom`, kind 1, app_id=label_id=`0x0dade675`
   (build_apps.py's FNV-1a scheme over `org.nanoapps.dumprom`, linear-probed
   against the 8 existing ids), same screen/layout ids and tags as the 8
   existing bundles, screen DB = byte-identical `tools/hb_silverdb.bin`
   (verified equal to the section inside the on-device pack).
4. Icon `install/DumpROM.bin` generated with `build_apps.make_png` +
   `ipodhax.silverdb.pack_silverdb` (glyph "download", #e65d53): 50208 bytes,
   header byte-identical to the device icons. The resident patches the bitmap
   id at buf+24 when serving, so the embedded id is irrelevant.
5. `repack.py`: parsed the device pack (`packtool.py`), appended the DumpROM
   bundle AT THE END (index 8; icon ids are positional = pack order, so the
   old 8 bundles stay byte-identical and keep their icons), rebuilt the HBPK
   header/offset table, verified by re-parsing.
6. Wrote to the device IN THIS ORDER (pack last): `DumpROM.hbapp` ->
   /Apps/Executables/, `DumpROM.bin` -> /Apps/Icons/, then
   `AllApps-B.pack.new` -> /Apps/AllApps-B.pack. Every write re-read and
   hash-checked (all ok). Old files re-hashed afterwards: unchanged.
7. sync, umount, `eject /dev/sdb` (standard USB eject — no custom SCSI
   needed). iPod left ejected.

Pack on device now (7884 bytes, 9 apps, verified by re-reading):
Blocks, Game Boy, Probe, NanoBtFix, Lines, Nanoracle, Nanosync, Mario Kart,
**DumpROM**.

## User steps

1. Reboot the iPod (hold Sleep + Home if needed) — the untether re-runs the
   resident at boot/Home, which re-registers the on-disk pack.
2. On the Home screen, tap the red **DumpROM** icon (last page/position).
3. Tap **DUMP ROM**. Wait ~1 s. It shows **OK - REBOOT ME**.
   (On failure it shows FAIL + stage: REPORT / GATE / VROM0-3 / FINAL.)
4. Reboot manually. Files are in `/Apps/Data/probe/` (report.txt, gate.txt,
   vrom0..3.bin; report.txt ends with ALL_DONE on a full run).

## Rollback

```
sudo mount /dev/sdb1 /mnt/ipod
sudo cp ../dumps/apps-backup-20261013T032913Z/AllApps-B.pack /mnt/ipod/Apps/AllApps-B.pack
sudo rm -f "/mnt/ipod/Apps/Executables/DumpROM.hbapp" /mnt/ipod/Apps/Icons/DumpROM.bin
sync && sudo umount /mnt/ipod && sudo eject /dev/sdb
```

## DumpRAM v2 (torn-map) — upgrade in place

Only the executable changes; the bundle (`hb.dumpram`, app_id 0x0dade1e3), icon and
`AllApps-B.pack` stay as installed. Build: `make -C dumpram` -> `install/DumpRAM.hbapp`.
1. `sudo mount /dev/sdb1 /mnt/ipod`; back up `/mnt/ipod/Apps/Executables/DumpRAM.hbapp`
   (v1) and delete old `/mnt/ipod/Apps/Data/probe/{ram-full,iram,ctx,report}*`.
2. `sudo cp install/DumpRAM.hbapp /mnt/ipod/Apps/Executables/DumpRAM.hbapp`, re-read + sha256.
3. `sync && sudo umount /mnt/ipod && sudo eject /dev/sdb`; unplug, reboot, open DumpRAM
   (title shows "V2"), tap DUMPRAM, wait for OK - REBOOT ME, reboot.
4. Copy `/Apps/Data/probe/` (adds `torn-map.bin`); run
   `python3 ../emulator/fixups/torn_map.py torn-map.bin` for the torn ranges.
Rollback: copy the v1 executable back.

## DumpRAM v4 (IRQ-off overlay) — upgrade in place

Same procedure as v2: only `Apps/Executables/DumpRAM.hbapp` changes (title shows "V4").
Build: `make -C dumpram`, then `cp dumpram/build/dumpram.hbapp install/DumpRAM.hbapp`.
- Do NOT delete the old `/Apps/Data/probe/torn-map.bin` first: v4 reads it and adds every block
  that tore last time to its overlay set (on top of the built-in snap3+snap4 list).
- New output `/Apps/Data/probe/overlay.bin` (~3.8 MB): the volatile DRAM blocks, copied with
  IRQs off at the same instant as ctx.bin. report.txt gains overlay_blocks / overlay_wanted /
  overlay_missing (heap allocation failures; v4 keeps >= 2 MB heap free) / overlay_copy_us /
  irq_mask_saved / fmc_idle_retries. The free space needed on disk grows by the overlay size.
- Copy `/Apps/Data/probe/` as usual; check with
  `python3 ../emulator/fixups/overlay_info.py overlay.bin ram-full.bin torn-map.bin`.
- The screen may freeze for a few tens of ms at the tap (interrupts are off during the copy).
Rollback: copy a v3 executable back (sha256 351285a3...1b95).

## DumpRAM v5 (verified overlay, IRQs on) — replaces v4 (v4 rebooted the device on tap)

Same install steps; title shows "V5". Keep the old torn-map.bin (still read). report.txt is now
written TWICE: a `stage=prepared` breadcrumb (heap free/largest before+after, every overlay buffer
address) before the capture, then the full report. If the device crashes again, copy report.txt:
the breadcrumb tells how far it got. Expected new fields: overlay_scratch_rejected,
overlay_unverified, overlay_retries, overlay_window_us, heap_largest_after_alloc.

## DumpRAM v6 (masked core + verified rest) — replaces v5

Same install steps; title shows "V6". Keep the old torn-map.bin. The screen freezes ~1-2 ms at
the tap (kernel 0x08900000 + stacks 0x0b400000 + IRAM copied with IRQs off). New report.txt fields:
overlay_masked (expect 2), overlay_masked_fallback, masked_section_us, irq_mask_saved,
fmc_idle_retries. If it reboots on tap like v4 did, the stage=prepared breadcrumb still lands
first; roll back to v5 (scratchpad copy).
