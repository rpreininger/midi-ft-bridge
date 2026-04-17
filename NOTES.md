# midi-ft-bridge - Notes

## What This Is

C++ daemon that receives USB MIDI events from a Roland Fantom 06 and streams
mapped MP4 video clips (with audio) to multiple Flaschen-Taschen LED panels
over WiFi, plus an iPixel Color BLE panel. Runs on Pi 4B as WiFi AP, or
natively on macOS for development.

```
  Fantom 06 --USB MIDI--> Pi 4B (WiFi AP "LED-NET")
                              |
                    +---------+---------+---------+
                    |         |         |         |
                 Panel A   Panel B   Panel C   iPixel
                 .10.21    .10.22    .10.20    BLE
                 128x128   128x64   128x64    32x16
```

## Building

### macOS (development)

```bash
# One-time setup:
brew install ffmpeg sdl2 cmake pkg-config

# Build:
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Run (two terminals):
./build/panel_viewer --config config_local.json --scale 4
./build/midi_ft_bridge --config config_local.json --test
```

### Linux native (Pi or WSL)

```bash
sudo apt install libasound2-dev libavformat-dev libavcodec-dev \
    libswscale-dev libswresample-dev libavutil-dev libdbus-1-dev
cmake -B build && cmake --build build
```

### Cross-compile for Pi (aarch64)

```bash
cmake -B build-arm \
    -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build-arm
```

Requires FFmpeg/ALSA/D-Bus ARM libs in `../raspi/libs/`.

## Running

```bash
./midi_ft_bridge --config config.json         # production (MIDI input)
./midi_ft_bridge --config config.json --test  # keyboard test mode (1-9,0,-,=)
```

Status web page at http://localhost:8080

## Architecture

Audio-master A/V sync: audio playback drives the master clock, video frames
are picked to match. Falls back to wall-clock for clips without audio.

```
Decode thread:  demux MP4 → audio packets to AudioPlayer
                           → video frames to 8-frame PTS ring buffer
Audio thread:   ring buffer → ALSA (master clock via sample count)
Main thread:    query audio clock → pick matching video frame → send to panels
```

| File | Purpose |
|------|---------|
| `main.cpp` | Main loop: MIDI poll → get frame from ClipPlayer → send to panels |
| `clip_player.h/cpp` | Audio-master orchestrator, ties video + audio + clock |
| `video_player.h/cpp` | Threaded FFmpeg decoder, 8-frame PTS ring buffer |
| `audio_player.h/cpp` | ALSA PCM output, ring buffer, master clock |
| `midi_input.h/cpp` | ALSA sequencer listener, auto-connects USB MIDI |
| `ft_sender.h/cpp` | UDP PPM sender, tile mode to avoid IP fragmentation |
| `ble_sender.h/cpp` | BlueZ D-Bus BLE sender for iPixel panel |
| `status_server.h/cpp` | HTTP status page with panel state, MIDI log, test buttons |
| `config.h` | JSON config parser (header-only) |

### macOS stubs

On macOS, ALSA/BlueZ are replaced with stubs:
- `midi_input_stub.cpp` — no-op, use --test mode for keyboard input
- `audio_player_stub.cpp` — reports uninitialized, ClipPlayer uses wall clock
- `ble_sender_stub.cpp` — drops frames silently

### Panel viewer (macOS dev tool)

`tools/panel_viewer.cpp` — SDL2 app that listens on UDP, renders all panels
in one window. Use with `config_local.json` (all panels on localhost).

## macOS as Production Platform

macOS can potentially replace the Pi entirely. Remaining work:

1. **CoreBluetooth BLE sender** (`ble_sender_macos.mm`) — native Obj-C++,
   replaces the stub. ~150 lines vs 700-line BlueZ D-Bus version. Same iPixel
   protocol (chunked PNG over GATT write). Would make Python `bt_bridge.py`
   unnecessary on macOS.

2. **CoreMIDI input** — native macOS MIDI for real Fantom USB input.

3. **CoreAudio output** — native audio output, eliminates ALSA underrun
   issues entirely. CoreAudio is rock solid.

4. **Networking** — Pi acts as WiFi AP for panel Pi Zeros. On macOS, use a
   cheap travel router instead (same subnet, panels don't care who the AP is).

## Bluetooth Pixel Panel (iPixel Color)

- **Device**: `LED_BLE_25F1E13D` at `D2:DF:25:F1:E1:3D`, 32x16 pixels
- BLE GATT write UUID: `0000fa02-0000-1000-8000-00805f9b34fb`
- BLE GATT notify UUID: `0000fa03-0000-1000-8000-00805f9b34fb`
- Frames sent as PNG bytes, chunked into 244-byte BLE writes
- 12KB windows with ACK protocol
- Max ~2-5 FPS due to BLE throughput limits

Two implementations exist:
- **C++ native** (`ble_sender.cpp`) — BlueZ D-Bus, runs in-process
- **Python bridge** (`bt_bridge.py`) — receives UDP from C++, forwards via bleak

## Clip Preparation

Pre-encode at full canvas resolution (288x128) with audio:
```bash
ffmpeg -i input.mp4 -vf scale=288:128 -c:v libx264 -profile:v baseline \
    -crf 23 -r 25 -c:a aac -b:a 128k output.mp4
```

## Deployment

```bash
cd deploy
./deploy.sh pi@192.168.10.1 --install   # copy binary + services
./deploy.sh pi@192.168.10.1 --setup     # full setup (packages + services)
./deploy.sh pi@192.168.10.1 --restart   # restart services
```
