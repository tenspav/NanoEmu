# NanoSync (GL build for NanoEmu)

The NanoSync homebrew app, rebuilt as a GL surface app so it runs on the
homebrew resident captured in the emulator's RAM snapshot. NanoEmu.app lays
`nanosync.hbapp` over the disk's `/Apps/Executables/Nanosync.hbapp` at start
(machine option `nanosync-app=`), so the Nanosync icon on the Home screen
opens this build.

What it does inside the emulated iPod:

1. Imports the MP3 files in `/Apps/Data/Importar` (NanoEmu.app's
   File > Import Music puts them there) into the music library: it copies
   them to `iPod_Control/Music` and writes iTunesCDB, the SQLite databases
   and the artwork database, signed with the iPod's own serial, which it
   reads from `/iPod_Control/Device/SysInfo` (or `SysInfoExtended`).
2. Reloads the library in place when the import is done, so the new songs
   show up in Music and play without a restart.

The databases are written with the schema of iPod nano 7G firmware 1.1.2
(version 1109, `core/mdb_schema.h`, generated from the device's own
databases). The in-place reload needs it: the OS's statements use the
columns of that schema, and only a full boot migrates older ones.

`dryrun.txt` in the import folder still means a rehearsal: everything is
checked and nothing is written.

## Build

The sources build inside the NanoApps SDK (`nano7-untethered/nanoapps`):
copy this folder to `nanoapps/apps/nanosync_gl`, then

    make -f Makefile.gl
    python3 ../../tools/mkrelocapp.py build/nanosync.reloc.elf build/nanosync.hbapp payload_entry

`NS_GUID=<16 hex digits>` fixes the serial at build time instead.

License: GPL-3.0 (see LICENSE and THIRD_PARTY.md).
