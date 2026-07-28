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

## Test 2 — Fantom MIDI  ✅ PASSED (15 Pro, 2026-07-28)

Needs a USB-C to USB-B cable (15 Pro), or a Lightning camera adapter (SE).

**Critical Fantom setting:** MENU → SYSTEM → `USB Driver` = **GENERIC** (not
VENDOR), then **power-cycle the Fantom** — the setting only applies after a
restart. Without this, iOS sees nothing at all (no MIDI, no audio).

1. [x] Fantom → phone, Fantom powered on
2. [x] `MIDI:` line names the **Fantom** (`FANTOM-06 07 08 MIDI OUT1`)
3. [x] Keys 36–48 on channel 10 trigger clips reliably

**Result:** MIDI in works end to end on the 15 Pro. Not yet tested on the SE
(should be the same via camera adapter; Fantom is self-powered so the cheap
A1440 adapter may suffice for MIDI-only).

## Test 2b — Fantom audio  ❌ NOT POSSIBLE (confirmed 2026-07-28)

The Fantom-0's USB **audio** needs Roland's vendor driver, which does not exist
on iOS. iOS Control Center offers only the built-in speaker — the Fantom never
appears as an audio output. Confirmed at the iOS system level; no app change
fixes it. Only the Fantom's **MIDI** is class-compliant on iOS.

**Consequence for the show:** clip audio must reach the PA via a *separate*
class-compliant output on the phone — see the dock spec below.

**Pass:** N/A — this path is ruled out; plan around it.

## Show-rig dock spec (15 Pro)

The 15 Pro has ONE USB-C port; the show rig must carry four things through it
at once:

| Function | Why |
|---|---|
| **Power (PD in)** | keep the phone charged through a full set |
| **Ethernet (RJ45)** | panels wired = no 2.4 GHz stutter |
| **USB for Fantom** | MIDI in (proven working) |
| **Audio out** | clip audio → PA (the Fantom can't take USB audio) |

**Buy:** a *powered* USB-C dock with **PD-in + RJ45 + 2 USB ports** (one for the
Fantom's USB-B cable, one spare), plus an **Apple USB-C-to-3.5 mm adapter**.

**Audio path — the one real risk.** Dock-builtin 3.5 mm jacks are hit-or-miss on
iOS. Don't rely on one: plug a **known class-compliant DAC** into a spare dock
port instead — either the **Apple USB-C→3.5 mm** dongle (needs a spare USB-C
*data* port, NOT the PD-in port) or a cheap **USB-A→3.5 mm** sound adapter
(UGREEN etc., uses a USB-A port). Its line-out goes to the PA, or into the
Fantom's analog input to keep the Fantom as the PA mixer as today. (A plain
bus-powered dongle won't do — the dock needs PD-in.)

**Recommended brands (iOS track record):** Belkin Connect (lowest-risk, Apple-
Store-stocked), Anker (best value), Hyper/HyperDrive, Satechi/UGREEN. Avoid
no-name hubs — Ethernet chipset / PD logic often fails on iOS. Verify current
availability + recent "works with iPad/iPhone" reviews before buying; confirm
both Ethernet AND PD-passthrough work on iOS. Whatever you buy still has to pass
the all-at-once bench test below.

**Which audio DAC / adapter:**
- **Start here — Apple USB-C→3.5 mm** (~€10). Cirrus Logic DAC, clean, low
  noise, class-compliant so iOS always sees it. Plenty good into the Fantom's
  analog input or a DI. Only limit: unbalanced headphone-level out, so avoid
  long runs straight to the PA (hum pickup). Prove the whole rig with this.
- **Upgrade only if needed — a class-compliant USB interface** with balanced
  TRS/XLR outs for long PA runs / more headroom: **MOTU M2** (excellent DAC) or
  **Focusrite Scarlett** (Solo/2i2, recent gen). Bigger, ~€150–200, needs its
  own dock USB port + maybe bus power. Move up only if the Apple dongle picks
  up noise on your actual stage cabling. (Verify class-compliance + iOS before
  buying — product knowledge has a cutoff.)

## Test 3 — Everything at once (the pre-gig test)

Ethernet + Fantom + audio + charging on one powered dock (15 Pro). This is the
remaining unknown: whether the iPhone reliably drives all of it through one hub
simultaneously (iPadOS does; iPhone is less exercised). **Do this on the bench
before a real gig, not on stage.**

1. [ ] Dock powered (PD in), all four connected
2. [ ] Repeat Test 1 (frames to panels) and Test 2 (Fantom triggers) together
3. [ ] Confirm clip audio comes out the 3.5 mm adapter (route ≠ Lautsprecher)
4. [ ] Confirm the phone is charging

**Pass:** frames flow, MIDI triggers, audio reaches the PA, phone charges — all
at the same time.

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
