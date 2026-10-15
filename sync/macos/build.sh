#!/bin/bash
# build.sh -- builds "NanoSync Mac.app" (sync/macos/NanoSyncMac.swift) into
# emulator/dist/, ad-hoc signed. Needs the Xcode command line tools.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT="${1:-$ROOT/emulator/dist}"
APP="$OUT/NanoSync Mac.app"
C="$APP/Contents"
VERSION=$(sed -n 's/^## \[v\([0-9][0-9.]*\)\].*/\1/p' "$ROOT/CHANGELOG.md" | head -1)

rm -rf "$APP"
mkdir -p "$C/MacOS" "$C/Resources"
swiftc -O -swift-version 5 -parse-as-library -target arm64-apple-macos13.0 \
    -o "$C/MacOS/NanoSyncMac" "$HERE/NanoSyncMac.swift"
cat > "$C/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleName</key><string>NanoSync Mac</string>
	<key>CFBundleDisplayName</key><string>NanoSync Mac</string>
	<key>CFBundleIdentifier</key><string>io.github.tenspav.nanosync-mac</string>
	<key>CFBundleExecutable</key><string>NanoSyncMac</string>
	<key>CFBundleIconFile</key><string>NanoSyncMac</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleShortVersionString</key><string>$VERSION</string>
	<key>CFBundleVersion</key><string>$VERSION</string>
	<key>LSMinimumSystemVersion</key><string>13.0</string>
	<key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST
ICONSET=$(mktemp -d)/NanoSyncMac.iconset
mkdir -p "$ICONSET"
for s in 16 32 128 256 512; do
    sips -z $s $s "$ROOT/assets/icon-512.png" --out "$ICONSET/icon_${s}x${s}.png" >/dev/null
    d=$((s * 2)); [ $d -gt 512 ] && d=512
    sips -z $d $d "$ROOT/assets/icon-512.png" --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o "$C/Resources/NanoSyncMac.icns"
rm -rf "$(dirname "$ICONSET")"
codesign --force --deep -s - "$APP" >/dev/null 2>&1
echo "[sync] $APP ($VERSION)"
