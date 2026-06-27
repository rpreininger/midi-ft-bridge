# midi-ft-bridge - Notes

## What This Is

A native macOS app that receives USB MIDI events from a Roland Fantom 06 and
streams mapped MP4 video clips (with audio) to multiple Flaschen-Taschen LED
panels over WiFi, plus an iPixel Color BLE panel.

```
  Fantom 06 --USB MIDI--> Mac ---wired--> GL.iNet Mango (WiFi AP, 192.168.10.1)
                                                |
                              +---------+-------+-+---------+
                              |         |         |         |
                           Panel A   Panel B   Panel C   iPixel
                           .10.21    .10.22    .10.20    BLE
                           128x128   128x64    128x64    32x16
```

The Mac drives everything natively — AVFoundation decode, CoreAudio output,
CoreMIDI input, and CoreBluetooth for the BLE panel. The LED panels themselves
are still Pi Zeros running flaschen-taschen `ft-server`.

> The earlier Raspberry-Pi-as-hub build (ALSA / FFmpeg / BlueZ, plus the
> Python `bt_bridge.py` BLE forwarder) is frozen at git tag **`raspi-final`**.
> Restore it with `git checkout raspi-final`.

## Building

### macOS app (the deliverable)

Open the Xcode project and build the `MidiFtBridge` scheme (Release):

```bash
xcodegen generate --spec mac-app/project.yml          # regenerate the .xcodeproj
xcodebuild -project mac-app/MidiFtBridge.xcodeproj \
    -scheme MidiFtBridge -configuration Release build
```

The Xcode build adds real CoreBluetooth BLE output. No Homebrew dependencies —
only system frameworks — so the `.app` is self-contained.

### Headless CLI (optional)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/midi_ft_bridge --config config.json --test
```

The CLI build uses the BLE *stub* (CoreBluetooth needs an app bundle for the
Bluetooth permission prompt), so BLE output only works from the `.app`.

`deploy/build-macos.sh` packages the CLI binary + clips into a zip.

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
Decode thread:  AVAssetReader → audio samples to the audio output
                              → video frames to a PTS ring buffer
Audio thread:   ring buffer → CoreAudio (master clock via sample count)
Main thread:    query audio clock → pick matching video frame → send to panels
```

| File | Purpose |
|------|---------|
| `main.cpp` | CLI entry: MIDI poll → get frame from ClipPlayer → send to panels |
| `engine.cpp/h` | Orchestrates clip playback, panels, MIDI, status server (used by the app) |
| `clip_player.h` | Player interface; macOS impl is the AVFoundation pimpl |
| `clip_player_macos.mm` | AVFoundation decode + audio-master sync |
| `audio_output_macos.mm/.h` | CoreAudio (AudioQueue) output, ring buffer, master clock |
| `audio_player_macos_native.cpp` | Audio-master orchestration glue |
| `midi_input_macos.cpp` | CoreMIDI listener; enumerates + connects USB MIDI sources |
| `ft_sender.cpp/h` | UDP PPM sender, tile mode to avoid IP fragmentation |
| `ble_sender_macos.mm` | CoreBluetooth BLE sender for the iPixel panel |
| `ble_sender_stub.cpp` | No-op BLE for the headless CLI build |
| `status_server.cpp/h` | HTTP status page with panel state, MIDI log, test buttons |
| `config.h` | JSON config parser (header-only) |

The Swift app in `mac-app/` wraps the C++ engine through `EngineBridge.mm`
(Obj-C++ facade) and adds the GUI: preview, transport, config editor, panel
status, and the MIDI device picker.

### Panel viewer (dev tool)

`tools/panel_viewer.cpp` — optional SDL2 app that listens on UDP and renders
all panels in one window. Point its config at localhost panels to preview
without hardware. Built by CMake only if SDL2 is installed.

## Bluetooth Pixel Panel (iPixel Color)

- **Device**: `LED_BLE_25F1E13D` at `D2:DF:25:F1:E1:3D`, 32x16 pixels
- BLE GATT write UUID: `0000fa02-0000-1000-8000-00805f9b34fb`
- BLE GATT notify UUID: `0000fa03-0000-1000-8000-00805f9b34fb`
- Frames sent as PNG bytes, chunked into 244-byte BLE writes
- 12KB windows with ACK protocol
- Max ~2-5 FPS due to BLE throughput limits
- macOS matches the device by local name (config `ble_name`), via CoreBluetooth

## Networking

The panels share one WiFi AP (a GL.iNet Mango travel router at 192.168.10.1),
with the Mac wired in over a USB-C Ethernet dongle. Panels get fixed IPs by
DHCP reservation. See `setup/` for the AP tuning and static-ARP helpers that
keep a rebooting panel from jittering the others.

## Clip Preparation

Pre-encode at full canvas resolution (288x128) with audio:
```bash
ffmpeg -i input.mp4 -vf scale=288:128 -c:v libx264 -profile:v baseline \
    -crf 23 -r 25 -c:a aac -b:a 128k output.mp4
```
(FFmpeg is only used here, offline, to prepare clips — the app itself does not
depend on it.)

## Configuration

See `INSTALL.md` for the full `config.json` reference, panel table, and MIDI
note mapping.
