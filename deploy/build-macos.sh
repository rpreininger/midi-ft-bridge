#!/bin/bash
# Build a self-contained macOS CLI package (headless midi_ft_bridge + the
# optional SDL2 panel_viewer dev tool).
# Usage: ./deploy/build-macos.sh
#
# The shipping GUI app is built from the Xcode project in mac-app/ — this
# script is only for the headless CLI binary. The bridge is fully native
# (AVFoundation + AudioToolbox + CoreMIDI), so there are NO third-party
# dylibs to bundle; it runs on any Mac at the deployment target as-is.
#
# Prerequisites: cmake (+ optional: brew install sdl2 pkg-config — only for
# the panel_viewer dev tool).

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
DEPLOY_DIR="$PROJECT_DIR/deploy-macos"

echo "=== Building midi-ft-bridge (CLI) for macOS ==="

# Build
cmake -B "$PROJECT_DIR/build" -DCMAKE_BUILD_TYPE=Release "$PROJECT_DIR"
cmake --build "$PROJECT_DIR/build"

# Fresh deploy directory
rm -rf "$DEPLOY_DIR"
mkdir -p "$DEPLOY_DIR/clips"

# Binaries (panel_viewer only exists if SDL2 was available at configure time)
cp "$PROJECT_DIR/build/midi_ft_bridge" "$DEPLOY_DIR/"
[ -f "$PROJECT_DIR/build/panel_viewer" ] && cp "$PROJECT_DIR/build/panel_viewer" "$DEPLOY_DIR/"

# Config
cp "$PROJECT_DIR/config.json" "$DEPLOY_DIR/" 2>/dev/null || true

# Runtime clips (the .mp4 playback files; ProRes masters aren't needed at
# runtime and are skipped to keep the package small).
echo "Copying clips..."
for sub in mp4 Dummies; do
    if [ -d "$PROJECT_DIR/clips/$sub" ]; then
        mkdir -p "$DEPLOY_DIR/clips/$sub"
        cp "$PROJECT_DIR/clips/$sub"/*.mp4 "$DEPLOY_DIR/clips/$sub/" 2>/dev/null || true
    fi
done
cp "$PROJECT_DIR/clips/README.txt" "$DEPLOY_DIR/clips/" 2>/dev/null || true

# Ad-hoc sign (no Developer ID needed to run locally)
echo "Signing..."
codesign --force --sign - "$DEPLOY_DIR/midi_ft_bridge"
[ -f "$DEPLOY_DIR/panel_viewer" ] && codesign --force --sign - "$DEPLOY_DIR/panel_viewer"

# Smoke test
"$DEPLOY_DIR/midi_ft_bridge" --help >/dev/null 2>&1 || true

# Zip
ZIP="$PROJECT_DIR/deploy-macos.zip"
rm -f "$ZIP"
(cd "$PROJECT_DIR" && zip -rq deploy-macos.zip deploy-macos/ -x "*.DS_Store")

SIZE=$(du -sh "$ZIP" | awk '{print $1}')
echo ""
echo "=== Done ==="
echo "  Folder: $DEPLOY_DIR"
echo "  Zip:    $ZIP ($SIZE)"
