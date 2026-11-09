// USB Audio Class 2.0 output on the USB-A port (Yamaha AG06/AG06MK2 and
// similar UAC2 interfaces with a 48 kHz stereo isochronous OUT endpoint).
// Needs the USB host installed (midi_host_start). The device is the clock:
// OUT packet sizes follow the device's capture stream (implicit feedback).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// name_filter: substring of product/manufacturer or "vvvv:pppp" ("" = any
// UAC2 device; "off" = disabled).
esp_err_t usb_audio_start(const char *name_filter);
// true while a device is streaming; player audio then goes here, not I2S.
bool usb_audio_active(void);
const char *usb_audio_name(void);
// Frames between usb_audio_write() and the device (ring + queued transfers).
uint32_t usb_audio_latency_frames(void);
// Blocking write of interleaved s16 stereo 48 kHz PCM. false if the device
// went away (the caller falls back to I2S).
bool usb_audio_write(const void *pcm, size_t bytes);
// 1 kHz test tone for secs seconds; with the AG06 on LOOPBACK the capture
// stream is checked for dropouts (logged). Blocking.
void usb_audio_tone_test(int secs);
// Same check while a clip plays: its audio is replaced by the tone.
void usb_audio_tone_load(bool on);
// Keep this many extra frames queued in the device (more latency, more slack).
void usb_audio_set_cushion(int frames);
// Record <secs> s of the OUT stream and the capture stream, dump as base64.
void usb_audio_record(int secs);
// Digital gain on the USB output (0 dB = unchanged).
void usb_audio_set_gain_db(float db);
bool usb_audio_tone_replace(int16_t *buf, int frames);  // player hook
