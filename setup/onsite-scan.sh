#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# On-site 2.4 GHz survey — recommend the cleanest of channels 1/6/11 for the
# Mango AP, then (optionally) stress-test under real venue conditions.
#
# WHY: the home RF picture does NOT transfer to a venue — foreign APs and a
# crowd of phones saturate 2.4 GHz airtime (see WISSEN/wlan-panel-stutter.md).
# Run this on arrival, set the Mango to the winner, re-run with --test.
#
# It scans from EVERY reachable panel (they sit in different spots → hear
# different neighbours) and merges the worst case per candidate channel.
#
# Usage:
#   setup/onsite-scan.sh              # scan + recommend a channel
#   setup/onsite-scan.sh --set        # ... and pin the Mango to the winner (HT20)
#   setup/onsite-scan.sh --set --test # ... then a 120 s 3-panel stress log
#   (--test may be given alone to skip re-setting the channel)
#
# Run from the Mac (must be on the AP LAN). Needs key-based SSH to the panels
# (passwordless sudo) and to the Mango router (set up 2026-07-23).
# ---------------------------------------------------------------------------
set -uo pipefail

OWN_SSID="strato_accesspoint"
# user@ip:label — panel login names are NOT consistent; adjust if IPs change.
PANELS=(
  "stratojets@192.168.10.21:bigpanel"
  "ralfpanel@192.168.10.20:ralfpanel"
  "stratopanel@192.168.10.22:ericpanel"
)
ROUTER="root@192.168.10.1"          # GL-MT300N-V2 (Mango), OpenWrt; radio = wireless.mt7628
SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=6)
STRESS_SECS=120

SET=0; TEST=0
for a in "$@"; do case "$a" in
  --set)  SET=1 ;;
  --test) TEST=1 ;;
  *) echo "unknown arg: $a (use --set and/or --test)" >&2; exit 2 ;;
esac; done

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

# --- 1. collect scans -------------------------------------------------------
echo "== 2.4 GHz survey =="
reached=0
for entry in "${PANELS[@]}"; do
  ua="${entry%%:*}"; label="${entry##*:}"
  raw="$(ssh "${SSH_OPTS[@]}" "$ua" 'sudo iw dev wlan0 scan 2>/dev/null || iw dev wlan0 scan 2>/dev/null' 2>/dev/null)"
  if [ -z "$raw" ]; then
    echo "  ! $label ($ua): unreachable or scan failed — SKIPPED"
    continue
  fi
  reached=$((reached+1))
  printf '%s\n' "$raw" | awk -v own="$OWN_SSID" '
    /^BSS / { if (f!="") emit(); f=""; s=""; ss="" }
    /freq:/   { f=$2 }
    /signal:/ { s=$2 }
    /SSID:/   { $1=""; sub(/^ /,""); ss=$0 }
    END { if (f!="") emit() }
    function emit(   c) {
      if (f=="" || s=="") return
      c = int((f - 2407)/5 + 0.5)      # 2412->1, 2437->6, 2462->11
      if (c < 1 || c > 14) return
      printf "%d %s %s\n", c, s, (ss==own ? "OWN" : "X")
    }' >> "$tmp/all"
done
[ "$reached" -eq 0 ] && { echo "No panels reachable — are you on the AP LAN?" >&2; exit 1; }
echo "  scanned from $reached panel(s)"

own_chan="$(awk '$3=="OWN"{print $1}' "$tmp/all" 2>/dev/null | sort -u | tr '\n' ' ')"
[ -n "$own_chan" ] && echo "  our AP ($OWN_SSID) currently on channel: $own_chan"

# --- 2. rank candidate channels --------------------------------------------
# For each of 1/6/11, the worst (strongest) FOREIGN AP whose channel overlaps
# (±4 on 2.4 GHz), across all panel vantage points. Weaker worst = better.
echo
echo "== ranking 1 / 6 / 11 (worst overlapping foreign interferer) =="
awk '
  BEGIN{ C[0]=1; C[1]=6; C[2]=11 }
  $3=="X" { chan=$1; sig=$2+0
            for (i=0;i<3;i++){ cand=C[i]
              d = cand-chan; if (d<0) d=-d
              if (d<=4){ if (!(cand in worst) || sig>worst[cand]) worst[cand]=sig; cnt[cand]++ }
            }
          }
  END{ for (i=0;i<3;i++){ cand=C[i]
         w=(cand in worst)?worst[cand]:-999; n=(cand in cnt)?cnt[cand]:0
         printf "%d %d %d\n", cand, w, n } }
' "$tmp/all" | sort -k2,2n > "$tmp/rank"

rank=1
while read -r ch worst n; do
  if [ "$worst" -le -900 ]; then desc="clean (no overlapping AP heard)"
  else desc="worst ${worst} dBm, ${n} AP-sightings in range"; fi
  mark=""; [ "$rank" -eq 1 ] && mark="   <== RECOMMENDED"
  printf "  #%d  channel %-2s  %s%s\n" "$rank" "$ch" "$desc" "$mark"
  rank=$((rank+1))
done < "$tmp/rank"

best="$(head -1 "$tmp/rank" | awk '{print $1}')"
echo
# --- 3. optionally pin the channel on the router ----------------------------
if [ "$SET" -eq 1 ]; then
  echo
  echo "== pinning Mango to channel $best (HT20) =="
  ssh "${SSH_OPTS[@]}" "$ROUTER" \
    "uci set wireless.mt7628.channel=$best; uci set wireless.mt7628.htmode=HT20; uci commit wireless; wifi reload" 2>&1 \
    && echo "  done — panels drop ~5 s and reconnect on channel $best" \
    || echo "  ! failed to reach router ($ROUTER) — set it manually in the GL.iNet UI"
else
  echo
  echo "Set the Mango to channel $best:  GL.iNet UI → Wireless → Channel $best,"
  echo "  or re-run with --set to pin it automatically."
fi

# --- 4. optional stress test ------------------------------------------------
[ "$TEST" -eq 1 ] || { echo; echo "Next: play real content on all panels, then: setup/onsite-scan.sh --test"; exit 0; }

echo
echo "== ${STRESS_SECS}s stress log (real content should be playing on all panels) =="
for entry in "${PANELS[@]}"; do
  ua="${entry%%:*}"; label="${entry##*:}"
  ( out="$(ssh "${SSH_OPTS[@]}" "$ua" "
        prev=\$(grep Udp: /proc/net/snmp|tail -1|awk '{print \$2}')
        pr=\$(awk '/wlan0:/{print \$7}' /proc/net/wireless)
        min=9999999; max=0; sum=0; retr=0
        for i in \$(seq 1 $STRESS_SECS); do
          sleep 1
          cur=\$(grep Udp: /proc/net/snmp|tail -1|awk '{print \$2}'); pps=\$((cur-prev)); prev=\$cur
          rt=\$(awk '/wlan0:/{print \$7}' /proc/net/wireless); pr2=\$((rt-pr)); pr=\$rt
          retr=\$((retr+pr2)); sum=\$((sum+pps))
          [ \$pps -lt \$min ] && min=\$pps; [ \$pps -gt \$max ] && max=\$pps
        done
        printf 'avg %d pps (min %d max %d) | retry-discards %d\n' \$((sum/$STRESS_SECS)) \$min \$max \$retr
      " 2>/dev/null)"
    printf '  %-10s %s\n' "$label:" "${out:-UNREACHABLE}" ) &
done
wait
echo
echo "Read: retry-discards near 0 and min≈avg = healthy. Retries climbing or min<<avg = airtime"
echo "trouble → fallback ladder: set channel → bigpanel 20 fps → wire the 128x128 → wire more."
