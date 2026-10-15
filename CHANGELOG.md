# Changelog

All notable milestones of NanoEmu, in reverse order. This project uses semantic-ish versioning: every guest-visible capability bump is a minor version.

## [v1.2.1] - 2026-10-15
### Added
- **NanoSync Mac**: a small Mac app that sends music to a real iPod nano 7G for the NanoSync homebrew app to add to the library. Drag songs or folders onto its window, plug the iPod in over USB (it shows up as a drive), click Sync, eject, then tap Importar in NanoSync on the iPod
  - MP3 files are copied as they are; M4A, AAC, WAV, AIFF, FLAC, OGG, Opus and WMA are converted to MP3 with ffmpeg, keeping tags and cover art
  - Short, FAT-safe file names; files already waiting on the iPod are never overwritten
  - Shows free space, how many songs are waiting and NanoSync's last report; warns when NanoSync is missing or in rehearsal mode
  - Removes the hidden "._" files macOS leaves on the iPod's drive
  - Videos are listed but not sent: NanoSync imports music only
  - Also a command line: `"NanoSync Mac.app/Contents/MacOS/NanoSyncMac" --cli FILES...`
- The `.dmg` now contains both NanoEmu and NanoSync Mac

## [v1.2.0] - 2026-10-15
### Added
- NanoEmu.app for macOS: a real desktop app, 780 x 836 points on a 13" MacBook (1.5x), with the iPod nano 7G drawn around the screen and clickable Sleep/Wake, Volume Up/Down, Play/Pause and Home buttons
- Side panel next to the iPod: live FPS, CPU and GPU clock and load, RAM, temperature and rotation in large text, plus labelled buttons for the Stock / Safe / Max clock profiles, the iPod buttons, Pause, Reset, Mute, Rotate and Screenshot
- Native menu bar: Reset, Save/Load State, Screenshot, Pause, Mute, Speed, clock profiles, Size, Rotation (0/90/180/270), Side Panel, Full Screen, iPod buttons
- Sound through CoreAudio, with Mute
- Battery always full and on USB power: no low battery warning or shutdown (`BATTERY_DRAIN` / `--battery-drain` to test power)
- Sleep turns the screen off, Wake turns it back on; the iPod's own 1 minute auto-lock works the same way
- The app opens on the first home page (Music, Videos, Fitness ...)
- `.dmg` installer (`packaging/macos/make-dmg.sh`)

### Fixed
- Short clicks on the drawn buttons are held for at least 200 ms, so the OS never misses them (waking from auto-lock needed it)
- The window no longer grabs the mouse pointer ("Press Ctrl-Alt-G to exit grab")
- Each frame updates only the screen area of the window, so a big window no longer slows the emulated iPod down

## [v0.5.0] - 2026-10-14
### Added
- Live display window over VNC (`run.sh --vnc N`, port 5900+N)
- Touch controller emulation: mouse press/drag/release become real touch reports (tapping Music opens the Music app)
- Keyboard buttons over VNC: h = Home, p = Power, space = Play/Pause

## [v0.4.0] - 2026-10-14
### Added
- PMU real-time clock (1 Hz, `--rtc snapshot|host|<unix>`)
### Fixed
- Blitter fill transparency artifacts (dark boxes behind clock digits)

## [v0.3.0] - 2026-10-14
### Added
- PMU battery/temperature/charger emulation (full battery, USB power)
### Fixed
- Low Battery shutdown loop

## [v0.2.0] - 2026-10-13
### Added
- I2C PMU buttons (Home/Power/Play/Pause), GPIO interrupt groups
- Home button wakes the device and exits to the real home screen (icons, page dots, live clock, battery)

## [v0.1.0] - 2026-10-13
### Added
- First OS-rendered screen in emulation (240x432)
- Display blitter (0x39000000) + LCD controller (0x38900000) with 60 Hz sync
- NAND sparse backing store; FTL accepts read-after-write
- Stable snapshot resume: interrupts (dual VIC), timers, FMSS sequencer, SPI engine
- Probe apps: DumpROM (bootrom), DumpRAM v6 (near-atomic 64MB snapshot with tear map + overlay)
