# NanoRework — worker instructions

Homebrew QEMU emulator for a legacy ARM media-player SoC (Samsung S5L8747-class, Cortex-A5). Hardware preservation on the user's own device.

## Delegation policy (follow always)
- Delegate independent sub-analyses (disassembly one-offs, register-map lookups, data-structure walks, log analysis) to internal subagents running on **sonnet with medium effort** — run several in parallel when independent.
- Keep Opus for: synthesis, design decisions, writing/editing emulator C code, and final verification.
- Every subagent prompt must demand a <=20-line summary as its deliverable.

## On-device apps (probe/)
- Any long operation MUST show live progress on screen: a "BUSY..." state the instant the button is tapped, plus progress (percent or MB counter) during dumps, and a clear "OK - REBOOT ME" only at the end. Never leave the screen static during work — the user can't tell if it's alive.

## Mission (user's goals, in order)
1. Preservation: a working emulator that boots the original RetailOS to the Home screen, visible window, mouse = touch, keyboard = buttons.
2. Use the emulator as a safe testbed for the real prize: giving homebrew games access to the FULL 64MB RAM (apps are confined to a small ~8MB heap today). Experiments go to the emulator first, real iPod only after proven safe.
3. Bluetooth done right: investigate classic BT pairing/handshake with other devices (phone/PC) so we are not stuck with BLE-only, which is slow and unreliable. Goal: a fast reliable host channel for file transfer, app installs, and debugging.

## Emulator feature checklist (user requirements — do not drop)
- Visible screen window (QEMU display), mouse = touchscreen (touch spec: workers/codex-touch.txt)
- Physical buttons mapped to keyboard: volume up/down, play/pause, sleep/wake, home (button spec: workers/codex-touch.txt — Home/Power via I2C1 slave 0x73 reg 0x07, wheel GPIO)
- Must boot the original RetailOS to the Home screen with real disk content
- Eventually buildable on macOS (user's MacBook M2)
- In-emulator usage HUD with simple graphs: guest CPU busy % (from PC sampler: idle-loop vs work), guest RAM usage (read the OS's own heap/allocator stats, we know the structures), GPU busy % once the GPU is modeled. Ships with the live display window.

## Project rules
- Read emulator/boot-progress.md first — it is the session journal; append a dated entry for every work session.
- Machine code lives in emulator/machine/hw/arm/ and must be copied into the vendor QEMU tree per build.sh.
- Never modify files under dumps/. Scratch work in /tmp.
- Keep all run.sh options working.
- Firmware binaries are read-only data; never patch them.
