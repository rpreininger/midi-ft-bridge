// USB-MIDI host on the Tab5's USB-A port (class-compliant MIDI streaming
// interface, e.g. the Fantom in "Generic" USB driver mode).
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint8_t cable;    // USB-MIDI virtual cable (Fantom has several ports)
    uint8_t status;   // full status byte (type | channel)
    uint8_t d1, d2;
} midi_msg_t;

esp_err_t midi_host_start(void);
// Next message, waiting up to timeout_ms. false if none.
bool midi_host_get(midi_msg_t *m, int timeout_ms);
bool midi_host_connected(void);
