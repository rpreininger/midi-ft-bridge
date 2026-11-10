# Tab5 as live playout device (replacement for the MacBook)

Goal: M5Stack Tab5 (ESP32-P4) does the whole job on stage: USB-MIDI from the
Fantom in (USB-A host port), plays the 288x128@25 clips from microSD, audio
out via its codec, FT/UDP streams to the three panels, possibly acting as the
Wi-Fi hotspot itself.

## Status (2026-11-05, late)

**The Tab5 does the MacBook's job** (branch `tab5-playout`, `firmware/`):
Fantom pad (USB-MIDI) -> clip from SD with audio (audio = master clock) ->
FT/UDP to the three panels at 25 fps, as its own Wi-Fi hotspot, with a live
preview on its display. Not yet tested with the real panels.

Hardware: ESP32-P4 rev v1.3, 16 MB flash, 32 MB PSRAM (200 MHz), 360 MHz;
display ST7121 (touch fw 1 @ I2C 0x55); Wi-Fi/BLE via ESP32-C6 (esp_hosted
1.4.0 over SDIO, C6 firmware V1.4.1). Factory firmware backup on the Mac:
`~/esp/tab5-backup/tab5-factory-16MB.bin`.

Measured on the device:
- Decode from SD, all 14 clips: 0.3-6.5 ms avg, worst 12 ms (budget 40 ms).
- SD (32 GB SDHC, 4-bit 40 MHz): 16.5 MB/s with read() into DMA-capable
  internal RAM. Never read SD through stdio or into PSRAM (80 KB/s resp.
  per-sector bounce copies).
- Full load (playback + audio + preview + FT streams of all 3 panels,
  19.7 Mbit/s, to an iPhone on the hotspot): 25.0 fps, 0 lost tiles, 0 late
  frames. Sending costs ~20-23 ms/frame on its own task (core 1).
- Panels absent: a panel that does not answer ARP is skipped per frame
  (0.9 ms instead of stalling everything).
- Fantom (0582:0221, class-compliant MIDI, pads on channel 10): note to
  clip start 15-45 ms.

Behaviour (`main/main.c`, `main/player.c`):
- `/sdcard/config.json` in the Mac app's schema (+ `"tab5"` block, see
  `main/config.h`); without it: notes 36-49 -> Intro..Axel, stop note 51,
  hotspot `FT-TAB5` / `flaschentaschen`, 192.168.10.1, and the streams are
  mirrored to the first hotspot client (test mode).
- Clips: `/sdcard/ftc/<Name>.ftc` (transcode with `tools/ftc_encode.py`).
- Volume: web page `http://192.168.10.1/`, Fantom CC7, or config. Kept per
  output: jack/line out default 85 (keep it high, set the level on the
  mixer), internal speaker default 20. A cable in the jack switches the
  speaker off. MIDI input defaults to devices named "FANTOM" (`midi_device`).
- Web page `http://192.168.10.1/` (verified on the iPhone): live status,
  clip list with play/stop, volume, upload (`POST /upload?name=X.ftc`,
  checked for the FTC1 magic, written via a temp file) and delete.
- Hotspot DHCP: own server (`main/dhcpd.c`, IDF's dhcps has no reservations)
  with fixed leases for the panels, same IPs as on the Mango/Cudy
  (A .21, B .22, C .20 by MAC); others get .100-.199. Override with
  `"leases"` in the `tab5` block. SSID/password can be stored on the device
  (console `wifi <ssid> <password>`, then `reboot`) so it stays out of git.
- USB-C console (115200, dtr/rts off): `play <Name>`, `stop`, `heap`,
  `wifi ...`, `reboot`, `stress <s> [n]` (random clip every s seconds, a stop every 5th round).
- `/sdcard/bench` -> run the decode/SD benchmark instead;
  `/sdcard/streamtest` -> the 60 s streaming load test.

Audio out:
- Now: Tab5 headphone jack (ES8388, ~96 dB SNR, 24-bit DAC; clips are 16-bit)
  analog into a stereo line input of the AG06. Verified working. Check for
  ground-loop hum (run the Tab5 on battery or use an isolator) and Wi-Fi
  buzz at gig volume. Both ES8388 output pairs run at 0 dB analog (the codec
  driver leaves OUT2 at -30 dB).
- **USB audio to the AG06 works** (`main/usb_audio.c`, own UAC2 driver; IDF's
  usb_host_uac is UAC1 only). AG06MK2 = Yamaha 0499:1757, high speed, behind
  its own internal hub; stereo 24-bit in 4-byte subslots, iso async OUT 0x01.
  The explicit feedback endpoint (0x81, every 128 microframes) cannot be
  scheduled by IDF's HCD (64-entry isoc list), so the capture stream (0x82)
  is opened as implicit feedback: OUT packets follow the frames the AG06
  delivers -> the AG06 is the clock (measured 48.000 kHz, error stable).
  Dropouts fixed (2026-11-08): the HCD chains isoc transfers from its ISR;
  when that ISR is >125 us late, the first packets of the next transfer were
  "not executed" (clicks, then 0.5 s distortion as the AG06's buffer ran dry).
  Fixes: USB interrupt at level 3 on core 1, 6 ms transfers,
  `components/usb` = IDF v5.5.1 usb component with a patch in `hcd_dwc.c`
  (late buffers are retimed instead of skipped, and the left-out microframes
  are reported), and the rate controller makes up for them (proportional,
  up to 12 frames per transfer; capture count interpolated). Measured on
  Intro: 0 lost packets, error -0.3..3.4 frames. Console `tone <s>` plays
  1 kHz and counts glitches via the AG06 LOOPBACK capture; `toneclip <Name>
  <s>` does the same while a clip plays (video, preview, streams = real load).
  That test found the real culprits under load: the display preview did one
  1.2 MB cache write-back per frame (now drawn in 8 KB bands) and the FT
  sender shared core 1 with the USB ISR (now core 0). Result: tone under load
  1-3 glitches per 30 s right after boot (was 200-500). Caveat: LOOPBACK also
  records the AG06 inputs, so room noise counts too. Console `diag <nosend>
  <preview_div>` switches the loads for such tests. Also fixed: the rate
  controller lost precision after ~6 min (float of 2^24+ frame counters).
  When it is plugged in, audio goes to USB only (jack silent, volume at the
  mixer); unplugged -> back to the jack. Config `"usb_audio": "AG06"`
  (default; `"off"` = jack only). `"usb_gain_db": -6` (default): at 0 dB
  loud masters (Intro peaks at -0.2 dBFS) sounded distorted through the
  AG06 although the stream is bit-exact (verified with `rec <s>`, which
  records sent + loopback-captured audio and dumps it as base64 on the
  console; `usbgain <dB>` changes it live). Fantom + AG06 need a hub on USB-A.
- **Through the Fantom in VENDOR mode** (most promising single-cable path):
  it then enumerates as 0582:0283, USB 2.0 high speed, class 0xFF but with
  plain UAC1 format descriptors (+ Roland 0xF1 extras):
  - playback: interface 1, alt 1 = 4 ch / alt 2 = 2 ch, 32-bit slots,
    48 kHz, iso async OUT ep 0x0D (mps 116 / 60), no feedback endpoint
    -> implicit feedback from the capture stream (as Linux does for Roland)
  - capture: interface 2, alt 1 = 32 ch / alt 2 = 2 ch, iso IN ep 0x8E
  - MIDI: interface 3 (0xFF/0x03), bulk 0x84 in, same USB-MIDI packets -
    **works already** (pads play clips in VENDOR mode).
  To do: iso OUT driver with implicit feedback; the Fantom becomes the
  audio master clock. Downside: the Mac then needs Roland's driver.
  In GENERIC mode the Fantom is USB 1.1 full speed, MIDI only.
- Not the USB-C port: full-speed only, shares the PHY with flashing/console,
  and it is unknown whether it can source 5 V.

Flashing notes:
- Download mode: hold reset ~2 s until the green LED blinks fast, release.
  Only worked on one USB-C port of the Mac (`/dev/cu.usbmodem3101`). Once our
  firmware runs, `idf.py flash` works via auto-reset.
- After a long-press the strap stays latched: USB resets boot into download
  mode until one SHORT press of the reset button.
- Open the console with dtr=rts=False (pyserial defaults reset into
  download mode).

Done: status on the display, battery, web page (status/play/stop/volume/
upload/delete), MIDI hot-plug (devices behind a hub come and go).

Real panels (2026-11-10): all three Pi panels join the Tab5 hotspot
(SSID strato_accesspoint, fixed leases .20/.21/.22) and show the clips;
25 fps, ~19 ms send time per frame, no lost tiles. Without a config.json
the streams go to the panels as soon as one joins (otherwise to the first
phone, as a test load).

Panel shutdown: web page "Panels ausschalten" (or console `paneloff`;
`panelping` checks the endpoints) calls the panels' HTTP endpoint
(setup/panel-shutdown-service.sh, port 8081). The shared token is never in
git: `shutdown_token` in config.json, or stored on the device with the
console command `token <hex>` (NVS, survives reflashing). Verified
2026-11-10: all three panels powered off.

Next steps:
1. Long run with the real panels; Fantom + AG06 through a USB 2.0 hub.
2. Measure the Wi-Fi upload speed of a big clip; panel shutdown button.
3. Stage hardening: power loss during upload, battery runtime.
4. Internal DMA heap still shrinks slowly (188 KB idle at boot -> ~59 KB
   after 6 h); harmless now that the SD buffers are allocated once, but
   find the consumer (Wi-Fi/lwIP?).
5. BLE panel (iPixel) via the C6: the C6 firmware reports "HCI over SDIO,
   BLE only", so a NimBLE host on the P4 can drive it (started, paused).
6. Optional: Fantom VENDOR-mode audio.

## Tooling

- ESP-IDF v5.5.1 in `~/esp/esp-idf-v5.5` (M5's demo uses v5.4.2), target `esp32p4`:
  `. ~/esp/esp-idf-v5.5/export.sh && cd firmware && idf.py build flash`
- Transcoder: `python3 -m venv .venv && .venv/bin/pip install numpy lz4`,
  needs ffmpeg on PATH (works on Windows too).
