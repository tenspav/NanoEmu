#!/bin/bash
# NanoEmu.app/Contents/MacOS/NanoEmu -- starts the emulator in a window.
#
# Device data (your own captures, never shipped in the repo) is read from
# the data dir, default ~/Library/Application Support/NanoEmu
# (NANOEMU_DATA overrides):
#   snapshot/ram-full.bin iram.bin overlay.bin torn-map.bin   required
#             (a DumpRAM v6 capture, the snap6 layout; or bundled into
#             the app by make-app.sh --snapshot)
#   ipod-disk.dd      optional: the disk image (apps, music); without it
#                     unread flash comes from the snapshot's caches (and
#                     there is no music import)
#   Import/           MP3s for the iPod's import folder (File > Import
#                     Music copies them here)
#   rsrc.img1         optional: OS resource partition (UI images)
#   bootrom-vrom.bin  optional: the real bootrom
#   nanoemu.conf      optional settings (shell syntax), e.g.
#                       ZOOM=auto|100|150|200|300 (percent, points)
#                       FRAME=0 (screen only)  HUD=1  CLOCK_PROFILE=safe
#                       PAD_KEYS=1  AUDIO=0 (mute)  BATTERY_DRAIN=MIN
#                       HOME_AT_START=0 (stay in the captured app)
#                       EXTRA_ARGS="-M nano7,..."
#   state/            save states (Cmd-S / Cmd-L) and the reset point
# Screenshots (Cmd-E) go to ~/Pictures/NanoEmu, the log to
# ~/Library/Logs/NanoEmu.log (capped at 20 MB per run).
# bash 3.2 (macOS /bin/bash): no bash 4 features.

MACOS_DIR=$(cd "$(dirname "$0")" && pwd)
RES=$(cd "$MACOS_DIR/../Resources" && pwd)
DATA="${NANOEMU_DATA:-$HOME/Library/Application Support/NanoEmu}"
SCREENS="${NANOEMU_SCREENS:-$HOME/Pictures/NanoEmu}"
LOG="$HOME/Library/Logs/NanoEmu.log"
mkdir -p "$DATA/state" "$SCREENS" "$HOME/Library/Logs"

alert() {   # alert TEXT [open-data-dir]
    local btn='buttons {"Quit"} default button 1'
    [ -n "${2:-}" ] && btn='buttons {"Quit", "Open Data Folder"} default button 2'
    local r
    r=$(/usr/bin/osascript -e "display alert \"NanoEmu\" message \"$1\" as critical $btn" 2>/dev/null)
    case "$r" in *"Open Data Folder"*) /usr/bin/open "$DATA" ;; esac
    exit 1
}

ZOOM=auto; FRAME=1; HUD=1; CLOCK_PROFILE=""; PAD_KEYS=0; AUDIO=1
BATTERY_DRAIN=""; HOME_AT_START=1; EXTRA_ARGS=""
# shellcheck disable=SC1091
[ -f "$DATA/nanoemu.conf" ] && . "$DATA/nanoemu.conf"
ZOOM="${NANOEMU_ZOOM:-$ZOOM}"

SNAP=""
for d in "$DATA/snapshot" "$RES/snapshot"; do
    if [ -f "$d/ram-full.bin" ]; then SNAP=$d; break; fi
done
[ -n "$SNAP" ] || alert "No device snapshot found.\n\nCopy your DumpRAM capture (ram-full.bin, iram.bin, overlay.bin, torn-map.bin) into:\n$DATA/snapshot\n\nSee docs/MACOS.md." open
for f in iram.bin overlay.bin torn-map.bin; do
    [ -f "$SNAP/$f" ] || alert "The snapshot in $SNAP is missing $f." open
done

q() { printf '%s' "${1//,/,,}"; }      # escape commas in -M values
M="nano7,state-dir=$(q "$DATA/state"),screens-dir=$(q "$SCREENS")"
M="$M,os-image=$(q "$SNAP/ram-full.bin"),iram-image=$(q "$SNAP/iram.bin")"
M="$M,ctx=$(q "$RES/fixups/snap6-ctx.bin"),overlay=$(q "$SNAP/overlay.bin")"
M="$M,os-poke-file=$(q "$RES/fixups/snap6-resume.poke")"
M="$M,torn-map=$(q "$SNAP/torn-map.bin"),nand-miss=oracle"
M="$M,mmu=0x08b28000,torn-ignore-pc=0x093d2a80:0x093d4100"
if [ -f "$DATA/ipod-disk.dd" ]; then
    M="$M,nand-disk=$(q "$DATA/ipod-disk.dd"),nand-disk-offset=49216"
    # music import: File > Import Music (or MP3s dropped on the window)
    # copies files to $DATA/Import; at every start and reset they are put
    # in the iPod's /Apps/Data/Importar, and the Nanosync app on the Home
    # screen (the GL build bundled here) adds them to the library
    mkdir -p "$DATA/Import"
    M="$M,import-dir=$(q "$DATA/Import")"
    [ -f "$RES/nanosync.hbapp" ] && \
        M="$M,nanosync-app=$(q "$RES/nanosync.hbapp")"
fi
[ -f "$DATA/rsrc.img1" ] && M="$M,nand-rsrc=$(q "$DATA/rsrc.img1")"
[ -f "$DATA/bootrom-vrom.bin" ] && M="$M,bootrom=$(q "$DATA/bootrom-vrom.bin")"
# the snap6 capture resumes inside the DumpRAM probe app ("OK - REBOOT ME"):
# press Home at 3, 6 and 9 s (300 ms each): the first leaves DumpRAM, the
# next ones go to the first home page (Music, Videos, Fitness ...); the
# repeats also cover a press the guest misses while the window comes up
[ "$HOME_AT_START" = 1 ] && M="$M,buttons=3:home:300/6:home:300/9:home:300"
AUDIOARGS=()
if [ "$AUDIO" = 1 ]; then       # CoreAudio, through QEMU's audio layer
    AUDIOARGS=(-audiodev coreaudio,id=hostsnd)
    M="$M,audiodev=hostsnd"
fi

# window size: the screen's backing scale (2 on Retina) and the canvas
# (device 320 x 536 units plus the 200-unit side panel with the HUD on, or
# the bare 240 x 432 screen plus the HUD strip)
read -r VW VH SCALE <<<"$(/usr/bin/osascript -l JavaScript -e '
    ObjC.import("AppKit"); var s = $.NSScreen.mainScreen,
    f = s.visibleFrame;
    Math.floor(f.size.width) + " " + Math.floor(f.size.height) + " " +
    Math.round(s.backingScaleFactor)' 2>/dev/null)"
SCALE=${SCALE:-1}
if [ "$FRAME" = 1 ]; then CW=320; CH=536; [ "$HUD" = 1 ] && CW=$((CW + 200))
else CW=240; CH=432; [ "$HUD" = 1 ] && CH=$((CH + 47)); fi
if [ "$ZOOM" = auto ]; then
    # the largest pixel zoom whose window (in points, plus the 32 pt title
    # bar of macOS 26) fits the screen's visible area
    PZ=$SCALE
    if [ -n "$VH" ]; then
        z=$(( (VH - 32) * SCALE / CH )); zw=$(( VW * SCALE / CW ))
        [ $zw -lt $z ] && z=$zw
        [ $z -gt $((6 * SCALE)) ] && z=$((6 * SCALE))
        [ $z -ge 1 ] && PZ=$z
    fi
else
    PZ=$(( ZOOM * SCALE / 100 )); [ $PZ -ge 1 ] || PZ=1
fi
export NANO7_ZOOM="$PZ"
[ "$FRAME" = 1 ] && export NANO7_FRAME=1
[ "$HUD" = 1 ] && export NANO7_HUD=1
[ "$PAD_KEYS" = 1 ] && export NANO7_PAD_KEYS=1
[ -n "$CLOCK_PROFILE" ] && export NANO7_CLOCK_PROFILE="$CLOCK_PROFILE"
[ -n "$BATTERY_DRAIN" ] && export NANO7_BATTERY_DRAIN="$BATTERY_DRAIN"

ARGS=()                 # drop the -psn_* argument old Finders pass
for a in "$@"; do case "$a" in -psn_*) ;; *) ARGS+=("$a") ;; esac; done

{ echo "=== NanoEmu $(date) ==="; echo "data: $DATA  snapshot: $SNAP"; } >"$LOG"
# exec keeps this process (the one Launch Services started) as the app, so
# the Dock icon and the menu bar belong to NanoEmu.app; stderr goes through
# awk, which keeps draining the pipe after the cap
# shellcheck disable=SC2086
exec "$MACOS_DIR/qemu-system-arm" -M "$M" -name NanoEmu \
    -display sdl,show-cursor=on -serial none -monitor none \
    ${AUDIOARGS[@]+"${AUDIOARGS[@]}"} $EXTRA_ARGS ${ARGS[@]+"${ARGS[@]}"} \
    2> >(awk -v max=20000000 -v f="$LOG" \
         '{ n += length($0) + 1; if (n < max) { print >> f; fflush(f) } }')
