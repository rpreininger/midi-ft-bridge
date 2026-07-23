# Panel stutter on full-WLAN — cause & fix

## Symptom
With a **full-WLAN setup** (Mac on WiFi + all panels on WiFi), all panels stutter
together after **10–30 s**. Not a single-panel glitch — every panel drops at once,
and only after a delay.

## Root cause: 2.4 GHz airtime saturation + UDP retry storm

Fixed constraints:
- **Panels are Pi Zero 2 W → 2.4 GHz only.**
- **Mango (GL-MT300N-V2) is single-band 2.4 GHz.** One radio, one channel for everything.

Load (from `config.json`, 25 fps, RGB):
| Panel        | Size     | Bytes/frame | Mbps  |
|--------------|----------|-------------|-------|
| A_bigpanel   | 128×128  | 49 KB       | ~9.8  |
| B_ericpanel  | 128×64   | 24 KB       | ~4.9  |
| C_ralfpanel  | 128×64   | 24 KB       | ~4.9  |
| **Total**    |          | **98 KB**   | **~20** |

~2,175 packets/s (87 UDP datagrams/frame after `TILE_ROWS=3` MTU tiling, ~1,180 B each).

Why it saturates:
- With the Mac on WiFi, ~20 Mbps of payload crosses the air **twice** (Mac→AP, AP→panel)
  → **~40 Mbps airtime demand** on one half-duplex 2.4 GHz channel. Beyond what a single
  11n stream delivers.

Why it ramps over 10–30 s (not instant):
- Transport is **UDP, fire-and-forget** (`src/ft_sender.cpp`: non-blocking `sendto`,
  `SO_SNDBUF` 256 KB, no retry, return value ignored). **No backpressure** — the Mac keeps
  injecting 2,175 pkt/s regardless of the link.
- Each panel packet is an **L2 unicast** → the AP ACKs and **retries** on loss. Under
  congestion, retries multiply → more airtime consumed → more loss → **retry storm**.
- WiFi Tx queues (bufferbloat) absorb the first several seconds, then saturate and drop →
  all panels stutter at once. The delay = buffer-fill time.

## Key insight: the Mac's uplink is the killer
The Mac's WiFi uplink carries the **sum of all panel traffic**. Wiring one panel only
removes *that panel's downlink* airtime — not its share of the Mac's uplink. So the Mac is
the first thing to take off the air.

## Target topology (the plan)
Wire **both the Mac and the 128×128 panel** into the Mango via LAN; leave the two 128×64
panels on WiFi.

| What              | Over-the-air contribution      |
|-------------------|--------------------------------|
| Mac wired         | uplink airtime = 0             |
| A_bigpanel wired  | its downlink = 0 (was ~9.8)    |
| B + C on WiFi     | ~9.8 Mbps downlink only        |

Air carries only **~9.8 Mbps** → comfortable on one 2.4 GHz radio. Stutter resolved.

## Option ladder
1. **Wire the Mac** (USB-C Ethernet dongle → Mango LAN). Biggest single win (~20 Mbps off
   the air). This alone likely fixes it.
2. **Also wire the 128×128** — removes another ~9.8 Mbps. Both wired = the target above.
3. **Dual-band router** *only if the Mac must stay wireless* (e.g. GL.iNet Beryl/Slate):
   Mac on 5 GHz, panels on 2.4 GHz → hops on separate radios, no airtime doubling. Costs a
   new router.
4. **Reduce load** (band-aid, keeps Mango + wireless Mac): `default_fps` 25→15 (~20→12 Mbps),
   or throttle A_bigpanel via the existing per-panel `max_fps` lever (already used for the
   BLE panel at 10 fps). Degrades the show.

## Hygiene (do regardless — these compound under congestion)
- `iw <dev> set power_save off` on all three panels (Pi Zero 2 W default is `power_save on`;
  adds latency spikes that worsen the retry storm).
- Pin the AP to a clean 2.4 GHz channel (1/6/11) away from house WiFi.
- `setup/tune-glinet-ap.sh` already handles the *separate* "one panel reboots → others
  jitter" case (fast dead-client eviction + DHCP reservations).

## What does NOT help
- QoS/WMM — reorders airtime, doesn't create more; UDP gives no backpressure to shape.
