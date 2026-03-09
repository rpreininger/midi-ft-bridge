#!/bin/bash
#
# Panel Raspi setup for midi-ft-bridge
# Configures a panel Pi to auto-connect to LED-NET WiFi
# and run the FT server (from the raspi project)
#
# Run on each panel Raspi:
#   sudo bash setup-panel.sh
#

set -e

SSID="LED-NET"
PASSPHRASE="ledpanels"

echo "=== Panel Setup for midi-ft-bridge ==="
echo "This Pi will connect to WiFi '$SSID' and run FT server"
echo ""

# Configure WiFi connection to LED-NET
echo "[1/3] Configuring WiFi..."
cat >> /etc/wpa_supplicant/wpa_supplicant.conf << EOF

network={
    ssid="$SSID"
    psk="$PASSPHRASE"
    priority=10
}
EOF

# Make sure wlan0 uses DHCP (default behavior)
echo "[2/3] Ensuring DHCP on wlan0..."
# Remove any static IP config for wlan0 if present
if grep -q "interface wlan0" /etc/dhcpcd.conf; then
    echo "Warning: Found existing wlan0 config in /etc/dhcpcd.conf"
    echo "Please verify it uses DHCP (no static ip_address)"
fi

# Create FT server systemd service
# This assumes the audio_led binary from the raspi project is deployed
# and runs in FT mode (receives PPM over UDP and displays on LED matrix)
echo "[3/3] Creating FT server service..."
cat > /etc/systemd/system/ft-server.service << 'EOF'
[Unit]
Description=Flaschen-Taschen LED Panel Server
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStartPre=/bin/sleep 5
ExecStart=/home/pi/audio_led/audio_led --ft-mode
WorkingDirectory=/home/pi/audio_led
Restart=on-failure
RestartSec=5
User=root

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable ft-server

echo ""
echo "=== Panel setup complete! ==="
echo ""
echo "Next steps:"
echo "  1. Deploy the audio_led binary to /home/pi/audio_led/"
echo "  2. Reboot to connect to $SSID and start FT server"
echo "  3. The panel will get an IP from DHCP (192.168.4.10x)"
echo ""
echo "To check status after reboot:"
echo "  sudo systemctl status ft-server"
echo "  ip addr show wlan0"
echo ""
echo "Reboot now? (y/n)"
read -r answer
if [ "$answer" = "y" ]; then
    reboot
fi
