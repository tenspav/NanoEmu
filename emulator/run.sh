#!/bin/bash
# run.sh -- boot the nano7 emulator.
#
#   ./run.sh                     # boot bootrom-vrom.bin (or loop stub) at 0x20000000
#   ./run.sh --os [file]         # also preload RetailOS at 0x08000000
#   ./run.sh --os-entry [file]   # preload OS and jump straight to 0x08000000
#   ./run.sh --stub              # ignore bootrom file, use infinite-loop stub
#   --nand-store FILE / --nand-miss blank|oracle / --nand-disk IMG: NAND page
#                                store (see nano7_fmc.c); --nand-disk-offset LBA:
#                                FTL LBA of the image's sector 0 (snap6 device: 49216)
#   --nanosync-app FILE          GL NanoSync .hbapp laid over the disk's Nanosync.hbapp
#   --import-dir DIR             host folder added to the disk's /Apps/Data/Importar
#   --nand-patch FILE            replaced disk blocks, e.g. a test app in place of
#                                an installed one (make it with tools/disk_patch.py)
#   --nand-rsrc FILE[@LBA]       resource partition image for NAND misses (OS UI
#                                images; make it with tools/extract_rsrc.py IPSW)
#   --disp-dump DIR              write display-pipe frames as PPMs
#   --buttons SEC:BTN[:MS]/...   timed presses (home|power|play|volup|voldown);
#                                keys: h or Esc/p/space, Up/Down = volume (VNC,
#                                window or sendkey); Cmd-S/L/R/P/... mirror
#                                the F keys (docs/MACOS.md)
#   --rtc snapshot|host|SECONDS  PMU real-time clock at start (default snapshot)
#   --audio-play                 play the audio live on the host (ffplay, else
#                                aplay, reading a FIFO; 44.1 kHz s16 stereo)
#   --host-audio [DRIVER]        play the audio through QEMU's own audio
#                                backend (default: coreaudio on macOS, pa
#                                elsewhere; needs a build with that driver)
#   --audio-out FILE|FIFO        audio PCM sink: raw s16le stereo, *.wav adds a
#                                header, a FIFO streams (e.g. into aplay/ffplay)
#   keys (VNC/display/sendkey): F1 reset (reload the boot state), F2 save
#                                state, F4 load state (emulator/state/), F5/Pause
#                                pause, F6 speed 1x/2x/4x, Tab hold 4x, F7 rotate,
#                                F9 zoom 1x/2x/3x, F12 screenshot (emulator/screens/)
#   --bt-hci-socket PATH|tcp:[HOST]:PORT  Bluetooth UART: listen there and
#                                carry the raw HCI H4 stream (a host bridge /
#                                real adapter) instead of the built-in fake
#                                controller; NANO7_BT_TRACE=1 logs all packets
#   --bt-peer gamepad|gamepad-connect|none  device in range of the fake BT
#                                controller (default gamepad: HID "NanoPad",
#                                found by inquiries, accepts connections and
#                                pairing; gamepad-connect also pages the iPod)
#   --bt-pad SCRIPT              the BT gamepad's input once its HID link is up:
#                                SEC:CONTROLS[:MS]/... (a b x y l r select start
#                                home b1-b16 up down left right lx= ly= rx= ry=
#                                joined by +; MS = hold, default 100), none
#   --aes-gid-key HEX32 / --aes-uid-key HEX32  AES engine hardware keys
#                                (unknown by default: those jobs just copy)
#   --hud                        start with the usage HUD shown (CPU / GPU load
#                                graphs under the display; F10 toggles it)
#   --pad-keys                   start with the keyboard driving the fake BT
#                                gamepad (F8 toggles: WASD left stick, arrows
#                                right stick, J K U I = A B X Y, Q / E = L / R,
#                                Enter start, Backspace select, Esc home)
#   --host-pad auto|PATH|NAME    a real gamepad on the host drives the fake BT
#                                gamepad (hot-plug). Linux evdev: auto = first
#                                /dev/input/event* gamepad, or a node / FIFO
#                                path; needs the "input" group. macOS IOKit
#                                HID: auto, or a product-name substring
#                                (NANO7_HOST_PAD_BUTTONS / _TRACE: see
#                                nano7_hostpad.c)
#   --tilt DEG                   pin the accelerometer tilt (steering-wheel
#                                angle, + = right; default: Left / Right keys)
#   --clock-profile stock|safe|max  start with a clock profile (CPU/GPU MHz
#                                342/133, 684/171, 684/266; F11 cycles) and the
#                                65 C governor; default: the guest's own clocks
#   --battery-drain [MIN]        run on battery, draining it over MIN guest
#                                minutes (default 30); without it the PMU
#                                reports USB power and a full battery
#   --temp C                     pin the PMU die temperature (default: an
#                                illustrative thermal model)
#   --frame                      draw the iPod around the screen, with
#                                clickable Sleep / Home / Volume / Play
#                                buttons (the macOS app's look)
#   --zoom N                     start zoom 1-6 (default 1; F9 cycles 1-3,
#                                and a resized SDL window picks the
#                                largest whole factor that fits)
#   --vnc [N]                    serve the LCD on VNC display :N (port 5900+N,
#                                default :0); mouse = touch, keys h/p/space
#   extra qemu args are passed through
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
QEMU="$HERE/build/qemu-system-arm"
OS_DEFAULT=/home/pavliukov/ipodwork/bt-research-20261007/nano-os-20261007/retailos-08000000.bin
IRAM_DEFAULT=/home/pavliukov/ipodwork/bt-research-20261007/nano-os-20261007/iram-22000000.bin
RAMFULL_DEFAULT=/home/pavliukov/ipodwork/bt-research-20261007/nano-os-20261007/ram-full-08000000.bin
CTX_DEFAULT=/home/pavliukov/ipodwork/bt-research-20261007/nano-os-20261007/ctx.bin

MARGS="nano7,state-dir=$HERE/state,screens-dir=$HERE/screens"
# default bootrom: the real dump, if present
if [ -f "$HERE/../dumps/bootrom-vrom.bin" ]; then
    MARGS="$MARGS,bootrom=$HERE/../dumps/bootrom-vrom.bin"
fi
EXTRA=()
# consume next arg as an optional value only if it is not another flag
optval() { case "${2:-}" in -*|"") echo "$1"; return 1 ;; *) echo "$2"; return 0 ;; esac; }
while [ $# -gt 0 ]; do
    case "$1" in
        --stub)
            MARGS="$MARGS,bootrom=/dev/null"; shift ;;
        --os)
            v=$(optval "$OS_DEFAULT" "${2:-}") && shift 2 || shift
            MARGS="$MARGS,os-image=$v" ;;
        --os-entry)
            v=$(optval "$OS_DEFAULT" "${2:-}") && shift 2 || shift
            MARGS="$MARGS,os-image=$v,entry=0x08000000" ;;
        --entry)
            MARGS="$MARGS,entry=${2:?--entry needs a value}"; shift 2 ;;
        --pcsample)
            v=$(optval 1000 "${2:-}") && shift 2 || shift
            MARGS="$MARGS,pcsample-ms=$v" ;;
        --iram)
            v=$(optval "$IRAM_DEFAULT" "${2:-}") && shift 2 || shift
            MARGS="$MARGS,iram-image=$v" ;;
        --iram-protect)
            v=$(optval 1 "${2:-}") && shift 2 || shift
            MARGS="$MARGS,iram-protect=$v" ;;
        --warm)
            MARGS="$MARGS,warm=1"; shift ;;
        --ctx)
            v=$(optval "$CTX_DEFAULT" "${2:-}") && shift 2 || shift
            MARGS="$MARGS,ctx=$v" ;;
        --os-zero-file)
            MARGS="$MARGS,os-zero-file=${2:?--os-zero-file needs a file}"; shift 2 ;;
        --overlay)
            MARGS="$MARGS,overlay=${2:?--overlay needs a file}"; shift 2 ;;
        --nand-store)
            MARGS="$MARGS,nand-store=${2:?--nand-store needs a file}"; shift 2 ;;
        --nand-miss)
            MARGS="$MARGS,nand-miss=${2:?--nand-miss needs blank|oracle}"; shift 2 ;;
        --nand-disk-offset)
            MARGS="$MARGS,nand-disk-offset=${2:?--nand-disk-offset needs an LBA}"; shift 2 ;;
        --nand-disk)
            MARGS="$MARGS,nand-disk=${2:?--nand-disk needs a file}"; shift 2 ;;
        --disp-dump)
            MARGS="$MARGS,disp-dump=${2:?--disp-dump needs a directory}"; shift 2 ;;
        --vnc)
            v=$(optval 0 "${2:-}") && shift 2 || shift
            EXTRA+=(-vnc ":$v") ;;
        --nand-patch)
            MARGS="$MARGS,nand-patch=${2:?--nand-patch needs a file}"; shift 2 ;;
        --nanosync-app)
            MARGS="$MARGS,nanosync-app=${2:?--nanosync-app needs a .hbapp}"; shift 2 ;;
        --import-dir)
            MARGS="$MARGS,import-dir=${2:?--import-dir needs a folder}"; shift 2 ;;
        --nand-rsrc)
            MARGS="$MARGS,nand-rsrc=${2:?--nand-rsrc needs a file}"; shift 2 ;;
        --audio-play)
            AUDIO_PLAY=1; shift ;;
        --host-audio)
            d=coreaudio; [ "$(uname -s)" = Darwin ] || d=pa
            v=$(optval "$d" "${2:-}") && shift 2 || shift
            EXTRA+=(-audiodev "$v,id=hostsnd"); MARGS="$MARGS,audiodev=hostsnd" ;;
        --audio-out)
            MARGS="$MARGS,audio-out=${2:?--audio-out needs a file or FIFO}"; shift 2 ;;
        --bt-hci-socket)
            MARGS="$MARGS,bt-hci=${2:?--bt-hci-socket needs a path or tcp:HOST:PORT}"; shift 2 ;;
        --bt-peer)
            MARGS="$MARGS,bt-peer=${2:?--bt-peer needs gamepad, gamepad-connect or none}"; shift 2 ;;
        --bt-pad)
            MARGS="$MARGS,bt-pad=${2:?--bt-pad needs a script or none}"; shift 2 ;;
        --aes-gid-key)
            MARGS="$MARGS,aes-gid-key=${2:?--aes-gid-key needs 32 hex digits}"; shift 2 ;;
        --aes-uid-key)
            MARGS="$MARGS,aes-uid-key=${2:?--aes-uid-key needs 32 hex digits}"; shift 2 ;;
        --hud)
            export NANO7_HUD=1; shift ;;
        --pad-keys)
            export NANO7_PAD_KEYS=1; shift ;;
        --host-pad)
            export NANO7_HOST_PAD="${2:?--host-pad needs auto, a path or a name}"; shift 2 ;;
        --tilt)
            export NANO7_TILT="${2:?--tilt needs degrees}"; shift 2 ;;
        --clock-profile)
            export NANO7_CLOCK_PROFILE="${2:?--clock-profile needs stock, safe or max}"; shift 2 ;;
        --frame)
            export NANO7_FRAME=1; shift ;;
        --zoom)
            export NANO7_ZOOM="${2:?--zoom needs a factor 1-6}"; shift 2 ;;
        --battery-drain)
            v=$(optval 30 "${2:-}") && shift 2 || shift
            export NANO7_BATTERY_DRAIN="$v" ;;
        --temp)
            export NANO7_TEMP="${2:?--temp needs degrees C}"; shift 2 ;;
        --rtc)
            MARGS="$MARGS,rtc=${2:?--rtc needs snapshot, host or Unix seconds}"; shift 2 ;;
        --buttons)
            MARGS="$MARGS,buttons=${2:?--buttons needs SEC:BTN[:MS]/...}"; shift 2 ;;
        --torn-map)
            MARGS="$MARGS,torn-map=${2:?--torn-map needs a file}"; shift 2 ;;
        --os-poke-file)
            MARGS="$MARGS,os-poke-file=${2:?--os-poke-file needs a file}"; shift 2 ;;
        --cpsr)
            MARGS="$MARGS,cpsr=${2:?--cpsr needs a value}"; shift 2 ;;
        --ramfull)
            v=$(optval "$RAMFULL_DEFAULT" "${2:-}") && shift 2 || shift
            MARGS="$MARGS,os-image=$v" ;;
        *) EXTRA+=("$1"); shift ;;
    esac
done

if [ -n "${AUDIO_PLAY:-}" ]; then
    # the guest's PCM goes into a FIFO that a host player drains; the
    # emulator writes it non-blocking, so a missing player costs nothing
    fifo="$HERE/state/audio.fifo"
    mkdir -p "$HERE/state"
    [ -p "$fifo" ] || { rm -f "$fifo"; mkfifo "$fifo"; }
    if command -v ffplay >/dev/null; then
        player=(ffplay -nodisp -autoexit -loglevel error -f s16le -ar 44100 -ac 2)
    elif command -v aplay >/dev/null; then
        player=(aplay -q -f S16_LE -r 44100 -c 2)
    else
        echo "run.sh: --audio-play needs ffplay or aplay" >&2; exit 1
    fi
    # the player ends when the emulator closes its end
    "${player[@]}" "$fifo" &
    MARGS="$MARGS,audio-out=$fifo"
fi

exec "$QEMU" -M "$MARGS" -nographic -serial none -monitor none \
     "${EXTRA[@]+"${EXTRA[@]}"}"
