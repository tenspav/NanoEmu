#!/bin/bash
# Runs probe/ramscope/ramscope.c (the real app logic) bare-metal in the nano7 emulator,
# user mode + MMU, for each PA 0x0c000000 model: 0 unmapped, 1 open bus, 2 own RAM, 3 alias.
# Expect: NO STORAGE, NO STORAGE, INDEPENDENT MEMORY, ALIASED.
set -e
H=$(cd "$(dirname "$0")" && pwd); R=$H/../../..; SDK=/home/pavliukov/ipodwork/NanoApps/sdk
T=${TMPDIR:-/tmp}/ramscope-h; mkdir -p $T
arm-none-eabi-gcc -marm -mcpu=cortex-a7 -O1 -ffreestanding -nostdlib -fno-builtin -I$SDK \
  -I$SDK/freestanding -I$R/probe/ramscope -Wl,-T,$H/link.ld -o $T/h.elf $H/harness.c -lgcc
arm-none-eabi-objcopy -O binary $T/h.elf $T/h.bin
for m in ${@:-0 1 2 3}; do
  echo "== extra-dram=$m"
  timeout 90 $R/emulator/build/qemu-system-arm -M nano7,os-image=$T/h.bin,entry=0x08000000,extra-dram=$m \
    -display none -monitor none -serial none \
    -semihosting-config enable=on,target=native,userspace=on -no-reboot 2>&1 | grep -E "=== state|EXCEPTION"
done
