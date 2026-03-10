#!/bin/bash
#
# Full Pi Zero setup for midi-ft-bridge
# Run this WHILE the Pi still has internet (before switching to AP mode).
#
# Usage:
#   sudo bash setup/setup-pizero.sh
#
# What it does:
#   1. System packages (FFmpeg libs, ALSA, Bluetooth, Python)
#   2. Python packages for BLE bridge (bleak, Pillow)
#   3. Deploy directory structure
#   4. systemd services (midi-ft-bridge + bt-bridge)
#   5. Bluetooth setup (power on, trust iPixel panel)
#   6. WiFi AP setup (LED-NET hotspot)
#
# After reboot the Pi will be an AP with no internet.
# Everything needed is pre-installed.

set -e

# ---- Configuration ----
SSID="GIGAHORST"
PASSPHRASE="horstkrause"
AP_IP="192.168.4.1"
DHCP_START="192.168.4.100"
DHCP_END="192.168.4.120"
INTERFACE="wlan0"
BLE_ADDR="D2:DF:25:F1:E1:3D"
DEPLOY_DIR="/home/$(logname)/midi-ft-bridge"

echo "============================================"
echo "  midi-ft-bridge Pi Zero Setup"
echo "============================================"
echo ""
echo "Deploy dir:  $DEPLOY_DIR"
echo "WiFi SSID:   $SSID"
echo "AP IP:       $AP_IP"
echo "BLE device:  $BLE_ADDR"
echo ""
echo "Make sure the Pi has internet access!"
echo "Press Enter to continue or Ctrl+C to cancel..."
read -r

# ---- Step 1: System packages ----
echo ""
echo "[1/6] Installing system packages..."
apt-get update -qq
apt-get install -y \
    libavformat-dev libavcodec-dev libswscale-dev libavutil-dev \
    libasound2-dev \
    bluetooth bluez \
    python3 python3-pip python3-pillow \
    hostapd dnsmasq

echo "  System packages installed."

# ---- Step 2: Python packages ----
echo ""
echo "[2/6] Installing Python packages (bleak)..."
# Use --break-system-packages for newer Debian/Bookworm that enforces PEP 668
pip3 install --break-system-packages bleak 2>/dev/null || pip3 install bleak

echo "  Python packages installed."

# Verify
echo "  Verifying..."
python3 -c "import bleak; print(f'  bleak {bleak.__version__}')"
python3 -c "from PIL import Image; print(f'  Pillow OK')"

# ---- Step 3: Deploy directory ----
echo ""
echo "[3/6] Creating deploy directory..."
REAL_USER=$(logname)
mkdir -p "$DEPLOY_DIR/clips"
mkdir -p "$DEPLOY_DIR/media"
chown -R "$REAL_USER:$REAL_USER" "$DEPLOY_DIR"
echo "  $DEPLOY_DIR created."

# ---- Step 4: systemd services ----
echo ""
echo "[4/6] Installing systemd services..."

# Main midi-ft-bridge service
cat > /etc/systemd/system/midi-ft-bridge.service << EOF
[Unit]
Description=MIDI to Flaschen-Taschen Bridge
After=network.target sound.target

[Service]
Type=simple
User=$REAL_USER
WorkingDirectory=$DEPLOY_DIR
ExecStart=$DEPLOY_DIR/midi_ft_bridge --config config.json
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
EOF

# BLE bridge service
cat > /etc/systemd/system/bt-bridge.service << EOF
[Unit]
Description=BLE Pixel Panel Bridge
After=bluetooth.target midi-ft-bridge.service
Wants=bluetooth.target

[Service]
Type=simple
User=$REAL_USER
WorkingDirectory=$DEPLOY_DIR
ExecStartPre=/bin/sleep 5
ExecStart=/usr/bin/python3 $DEPLOY_DIR/bt_bridge.py --addr $BLE_ADDR --port 1340 --brightness 80
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable midi-ft-bridge
systemctl enable bt-bridge
echo "  Services installed and enabled."

# ---- Step 5: Bluetooth setup ----
echo ""
echo "[5/6] Setting up Bluetooth..."
systemctl enable bluetooth
systemctl start bluetooth

# Power on BT and trust the iPixel panel
bluetoothctl power on 2>/dev/null || true
bluetoothctl trust "$BLE_ADDR" 2>/dev/null || true
echo "  Bluetooth enabled, iPixel panel trusted."

# ---- Step 6: WiFi AP setup ----
echo ""
echo "[6/6] Configuring WiFi Access Point..."

# Stop services during config
systemctl stop hostapd 2>/dev/null || true
systemctl stop dnsmasq 2>/dev/null || true

# Static IP for wlan0
if ! grep -q "midi-ft-bridge AP config" /etc/dhcpcd.conf 2>/dev/null; then
    cat >> /etc/dhcpcd.conf << EOF

# midi-ft-bridge AP config
interface $INTERFACE
    static ip_address=$AP_IP/24
    nohook wpa_supplicant
EOF
fi

# hostapd config
cat > /etc/hostapd/hostapd.conf << EOF
interface=$INTERFACE
driver=nl80211
ssid=$SSID
hw_mode=g
channel=7
wmm_enabled=0
macaddr_acl=0
auth_algs=1
ignore_broadcast_ssid=0
wpa=2
wpa_passphrase=$PASSPHRASE
wpa_key_mgmt=WPA-PSK
wpa_pairwise=TKIP
rsn_pairwise=CCMP
EOF

sed -i 's|#DAEMON_CONF=""|DAEMON_CONF="/etc/hostapd/hostapd.conf"|' /etc/default/hostapd 2>/dev/null || true

# dnsmasq config (DHCP)
mv /etc/dnsmasq.conf /etc/dnsmasq.conf.backup 2>/dev/null || true
cat > /etc/dnsmasq.conf << EOF
interface=$INTERFACE
dhcp-range=$DHCP_START,$DHCP_END,255.255.255.0,24h
EOF

systemctl unmask hostapd
systemctl enable hostapd
systemctl enable dnsmasq

echo "  WiFi AP configured."

# ---- Done ----
echo ""
echo "============================================"
echo "  Setup complete!"
echo "============================================"
echo ""
echo "Next steps:"
echo "  1. Copy files to $DEPLOY_DIR:"
echo "     - midi_ft_bridge (cross-compiled binary)"
echo "     - config.json"
echo "     - bt_bridge.py"
echo "     - clips/*.mp4 or media/*.mp4"
echo ""
echo "  2. Reboot to activate the WiFi AP."
echo "     After reboot there will be NO internet."
echo ""
echo "  3. Connect to WiFi '$SSID' (password: $PASSPHRASE)"
echo "     Pi Zero IP: $AP_IP"
echo ""
echo "Reboot now? (y/n)"
read -r answer
if [ "$answer" = "y" ]; then
    reboot
fi
