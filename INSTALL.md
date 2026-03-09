# midi-ft-bridge - Installation Guide

MIDI to Flaschen-Taschen bridge for Roland Fantom 06/07/08.
Receives USB MIDI note events and streams mapped MP4 video clips
to Flaschen-Taschen LED panels over UDP.

## Network Setup

| Device | IP | Role |
|--------|-----|------|
| MIDI Bridge Pi | 192.168.10.1 | Runs `midi_ft_bridge`, USB connected to Fantom |
| FT Server Pi | 192.168.10.20 | Runs `audio_led` with FT server on UDP port 1337 |

## Prerequisites

### MIDI Bridge Pi (192.168.10.1)

```bash
sudo apt install libavformat-dev libavcodec-dev libswscale-dev libavutil-dev libasound2-dev
```

If the Pi has no internet, download the `-dev` .deb packages from
https://packages.debian.org (match your Debian version) and install with:

```bash
sudo dpkg -i --force-depends libavutil-dev_*.deb libavcodec-dev_*.deb \
    libavformat-dev_*.deb libswscale-dev_*.deb
```

### FT Server Pi (192.168.10.20)

The `audio_led` binary must be deployed and running with FT mode enabled (default).

## Building

### Cross-compile on PC (WSL)

Requires `aarch64-linux-gnu-gcc` cross-compiler in WSL:

```bash
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
```

FFmpeg headers and libraries must be copied from the Pi to the local `libs/` directory:

- Headers: `libs/ffmpeg/libavformat/`, `libs/ffmpeg/libavcodec/`, etc.
- Libraries: `libs/aarch64/libavformat.so`, `libs/aarch64/libavcodec.so`, etc.

```bash
# Configure (one time)
mkdir -p build-arm && cd build-arm
cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/aarch64-toolchain.cmake ..

# Build
cmake --build .
```

### Native build (on Pi or WSL)

```bash
mkdir -p build-native && cd build-native
cmake ..
cmake --build .
```

## Deployment

### 1. Transfer files to MIDI Bridge Pi

Via SFTP:

```bash
sftp sftpuser@192.168.10.1
mkdir midi-ft-bridge
mkdir midi-ft-bridge/clips
cd midi-ft-bridge
put build-arm/midi_ft_bridge
put config.json
put midi-ft-bridge.service
cd clips
put clips/*.mp4
bye
```

### 2. Set permissions

```bash
ssh stratojets@192.168.10.1
chmod +x ~/midi-ft-bridge/midi_ft_bridge
```

### 3. Test manually

Connect the Roland Fantom via USB (MIDI-to-USB cable or direct USB-B).

```bash
# Verify MIDI device is detected
lsusb                # Should show "Roland Corp. FANTOM-06"
aconnect -l          # Should list the Fantom as a client

# Run in test mode (keyboard triggers clips)
cd ~/midi-ft-bridge
sudo ./midi_ft_bridge --config config.json --test
```

Test mode key mapping:

| Key | Clip |
|-----|------|
| 1 / F1 | 1.INTRO.mp4 |
| 2 / F2 | 2.FRF.mp4 |
| 3 / F3 | 3.MOON.mp4 |
| 4 / F4 | 4.PIXL.mp4 |
| 5 / F5 | 5.TENNIS.mp4 |
| 6 / F6 | 6.NEU.mp4 |
| 7 / F7 | 7.MAGNETO.mp4 |
| 8 / F8 | 8.JIFFY.mp4 |
| 9 / F9 | 10.SHIN.mp4 |
| 0 / F10 | 11.SHRINK.mp4 |
| - / F11 | 12.AXEL.mp4 |
| = / F12 | AXEL START.mp4 |

Press Q to quit test mode.

### 4. Install systemd service

```bash
ssh stratojets@192.168.10.1
sudo cp ~/midi-ft-bridge/midi-ft-bridge.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable midi-ft-bridge
sudo systemctl start midi-ft-bridge
```

## Service Management

```bash
# Check status
sudo systemctl status midi-ft-bridge

# View logs
sudo journalctl -u midi-ft-bridge -f

# Restart after config change
sudo systemctl restart midi-ft-bridge

# Stop
sudo systemctl stop midi-ft-bridge

# Disable autostart
sudo systemctl disable midi-ft-bridge
```

## Configuration

Edit `config.json` on the Pi at `~/midi-ft-bridge/config.json`:

```json
{
  "panels": [
    { "name": "A", "ip": "192.168.10.20", "port": 1337 }
  ],
  "clips_dir": "./clips",
  "midi_channel": 9,
  "mappings": [
    { "note": 36, "panel": "all", "clip": "1.INTRO.mp4" },
    { "note": 37, "panel": "all", "clip": "2.FRF.mp4" }
  ],
  "default_fps": 25,
  "width": 128,
  "height": 64,
  "web_port": 8080
}
```

### Key settings

| Field | Description |
|-------|-------------|
| `panels` | FT server targets (IP and port) |
| `midi_channel` | MIDI channel filter, 0-indexed. 9 = channel 10. Set to -1 for any channel |
| `mappings` | MIDI note to video clip mapping. `"panel": "all"` sends to all panels |
| `clips_dir` | Directory containing MP4 video clips |
| `width` / `height` | Output resolution (must match LED panel) |
| `web_port` | Status web UI port |

### MIDI note reference (Roland convention, C4 = 60)

| Note | MIDI # | Note | MIDI # |
|------|--------|------|--------|
| C1 | 24 | C2 | 36 |
| C#1 | 25 | C#2 | 37 |
| D1 | 26 | D2 | 38 |
| D#1 | 27 | D#2 | 39 |
| E1 | 28 | E2 | 40 |
| F1 | 29 | F2 | 41 |
| F#1 | 30 | F#2 | 42 |
| G1 | 31 | G2 | 43 |
| G#1 | 32 | G#2 | 44 |
| A1 | 33 | A2 | 45 |
| A#1 | 34 | A#2 | 46 |
| B1 | 35 | B2 | 47 |

## Status Web UI

Access at `http://192.168.10.1:8080` to view:
- Connected MIDI device
- Panel status and frame counters
- MIDI event log (note, channel, velocity)
- Test buttons to trigger clips from the browser

## BLE Pixel Panel (iPixel Color)

The BT panel region (32x16 pixels at x=256, y=0 in the 288x128 video) is sent
via UDP to a Python bridge daemon that forwards frames over Bluetooth Low Energy.

### Prerequisites

```bash
# Bluetooth stack
sudo apt install -y bluetooth bluez

# Python + BLE library
sudo apt install -y python3 python3-pip python3-pillow
pip3 install bleak
```

### Deploy

```bash
# Copy bridge script
scp bt_bridge.py stratojets@192.168.10.1:~/midi-ft-bridge/
scp bt-bridge.service stratojets@192.168.10.1:~/midi-ft-bridge/
```

### Pair the panel (one-time)

Power on the iPixel Color panel, then:

```bash
sudo bluetoothctl
  power on
  scan on
  # Wait for "LED_BLE_25F1E13D" to appear
  trust D2:DF:25:F1:E1:3D
  scan off
  exit
```

### Test manually

```bash
# Terminal 1: run the main bridge (sends BT region to UDP 1340)
cd ~/midi-ft-bridge
sudo ./midi_ft_bridge --config config.json --test

# Terminal 2: run the BLE bridge
python3 bt_bridge.py --addr D2:DF:25:F1:E1:3D --port 1340 --brightness 80
```

### Install systemd service

```bash
sudo cp ~/midi-ft-bridge/bt-bridge.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable bt-bridge
sudo systemctl start bt-bridge
```

The service starts after `midi-ft-bridge` and `bluetooth`, with a 5-second delay
to let BLE initialize.

### BLE service management

```bash
sudo systemctl status bt-bridge
sudo journalctl -u bt-bridge -f
sudo systemctl restart bt-bridge
```

### Known limitations

- Pi Zero 2 W shares one radio for WiFi and BLE — expect ~3-5 FPS on the BLE panel
- BLE range is limited (~10m), keep the panel close to the Pi
- If the panel disconnects, the bridge auto-reconnects after 3 seconds

## Troubleshooting

### No MIDI device detected
- Check USB connection: `lsusb` should show Roland device
- Check ALSA: `aconnect -l` should list the Fantom
- Pi Zero 2 W: use the inner micro-USB port (data), not outer (power only)
- USB OTG adapter required for full-size USB-A plugs

### Clips not showing on panel
- Verify FT server is reachable: `ping 192.168.10.20`
- Verify audio_led is running in FT mode on the panel Pi
- Check status page at http://192.168.10.1:8080 for frame counters
- Video frames are sent as row-strip tiles (3 rows per UDP packet) to avoid IP fragmentation

### Service won't start
- Check logs: `sudo journalctl -u midi-ft-bridge -n 50`
- Verify binary exists: `ls -la ~/midi-ft-bridge/midi_ft_bridge`
- Verify config: `cat ~/midi-ft-bridge/config.json`
- Test manually first: `sudo ./midi_ft_bridge --config config.json --test`
