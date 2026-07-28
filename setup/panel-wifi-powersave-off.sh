#!/bin/sh
# ====================================================================
#  Persist "power_save off" on every WiFi interface of a panel
#
#  Pi Zero 2 W (and the Edimax/rtw88 5 GHz USB adapter on bigpanel) come
#  up with WiFi power management ON. That is what produces the irregular
#  <1ms / 20ms / 500ms ping seen on the panels: the radio sleeps between
#  beacons and the AP's unicast frames queue until it wakes. Turning it
#  off at runtime works but is lost on reboot, so this installs a systemd
#  oneshot that reapplies it on every boot, for whatever wlanN exist.
#
#  Idempotent: re-running just rewrites the unit + script and re-enables.
#
#  Run ON a panel, or from the Mac over SSH:
#      ssh root@192.168.10.21 'sh -s' < setup/panel-wifi-powersave-off.sh
# ====================================================================
set -e

SCRIPT=/usr/local/sbin/wifi-powersave-off.sh
UNIT=/etc/systemd/system/wifi-powersave-off.service

echo "[1/4] Writing $SCRIPT ..."
mkdir -p /usr/local/sbin
cat > "$SCRIPT" <<'EOS'
#!/bin/sh
# Disable power save on every wireless interface. Tolerant: an absent
# adapter (e.g. no 5 GHz USB stick) is simply skipped.
for dev in /sys/class/net/wlan*; do
    [ -e "$dev" ] || continue
    ifn=$(basename "$dev")
    iw dev "$ifn" set power_save off 2>/dev/null && \
        logger -t wifi-powersave "power_save off on $ifn" || true
done
EOS
chmod +x "$SCRIPT"

echo "[2/4] Writing $UNIT ..."
cat > "$UNIT" <<EOS
[Unit]
Description=Disable WiFi power save on all wlan interfaces
After=multi-user.target
Wants=network.target

[Service]
Type=oneshot
ExecStart=$SCRIPT
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOS

echo "[3/4] Enabling + starting ..."
systemctl daemon-reload
systemctl enable wifi-powersave-off.service >/dev/null 2>&1
systemctl start wifi-powersave-off.service

echo "[4/4] Current state:"
for dev in /sys/class/net/wlan*; do
    [ -e "$dev" ] || continue
    ifn=$(basename "$dev")
    printf "    %s: " "$ifn"
    iw dev "$ifn" get power_save 2>/dev/null | tail -1
done
echo "Done."
