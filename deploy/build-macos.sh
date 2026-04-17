#!/bin/bash
# Build a self-contained macOS deployment package
# Usage: ./deploy/build-macos.sh
#
# Prerequisites: brew install ffmpeg sdl2 cmake pkg-config

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
DEPLOY_DIR="$PROJECT_DIR/deploy-macos"

echo "=== Building midi-ft-bridge for macOS ==="

# Build
cmake -B "$PROJECT_DIR/build" -DCMAKE_BUILD_TYPE=Release "$PROJECT_DIR"
cmake --build "$PROJECT_DIR/build"

# Create deploy directory
rm -rf "$DEPLOY_DIR"
mkdir -p "$DEPLOY_DIR/lib" "$DEPLOY_DIR/clips"

# Copy binaries and config
cp "$PROJECT_DIR/build/midi_ft_bridge" "$DEPLOY_DIR/"
cp "$PROJECT_DIR/build/panel_viewer" "$DEPLOY_DIR/"
cp "$PROJECT_DIR/config_local.json" "$DEPLOY_DIR/"

# Copy README
cp "$SCRIPT_DIR/README-macos.txt" "$DEPLOY_DIR/README.txt" 2>/dev/null || true

# Bundle dylibs (all Homebrew dependencies + transitive)
echo "Bundling libraries..."
for pass in 1 2 3; do
    for bin in "$DEPLOY_DIR/midi_ft_bridge" "$DEPLOY_DIR/panel_viewer" "$DEPLOY_DIR"/lib/*.dylib; do
        [ -f "$bin" ] || continue
        for lib in $(otool -L "$bin" 2>/dev/null | grep /opt/homebrew | awk '{print $1}'); do
            libname=$(basename "$lib")
            if [ ! -f "$DEPLOY_DIR/lib/$libname" ]; then
                cp "$lib" "$DEPLOY_DIR/lib/$libname"
            fi
        done
    done
done

# Rewrite library paths to @executable_path/lib/
echo "Fixing library paths..."
for bin in "$DEPLOY_DIR/midi_ft_bridge" "$DEPLOY_DIR/panel_viewer"; do
    for lib in $(otool -L "$bin" | grep /opt/homebrew | awk '{print $1}'); do
        install_name_tool -change "$lib" "@executable_path/lib/$(basename $lib)" "$bin"
    done
done

for lib in "$DEPLOY_DIR"/lib/*.dylib; do
    chmod u+w "$lib"
    install_name_tool -id "@executable_path/lib/$(basename $lib)" "$lib"
    for dep in $(otool -L "$lib" | grep /opt/homebrew | awk '{print $1}'); do
        install_name_tool -change "$dep" "@executable_path/lib/$(basename $dep)" "$lib"
    done
done

# Re-sign (ad-hoc)
echo "Signing..."
codesign --force --sign - "$DEPLOY_DIR/midi_ft_bridge"
codesign --force --sign - "$DEPLOY_DIR/panel_viewer"
for lib in "$DEPLOY_DIR"/lib/*.dylib; do
    codesign --force --sign - "$lib"
done

# Verify
echo "Verifying..."
"$DEPLOY_DIR/midi_ft_bridge" --help >/dev/null 2>&1
"$DEPLOY_DIR/panel_viewer" --help >/dev/null 2>&1

# Create zip
ZIP="$PROJECT_DIR/deploy-macos.zip"
rm -f "$ZIP"
(cd "$PROJECT_DIR" && zip -r deploy-macos.zip deploy-macos/ -x "*.DS_Store")

SIZE=$(du -sh "$ZIP" | awk '{print $1}')
echo ""
echo "=== Done ==="
echo "  Folder: $DEPLOY_DIR"
echo "  Zip:    $ZIP ($SIZE)"
echo ""
echo "  To test: cd deploy-macos && ./panel_viewer --config config_local.json --scale 4"
