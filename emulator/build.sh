#!/bin/bash
# build.sh -- download (if needed), extract, patch, configure and build
# QEMU with the iPod nano 7G machine. Idempotent; safe to re-run.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
QEMU_VER=9.1.2
QEMU_DIR="$HERE/vendor/qemu-$QEMU_VER"
TARBALL="$HERE/vendor/qemu-$QEMU_VER.tar.xz"
URL="https://download.qemu.org/qemu-$QEMU_VER.tar.xz"
SYSROOT="$HERE/sysroot"

mkdir -p "$HERE/vendor"

if [ ! -d "$QEMU_DIR" ]; then
    if [ ! -s "$TARBALL" ]; then
        echo "[build] downloading $URL"
        curl -fSL -o "$TARBALL" "$URL"
    fi
    echo "[build] extracting $TARBALL"
    tar -C "$HERE/vendor" -xf "$TARBALL"
fi

# --- no-root build dependencies -------------------------------------------
# ninja via pip --user; pkg-config + glib/pixman/zlib headers from Ubuntu
# debs extracted into sysroot/root.
if ! command -v ninja >/dev/null 2>&1; then
    if [ -x "$HOME/.local/bin/ninja" ]; then
        export PATH="$HOME/.local/bin:$PATH"
    else
        echo "[build] installing ninja via pip --user"
        pip3 install --user ninja
        export PATH="$HOME/.local/bin:$PATH"
    fi
fi

if [ ! -x "$SYSROOT/root/usr/bin/pkg-config" ]; then
    echo "[build] fetching build-dep .debs into local sysroot"
    mkdir -p "$SYSROOT/debs" "$SYSROOT/root"
    (cd "$SYSROOT/debs" && apt-get download pkg-config libglib2.0-dev \
        libglib2.0-dev-bin libpixman-1-dev zlib1g-dev libpcre3-dev \
        libffi-dev)
    for d in "$SYSROOT"/debs/*.deb; do dpkg -x "$d" "$SYSROOT/root"; done
fi

# --- apply nano7 machine ----------------------------------------------------
echo "[build] installing nano7 machine files"
cp "$HERE/machine/hw/arm/nano7.c" "$HERE/machine/hw/arm/nano7_unimpl.c" \
   "$HERE/machine/hw/arm/nano7_unimpl.h" "$HERE/machine/hw/arm/nano7_regfile.c" \
   "$HERE/machine/hw/arm/nano7_regfile.h" "$HERE/machine/hw/arm/nano7_fmc.c" \
   "$HERE/machine/hw/arm/nano7_fmc.h" "$HERE/machine/hw/arm/nano7_vic.c" \
   "$HERE/machine/hw/arm/nano7_vic.h" "$HERE/machine/hw/arm/nano7_timer.c" \
   "$HERE/machine/hw/arm/nano7_timer.h" "$HERE/machine/hw/arm/nano7_spicmd.c" \
   "$HERE/machine/hw/arm/nano7_spicmd.h" "$HERE/machine/hw/arm/nano7_torn.c" \
   "$HERE/machine/hw/arm/nano7_torn.h" "$HERE/machine/hw/arm/nano7_disp.c" \
   "$HERE/machine/hw/arm/nano7_disp.h" "$HERE/machine/hw/arm/nano7_gpio.c" \
   "$HERE/machine/hw/arm/nano7_gpio.h" "$HERE/machine/hw/arm/nano7_i2c.c" \
   "$HERE/machine/hw/arm/nano7_i2c.h" "$HERE/machine/hw/arm/nano7_pmu.c" \
   "$HERE/machine/hw/arm/nano7_pmu.h" "$QEMU_DIR/hw/arm/"

grep -q "^config NANO7" "$QEMU_DIR/hw/arm/Kconfig" || \
    cat "$HERE/machine/kconfig.snippet" >> "$QEMU_DIR/hw/arm/Kconfig"
SNIPPET=$(tr -d '\n' < "$HERE/machine/meson.snippet")
if grep -q "CONFIG_NANO7" "$QEMU_DIR/hw/arm/meson.build"; then
    sed -i "s|^arm_ss.add(when: 'CONFIG_NANO7'.*|$SNIPPET|" \
        "$QEMU_DIR/hw/arm/meson.build"
else
    sed -i "s|^hw_arch += {'arm': arm_ss}|$SNIPPET\n\nhw_arch += {'arm': arm_ss}|" \
        "$QEMU_DIR/hw/arm/meson.build"
fi
grep -q "CONFIG_NANO7" "$QEMU_DIR/configs/devices/arm-softmmu/default.mak" || \
    cat "$HERE/machine/default-mak.snippet" >> \
        "$QEMU_DIR/configs/devices/arm-softmmu/default.mak"

# --- configure & build ------------------------------------------------------
export PATH="$HERE/tools:$PATH"   # pkg-config wrapper
export PKG_CONFIG="$HERE/tools/pkg-config"

if [ ! -f "$HERE/build/build.ninja" ]; then
    echo "[build] configuring"
    mkdir -p "$HERE/build"
    (cd "$HERE/build" && "$QEMU_DIR/configure" \
        --target-list=arm-softmmu \
        --disable-user --disable-docs --disable-guest-agent \
        --disable-slirp --disable-sdl --disable-gtk --disable-opengl \
        --disable-virglrenderer --disable-xen --disable-kvm \
        --disable-werror --audio-drv-list=)
fi

echo "[build] building (ninja -j$(nproc) qemu-system-arm)"
# Only the emulator target: the bundled qtest suite mislinks against the
# sysroot's static glib and is not needed here.
ninja -C "$HERE/build" -j"$(nproc)" qemu-system-arm
echo "[build] done: $HERE/build/qemu-system-arm"
