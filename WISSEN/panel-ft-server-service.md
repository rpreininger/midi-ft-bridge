# Panel ft-server service — what actually runs (vs. the repo template)

The panel that displays the LED matrix runs the **upstream Flaschen-Taschen
server** as a systemd service — **not** the `audio_led --ft-mode` binary that
`setup/setup-panel.sh` creates. The Mac just sends the UDP FT protocol; the panel
runs the standard `ft-server` from the flaschen-taschen project.

## Where it's defined
- **On the panel:** `/etc/systemd/system/ft-server.service` (enabled). Inspect with
  `systemctl cat ft-server` / `systemctl status ft-server`.
- **In the repo:** `setup/setup-panel.sh` writes a unit of the *same name* but a
  **different** one — treat the repo template as stale for the panel side.

## The installed unit on A_bigpanel (192.168.10.21, hostname `bigpanel`)
```ini
[Unit]
Description=Flaschen-Taschen Server
After=network.target

[Service]
Type=simple
ExecStart=/home/bigpanel/dev/flaschen-taschen/server/ft-server \
  -D 128x128 --led-cols=64 --led-rows=64 --led-chain=2 --led-parallel=2 \
  --led-pwm-lsb-nanoseconds=50 --led-pwm-bits=7 --led-slowdown-gpio=2
WorkingDirectory=/home/bigpanel/dev/flaschen-taschen/server
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```
No `User=` → runs as **root**. Binary lives under `/home/bigpanel/dev/...`.
SSH login user on the panel is `stratojets` (key-based access set up on the Mac).

## How the repo template differs (`setup/setup-panel.sh`)
| | Repo template | Actual on .21 |
|---|---|---|
| ExecStart | `/home/pi/audio_led/audio_led --ft-mode` | upstream `ft-server -D 128x128 …` |
| Binary | our `audio_led` in FT mode | flaschen-taschen upstream server |
| Path/User | `/home/pi/…`, `User=root` | `/home/bigpanel/…`, no `User=` (→ root) |
| Extras | `ExecStartPre=/bin/sleep 5`, `network-online.target` | no sleep, `network.target` |

## LED param notes (relevant to performance)
- `--led-chain=2 --led-parallel=2` with `--led-cols=64 --led-rows=64` → four
  64×64 tiles = 128×128.
- **`--led-pwm-bits=7`** (default is 11) → deliberately reduced colour depth to keep
  refresh rate / CPU sane on the Pi Zero 2 W. This PWM bit-banging is the cost
  driver behind the one render thread sitting near ~100% of one (isolated) core —
  **not** the network. `--led-slowdown-gpio=2` is the standard Pi Zero 2 W GPIO
  timing margin.

## Handy commands (login as `stratojets@<panel-ip>`)
```sh
systemctl cat ft-server          # see the unit
systemctl status ft-server       # running? since when? PID
sudo systemctl restart ft-server # apply a changed unit / recover
journalctl -u ft-server -e       # logs
```
