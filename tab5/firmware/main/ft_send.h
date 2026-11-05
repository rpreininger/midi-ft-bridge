// Flaschen-Taschen UDP sender, same wire format as src/ft_sender.cpp:
// PPM P6 with "#FT: x y" offset, 3-row tiles so no packet is fragmented.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int sock;
    uint32_t addr;      // network order
    uint16_t port;
    uint8_t *pkt;       // packet scratch
    bool down;          // last frame failed: no retries until a tile gets through
    // stats
    uint32_t frames, packets, send_errors, retries;
} ft_sender_t;

int ft_sender_init(ft_sender_t *s, const char *ip, uint16_t port);
// Sends the src_w x src_h region at (src_x, src_y) of an RGB888 canvas that is
// canvas_w wide. Returns packets that could not be sent.
int ft_sender_send(ft_sender_t *s, const uint8_t *canvas, int canvas_w,
                   int src_x, int src_y, int src_w, int src_h);
