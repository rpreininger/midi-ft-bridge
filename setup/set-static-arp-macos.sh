#!/usr/bin/env bash
# ====================================================================
#  Static ARP entries for the FT panels (run on the Mac)
#
#  Belt-and-suspenders for the reboot jitter: with a static ARP entry the
#  Mac never has to re-resolve a panel's MAC when it drops/reboots, so no
#  broadcast ARP churn on the shared WiFi.
#
#  Pairs with setup/tune-glinet-ap.sh (which pins the same IP->MAC on the AP).
#  EDIT the PANELS table: fill in each panel's WiFi MAC, then run:
#      ./setup/set-static-arp-macos.sh
#
#  Note: macOS static ARP entries do NOT survive a reboot. Re-run after
#  restarting the Mac (or wrap in a launchd agent if you want it permanent).
# ====================================================================
set -euo pipefail

# "ip mac"  (mac lowercase, colon-separated)
PANELS=(
    "192.168.10.21 FILL_ME"   # A_bigpanel
    "192.168.10.22 FILL_ME"   # B_ericpanel
    "192.168.10.20 FILL_ME"   # C_ralfpanel
)

for entry in "${PANELS[@]}"; do
    ip="${entry%% *}"
    mac="${entry##* }"
    if [ "$mac" = "FILL_ME" ]; then
        echo "ERROR: edit PANELS in this script and replace every FILL_ME with the panel's MAC." >&2
        exit 1
    fi
    sudo arp -s "$ip" "$mac" && echo "  set $ip -> $mac"
done

echo "Done. Verify with:  arp -an | grep 192.168.10"
