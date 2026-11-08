// USB Audio Class 2.0 output on the USB-A port (Yamaha AG06/AG06MK2 and
// similar UAC2 interfaces with a 48 kHz stereo isochronous OUT endpoint).
// Needs the USB host installed (midi_host_start). The device is the clock:
// OUT packet sizes follow the device's capture stream (implicit feedback).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// name_filter: substring of product/manufacturer or "vvvv:pppp" ("" = any
// UAC2 device; "off" = disabled).
esp_err_t usb_audio_start(const char *name_filter);
// true while a device is streaming; player audio then goes here, not I2S.
bool usb_audio_active(void);
const char *usb_audio_name(void);
// Blocking write of interleaved s16 stereo 48 kHz PCM. false if the device
// went away (the caller falls back to I2S).
bool usb_audio_write(const void *pcm, size_t bytes);
