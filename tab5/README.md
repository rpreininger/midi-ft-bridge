# Tab5 as live playout device (replacement for the MacBook)

Goal: M5Stack Tab5 (ESP32-P4) does the whole job on stage: USB-MIDI from the
Fantom in (USB-A host port), plays the 288x128@25 clips from microSD, audio
out via its codec, FT/UDP streams to the three panels, possibly acting as the
Wi-Fi hotspot itself.

## Status (2026-11-05)

Done:
- Clip format FTC1 (`FORMAT.md`): lossless RGB888, LZ4 on XOR-delta, raw PCM.
- `tools/ftc_encode.py` transcoder; `tools/ftc_verify.c` host check.
  Decoder (`firmware/components/ftc`) verified **bit-exact** against ffmpeg
  (video + audio MD5) on Intro and Pixel.
- All 14 clips transcoded: 2.0 GB total, peak video rate 1.05 MB/s
  (Neutrino), typ. 0.2-0.9 MB/s; plus 192 KB/s audio. Any class-10 SD works.
  Output goes to `clips/ftc/` (git-ignored, regenerate).

Blocked:
- Flashing. On macOS the Tab5 enumerates as "Generic CDC" (the demo
  firmware's TinyUSB CDC, ignores esptool). Long-press reset (LED blinks
  ~10 s) only reboots into the demo; the ROM's "USB JTAG_serial debug unit"
  port never stayed up long enough for esptool to sync. Next: try from
  Windows / M5Burner, check cable/hub, look for a separate BOOT button.

Next steps once flashing works:
1. Bench firmware: SD read throughput, FTC decode time per frame, display.
2. Audio out (ES8388 via I2S) with audio-master sync.
3. Wi-Fi via the ESP32-C6 co-processor (esp_hosted): STA to the Cudy and
   SoftAP; measure FT/UDP throughput (~2.5 MB/s needed for all panels at
   25 fps, 3-row tiles like `src/ft_sender.cpp`). 2.4 GHz only (C6), so the
   airtime issue from `WISSEN/wlan-panel-stutter.md` applies.
4. USB-MIDI host (class driver over usb_host, 4-byte USB-MIDI packets).

## Tooling

- ESP-IDF v5.5.1 (M5's demo recommends v5.4.2), target `esp32p4`.
- Transcoder: `python3 -m venv .venv && .venv/bin/pip install numpy lz4`,
  needs ffmpeg on PATH (works on Windows too).
