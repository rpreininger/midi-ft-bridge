// iPixel Color BLE panel (32x16) through the ESP32-C6's BLE controller
// (esp_hosted HCI over SDIO, NimBLE host on the P4). Same protocol as the Mac
// app (src/ble_sender_macos.mm): frames as PNG, 12 KB windows with a
// header, written in MTU-sized GATT writes with response, one ACK
// notification per window.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool enabled;
    char name[32];  // advertised name, case-insensitive substring
    int src_x, src_y, src_w, src_h;
    int brightness;  // 0..100
    int max_fps;
} ble_panel_cfg_t;

void ble_panel_start(const ble_panel_cfg_t *cfg);
// From the FT sender for every canvas: takes a frame when the panel is
// ready for one (rate-limited to max_fps), otherwise returns at once.
void ble_panel_offer(const uint8_t *canvas, int canvas_w);

typedef struct {
    bool connected;
    float fps;            // frames delivered per second (last 10 s)
    uint32_t frames, ack_timeouts, reconnects;
} ble_panel_status_t;
void ble_panel_status(ble_panel_status_t *st);
