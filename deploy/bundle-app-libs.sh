#!/bin/bash
# ====================================================================
#  Bundle Homebrew dylibs into MIDI-FT Bridge.app
#
#  The app links against /opt/homebrew (ffmpeg + SDL2), so it won't launch
#  on a Mac without Homebrew. This copies those dylibs (and their transitive
#  Homebrew deps) into Contents/Frameworks, rewrites the load paths to
#  @executable_path/../Frameworks, and re-signs ad-hoc — making the .app
#  self-contained, the same way deploy/build-macos.sh does for the CLI.
#
#  Usage: ./deploy/bundle-app-libs.sh "/path/to/MIDI-FT Bridge.app"
#  Build the app in RELEASE first (Debug splits into a .debug.dylib).
# ====================================================================
set -e

APP="$1"
[ -d "$APP" ] || { echo "usage: $0 <path-to .app>" >&2; exit 1; }

EXE_NAME="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$APP/Contents/Info.plist")"
EXE="$APP/Contents/MacOS/$EXE_NAME"
FW="$APP/Contents/Frameworks"
DEST="@executable_path/../Frameworks"
mkdir -p "$FW"

hb() { otool -L "$1" | awk '/\/opt\/homebrew/ {print $1}'; }

echo "[1/4] Copying Homebrew dylibs (+ transitive deps) into Frameworks..."
# Several passes so dependencies-of-dependencies get pulled in too.
for pass in 1 2 3 4 5; do
    for bin in "$EXE" "$FW"/*.dylib; do
        [ -f "$bin" ] || continue
        for lib in $(hb "$bin"); do
            name="$(basename "$lib")"
            if [ ! -f "$FW/$name" ]; then
                cp "$lib" "$FW/$name"
                chmod u+w "$FW/$name"
            fi
        done
    done
done

echo "[2/4] Rewriting the executable's load paths..."
for lib in $(hb "$EXE"); do
    install_name_tool -change "$lib" "$DEST/$(basename "$lib")" "$EXE"
done

echo "[3/4] Rewriting each bundled dylib's id and inter-deps..."
for dylib in "$FW"/*.dylib; do
    [ -f "$dylib" ] || continue
    install_name_tool -id "$DEST/$(basename "$dylib")" "$dylib"
    for dep in $(hb "$dylib"); do
        install_name_tool -change "$dep" "$DEST/$(basename "$dep")" "$dylib"
    done
done

echo "[4/4] Re-signing ad-hoc (dylibs first, then the bundle)..."
for dylib in "$FW"/*.dylib; do
    codesign --force --timestamp=none --sign - "$dylib"
done
codesign --force --timestamp=none --sign - "$EXE"
codesign --force --timestamp=none --sign - "$APP"

echo
echo "Bundled $(ls "$FW" | wc -l | tr -d ' ') dylibs into Contents/Frameworks."
if otool -L "$EXE" | grep -q /opt/homebrew; then
    echo "WARNING: executable still references /opt/homebrew:" >&2
    otool -L "$EXE" | grep /opt/homebrew >&2
    exit 1
fi
echo "OK: no /opt/homebrew references remain in the executable."
