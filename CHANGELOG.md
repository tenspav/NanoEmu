# Changelog

All notable milestones of NanoEmu, in reverse order. This project uses semantic-ish versioning: every guest-visible capability bump is a minor version.

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
