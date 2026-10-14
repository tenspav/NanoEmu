#!/bin/bash
# run.sh -- boot the nano7 emulator.
#
#   ./run.sh                     # boot bootrom-vrom.bin (or loop stub) at 0x20000000
#   ./run.sh --os [file]         # also preload RetailOS at 0x08000000
#   ./run.sh --os-entry [file]   # preload OS and jump straight to 0x08000000
#   ./run.sh --stub              # ignore bootrom file, use infinite-loop stub
#   --nand-store FILE / --nand-miss blank|oracle / --nand-disk IMG: NAND page
#                                store (see nano7_fmc.c); --nand-disk-offset LBA:
#                                FTL LBA of the image's sector 0 (snap6: 8661)
#   --disp-dump DIR              write display-pipe frames as PPMs
#   --buttons SEC:BTN[:MS]/...   timed presses (home|power|play|volup|voldown);
#                                keys: h/p/space, Up/Down = volume (VNC or sendkey)
#   --rtc snapshot|host|SECONDS  PMU real-time clock at start (default snapshot)
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

MARGS="nano7"
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

exec "$QEMU" -M "$MARGS" -nographic -serial none -monitor none \
     "${EXTRA[@]+"${EXTRA[@]}"}"
