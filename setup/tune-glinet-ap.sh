#!/bin/sh
# ====================================================================
#  GL.iNet Mango (OpenWrt) AP tuning for midi-ft-bridge
#
#  Fixes the "one panel reboots -> the others jitter" problem. That jitter
#  is shared-WiFi airtime loss: while a panel is gone the AP keeps retrying
#  unicast delivery to it, starving the healthy panels. This script:
#    1. Evicts dead clients fast (disassoc_low_ack + short max_inactivity),
#       so the AP stops wasting airtime on a rebooting panel almost at once.
#    2. Pins each panel to a fixed IP via DHCP reservation, so a reboot never
#       changes its address or churns ARP.
#
#  Run ON the router (BusyBox ash), or from the Mac over SSH:
#      ssh root@192.168.10.1 'sh -s' < setup/tune-glinet-ap.sh
#
#  EDIT the PANELS table below first: fill in each panel's WiFi MAC.
#  Find a MAC from the router with:  cat /tmp/dhcp.leases
# ====================================================================
set -e

# name   ip              mac (lowercase, colons)
PANELS="
A_bigpanel	192.168.10.21	FILL_ME
B_ericpanel	192.168.10.22	FILL_ME
C_ralfpanel	192.168.10.20	FILL_ME
"

if echo "$PANELS" | grep -q FILL_ME; then
    echo "ERROR: edit PANELS in this script and replace every FILL_ME with the panel's MAC." >&2
    exit 1
fi

echo "[1/3] Tuning WiFi to evict dead clients fast..."
# Apply to every wifi-iface so it works regardless of radio naming.
for sec in $(uci show wireless | sed -n 's/wireless\.\(.*\)=wifi-iface/\1/p'); do
    uci set wireless."$sec".disassoc_low_ack='1'   # drop a client we can't reliably reach
    uci set wireless."$sec".max_inactivity='5'     # seconds of silence before eviction
    echo "    wireless.$sec: disassoc_low_ack=1 max_inactivity=5"
done

echo "[2/3] Pinning panels to fixed IPs (DHCP reservations)..."
# Named sections -> re-running this script updates rather than duplicates.
echo "$PANELS" | while IFS="	" read -r name ip mac; do
    [ -n "$name" ] || continue
    uci set dhcp."$name"=host
    uci set dhcp."$name".name="$name"
    uci set dhcp."$name".mac="$mac"
    uci set dhcp."$name".ip="$ip"
    uci set dhcp."$name".leasetime='infinite'
    echo "    $name -> $ip ($mac)"
done

echo "[3/3] Committing and reloading..."
uci commit wireless
uci commit dhcp
wifi reload
/etc/init.d/dnsmasq restart

echo "Done. Reboot a panel and confirm the others hold steady."
