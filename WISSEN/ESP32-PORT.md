# midi-ft-bridge on an ESP32 - Feasibility

**Status 2026-10-15:** assessed from the code, nothing built yet. First tests start
when the **M5Stack Tab5** arrives (ESP32-P4, ordered 2026-10-15 for the SID project in
`EZ-AG-hexmono`, doubles as the prototype here). The Fantom is not at the dev desk, so
every test that needs it happens on site.

Question: can a powerful ESP32 replace the Mac as the hub? Short answer: **yes — compute
is not the problem, the panel network is.**

## Load

| | per frame | at 25 fps |
|---|---|---|
| Panel A, 128x128 | 49,152 B, 43 UDP packets | 9.8 Mbit/s |
| Panels B and C, 128x64 each | 24,576 B, 22 packets | 4.9 Mbit/s each |
| **Total FT** | 98,304 B, 87 packets | **~20 Mbit/s, ~2,200 packets/s, continuous** |
| BLE panel, 32x16 | PNG, 244-byte writes, ACK per 12 KB window | 2-10 fps |

On top: decode 288x128 video at 25 fps, 44.1 kHz stereo audio, MIDI note-on, the
read-only web monitor.

## Mac part → ESP32 part

| Part | Mac today | ESP32 |
|---|---|---|
| MIDI | CoreMIDI, Fantom over USB | USB host, Fantom over USB (see MIDI below) |
| Video | AVFoundation, H.264 | MJPEG clips, P4 hardware JPEG decoder (see Clips) |
| Audio | CoreAudio → USB → Yamaha AG06 MK2 | I2S codec/DAC → analog line → AG06 line input |
| A/V sync | audio-master clock (AudioQueue sample count) | same idea, I2S sample count |
| FT panels | UDP, Mac wired to the AP | UDP over lwIP sockets — `ft_sender.cpp` ports almost 1:1 |
| BLE panel | CoreBluetooth | see BLE panel below |
| Monitor, config, preview | Swift app + `status_server` | web page (like the SID editor), config JSON on microSD |
| Engine logic | `engine.cpp` | stays: note → clip, region crop, per-panel `max_fps` |

## Network — the bottleneck

The Mac is wired to the AP, so every frame crosses the air once. A WLAN-connected
ESP32 would send every byte over the air **twice** (ESP32 → AP → panel). ESP32 radios,
including the Tab5's ESP32-C6, are 2.4 GHz only — the band panels B and C still use.

→ **Wire the ESP32 to the AP**, like the Mac: an **ESP32-P4 board with Ethernet**
(RMII, 100 Mbit). Ruled out: ESP32-S3 — no Ethernet MAC (a W5500 on SPI manages
~10-15 Mbit/s in practice), USB host only Full-Speed, and WLAN and BLE share one radio.

Network state: branch `cudy-5ghz-migration` (Cudy TR1200 dual-band, panel A on 5 GHz,
not merged). INSTALL.md / NOTES.md on `main` still describe the Mango.

The Tab5 has no Ethernet: good for testing with one panel and for measuring whether
WLAN is enough after all, before buying a second board.

## Clips

No ESP32 decodes H.264 in hardware (the P4 only encodes it). Convert offline instead:

- video: MJPEG, 288x128, 25 fps — decoded by the P4's JPEG engine
- audio: WAV, 44.1 kHz, 16-bit stereo

Container still open (AVI, or JPEG frames + an index file). Sketch, untested:

```bash
ffmpeg -i in.mp4 -vf scale=288:128 -r 25 -c:v mjpeg -q:v 3 -an out.avi
ffmpeg -i in.mp4 -vn -ac 2 -ar 44100 -c:a pcm_s16le out.wav
```

Clips live on the microSD card.

## Audio

Analog line out of the ESP32 into an AG06 line input — the AG06 no longer needs USB for
the bridge. USB audio to the AG06 is not worth it: on the ESP32-S3 in the SID project only
the AG06's internal USB hub (Microchip USB2422) enumerated, the audio device behind it
stayed invisible. A local I2S clock should also sync tighter than today's USB path.

## MIDI

The Fantom 06 goes over USB directly (not DIN). Open:

- The Fantom is USB **Hi-Speed**. The ESP32-S3 host is Full-Speed only; the P4 (Tab5
  USB-A) is Hi-Speed.
- Class-compliant MIDI streaming interface, or a Roland vendor class?

Starting point: the USB-MIDI host code from the SID project (`EZ-AG-hexmono`, branch
`sid-synth`); its `usb` console command dumps the device descriptors.

## BLE panel

The P4 has no Bluetooth of its own. Options:

- BLE through the ESP32-C6 co-processor (ESP-Hosted)
- a small ESP32-C3/S3 as BLE bridge, fed through the existing `ble_udp` panel type
  (`FTSender::sendRaw`: uint16 LE width + height, then RGB24)

lodepng runs on an ESP32; a 32x16 PNG is tiny.

## Test plan with the Tab5

The Tab5 kit runs on battery, so it can be taken to the Fantom and the panels.

1. **Fantom on USB-A:** `usb` descriptors, then check that note-ons arrive (miditrace).
2. **One clip from microSD:** MJPEG decode time per frame; WAV through the ES8388
   headphone jack into the AG06.
3. **Panels over WLAN:** one panel at 25 fps, then all three — measure loss and jitter.
   Decides whether an Ethernet P4 board is needed.
4. **BLE panel** through the C6, or a separate bridge.

## Effort

Engine logic stays. New: clip player, audio output, USB MIDI, BLE, the web UI that replaces
the Swift app (config editor, transport, preview), and the clip conversion script.
