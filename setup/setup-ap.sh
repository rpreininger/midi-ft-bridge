#!/bin/bash
#
# WiFi Access Point setup for midi-ft-bridge Pi Zero
# Creates hotspot "LED-NET" with DHCP for panel Raspis
#
# Run on the Pi Zero that receives MIDI:
#   sudo bash setup-ap.sh
#

set -e

SSID="GIGAHORST"
PASSPHRASE="horstkrause"
AP_IP="192.168.4.1"
DHCP_START="192.168.4.100"
DHCP_END="192.168.4.120"
INTERFACE="wlan0"

echo "=== WiFi AP Setup for midi-ft-bridge ==="
echo "SSID:       $SSID"
echo "IP:         $AP_IP"
echo "DHCP range: $DHCP_START - $DHCP_END"
echo ""

# Install packages
echo "[1/5] Installing hostapd and dnsmasq..."
apt-get update -qq
apt-get install -y hostapd dnsmasq

# Stop services during config
systemctl stop hostapd 2>/dev/null || true
systemctl stop dnsmasq 2>/dev/null || true

# Configure static IP for wlan0
echo "[2/5] Configuring static IP..."
cat >> /etc/dhcpcd.conf << EOF

# midi-ft-bridge AP config
interface $INTERFACE
    static ip_address=$AP_IP/24
    nohook wpa_supplicant
EOF

# Configure hostapd
echo "[3/5] Configuring hostapd..."
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

# Point hostapd to config
sed -i 's|#DAEMON_CONF=""|DAEMON_CONF="/etc/hostapd/hostapd.conf"|' /etc/default/hostapd

# Configure dnsmasq (DHCP server)
echo "[4/5] Configuring dnsmasq..."
mv /etc/dnsmasq.conf /etc/dnsmasq.conf.backup 2>/dev/null || true
cat > /etc/dnsmasq.conf << EOF
interface=$INTERFACE
dhcp-range=$DHCP_START,$DHCP_END,255.255.255.0,24h

# Fixed IP assignments for panels (update MAC addresses for your panels)
# dhcp-host=b8:27:eb:xx:xx:xx,$DHCP_START   # Panel A
# dhcp-host=b8:27:eb:yy:yy:yy,192.168.4.102 # Panel B
# dhcp-host=b8:27:eb:zz:zz:zz,192.168.4.103 # Panel C
EOF

# Enable and start services
echo "[5/5] Enabling services..."
systemctl unmask hostapd
systemctl enable hostapd
systemctl enable dnsmasq

echo ""
echo "=== Setup complete! ==="
echo "Reboot to activate the access point."
echo ""
echo "After reboot:"
echo "  - SSID: $SSID"
echo "  - Password: $PASSPHRASE"
echo "  - AP IP: $AP_IP"
echo ""
echo "To assign fixed IPs to panels, edit /etc/dnsmasq.conf"
echo "and add dhcp-host entries with the panel MAC addresses."
echo ""
echo "Reboot now? (y/n)"
read -r answer
if [ "$answer" = "y" ]; then
    reboot
fi
