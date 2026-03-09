# midi-ft-bridge - Notes

## What This Is

Lightweight Pi Zero app that receives USB MIDI events from a Roland Fantom 06
and streams mapped MP4 video clips to multiple Flaschen-Taschen LED panel Raspis
over WiFi. The Pi Zero also acts as the WiFi access point (hotspot).

```
  Fantom 06 --USB MIDI--> Pi Zero (WiFi AP "LED-NET")
                              |
                    +---------+---------+
                    |         |         |
                 Panel A   Panel B   Panel C
                 .4.101    .4.102    .4.103
                 128x64    128x64    128x64
```

## Building

Native build (for testing on WSL):
```bash
# In WSL:
cd /mnt/d/Developer/C++/raspi/midi-ft-bridge
mkdir build-native && cd build-native
cmake .. && make -j$(nproc)
```

Cross-compile for Pi Zero 2 W (aarch64):
```bash
mkdir build-arm && cd build-arm
cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/aarch64-toolchain.cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

Cross-compile for Pi Zero v1 (armv6):
```bash
mkdir build-arm && cd build-arm
cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/arm-toolchain.cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

### Dependencies (native / WSL)

```bash
sudo apt install libasound2-dev libavformat-dev libavcodec-dev libswscale-dev libavutil-dev
```

### Dependencies (cross-compile)

FFmpeg ARM libs need to be copied from a Pi:
```bash
# On the Pi:
sudo apt install libavformat-dev libavcodec-dev libswscale-dev libavutil-dev

# Then copy to build machine:
scp pi@<ip>:/usr/lib/aarch64-linux-gnu/libav*.so*   ../libs/aarch64/
scp pi@<ip>:/usr/lib/aarch64-linux-gnu/libsw*.so*   ../libs/aarch64/
scp -r pi@<ip>:/usr/include/aarch64-linux-gnu/libav* ../libs/ffmpeg/
scp -r pi@<ip>:/usr/include/aarch64-linux-gnu/libsw* ../libs/ffmpeg/
```

ALSA headers are already in `raspi/libs/alsa/` from the parent project.

## Running

```bash
./midi_ft_bridge --config config.json
```

Status web page at http://localhost:8080 (or http://192.168.4.1:8080 on the AP).

## Config

Edit `config.json` to map MIDI notes to panels and clips:
- `"panel": "A"` sends to one panel
- `"panel": "all"` sends to all panels
- Clips are MP4 files in the `clips/` directory

## Clip Preparation

Pre-encode videos at panel resolution for best Pi Zero performance:
```bash
ffmpeg -i input.mp4 -vf scale=128:64 -c:v libx264 -profile:v baseline -crf 23 -r 25 output.mp4
```

## Deployment

```bash
# Copy binary + config + clips to Pi Zero:
cd deploy
./deploy.sh pi@192.168.4.1 --install

# Or manually:
scp build-arm/midi_ft_bridge pi@192.168.4.1:~/midi-ft-bridge/
scp config.json pi@192.168.4.1:~/midi-ft-bridge/
scp clips/*.mp4 pi@192.168.4.1:~/midi-ft-bridge/clips/
```

## Setup

### Pi Zero (MIDI receiver + WiFi AP)

```bash
sudo bash setup/setup-ap.sh
```

Creates WiFi hotspot "LED-NET" (password: `ledpanels`), DHCP on 192.168.4.100-120.

### Panel Raspis (FT server + LED matrix)

```bash
sudo bash setup/setup-panel.sh
```

Connects to LED-NET, runs `audio_led --ft-mode` to receive UDP frames.

## Architecture

| File | Purpose |
|------|---------|
| `main.cpp` | Main loop: MIDI poll → clip lookup → video decode → FT send |
| `midi_input.h/cpp` | ALSA sequencer listener, auto-connects USB MIDI, threaded |
| `video_player.h/cpp` | FFmpeg MP4 → RGB24 decoder, single-thread for Pi Zero |
| `ft_sender.h/cpp` | UDP PPM sender (one per panel), pre-allocated buffer |
| `status_server.h/cpp` | HTTP status page with panel state, MIDI log, test buttons |
| `config.h` | JSON config parser (header-only) |

## Bluetooth Pixel Panel (iPixel Color)

Plan to add an iPixel Color BLE LED panel as an additional output target alongside the
existing Flaschen-Taschen UDP panels.

Library: [pypixelcolor](https://github.com/lucagoc/pypixelcolor) (Python, BLE via `bleak`)

### Device Info

- Resolution varies by model (32x32, 64x64, 128x32, 96x16, etc.) — auto-detected on connect
- BLE GATT write UUID: `0000fa02-0000-1000-8000-00805f9b34fb`
- BLE GATT notify UUID: `0000fa03-0000-1000-8000-00805f9b34fb`
- Frames sent as **PNG file bytes** (not raw RGB), chunked into 244-byte BLE writes
- 12KB windows with ACK protocol (8s timeout per window)
- Dependencies: `bleak`, `Pillow`

### Key API

```python
from pypixelcolor import Client
client = Client("XX:XX:XX:XX:XX:XX")
client.connect()
client.send_image("frame.png")           # file path
client.send_image_hex(png_hex, ".png")   # hex-encoded PNG bytes
client.set_pixel(x, y, "FF0000")         # single pixel (no ACK, slow for full frames)
client.set_brightness(80)
client.disconnect()
```

To send a raw RGB buffer:
```python
from PIL import Image
from io import BytesIO
img = Image.frombytes("RGB", (w, h), rgb_data)
buf = BytesIO()
img.save(buf, format="PNG")
client.send_image_hex(buf.getvalue().hex(), ".png")
```

### Integration Plan

**Recommended approach: Python bridge daemon** (Option B)

The C++ `midi_ft_bridge` already decodes video to RGB24 frames and sends them to FT panels
via UDP. A small Python script runs alongside it, receives RGB frames on a local UDP socket,
converts to PNG, and forwards to the BLE panel via pypixelcolor.

```
main.cpp (C++)                    bt_bridge.py (Python)
  |                                  |
  |-- UDP RGB frame (loopback) ----> | receive RGB
  |                                  | RGB -> PNG (Pillow)
  |                                  | send_image_hex() via BLE
  |                                  v
  |                              iPixel Color panel
```

**Why Option B (Python bridge) over alternatives:**

| Option | Pros | Cons |
|--------|------|------|
| A: Subprocess per frame | Simple | Fork overhead, latency, no connection reuse |
| **B: Python bridge daemon** | **Persistent BLE connection, low latency, clean separation** | **Extra process to manage** |
| C: Native C++ BLE (BlueZ/D-Bus) | No Python dependency | Complex, re-implements pypixelcolor protocol |

**Implementation steps:**
1. Write `bt_bridge.py` — listens on UDP localhost:1338, connects to iPixel via BLE
2. Add a BLE panel type in `config.json` (e.g. `"type": "ble"`)
3. In `main.cpp`, send RGB frames to `bt_bridge.py` via localhost UDP alongside FT senders
4. Handle frame rate — BLE is slower than UDP; drop frames if BLE can't keep up
5. Clip prep: resize to device resolution (e.g. 64x64) before PNG encoding

**Concerns:**
- BLE throughput is limited (~20-50 KB/s effective). At 64x64 PNG (~5-10KB), expect ~2-5 FPS max
- Pi Zero BLE and WiFi share the same radio — possible interference
- Need to test BLE range and reliability during WiFi AP operation

## TODO

- [ ] Get FFmpeg ARM .so files for cross-compilation
- [ ] Test with actual Fantom 06 MIDI output
- [ ] Prepare test MP4 clips at 128x64
- [ ] Set up Pi Zero as WiFi AP
- [ ] Set up panel Raspis with FT server
- [ ] Integration test: pad hit → clip on panel
- [ ] Consider clip caching (keep decoded frames in memory for short clips)
- [ ] Consider Note Off → stop clip early (currently plays to end)
- [ ] Write `bt_bridge.py` for iPixel Color BLE panel
- [ ] Test BLE throughput / achievable FPS on Pi Zero
- [ ] Test BLE + WiFi AP coexistence on Pi Zero
