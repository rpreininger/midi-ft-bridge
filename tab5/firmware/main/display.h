#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "font.h"

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

// ---- touch UI (ui.c) ----
// All coordinates landscape (1280 x 720), buffers landscape RGB565 row-major.

// Where a canvas rectangle (or the whole preview) sits on the screen.
void display_canvas_rect(int cx, int cy, int cw, int ch, int *lx, int *ly, int *w, int *h);
void display_preview_rect(int *lx, int *ly, int *w, int *h);
// Reserve a canvas rectangle of the preview for an overlay (the song list):
// every preview frame shows the overlay there instead of the canvas. Once;
// false if the rectangle is not inside the canvas (or no display).
bool display_set_overlay(int cx, int cy, int cw, int ch);
// New overlay content (size of the reserved rectangle); drawn at once
// unless the preview is paused.
void display_overlay(const uint16_t *land);
// Draw a buffer at (lx, ly). One caller at a time (the UI task).
void display_draw_land(int lx, int ly, int w, int h, const uint16_t *land);
// Pause the preview while something covers it; false = resume and redraw
// the last frame (with the overlay).
void display_pause_preview(bool pause);
uint16_t display_rgb565(uint32_t rgb);
// Text into a bw x bh buffer, top-left (x, y), fg anti-aliased over bg,
// cut off before max_x. Returns the x after the last glyph drawn.
int display_text(uint16_t *buf, int bw, int bh, const font_t *f, int x, int y, int max_x,
                 const char *t, uint32_t fg, uint32_t bg);
int display_text_width(const font_t *f, const char *t);
