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

if [ "$(uname -s)" = "Darwin" ]; then
    # macOS: use Homebrew pkg-config/glib/pixman instead of the .deb sysroot
    export PATH="/opt/homebrew/bin:$PATH"
elif [ ! -x "$SYSROOT/root/usr/bin/pkg-config" ]; then
    echo "[build] fetching build-dep .debs into local sysroot"
    mkdir -p "$SYSROOT/debs" "$SYSROOT/root"
    (cd "$SYSROOT/debs" && apt-get download pkg-config libglib2.0-dev \
        libglib2.0-dev-bin libpixman-1-dev zlib1g-dev libpcre3-dev \
        libffi-dev)
    for d in "$SYSROOT"/debs/*.deb; do dpkg -x "$d" "$SYSROOT/root"; done
fi

# --- apply nano7 machine ----------------------------------------------------
# in-place sed, GNU or BSD (macOS needs an explicit empty backup suffix)
sedi() { if [ "$(uname -s)" = "Darwin" ]; then sed -i '' "$@"; else sed -i "$@"; fi; }
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
   "$HERE/machine/hw/arm/nano7_pmu.h" "$HERE/machine/hw/arm/nano7_lcd.c" \
   "$HERE/machine/hw/arm/nano7_lcd.h" "$HERE/machine/hw/arm/nano7_touch.c" \
   "$HERE/machine/hw/arm/nano7_touch.h" "$HERE/machine/hw/arm/nano7_ui.c" \
   "$HERE/machine/hw/arm/nano7_ui.h" "$HERE/machine/hw/arm/nano7_clock.c" \
   "$HERE/machine/hw/arm/nano7_clock.h" "$HERE/machine/hw/arm/nano7_dma.c" \
   "$HERE/machine/hw/arm/nano7_dma.h" "$HERE/machine/hw/arm/nano7_codec.c" \
   "$HERE/machine/hw/arm/nano7_codec.h" "$HERE/machine/hw/arm/nano7_i2s.c" \
   "$HERE/machine/hw/arm/nano7_i2s.h" "$HERE/machine/hw/arm/nano7_adsp.c" \
   "$HERE/machine/hw/arm/nano7_adsp.h" "$HERE/machine/hw/arm/minimp3.h" "$HERE/machine/hw/arm/nano7_mbx.c" \
   "$HERE/machine/hw/arm/nano7_mbx.h" "$HERE/machine/hw/arm/nano7_mbxr.c" "$HERE/machine/hw/arm/nano7_mbxr.h" \
   "$HERE/machine/hw/arm/nano7_aes.c" "$HERE/machine/hw/arm/nano7_aes.h" \
   "$HERE/machine/hw/arm/nano7_btuart.c" "$HERE/machine/hw/arm/nano7_btuart.h" \
   "$HERE/machine/hw/arm/nano7_hud.c" "$HERE/machine/hw/arm/nano7_hud.h" \
   "$HERE/machine/hw/arm/nano7_power.c" "$HERE/machine/hw/arm/nano7_power.h" \
   "$HERE/machine/hw/arm/nano7_hostpad.c" "$HERE/machine/hw/arm/nano7_hostpad.h" \
   "$HERE/machine/hw/arm/nano7_accel.c" "$HERE/machine/hw/arm/nano7_accel.h" \
   "$HERE/machine/hw/arm/nano7_skin.c" "$HERE/machine/hw/arm/nano7_skin.h" \
   "$HERE/machine/hw/arm/nano7_macmenu.m" "$HERE/machine/hw/arm/nano7_macmenu.h" "$QEMU_DIR/hw/arm/"

grep -q "^config NANO7" "$QEMU_DIR/hw/arm/Kconfig" || \
    cat "$HERE/machine/kconfig.snippet" >> "$QEMU_DIR/hw/arm/Kconfig"
SNIPPET=$(tr -d '\n' < "$HERE/machine/meson.snippet")
if grep -q "CONFIG_NANO7" "$QEMU_DIR/hw/arm/meson.build"; then
    sedi "s|^arm_ss.add(when: 'CONFIG_NANO7'.*|$SNIPPET|" \
        "$QEMU_DIR/hw/arm/meson.build"
else
    sedi "s|^hw_arch += {'arm': arm_ss}|$SNIPPET\n\nhw_arch += {'arm': arm_ss}|" \
        "$QEMU_DIR/hw/arm/meson.build"
fi
grep -q "CONFIG_NANO7" "$QEMU_DIR/configs/devices/arm-softmmu/default.mak" || \
    cat "$HERE/machine/default-mak.snippet" >> \
        "$QEMU_DIR/configs/devices/arm-softmmu/default.mak"

# QEMU source patches (machine/patches/, applied once; -R --dry-run detects
# an already patched tree). darwin-poll-ns: sub-ms timers on hosts without
# ppoll (macOS) - without it guest apps time out there (Session 66);
# sdl-hidpi: the SDL window at native Retina density (exact integer zoom).
for p in darwin-poll-ns sdl-hidpi; do
    pf="$HERE/machine/patches/$p.patch"
    if ! patch -d "$QEMU_DIR" -p1 -R --dry-run -s -f < "$pf" >/dev/null 2>&1; then
        echo "[build] applying $p.patch"
        patch -d "$QEMU_DIR" -p1 -s -f < "$pf"
    fi
done

# SDL window (macOS): the title is just the -name value (not "QEMU (name-0)"),
# and the panel is scaled by whole factors only (letterboxed otherwise)
# no pointer grab while the guest pointer is absolute (the nano7 touch
# screen): the cursor stays free and the title has no "exit grab" hint
grep -q "nano7: no grab" "$QEMU_DIR/ui/sdl2.c" || \
    sedi 's|^    if (!con \|\| !qemu_console_is_graphic(con)) {$|    if (con \&\& qemu_input_is_absolute(con)) { /* nano7: no grab */\
        return;\
    }\
\
&|' "$QEMU_DIR/ui/sdl2.c"
grep -q '"QEMU (%s-%d)%s"' "$QEMU_DIR/ui/sdl2.c" && \
    sedi 's|"QEMU (%s-%d)%s", qemu_name,|"%s%.0d%s", qemu_name,|' \
        "$QEMU_DIR/ui/sdl2.c"
grep -q "SDL_RenderSetIntegerScale" "$QEMU_DIR/ui/sdl2-2d.c" || \
    sedi 's|^    SDL_RenderSetLogicalSize(scon->real_renderer,$|    SDL_RenderSetIntegerScale(scon->real_renderer, SDL_TRUE);\
&|' "$QEMU_DIR/ui/sdl2-2d.c"

# --- configure & build ------------------------------------------------------
if [ "$(uname -s)" = "Darwin" ]; then
    export PKG_CONFIG="pkg-config"
else
    export PATH="$HERE/tools:$PATH"   # pkg-config wrapper
    export PKG_CONFIG="$HERE/tools/pkg-config"
fi

PYFLAG=""; SDLFLAG="--disable-sdl"; AUDIODRV=""
if [ "$(uname -s)" = "Darwin" ]; then
    [ -x /opt/homebrew/bin/python3.12 ] && PYFLAG="--python=/opt/homebrew/bin/python3.12"
    SDLFLAG="--enable-sdl"
    AUDIODRV="coreaudio"        # host sound (nano7 audiodev=, Session 66)
fi
CONF=(--target-list=arm-softmmu
      --disable-user --disable-docs --disable-guest-agent
      --disable-slirp $SDLFLAG --disable-gtk --disable-opengl
      --disable-virglrenderer --disable-xen --disable-kvm
      --disable-werror --audio-drv-list=$AUDIODRV $PYFLAG
      "--extra-cflags=-ffile-prefix-map=$HERE/=")
# (-ffile-prefix-map: no build-machine paths such as /Users/NAME in the
# binary's assert / debug strings)
# re-configure when the flags change (an existing build dir keeps its old
# configuration otherwise)
if [ ! -f "$HERE/build/build.ninja" ] || \
   [ "$(cat "$HERE/build/.nano7-configure" 2>/dev/null)" != "${CONF[*]}" ]; then
    echo "[build] configuring"
    mkdir -p "$HERE/build"
    (cd "$HERE/build" && "$QEMU_DIR/configure" "${CONF[@]}")
    echo "${CONF[*]}" > "$HERE/build/.nano7-configure"
fi

JOBS=$(command -v nproc >/dev/null && nproc || sysctl -n hw.ncpu)
echo "[build] building (ninja -j$JOBS qemu-system-arm)"
# Only the emulator target: the bundled qtest suite mislinks against the
# sysroot's static glib and is not needed here.
ninja -C "$HERE/build" -j"$JOBS" qemu-system-arm
echo "[build] done: $HERE/build/qemu-system-arm"
