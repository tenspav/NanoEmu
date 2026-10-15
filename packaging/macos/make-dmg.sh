#!/bin/bash
# make-dmg.sh -- NanoEmu-<version>.dmg with the drag-to-Applications layout
# (docs/tasks/packaging-spec.md).
#
#   emulator/build.sh && emulator/macos/make-app.sh && packaging/macos/make-dmg.sh
#
# Packs emulator/dist/NanoEmu.app (built without --snapshot: no device data
# ever goes into a release), an Applications link and a README into a
# compressed read-only UDZO image in emulator/dist/. The app is ad-hoc
# signed, not notarized: first open is right-click > Open.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
APP="$ROOT/emulator/dist/NanoEmu.app"
[ -d "$APP" ] || { echo "make-dmg.sh: run emulator/macos/make-app.sh first" >&2; exit 1; }
[ -d "$APP/Contents/Resources/snapshot" ] && {
    echo "make-dmg.sh: $APP contains a device snapshot (make-app.sh --snapshot); never publish that" >&2
    exit 1; }
VERSION=$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' "$APP/Contents/Info.plist")
DMG="$ROOT/emulator/dist/NanoEmu-$VERSION.dmg"

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
cat > "$STAGE/READ ME.txt" <<TXT
NanoEmu $VERSION - iPod nano 7G emulator

1. Drag NanoEmu to Applications.
2. First launch: right-click NanoEmu > Open (the app is not notarized).
3. NanoEmu ships no Apple firmware. Put your own DumpRAM capture in
   ~/Library/Application Support/NanoEmu/snapshot
   (ram-full.bin, iram.bin, overlay.bin, torn-map.bin), and optionally
   ipod-disk.dd and rsrc.img1 next to it for music and apps.
   Details: docs/MACOS.md in the NanoEmu repository.
TXT

rm -f "$DMG"
hdiutil create -quiet -volname "NanoEmu $VERSION" -srcfolder "$STAGE" \
    -fs HFS+ -format UDZO -imagekey zlib-level=9 "$DMG"
shasum -a 256 "$DMG" | sed "s|$ROOT/emulator/dist/||" > "$DMG.sha256"
echo "[dmg] $DMG ($(du -h "$DMG" | cut -f1))"
