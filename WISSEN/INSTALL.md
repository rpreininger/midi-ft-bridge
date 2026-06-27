# midi-ft-bridge - Installation Guide

MIDI to Flaschen-Taschen bridge for the Roland Fantom 06/07/08, running
natively on macOS. Receives USB MIDI note events and streams mapped MP4 video
clips (with audio) to Flaschen-Taschen LED panels and an iPixel Color BLE
panel.

> The earlier Raspberry-Pi-as-hub version is frozen at git tag `raspi-final`
> (`git checkout raspi-final`). This guide covers the macOS build only.

## Hardware

| Device | Role |
|--------|------|
| Mac | Hub: MIDI input, video decode, frame dispatch, BLE |
| GL.iNet Mango router | WiFi AP for the panels (LAN 192.168.10.1/24) |
| Panel A (bigpanel) | FT 128x128, IP 192.168.10.21, MAC 88:a2:9e:75:ff:eb |
| Panel B (ericpanel) | FT 128x64, IP 192.168.10.22, MAC d8:3a:dd:de:93:19 |
| Panel C (ralfpanel) | FT 128x64, IP 192.168.10.20, MAC 2c:cf:67:cd:6c:8c |
| BLE panel (iPixel Color) | BLE 32x16, `LED_BLE_25F1E13D` / D2:DF:25:F1:E1:3D |
| Roland Fantom 06 | USB MIDI input |

The Mac connects to the Mango router over a USB-C Ethernet dongle; the panels
(still Pi Zeros running `ft-server`) join the Mango over WiFi.

## 1. Build & Install the macOS App

The shipping app is the Xcode project in `mac-app/`.

```bash
# One-time: install the project generator
brew install xcodegen

# Generate the .xcodeproj (it is git-ignored, regenerated from project.yml)
xcodegen generate --spec mac-app/project.yml

# Build (Release)
xcodebuild -project mac-app/MidiFtBridge.xcodeproj \
    -scheme MidiFtBridge -configuration Release build
```

Then drag the built `MIDI-FT Bridge.app` into `/Applications`. No Homebrew
libraries are bundled — the app uses only system frameworks.

> **Distributing to another Mac:** the app is ad-hoc signed, so a fresh Mac
> reports it as "damaged". De-quarantine it on that machine:
> `xattr -dr com.apple.quarantine "/Applications/MIDI-FT Bridge.app"`.

## 2. Network (Mango AP)

Configure the Mango as the panel AP at 192.168.10.1/24 and pin each panel to a
fixed IP by DHCP reservation (router admin → DHCP/LAN → Address Reservation),
using the MAC/IP pairs from the hardware table above.

Two helper scripts reduce "one panel reboots → the others jitter" (shared-WiFi
airtime loss):

```bash
# On the router (evicts dead clients fast + pins IPs). Edit the MAC table first.
ssh root@192.168.10.1 'sh -s' < setup/tune-glinet-ap.sh

# On the Mac (static ARP so a panel reboot causes no ARP churn). Edit the table.
./setup/set-static-arp-macos.sh
```

## 3. Panel Setup (each FT panel)

The panels are Pi Zeros running flaschen-taschen `ft-server`. `setup/setup-panel.sh`
configures a panel Pi to auto-join the AP WiFi and run `ft-server`. Run it on
each panel (edit the SSID/passphrase at the top to match your Mango AP):

```bash
sudo bash setup-panel.sh
```

Panel A serves `-D 128x128`; panels B and C serve `-D 128x64`.

## 4. Bluetooth (iPixel BLE panel)

No setup needed beyond granting the app Bluetooth permission on first launch
(Info.plist already declares the usage string). macOS matches the panel by
local name (`ble_name` in config), so no manual pairing.

## 5. Configuration

Edit `config.json` (or use the in-app **Edit Config…** editor):

```json
{
  "panels": [
    { "name": "A_bigpanel", "ip": "192.168.10.21", "port": 1337, "src_x": 0, "src_y": 0, "src_w": 128, "src_h": 128 },
    { "name": "B_ericpanel", "ip": "192.168.10.22", "port": 1337, "src_x": 128, "src_y": 0, "src_w": 128, "src_h": 64 },
    { "name": "C_ralfpanel", "ip": "192.168.10.20", "port": 1337, "src_x": 128, "src_y": 64, "src_w": 128, "src_h": 64 },
    { "name": "BT", "ip": "127.0.0.1", "ble_name": "LED_BLE_25F1E13D", "brightness": 80, "src_x": 256, "src_y": 0, "src_w": 32, "src_h": 16, "max_fps": 10, "type": "ble" }
  ],
  "clips_dir": "./clips",
  "midi_channel": 9,
  "midi_device": "",
  "mappings": [
    { "note": 36, "panel": "all", "clip": "mp4/Fly.mp4" }
  ],
  "default_fps": 25,
  "video_width": 288,
  "video_height": 128,
  "web_port": 8080,
  "debug": 1
}
```

### Key settings

| Field | Description |
|-------|-------------|
| `panels` | FT/BLE panel targets with crop regions |
| `src_x/y/w/h` | Region to extract from the decoded video frame |
| `midi_channel` | MIDI channel filter (0-indexed, 9 = ch10). -1 = any |
| `midi_device` | Preferred MIDI input (name substring). Empty = all sources. Set it in the config editor's device picker |
| `mappings` | MIDI note → video clip mapping |
| `clips_dir` | Directory containing MP4 video clips |
| `video_width/height` | Full video decode resolution (288x128) |
| `type` | Panel type: omit for FT, `"ble"` for native CoreBluetooth BLE |
| `max_fps` | Per-panel FPS throttle (for BLE bandwidth) |
| `web_port` | Status web UI port |

### MIDI note reference (Roland convention, C4 = 60)

| Note | MIDI # | Note | MIDI # |
|------|--------|------|--------|
| C2 | 36 | F#2 | 42 |
| C#2 | 37 | G2 | 43 |
| D2 | 38 | G#2 | 44 |
| D#2 | 39 | A2 | 45 |
| E2 | 40 | A#2 | 46 |
| F2 | 41 | B2 | 47 |

## 6. Testing

- **Live MIDI:** launch the app, connect the Fantom via USB. The connected
  device shows next to the 🎹 icon; notes 36–47 on channel 10 trigger clips.
- **Without MIDI:** the app's "Loop All (Test)" button cycles every mapping;
  individual rows have a "Trigger" button.
- **Status web page:** http://localhost:8080 (also remotely from a phone)

## Troubleshooting

### No MIDI device detected
- Check the device shows in **Audio MIDI Setup → MIDI Studio**.
- In the app's **Edit Config… → General → MIDI input device**, hit the rescan
  button; pick the device (or "All sources").
- Make sure the Fantom is on a USB-A data port (not USB-C power only).

### No image on a panel
- Verify `ft-server` is running on the panel: `ss -ulnp | grep 1337`.
- Ping the panel: `ping 192.168.10.XX`.
- Check the `ft-server` resolution matches config (`-D 128x128` / `-D 128x64`).

### BLE panel not working
- Grant the app Bluetooth permission (System Settings → Privacy → Bluetooth).
- Confirm the panel name matches `ble_name` in config (`LED_BLE_25F1E13D`).
- BLE tops out ~2–5 FPS; keep the panel's `max_fps` low.

### WiFi panels won't connect / jitter
- Check the Mango admin shows the panels with their reserved IPs.
- Re-run `setup/tune-glinet-ap.sh` (router) and `setup/set-static-arp-macos.sh`
  (Mac). Note: macOS static ARP does not survive a reboot — re-run after one.
