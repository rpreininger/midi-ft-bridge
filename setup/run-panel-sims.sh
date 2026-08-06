#!/usr/bin/env bash
# Stand in for the three FT panels using the FT-Server debug viewer, so a full
# rehearsal can run with no panels on the bench.
#
# One FT-Server instance per panel, each bound to that panel's own IP on port
# 1337 — so nothing changes on the phone or in config.json, the app still
# thinks it is talking to 192.168.10.20/.21/.22.
#
#   setup/sim-panel-ips.sh en9 up      # once: give this Mac the panel IPs
#   setup/run-panel-sims.sh            # start the three viewers
#
# Each panel gets its own browser view on 8081 / 8082 / 8083. Ctrl-C stops all
# three. Panel geometry and IPs are read from config.json, so this follows the
# real rig automatically.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="${CONFIG:-$HERE/config.json}"
FT_SERVER="${FT_SERVER:-$HERE/../FT-Server}"

[[ -f "$CONFIG" ]] || { echo "no config at $CONFIG" >&2; exit 1; }
[[ -d "$FT_SERVER" ]] || { echo "no FT-Server at $FT_SERVER" >&2; exit 1; }

# name,ip,port,width,height for every UDP panel (the BT panel speaks a
# different protocol and is skipped).
# (while-read rather than mapfile: /bin/bash on macOS is still 3.2)
PANELS=()
while IFS= read -r line; do PANELS+=("$line"); done < <(python3 - "$CONFIG" <<'PY'
import json, sys
cfg = json.load(open(sys.argv[1]))
for p in cfg.get("panels", []):
    if p.get("type") in (None, "", "ft", "udp"):
        print(f'{p.get("name","?")},{p.get("ip")},{p.get("port",1337)},'
              f'{p.get("src_w",128)},{p.get("src_h",128)}')
PY
)

[[ ${#PANELS[@]} -gt 0 ]] || { echo "no UDP panels in $CONFIG" >&2; exit 1; }

PIDS=()
cleanup() {
    echo
    echo "stopping ${#PIDS[@]} viewer(s)…"
    for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null || true; done
    wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

http=8081
for entry in "${PANELS[@]}"; do
    IFS=, read -r name ip port w h <<<"$entry"

    # Fail loudly rather than silently listening on the wrong address: without
    # the alias the bind fails and the panel would just look "dead".
    if ! ifconfig | grep -q "inet $ip "; then
        echo "!! $ip is not an address on this Mac."
        echo "!! Run: setup/sim-panel-ips.sh <iface> up   (iface faces the panel router)"
        exit 1
    fi

    echo "starting $name  $ip:$port  ${w}x${h}  ->  http://localhost:$http"
    (cd "$FT_SERVER" && npx tsx src/server.ts \
        --bind "$ip" --port "$port" \
        --width "$w" --height "$h" \
        --http-port "$http" --name "$name") &
    PIDS+=($!)
    http=$((http + 1))
done

echo
echo "all viewers up — open the URLs above. Ctrl-C to stop."
wait
