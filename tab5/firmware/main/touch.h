// Tab5 touch (Sitronix controller in the ST7121/ST7123 panel, I2C 0x55,
// 16-bit registers; protocol as in espp/st7123touch and M5GFX Touch_ST7123).
// Taken over from the m5-bass-synth firmware, where it runs on this same unit.
// Coordinates are converted to the landscape screen (1280 x 720) the way
// display.c draws it.
#pragma once

#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

esp_err_t touch_init(i2c_master_bus_handle_t bus);
// First finger: true while touching, x/y landscape.
bool touch_read(int *x, int *y);
