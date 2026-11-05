#pragma once

#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

esp_err_t display_init(i2c_master_bus_handle_t bus, int canvas_w, int canvas_h);
// Show an RGB888 canvas (NULL = black). Non-blocking; frames are dropped
// while the previous one is still being drawn.
void display_preview(const uint8_t *rgb);
void display_backlight(int percent);

typedef struct {
    int x;            // landscape x in px; negative = right-aligned, -N px from the right edge
    const char *text; // ASCII
    uint32_t color;   // 0xRRGGBB
} display_span_t;

// Redraw a status band: 0 = above the preview, 1 = below it.
void display_status(int band, const display_span_t *spans, int n);
