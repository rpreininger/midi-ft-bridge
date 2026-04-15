# midi-ft-bridge - Installation Guide

MIDI to Flaschen-Taschen bridge for Roland Fantom 06/07/08.
Receives USB MIDI note events and streams mapped MP4 video clips
to Flaschen-Taschen LED panels and a BLE pixel panel.

## Hardware

| Device | Role |
|--------|------|
| Raspberry Pi 4B | Hub: WiFi AP, MIDI input, video decode, frame dispatch |
| Panel A (bigpanel) | FT 128x128, IP 192.168.10.21 |
| Panel B (ericpanel) | FT 128x64, IP 192.168.10.22 |
| Panel C (ralfpanel) | FT 128x64, IP 192.168.10.20 |
| BLE panel (iPixel Color) | BLE 32x16, via bt_bridge.py on localhost:1340 |
| Roland Fantom 06 | USB MIDI input |

## 1. Fresh Pi 4B Setup

Flash Raspberry Pi OS Bookworm (64-bit) to SD card. Create user `stratojets`.
Plug in Ethernet for internet during setup.

### Set hostname
```bash
sudo hostnamectl set-hostname strato_accesspoint
```

## 2. WiFi Access Point

### Tell NetworkManager to leave wlan0 alone
```bash
sudo tee /etc/NetworkManager/conf.d/unmanaged-wlan0.conf << 'EOF'
[keyfile]
unmanaged-devices=interface-name:wlan0
EOF
sudo systemctl restart NetworkManager
```

### Static IP on boot
```bash
sudo tee /etc/systemd/system/wlan0-static.service << 'EOF'
[Unit]
Description=Static IP for wlan0
Before=hostapd.service dnsmasq.service
After=sys-subsystem-net-devices-wlan0.device

[Service]
Type=oneshot
ExecStart=/sbin/ip addr add 192.168.10.1/24 dev wlan0
ExecStart=/sbin/ip link set wlan0 up
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF
sudo systemctl daemon-reload
sudo systemctl enable wlan0-static
```

### Install and configure hostapd
```bash
sudo apt install -y hostapd
sudo tee /etc/hostapd/hostapd.conf << 'EOF'
interface=wlan0
driver=nl80211
ssid=strato_accesspoint
hw_mode=g
channel=6
macaddr_acl=0
auth_algs=1
ignore_broadcast_ssid=0
wpa=2
wpa_passphrase=YOUR_PASSWORD
wpa_key_mgmt=WPA-PSK
rsn_pairwise=CCMP
EOF
sudo systemctl unmask hostapd
sudo systemctl enable hostapd
```

### Install and configure dnsmasq (DHCP)
```bash
sudo apt install -y dnsmasq
sudo tee /etc/dnsmasq.conf << 'EOF'
interface=wlan0
bind-interfaces
dhcp-range=192.168.10.50,192.168.10.100,255.255.255.0,24h
domain-needed
bogus-priv
dhcp-host=88:a2:9e:75:ff:eb,192.168.10.21,bigpanel
dhcp-host=d8:3a:dd:de:93:19,192.168.10.22,ericpanel
dhcp-host=2c:cf:67:cd:6c:8c,192.168.10.20,ralfpanel
EOF
sudo systemctl enable dnsmasq
```

### Disable WiFi power save
```bash
sudo tee /etc/systemd/system/wifi-powersave-off.service << 'EOF'
[Unit]
Description=Disable WiFi power save
After=wlan0-static.service

[Service]
Type=oneshot
ExecStart=/sbin/iw wlan0 set power_save off
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF
sudo systemctl daemon-reload
sudo systemctl enable wifi-powersave-off
```

### Reboot and verify
```bash
sudo reboot
# After reboot:
ip addr show wlan0              # should show 192.168.10.1
sudo systemctl status hostapd   # active (running)
sudo systemctl status dnsmasq   # active (running)
iw wlan0 get power_save         # Power save: off
```

## 3. Install Dependencies

```bash
sudo apt update && sudo apt install -y \
    ffmpeg libasound2-dev libavformat-dev libavcodec-dev \
    libswscale-dev libavutil-dev libdbus-1-dev \
    python3-full python3-venv bluetooth bluez
```

## 4. Bluetooth Setup (iPixel BLE panel)

```bash
sudo rfkill unblock bluetooth
sudo systemctl enable bluetooth
sudo systemctl start bluetooth

# Trust the BLE panel (no pairing needed)
sudo bluetoothctl
> trust D2:DF:25:F1:E1:3D
> exit
```

### Python venv for bt_bridge.py

Bookworm requires a venv for pip packages:
```bash
sudo python3 -m venv /opt/bt-bridge-venv
sudo /opt/bt-bridge-venv/bin/pip install bleak Pillow
```

## 5. Deploy midi-ft-bridge

### Option A: Deploy script (from dev machine)

Requires SSH key auth: `ssh-copy-id stratojets@192.168.10.1`

```bash
# First time (packages + venv + services):
bash deploy/deploy.sh stratojets@192.168.10.1 --setup

# Update binary, config, clips only:
bash deploy/deploy.sh stratojets@192.168.10.1

# Update and restart services:
bash deploy/deploy.sh stratojets@192.168.10.1 --restart
```

### Option B: Manual

```bash
# On the Pi:
mkdir -p /home/stratojets/midi-ft-bridge/clips

# From dev machine:
scp build-aarch64/midi_ft_bridge stratojets@192.168.10.1:/home/stratojets/midi-ft-bridge/
scp config.json stratojets@192.168.10.1:/home/stratojets/midi-ft-bridge/
scp bt_bridge.py stratojets@192.168.10.1:/home/stratojets/midi-ft-bridge/
scp clips/*.mp4 stratojets@192.168.10.1:/home/stratojets/midi-ft-bridge/clips/
ssh stratojets@192.168.10.1 "chmod +x ~/midi-ft-bridge/midi_ft_bridge"
```

## 6. Systemd Services

### midi-ft-bridge (main video bridge)
```bash
sudo cp midi-ft-bridge.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable midi-ft-bridge
sudo systemctl start midi-ft-bridge
```

### bt-bridge (BLE panel bridge)
```bash
sudo cp bt-bridge.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable bt-bridge
sudo systemctl start bt-bridge
```

The bt-bridge service uses `/opt/bt-bridge-venv/bin/python3` and starts after
bluetooth and midi-ft-bridge, with a 5-second delay for BLE init.

## 7. Panel Setup

Each FT panel needs ft-server running. Connect each panel to the AP first:
```bash
nmcli --ask device wifi connect "strato_accesspoint"
```

### Panel A (bigpanel) - 128x128
```bash
sudo tee /etc/systemd/system/ft-server.service << 'EOF'
[Unit]
Description=Flaschen-Taschen Server
After=network.target

[Service]
Type=simple
ExecStart=/home/bigpanel/dev/flaschen-taschen/server/ft-server -D 128x128
WorkingDirectory=/home/bigpanel/dev/flaschen-taschen/server
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF
sudo systemctl daemon-reload
sudo systemctl enable ft-server
sudo systemctl start ft-server
```

### Panel B (ericpanel) and Panel C (ralfpanel) - 128x64
Same as above but with `-D 128x64` and adjust the ExecStart path to match
each panel's ft-server binary location.

## 8. Building from Source

### Cross-compile on PC (WSL)

```bash
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu

mkdir -p build-aarch64 && cd build-aarch64
cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/aarch64-toolchain.cmake ..
cmake --build .
```

FFmpeg headers and libraries must be available in `libs/` (copied from the Pi).

### Native build (on Pi)

```bash
mkdir -p build && cd build
cmake ..
cmake --build .
```

## 9. Configuration

Edit `config.json`:

```json
{
  "panels": [
    { "name": "A_bigpanel", "ip": "192.168.10.21", "port": 1337, "src_x": 0, "src_y": 0, "src_w": 128, "src_h": 128 },
    { "name": "B_ericpanel", "ip": "192.168.10.22", "port": 1337, "src_x": 128, "src_y": 0, "src_w": 128, "src_h": 64 },
    { "name": "C_ralfpanel", "ip": "192.168.10.20", "port": 1337, "src_x": 128, "src_y": 64, "src_w": 128, "src_h": 64 },
    { "name": "BT", "ip": "127.0.0.1", "port": 1340, "brightness": 80, "src_x": 256, "src_y": 0, "src_w": 32, "src_h": 16, "max_fps": 10, "type": "ble_udp" }
  ],
  "clips_dir": "./clips",
  "midi_channel": 9,
  "mappings": [
    { "note": 36, "panel": "all", "clip": "1.INTRO.mp4" }
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
| `src_x/y/w/h` | Region to extract from decoded video frame |
| `midi_channel` | MIDI channel filter (0-indexed, 9 = ch10). -1 = any |
| `mappings` | MIDI note to video clip mapping |
| `clips_dir` | Directory containing MP4 video clips |
| `video_width/height` | Full video decode resolution (288x128) |
| `type` | Panel type: omit for FT, `"ble_udp"` for BLE via UDP bridge |
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

## 10. Testing

### Keyboard test (no MIDI needed)
```bash
sudo systemctl stop midi-ft-bridge
cd /home/stratojets/midi-ft-bridge
sudo ./midi_ft_bridge --test --config config.json
```

| Key | Clip | Key | Clip |
|-----|------|-----|------|
| 1 | 1.INTRO.mp4 | 7 | 7.MAGNETO.mp4 |
| 2 | 2.FRF.mp4 | 8 | 8.JIFFY.mp4 |
| 3 | 3.MOON.mp4 | 9 | 10.SHIN.mp4 |
| 4 | 4.PIXL.mp4 | 0 | 11.SHRINK.mp4 |
| 5 | 5.TENNIS.mp4 | - | 12.AXEL.mp4 |
| 6 | 6.NEU.mp4 | = | AXEL START.mp4 |
| Q | Quit | | |

### MIDI test (live)
```bash
sudo systemctl start midi-ft-bridge
```
Connect Roland Fantom 06 via USB. Notes 36-47 on channel 10 trigger clips.

### Check services
```bash
sudo systemctl status midi-ft-bridge
sudo systemctl status bt-bridge
journalctl -u midi-ft-bridge --no-pager -n 20
journalctl -u bt-bridge --no-pager -n 20
```

### Verify network
```bash
cat /var/lib/misc/dnsmasq.leases
iw dev wlan0 station dump
ping -c 3 192.168.10.20  # ralfpanel
ping -c 3 192.168.10.21  # bigpanel
ping -c 3 192.168.10.22  # ericpanel
```

### Web status page
http://192.168.10.1:8080

## Troubleshooting

### No MIDI device detected
- Check USB connection: `lsusb` should show Roland device
- Check ALSA: `aconnect -l` should list the Fantom
- Make sure MIDI is plugged into a USB-A port (not USB-C power)

### No image on a panel
- Verify ft-server is running on the panel: `ss -ulnp | grep 1337`
- Ping the panel from the Pi: `ping 192.168.10.XX`
- Check ft-server resolution matches config (`-D 128x128` or `-D 128x64`)

### BLE panel not working
- Check bt-bridge service: `sudo systemctl status bt-bridge`
- Verify Bluetooth is on: `bluetoothctl show` (Powered: yes)
- If blocked: `sudo rfkill unblock bluetooth`
- Check UDP: `ss -ulnp | grep 1340`

### WiFi panels won't connect
- Check hostapd: `sudo systemctl status hostapd`
- Check dnsmasq: `sudo systemctl status dnsmasq`
- Verify wlan0 has IP: `ip addr show wlan0`
- Check leases: `cat /var/lib/misc/dnsmasq.leases`

### Service won't start
- Check logs: `sudo journalctl -u midi-ft-bridge -n 50`
- Verify binary: `ls -la ~/midi-ft-bridge/midi_ft_bridge`
- Verify config: `cat ~/midi-ft-bridge/config.json`
- Test manually first: `sudo ./midi_ft_bridge --config config.json --test`
