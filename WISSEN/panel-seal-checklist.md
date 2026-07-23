# Panel prep & "seal" checklist (Pi Zero 2 W FT panels)

Order of operations to turn a fresh RPi-OS-Lite panel into a locked-down show panel.
Do the OverlayFS step **last**, only once topology + config are final (it makes the
root FS read-only, so every later change needs an overlay-off/reboot dance).

Reference: [panel-ft-server-service.md](panel-ft-server-service.md),
[wlan-panel-stutter.md](wlan-panel-stutter.md).

## Per-panel reference (naming is NOT consistent — watch out)
| Panel | IP | Login user | Display service | Web :8080 | Notes |
|-------|-----|-----------|-----------------|-----------|-------|
| A_bigpanel | .21 | `bigpanel` (+`stratojets` admin) | `ft-server.service` (upstream) | no | 128×128 |
| C_ralfpanel | .20 | `ralfpanel` | `audio_led.service` (repo) | **yes** | GAME/LUA mode |
| B_ericpanel | .22 | `stratopanel` | `ftserver.service` (upstream, **no hyphen**) | no | 128×64 |

All three (2026-07-23): trimmed, power_save off (persistent via NM
`strato_accesspoint` wifi.powersave=2), Mac key + NOPASSWD sudo for their login user,
Mac root key in `/root/.ssh/authorized_keys` → Mac-app shutdown works on all three.
Gotchas: login user differs per panel; service name differs (`ft-server` vs `ftserver`
vs `audio_led`) — always confirm with `systemctl list-units | grep -iE "ft|led|audio"`.
The BT panel is Bluetooth (10 fps), not on WLAN — irrelevant to airtime.

## Router (GL-MT300N-V2 "Mango", OpenWrt 22.03) — set up 2026-07-23
- SSH: `root@192.168.10.1`, Mac key installed (key-based, no password needed).
- Radio object is `wireless.mt7628`. **Channel pinned to 11, HT20** (was `channel=auto`
  + `HT40` — auto is non-deterministic for a show, HT40 on 2.4 GHz is bad practice).
  Change: `uci set wireless.mt7628.channel=N; uci set wireless.mt7628.htmode=HT20;
  uci commit wireless; wifi reload` (panels blip ~5 s). `iwinfo` shows live channel.
- **DHCP reservations already exist** for all three panels (MAC→IP): .21 bigpanel,
  .22 ericpanel, .20 ralfpanel. Pool is .100–.249, so the static panel IPs don't
  collide — panels already have deterministic IPs; panel-side static config is optional.
- On-site: `setup/onsite-scan.sh --set` scans from all panels, picks the cleanest of
  1/6/11 and pins it on the router in one command (`--test` adds a stress log).

## Two different display stacks (decided 2026-07-23 — keep both)
- **A_bigpanel (.21):** upstream flaschen-taschen `ft-server` — dumb display server.
- **C_ralfpanel (.20):** the repo's `audio_led` (`audio_led.service`,
  `/home/ralfpanel/dev/visualizer/`) — has a **GAME mode + LUA scripting** and its own
  web UI on :8080. GUI toggles GAME↔FT and works well; **leave it as-is** (lots of LUA
  work). Not worth unifying: both bind UDP 1337 and speak the same FT protocol, so
  **network/airtime behaviour is identical** — the stutter question doesn't care which
  binary runs. Putting audio_led's GAME mode on the 128×128 is deferred to a future
  Raspberry Pi 4 (CPU headroom). **Shutdown is unified** to the standard path below
  (Mac app `ssh root@ + sudo shutdown`); audio_led's own :8080 shutdown is unused.

## Accounts / access (set up on .21 and .20, 2026-07-23)
- `bigpanel` (uid 1000) — main/console user; the ft-server binary lives under
  `/home/bigpanel/dev/flaschen-taschen/`.
- `stratojets` (uid 1001) — SSH admin account, in `sudo` group. The Mac's key
  (`~/.ssh/id_ed25519`, `r.preininger@engineraum.com`) is in its authorized_keys.
  Passwordless sudo granted via `/etc/sudoers.d/stratojets`
  (`stratojets ALL=(ALL) NOPASSWD:ALL`, mode 440).
- **`root`** — same Mac key added to `/root/.ssh/authorized_keys` (mode 600).
  Required because the **shutdown button** in the Mac app does
  `ssh root@<ip> 'sudo shutdown now'` (`src/engine.cpp:31`). `PermitRootLogin
  prohibit-password` (default) already allows key login. **Never disable ssh or
  remove this key, or the shutdown button breaks.**
- **C_ralfpanel (.20)** uses the same scheme with its own main user `ralfpanel`
  (uid 1000, NOPASSWD sudo via `/etc/sudoers.d/ralfpanel`, Mac key in its
  authorized_keys) plus the Mac root key in `/root/.ssh/authorized_keys`. Root
  shutdown path verified 2026-07-23 — both panels now shut down via the Mac app.

## 1. Trim the bloat (done on .21 and .20)
Removes background network/CPU noise — the apt timers matter because unattended
`apt` downloads over the congested 2.4 GHz WLAN feed the retry storm.
```sh
sudo systemctl mask apt-daily.timer apt-daily-upgrade.timer man-db.timer \
     ModemManager.service udisks2.service NetworkManager-wait-online.service
sudo touch /etc/cloud/cloud-init.disabled     # stop cloud-init re-running each boot
sudo systemctl stop ModemManager.service udisks2.service
```
Left running (needed): NetworkManager, wpa_supplicant, dbus, polkit, cron, ssh,
journald, timesyncd, ft-server, getty@tty1, serial-getty@ttyS0 (UART rescue), udevd.
Optional extra: `avahi-daemon` (mDNS `bigpanel.local`) — mask it if you only use
static IPs.

## 2. WiFi hygiene (done on .21)
```sh
# runtime:
sudo iw dev wlan0 set power_save off
# persistent (NetworkManager manages the wifi; connection name "strato_accesspoint"):
sudo nmcli con modify strato_accesspoint wifi.powersave 2   # 2 = disabled
```
Pi Zero 2 W defaults to `power_save on` → latency spikes that worsen the retry storm
([wlan-panel-stutter.md](wlan-panel-stutter.md)).

## 3. Verify before sealing
```sh
systemctl is-active ft-server ssh NetworkManager      # all active
ss -ulpnH | grep :1337                                # ft-server UDP listening
ssh root@<ip> 'echo ok'                               # shutdown-button path
sudo iw dev wlan0 get power_save                       # off
```

## 4. Clock — WON'T FIX (decided 2026-07-23)
`timesyncd` can't reach any NTP server: the AP (Mango) has no internet and no
battery-backed RTC, so **NTP will never reach the panels** — and the router couldn't
serve correct time anyway. The panel clock stays wrong (was stuck on 2026-06-17).
This is **purely cosmetic**: the ft-server uses no wall-clock, the UDP display has no
date dependency — the only cost is useless log timestamps. Left as-is on purpose.
If correct logs are ever needed, the only thing that works is the Mac (correct time,
always LAN-wired to the panel) pushing it via `ssh root@<ip> 'date -s ...'` — resets
every reboot without a hardware RTC (DS3231). Not worth it for the show.

## 5. Seal with OverlayFS (LAST — do when config is final)
Protects the SD card against corruption from yank-the-power shutdowns (the #1 killer
of Pi installs). Root FS becomes read-only + RAM overlay; writes are discarded on
reboot. RAM is ample (panel uses ~150 MB of 416 MB).
```sh
sudo raspi-config nonint do_overlayfs 0    # enable; then reboot
# to make a change later: do_overlayfs 1 -> reboot -> edit -> do_overlayfs 0 -> reboot
```
After sealing, journald logs no longer persist across reboots — finish any log-based
debugging first.
