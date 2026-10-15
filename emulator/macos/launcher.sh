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
#                     unread flash comes from the snapshot's caches
#   rsrc.img1         optional: OS resource partition (UI images)
#   bootrom-vrom.bin  optional: the real bootrom
#   nanoemu.conf      optional settings (shell syntax), e.g.
#                       ZOOM=auto|1-6  HUD=1  CLOCK_PROFILE=safe  PAD_KEYS=1
#                       AUDIO=1 (needs ffplay)  EXTRA_ARGS="-M nano7,..."
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

ZOOM=auto; HUD=0; CLOCK_PROFILE=""; PAD_KEYS=0; AUDIO=0; EXTRA_ARGS=""
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
[ -f "$DATA/ipod-disk.dd" ] && \
    M="$M,nand-disk=$(q "$DATA/ipod-disk.dd"),nand-disk-offset=49216"
[ -f "$DATA/rsrc.img1" ] && M="$M,nand-rsrc=$(q "$DATA/rsrc.img1")"
[ -f "$DATA/bootrom-vrom.bin" ] && M="$M,bootrom=$(q "$DATA/bootrom-vrom.bin")"

if [ "$ZOOM" = auto ]; then
    # the largest whole factor whose window (plus its title bar, 32 pt on
    # macOS 26) fits
    # the screen's visible area; macOS would clip a bigger one
    vis=$(/usr/bin/osascript -l JavaScript -e 'ObjC.import("AppKit");
        var f = $.NSScreen.mainScreen.visibleFrame;
        Math.floor(f.size.width) + " " + Math.floor(f.size.height)' 2>/dev/null)
    rows=432; [ "$HUD" = 1 ] && rows=$((432 + 47))     # NANO7_HUD_ROWS
    ZOOM=1
    if [ -n "$vis" ]; then
        read -r vw vh <<<"$vis"
        z=$(( (vh - 32) / rows )); zw=$(( vw / 240 ))
        [ $zw -lt $z ] && z=$zw
        [ $z -gt 6 ] && z=6
        [ $z -ge 1 ] && ZOOM=$z
    fi
fi
export NANO7_ZOOM="$ZOOM"
[ "$HUD" = 1 ] && export NANO7_HUD=1
[ "$PAD_KEYS" = 1 ] && export NANO7_PAD_KEYS=1
[ -n "$CLOCK_PROFILE" ] && export NANO7_CLOCK_PROFILE="$CLOCK_PROFILE"

if [ "$AUDIO" = 1 ]; then
    FFPLAY=$(PATH="/opt/homebrew/bin:/usr/local/bin:$PATH" command -v ffplay)
    if [ -n "$FFPLAY" ]; then
        fifo="$DATA/state/audio.fifo"
        [ -p "$fifo" ] || { rm -f "$fifo"; mkfifo "$fifo"; }
        "$FFPLAY" -nodisp -autoexit -loglevel error -f s16le -ar 44100 \
            -ac 2 "$fifo" >/dev/null 2>&1 &
        M="$M,audio-out=$(q "$fifo")"
    fi
fi

ARGS=()                 # drop the -psn_* argument old Finders pass
for a in "$@"; do case "$a" in -psn_*) ;; *) ARGS+=("$a") ;; esac; done

{ echo "=== NanoEmu $(date) ==="; echo "data: $DATA  snapshot: $SNAP"; } >"$LOG"
# exec keeps this process (the one Launch Services started) as the app, so
# the Dock icon and the menu bar belong to NanoEmu.app; stderr goes through
# awk, which keeps draining the pipe after the cap
# shellcheck disable=SC2086
exec "$MACOS_DIR/qemu-system-arm" -M "$M" -name NanoEmu \
    -display sdl,show-cursor=on -serial none -monitor none \
    $EXTRA_ARGS ${ARGS[@]+"${ARGS[@]}"} \
    2> >(awk -v max=20000000 -v f="$LOG" \
         '{ n += length($0) + 1; if (n < max) { print >> f; fflush(f) } }')
