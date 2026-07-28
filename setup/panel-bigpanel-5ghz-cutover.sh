#!/bin/sh
# ====================================================================
#  bigpanel: cut the panel link over from 2.4 GHz (wlan0) to 5 GHz (wlan1)
#
#  bigpanel runs NetworkManager. This arms an atomic switch:
#    * wlan1 joins strato_5g with a STATIC 192.168.10.21 (config.json
#      targets .21 by address; static removes any DHCP-ordering doubt).
#    * wlan0 / strato_accesspoint is kept as a profile but its autoconnect
#      is turned off so it never fights wlan1 for .21.
#
#  DEAD-MAN'S SWITCH: the switch auto-reverts to 2.4 GHz after the timeout
#  UNLESS /tmp/5g_commit appears. So if 5 GHz is unreachable, the operator
#  (whose SSH runs over the very link being moved) gets the panel back with
#  no intervention. On success, `touch /tmp/5g_commit` cancels the revert.
#
#  Run from the Mac:
#      ssh root@192.168.10.21 'sh -s' < setup/panel-bigpanel-5ghz-cutover.sh
#  then, once reconnected over 5 GHz and verified:
#      ssh root@192.168.10.21 'touch /tmp/5g_commit'
# ====================================================================
set -e

CONN5=strato_5g;  SSID5=strato_5g;  IFACE5=wlan1
CONN24=strato_accesspoint
PSK=stratojets
IP=192.168.10.21/24;  GW=192.168.10.1
REVERT_AFTER=150      # seconds before auto-revert if not committed

echo "[1/4] (Re)creating the 5 GHz connection profile (static $IP)..."
nmcli con delete "$CONN5" 2>/dev/null || true
nmcli con add type wifi ifname "$IFACE5" con-name "$CONN5" ssid "$SSID5" >/dev/null
nmcli con modify "$CONN5" \
    802-11-wireless.band a \
    wifi-sec.key-mgmt wpa-psk wifi-sec.psk "$PSK" \
    ipv4.method manual ipv4.addresses "$IP" ipv4.gateway "$GW" ipv4.dns "$GW" \
    connection.autoconnect yes connection.autoconnect-priority 10

echo "[2/4] Stopping stale profiles from auto-grabbing an interface..."
for c in "$CONN24" "strato_access_point" "netplan-wlan0-WLAN-634444" "FRITZ!Box 7530 AL"; do
    nmcli con modify "$c" connection.autoconnect no 2>/dev/null && echo "    autoconnect off: $c" || true
done

echo "[3/4] Arming dead-man revert (${REVERT_AFTER}s unless /tmp/5g_commit)..."
rm -f /tmp/5g_commit
setsid sh -c '
    for i in $(seq 1 '"$REVERT_AFTER"'); do
        [ -f /tmp/5g_commit ] && exit 0
        sleep 1
    done
    logger -t bigpanel-5g "no commit -> reverting to 2.4 GHz"
    nmcli con modify '"$CONN24"' connection.autoconnect yes
    nmcli con down '"$CONN5"' 2>/dev/null
    nmcli con modify '"$CONN5"' connection.autoconnect no
    nmcli con up '"$CONN24"' 2>/dev/null
' >/dev/null 2>&1 &

echo "[4/4] Cutting over (detached; SSH over wlan0 will drop now)..."
# Detached so it survives this SSH session dying when wlan0 goes down.
setsid sh -c '
    sleep 1
    nmcli con down '"$CONN24"' 2>/dev/null
    nmcli con up '"$CONN5"' 2>/dev/null
    logger -t bigpanel-5g "cutover to 5 GHz executed"
' >/dev/null 2>&1 &

echo "ARMED. 2.4 down + 5 GHz up in ~1s. Auto-revert in ${REVERT_AFTER}s unless committed."
