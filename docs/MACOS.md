# NanoEmu on macOS

NanoEmu builds natively on Apple Silicon Macs and packages as a normal
double-clickable app, **NanoEmu.app**: its own Dock icon and menu bar, a
window titled "NanoEmu" showing the iPod's 240x432 screen at a whole-number
zoom, the mouse as the touchscreen and the keyboard as the iPod's buttons.

Tested on a MacBook Air M2, macOS 26.6, Homebrew SDL2 (sdl2-compat on SDL3).

## 1. Build

```sh
brew install pkg-config glib pixman sdl2 python@3.12 ninja
cd emulator
./build.sh                 # QEMU 9.1.2 + the nano7 machine, SDL window enabled
macos/make-app.sh          # -> emulator/dist/NanoEmu.app
```

`make-app.sh` options:

| Option | Effect |
|---|---|
| `--out DIR` | put `NanoEmu.app` in DIR (default `emulator/dist`) |
| `--snapshot DIR` | copy a device snapshot into the app (about 72 MB) so it needs no data folder. **Never publish such an app**: the snapshot is your own device's RAM. |
| `--no-libs` | do not copy the Homebrew libraries into the app (smaller; then it only runs on a Mac with the same Homebrew setup) |

By default the app is self-contained apart from your device data. Every
Homebrew library `qemu-system-arm` uses (glib, pixman, SDL2, and the SDL3
library that sdl2-compat loads at run time) is copied into
`Contents/Frameworks`, rewired to `@executable_path`, and ad-hoc signed.
Drag the app to `/Applications` if you like. It is not notarized, so if you
copy it to another Mac, open it the first time with right-click → Open.

## 2. Give it your device data

No Apple firmware ships with NanoEmu. The app reads your own captures
(made with the probe apps, see [`probe/`](../probe/)) from its data folder:

```
~/Library/Application Support/NanoEmu/
  snapshot/            required: a DumpRAM v6 capture (the snap6 layout)
    ram-full.bin  iram.bin  overlay.bin  torn-map.bin
  ipod-disk.dd         optional: disk image (apps, music); without it, flash
                       not in the snapshot's caches reads as blank
  rsrc.img1            optional: OS resource partition (tools/extract_rsrc.py)
  bootrom-vrom.bin     optional: the real bootrom
  nanoemu.conf         optional settings, see below
  state/               save states and the reset point (created by the app)
```

From a repo checkout that already has the dumps, a symlink is enough:

```sh
mkdir -p ~/Library/Application\ Support/NanoEmu
ln -s "$PWD/../dumps/snap6" ~/Library/Application\ Support/NanoEmu/snapshot
ln -s "$PWD/../dumps/ipod-disk.dd" ~/Library/Application\ Support/NanoEmu/   # optional
```

If the snapshot is missing, the app shows an alert with a button that opens
the data folder. `NANOEMU_DATA=/some/dir open NanoEmu.app` does not work,
because `open` drops environment variables. To use another data folder, run
`NanoEmu.app/Contents/MacOS/NanoEmu` directly with `NANOEMU_DATA` set.

Other files:
- Screenshots (Cmd-E / F12) go to `~/Pictures/NanoEmu/`.
- The log of the last run is `~/Library/Logs/NanoEmu.log`, capped at 20 MB.

Writes the emulated OS makes to flash are kept in a temporary store and
dropped on quit. To keep a session, save state (Cmd-S); a save state
includes the flash store.

### nanoemu.conf

Shell syntax, all optional:

```sh
ZOOM=auto            # or 1-6; auto = largest whole factor that fits the screen
HUD=1                # start with the CPU / GPU / RAM usage HUD
CLOCK_PROFILE=safe   # stock | safe | max (with the 65 C governor)
PAD_KEYS=1           # start with the keyboard driving the fake BT gamepad
AUDIO=1              # live sound through ffplay (brew install ffmpeg)
EXTRA_ARGS=""        # extra QEMU arguments, e.g. "-M nano7,bt-peer=none"
```

## 3. Using it

### Window

- **Size:** the window is always the panel at a whole-number zoom, so pixels stay square and sharp.
- **Start zoom:** with `ZOOM=auto`, the app picks the largest zoom that fits the screen's visible area. On a 13" MacBook Air with the Dock showing that is 1x; hide the Dock or use full screen for 2x.
- **Changing zoom:** Cmd-1 to Cmd-4 set the zoom. A zoom that doesn't fit falls back to the largest one that does.
- **Resizing:** drag-resize the window and, a second after you let go, it snaps to the largest whole zoom that fits.
- **Full screen:** the green button or Ctrl-Cmd-F. The panel is centred at a whole-number scale.
- **HUD:** turning the usage HUD on adds a strip under the screen and the window grows to fit.

### iPod buttons

| iPod | Keys |
|---|---|
| Home | H, Esc, Home (Fn-←) |
| Sleep / Wake (Power) | P |
| Play / Pause | Space |
| Volume up / down | ↑ / ↓ (also = / -) |
| Touchscreen | mouse: click = tap, drag = swipe |

The Mac's own media keys (the ▶︎ and 🔊 row) keep controlling the Mac.

### Emulator controls

MacBook F keys are media keys, so each F-key action also has a Cmd shortcut.
The F keys still work with Fn held, or directly on an external keyboard.
While Cmd is held, no key reaches the iPod, so Cmd-P never presses Power.

| Action | Cmd shortcut | F key |
|---|---|---|
| Save state (slot0) | Cmd-S | F2 |
| Load state | Cmd-L or Cmd-O | F4 |
| Reset (reload the boot state) | Cmd-R | F1 |
| Pause / resume | Cmd-P | F5 |
| Fast-forward 1x / 2x / 4x | Cmd-F (hold Tab: 4x while held) | F6 |
| Rotate 90° | Cmd-T | F7 |
| Gamepad keys on/off (WASD, arrows, J K U I, Q E) | Cmd-G | F8 |
| Zoom | Cmd-1 … Cmd-4 | F9 cycles 1x-3x |
| Usage HUD | Cmd-U | F10 |
| Clock profile STOCK → SAFE → MAX | Cmd-K | F11 |
| Screenshot (PNG of the panel) | Cmd-E | F12 |
| Hide / minimize / quit | Cmd-H / Cmd-M / Cmd-Q | |

The reset point is saved automatically when the snapshot first starts
running. When the emulated OS reboots itself (a PMGR reset), NanoEmu also
returns to that point.

## 4. Without the app

`./run.sh` works on macOS as on Linux. Add `-display sdl` for a window, and
`--zoom N` for the start zoom:

```sh
cd emulator
./run.sh --ramfull ../dumps/snap6/ram-full.bin --iram ../dumps/snap6/iram.bin \
  --ctx fixups/snap6-ctx.bin --overlay ../dumps/snap6/overlay.bin \
  --os-poke-file fixups/snap6-resume.poke --torn-map ../dumps/snap6/torn-map.bin \
  --nand-miss oracle --zoom 2 -display sdl,show-cursor=on -name NanoEmu \
  -M nano7,mmu=0x08b28000,torn-ignore-pc=0x093d2a80:0x093d4100
```

## How the app is put together

```
NanoEmu.app/Contents/
  Info.plist           bundle id io.github.tenspav.nanoemu, icon, version
  MacOS/NanoEmu        launcher (emulator/macos/launcher.sh): finds the
                       data, picks the zoom, then execs qemu-system-arm
  MacOS/qemu-system-arm
  Frameworks/          private copies of the Homebrew dylibs
  Resources/NanoEmu.icns   from assets/icon-256.png (+ icon-512.png)
  Resources/fixups/    snap6-ctx.bin, snap6-resume.poke
```

The launcher `exec`s QEMU, so the process macOS started is the emulator.
Its binary sits in `Contents/MacOS`, so macOS attributes it to the bundle:
the Dock icon, the app menu ("NanoEmu") and Cmd-Tab all show NanoEmu, not
QEMU.

`build.sh` makes two small changes to QEMU's SDL front end:
- The window title is the plain `-name` ("NanoEmu"), not "QEMU (NanoEmu-0)".
- The renderer scales by whole factors only (`SDL_RenderSetIntegerScale`).

The zoom-follows-window behaviour is the machine's own console `ui_info`
hook (`nano7_ui.c`). The Cmd shortcuts are also in `nano7_ui.c`, so they
work over VNC and QMP `send-key` too, e.g. `["meta_l","s"]`.
