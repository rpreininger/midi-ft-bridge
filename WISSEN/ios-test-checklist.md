# iOS bridge — on-site test checklist

Short version for doing at the panels. Background and reasoning live in
`ios-port.md`.

App is installed on both phones already (all 13 clips + real config.json,
signed until 2027-07-25). Just tap the icon — it starts the engine itself.

- **iFön** = iPhone 15 Pro = show master (USB-C)
- **Stratofon** = iPhone SE 2 = rehearsal (Lightning)

---

## Test 1 — Do frames reach the panels?

**Do this first.** Needs no purchase: reuse the USB-C Ethernet dongle the Mac
is wired with. This is the test that decides whether the port is viable — so
far we have only proved the engine *runs*, not that packets *arrive*.

1. [ ] Power the 3 panels, let them join the GL.iNet
2. [ ] USB-C Ethernet dongle → iPhone 15 Pro
3. [ ] Dongle → GL.iNet **LAN** port
4. [ ] iPhone: **Wi-Fi off** (otherwise two paths to the same subnet)
5. [ ] Settings → **Ethernet** appears — note the IP
6. [ ] Open MIDI-FT Bridge
7. [ ] **Allow** the Local Network prompt if it appears
8. [ ] Tap a clip

**Pass:** panels show the clip *and* the frame counters climb in the app.

| Symptom | Where to look |
|---|---|
| Counters climb, panels dark | Panel side — power, IP, ft-server |
| Counters stuck at 0 | Settings → Privacy & Security → **Local Network** → MIDI-FT Bridge → on |
| No Ethernet pane at all | Dongle not enumerating — try powered, or another dongle |

The phone can just take DHCP; only the *panel* addresses need to be fixed,
because those are what config.json targets.

## Test 2 — Fantom MIDI

Needs a USB-C to USB-B cable (15 Pro), or a Lightning camera adapter (SE).

1. [ ] Fantom → phone, Fantom powered on
2. [ ] In the app, the `MIDI:` line must name the **Fantom** — not
       "Netzwerk Session 1", which is what both phones have shown so far
3. [ ] Play a key mapped to notes 36–48, MIDI channel 10

**Pass:** clip fires from the keyboard.

## Test 3 — Both at once

Ethernet + Fantom on one powered dock (15 Pro). This is the remaining unknown:
whether iOS drives Ethernet and MIDI simultaneously through one hub.

1. [ ] Both attached, dock powered (PD in)
2. [ ] Repeat the checks from Test 1 and Test 2 together

**Pass:** MIDI triggers *and* frames flow at the same time.

## Test 4 — Stability

Do after 1–3, allow ~10 min.

1. [ ] Rapid-tap ~20 clips in a row → **must not crash**
       (verifies the use-after-free fix in `6e8b48b`)
2. [ ] Let one clip run — counters should climb smoothly, no stalls
3. [ ] Lock the screen → audio should keep playing (background audio mode)
4. [ ] Feel the phone after 10 min — watch for thermal throttling

---

## Optional — break-glass hotspot

Not the plan (see `ios-port.md`: Personal Hotspot needs a live SIM, is a fixed
`172.20.10.0/28` with no reservations, and is single-band). But as an emergency
fallback if the router dies at a gig, a stuttery 2.4 GHz hotspot beats no show.

- [ ] Put any old prepaid SIM in the 15 Pro — does **Settings → Personal
      Hotspot** appear? No data plan needed; the panels never reach the
      internet, they only talk to the phone.
- [ ] If yes, and you want this as real fallback: prepare a second config with
      `172.20.10.x` panel addresses, and pre-configure the panels'
      wpa_supplicant with the hotspot SSID as a secondary network.

Won't work on the SE either way — no 5 GHz hotspot before iPhone 12.
