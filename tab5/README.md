# Tab5 as live playout device (replacement for the MacBook)

Goal: M5Stack Tab5 (ESP32-P4) does the whole job on stage: USB-MIDI from the
Fantom in (USB-A host port), plays the 288x128@25 clips from microSD, audio
out via its codec, FT/UDP streams to the three panels, possibly acting as the
Wi-Fi hotspot itself.

## Status (2026-11-05, evening)

Hardware: ESP32-P4 rev v1.3, 16 MB flash, 32 MB PSRAM (200 MHz), 360 MHz.
Factory firmware backed up on the Mac: `~/esp/tab5-backup/tab5-factory-16MB.bin`.

Measured on the device (`firmware/`, bench in `main/main.c`):
- Decode, worst-case content (Neutrino excerpt from flash): 5.1 ms avg,
  5.7 ms worst per frame of a 40 ms budget.
- Real time with audio as master clock (ES8388, I2S 48 kHz): 24.8 fps,
  0 late frames, audio clock 48.01 kHz.
- SD (32 GB SDHC, 4-bit, 40 MHz): raw 18 MB/s, POSIX read() 16.5 MB/s;
  need is ~1.5 MB/s. All 14 clips are on the card in `/ftc`.
- Decoding from SD through stdio FILE was 6-11 ms/frame, worst 20 ms:
  newlib fread is pathological on the FAT VFS (unbuffered: 80 KB/s; every
  fseek drops the buffer). **ftc.c was just switched to its own read()-based
  reader** (verified bit-exact on the Mac, builds for the P4, NOT yet run
  on the device). Next: flash + rerun the bench.

Flashing notes:
- Download mode: hold reset ~2 s until the green LED blinks fast, release.
  Only worked on a different USB-C port of the Mac (`/dev/cu.usbmodem3101`).
  After that, flash with our firmware works via auto-reset (it keeps the
  USB-Serial-JTAG console).
- After a long-press the strap stays latched: USB resets boot into
  download mode until one SHORT press of the reset button.
- Opening the port with pyserial defaults (DTR/RTS asserted) resets the
  chip into download mode; open with dtr=rts=False.
- Bench volume is 20/100 (user cannot turn it down on the device).

Next steps:
1. Rerun bench with the new reader; full 30 s real time from SD.
2. Wi-Fi via the ESP32-C6 (esp_hosted 1.4.0 / esp_wifi_remote 0.8.5 like
   M5's demo; SDIO pins CLK12 CMD13 D0-D3 11,10,9,8, reset GPIO15).
   Try SoftAP first (no credentials), measure FT/UDP throughput (~20 Mbit/s
   for all panels at 25 fps, 3-row tiles like `src/ft_sender.cpp`). Gig
   router is the Cudy later. Also: clip upload over Wi-Fi.
3. USB-MIDI host on the USB-A port (Fantom).
4. Audio over USB to the Fantom: probably not (IDF UAC host = UAC1);
   headphone jack is the safe path.
5. config.json on SD (mappings, panels, volume), display preview.

## Tooling

- ESP-IDF v5.5.1 (M5's demo recommends v5.4.2), target `esp32p4`.
- Transcoder: `python3 -m venv .venv && .venv/bin/pip install numpy lz4`,
  needs ffmpeg on PATH (works on Windows too).
