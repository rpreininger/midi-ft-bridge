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
- `/sdcard/bench` -> run the decode/SD benchmark instead;
  `/sdcard/streamtest` -> the 60 s streaming load test.

Audio out:
- Now: Tab5 headphone jack (ES8388, ~96 dB SNR, 24-bit DAC; clips are 16-bit)
  analog into a stereo line input of the AG06. Verified working. Check for
  ground-loop hum (run the Tab5 on battery or use an isolator) and Wi-Fi
  buzz at gig volume. Both ES8388 output pairs run at 0 dB analog (the codec
  driver leaves OUT2 at -30 dB).
- USB audio to the AG06 is possible later on the **USB-A** port: AG06 =
  Yamaha 0499:1757 behind its own internal hub, **UAC 2.0**, stereo 24-bit in
  32-bit slots, async isochronous OUT (ep 0x01) with feedback (ep 0x81).
  Needs a custom UAC2 driver (IDF's usb_host_uac is UAC1 only) incl. feedback
  handling; then the AG06 becomes the master clock. Fantom + AG06 together
  need a USB hub on the USB-A port (hub support is enabled), and the MIDI
  driver must then pick the Fantom (the AG06 also has a MIDI interface).
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

Next steps:
1. Status on the display (clip, panels, MIDI, volume, Wi-Fi clients).
2. Test with the real panels (hotspot vs. joining the Cudy as STA).
3. Web page: clip upload over Wi-Fi, panel shutdown, status.
4. Stage hardening: autostart, power loss, MIDI hot-plug.
5. Optional: UAC2 driver for the AG06; BLE panel via the C6.

## Tooling

- ESP-IDF v5.5.1 in `~/esp/esp-idf-v5.5` (M5's demo uses v5.4.2), target `esp32p4`:
  `. ~/esp/esp-idf-v5.5/export.sh && cd firmware && idf.py build flash`
- Transcoder: `python3 -m venv .venv && .venv/bin/pip install numpy lz4`,
  needs ffmpeg on PATH (works on Windows too).
