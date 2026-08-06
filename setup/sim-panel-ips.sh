#!/usr/bin/env bash
# Give this Mac the panels' own IP addresses, so ft-panel-sim.py can stand in
# for them without touching config.json on the phone or the Mac app.
#
#   setup/sim-panel-ips.sh en9 up      # add 192.168.10.20/.21/.22
#   setup/sim-panel-ips.sh en9 down    # remove them again
#
# The interface is whichever one faces the panel router (en9 = the USB-C
# Ethernet dongle on the show Mac; use `ifconfig | grep 192.168.10` to check).
#
# NEVER run this while the real panels are powered on the same LAN — two hosts
# answering for one address is an address conflict, and the panels lose.
set -euo pipefail

IFACE="${1:-}"
ACTION="${2:-}"
IPS=(192.168.10.20 192.168.10.21 192.168.10.22)
# /32, not /24. The interface already carries 192.168.10.2/24, and macOS only
# installs an additional address in an existing subnet as a host route — with
# a /24 mask the alias appears in `ifconfig` but never becomes a local address,
# so even sending to it from this Mac fails with "No route to host".
MASK=255.255.255.255

if [[ -z "$IFACE" || ! "$ACTION" =~ ^(up|down)$ ]]; then
    echo "usage: $0 <interface> up|down" >&2
    exit 2
fi

if ! ifconfig "$IFACE" >/dev/null 2>&1; then
    echo "no such interface: $IFACE" >&2
    exit 1
fi

for ip in "${IPS[@]}"; do
    if [[ "$ACTION" == "up" ]]; then
        if ping -c1 -W300 -t1 "$ip" >/dev/null 2>&1; then
            echo "!! $ip already answers on the network — a real panel is up."
            echo "!! Power the panels down first, or you will collide with one."
            exit 1
        fi
        sudo ifconfig "$IFACE" alias "$ip" "$MASK"
        echo "added   $ip on $IFACE"
    else
        sudo ifconfig "$IFACE" -alias "$ip" 2>/dev/null && echo "removed $ip" \
            || echo "not set $ip"
    fi
done

echo
ifconfig "$IFACE" | grep "inet " || true
