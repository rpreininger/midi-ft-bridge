#pragma once

#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

esp_err_t display_init(i2c_master_bus_handle_t bus, int canvas_w, int canvas_h);
// Show an RGB888 canvas (NULL = black). Non-blocking; frames are dropped
// while the previous one is still being drawn.
void display_preview(const uint8_t *rgb);
void display_backlight(int percent);
