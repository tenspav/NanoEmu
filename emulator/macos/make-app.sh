#!/bin/bash
# make-app.sh -- package the macOS build as NanoEmu.app (see docs/MACOS.md).
#
#   ./build.sh && macos/make-app.sh [--snapshot DIR] [--out DIR] [--no-libs]
#
#   --snapshot DIR   copy a device snapshot (ram-full.bin iram.bin
#                    overlay.bin torn-map.bin) into the app (~72 MB), so it
#                    runs without a data dir; never publish such an app
#   --out DIR        where NanoEmu.app goes (default: emulator/dist)
#   --no-libs        leave the Homebrew libraries where they are (the app
#                    then only runs on a Mac with the same Homebrew setup)
# Without --no-libs every non-system dylib qemu-system-arm needs is copied
# into Contents/Frameworks, rewired to @executable_path and ad-hoc signed,
# so the app runs on any Apple Silicon Mac with the same macOS or newer.
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)        # emulator/
ROOT=$(cd "$HERE/.." && pwd)
OUT="$HERE/dist"; SNAP=""; LIBS=1
while [ $# -gt 0 ]; do
    case "$1" in
        --snapshot) SNAP="${2:?--snapshot needs a directory}"; shift 2 ;;
        --out)      OUT="${2:?--out needs a directory}"; shift 2 ;;
        --no-libs)  LIBS=0; shift ;;
        *) echo "make-app.sh: unknown option $1" >&2; exit 1 ;;
    esac
done
[ "$(uname -s)" = "Darwin" ] || { echo "make-app.sh: macOS only" >&2; exit 1; }
BIN="$HERE/build/qemu-system-arm"
[ -x "$BIN" ] || { echo "make-app.sh: build first ($BIN missing)" >&2; exit 1; }

APP="$OUT/NanoEmu.app"
C="$APP/Contents"
# bundle version: the newest "## [vX.Y.Z]" in CHANGELOG.md (packaging spec)
VERSION=$(sed -n 's/^## \[v\([0-9][0-9.]*\)\].*/\1/p' "$ROOT/CHANGELOG.md" | head -1)
VERSION=${VERSION:-0.0.0}
REV=$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || echo dev)
echo "[app] $APP ($VERSION, $REV)"
rm -rf "$APP"
mkdir -p "$C/MacOS" "$C/Resources/fixups" "$C/Frameworks"

cp "$HERE/macos/launcher.sh" "$C/MacOS/NanoEmu"
chmod 755 "$C/MacOS/NanoEmu"
cp "$BIN" "$C/MacOS/qemu-system-arm"
chmod u+w "$C/MacOS/qemu-system-arm"
cp "$HERE/fixups/snap6-ctx.bin" "$HERE/fixups/snap6-resume.poke" \
   "$C/Resources/fixups/"
sed "s/@VERSION@/$VERSION/g" "$HERE/macos/Info.plist.in" > "$C/Info.plist"
printf 'APPL????' > "$C/PkgInfo"

# --- icon: assets/icon-256.png (+ icon-512.png for the large sizes) -------
ICONSET=$(mktemp -d)/NanoEmu.iconset
mkdir -p "$ICONSET"
SRC256="$ROOT/assets/icon-256.png"
SRC512="$ROOT/assets/icon-512.png"; [ -f "$SRC512" ] || SRC512="$SRC256"
for s in 16 32 128 256 512; do
    src="$SRC256"; [ $s -ge 256 ] && src="$SRC512"
    sips -z $s $s "$src" --out "$ICONSET/icon_${s}x${s}.png" >/dev/null
    d=$((s * 2)); src="$SRC256"; [ $d -gt 256 ] && src="$SRC512"
    sips -z $d $d "$src" --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o "$C/Resources/NanoEmu.icns"
rm -rf "$(dirname "$ICONSET")"

if [ -n "$SNAP" ]; then
    mkdir -p "$C/Resources/snapshot"
    for f in ram-full.bin iram.bin overlay.bin torn-map.bin; do
        cp "$SNAP/$f" "$C/Resources/snapshot/"
    done
    echo "[app] snapshot bundled from $SNAP (keep this app private)"
fi

# --- private copies of the Homebrew libraries -----------------------------
deps() {    # the non-system install names a Mach-O file links against
    otool -L "$1" | tail -n +2 | awk '{print $1}' | \
        grep -v -E '^(/usr/lib/|/System/|@executable_path/)' || true
}
resolve() { # resolve DEP REFERRER -> a real file path
    case "$1" in
        @loader_path/*) echo "$(dirname "$2")/${1#@loader_path/}" ;;
        @rpath/*)
            for d in "$(dirname "$2")" /opt/homebrew/lib /usr/local/lib; do
                [ -f "$d/${1#@rpath/}" ] && { echo "$d/${1#@rpath/}"; return; }
            done
            echo "$1" ;;
        *) echo "$1" ;;
    esac
}
if [ $LIBS = 1 ]; then
    todo=("$C/MacOS/qemu-system-arm")
    orig=("$BIN")                   # where each file's @loader_path points
    i=0
    while [ $i -lt ${#todo[@]} ]; do
        f=${todo[$i]}; o=${orig[$i]}; i=$((i + 1))
        for dep in $(deps "$f"); do
            real=$(resolve "$dep" "$o")
            [ -f "$real" ] || { echo "make-app.sh: cannot find $dep" >&2; exit 1; }
            name=$(basename "$real")
            if [ ! -f "$C/Frameworks/$name" ]; then
                cp -L "$real" "$C/Frameworks/$name"
                chmod u+w "$C/Frameworks/$name"
                install_name_tool -id "@executable_path/../Frameworks/$name" \
                    "$C/Frameworks/$name" 2>/dev/null
                todo+=("$C/Frameworks/$name"); orig+=("$(cd "$(dirname "$real")" && pwd -P)/$(basename "$real")")
            fi
            install_name_tool -change "$dep" \
                "@executable_path/../Frameworks/$name" "$f" 2>/dev/null
        done
    done
    # Homebrew's SDL2 is sdl2-compat: it dlopen()s SDL3 (invisible to
    # otool), trying @loader_path/libSDL3.dylib first
    if [ -f "$C/Frameworks/libSDL2-2.0.0.dylib" ] && \
       strings "$C/Frameworks/libSDL2-2.0.0.dylib" | grep -q '^libSDL3.dylib$'; then
        sdl3=""
        for d in "$(pkg-config --variable=libdir sdl3 2>/dev/null)" \
                 /opt/homebrew/opt/sdl3/lib /usr/local/opt/sdl3/lib; do
            [ -n "$d" ] && [ -f "$d/libSDL3.0.dylib" ] && { sdl3="$d/libSDL3.0.dylib"; break; }
        done
        [ -n "$sdl3" ] || { echo "make-app.sh: SDL2 is sdl2-compat but SDL3 was not found" >&2; exit 1; }
        cp -L "$sdl3" "$C/Frameworks/libSDL3.dylib"
        chmod u+w "$C/Frameworks/libSDL3.dylib"
        install_name_tool -id "@executable_path/../Frameworks/libSDL3.dylib" \
            "$C/Frameworks/libSDL3.dylib" 2>/dev/null
        for dep in $(deps "$C/Frameworks/libSDL3.dylib"); do
            [ "$(basename "$dep")" = libSDL3.0.dylib ] && continue    # itself
            echo "make-app.sh: SDL3 needs $dep (not bundled)" >&2; exit 1
        done
    fi
    echo "[app] bundled $(ls "$C/Frameworks" | wc -l | tr -d ' ') libraries"
    # install_name_tool voids the signatures; arm64 needs a valid one
    for f in "$C/Frameworks/"* ; do codesign --force -s - "$f" 2>/dev/null; done
fi
codesign --force -s - "$C/MacOS/qemu-system-arm" 2>/dev/null
rmdir "$C/Frameworks" 2>/dev/null || true
codesign --force -s - "$APP" 2>/dev/null || true
touch "$APP"                        # make Finder pick up the new icon
echo "[app] done: open \"$APP\""
