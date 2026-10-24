#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Assemble EmmexUO.app from a CMake build directory and ad-hoc sign it. The app opens the
# client (servers are managed on its login screen); the launcher is bundled for game data setup.
#
#   tools/macos/make_app.sh [build-dir] [output-dir]
#
# Defaults: build-dir=build, output-dir=<build-dir>/dist
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=$(cd "${1:-$ROOT/build}" && pwd)
OUT="${2:-$BUILD/dist}"
APP="$OUT/EmmexUO.app"
VERSION=$(git -C "$ROOT" describe --tags --always 2>/dev/null || echo dev)

for f in src/crossuo src/crossuo.so src/xuolauncher; do
    [ -f "$BUILD/$f" ] || { echo "missing $BUILD/$f - build first" >&2; exit 1; }
done

mkdir -p "$OUT"
[ -e "$APP" ] && rm -r -- "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

cp "$BUILD/src/xuolauncher" "$BUILD/src/crossuo" "$BUILD/src/crossuo.so" "$APP/Contents/MacOS/"
cp "$ROOT/resources/crossuo.icns" "$APP/Contents/Resources/"
cp -R "$ROOT/resources/fonts" "$ROOT/resources/login" "$APP/Contents/Resources/"

cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleIdentifier</key>        <string>com.christopherbrand.emmexuo</string>
    <key>CFBundleName</key>              <string>EmmexUO</string>
    <key>CFBundleDisplayName</key>       <string>EmmexUO</string>
    <key>CFBundleExecutable</key>        <string>crossuo</string>
    <key>CFBundleIconFile</key>          <string>crossuo.icns</string>
    <key>CFBundlePackageType</key>       <string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$VERSION</string>
    <key>CFBundleVersion</key>           <string>$VERSION</string>
    <key>LSMinimumSystemVersion</key>    <string>11.0</string>
    <key>LSApplicationCategoryType</key> <string>public.app-category.role-playing-games</string>
    <key>NSHighResolutionCapable</key>   <true/>
</dict>
</plist>
EOF

# Ad-hoc signature: required for arm64 binaries to run and keeps Gatekeeper prompts to the
# usual "downloaded from the internet" one. Sign inner binaries first, then the bundle.
codesign --force --sign - "$APP/Contents/MacOS/crossuo.so" "$APP/Contents/MacOS/xuolauncher" "$APP/Contents/MacOS/crossuo"
codesign --force --sign - "$APP"
codesign --verify --strict "$APP"

echo "$APP ($(lipo -archs "$APP/Contents/MacOS/crossuo"), $VERSION)"
