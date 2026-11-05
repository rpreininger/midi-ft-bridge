#include "ft_send.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#define TILE_ROWS 3
#define MAX_W 256

int ft_sender_init(ft_sender_t *s, const char *ip, uint16_t port)
{
    memset(s, 0, sizeof(*s));
    s->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s->sock < 0) return -1;
    s->addr = inet_addr(ip);
    s->port = htons(port);
    s->pkt = malloc(64 + MAX_W * TILE_ROWS * 3);
    return s->pkt ? 0 : -1;
}

int ft_sender_send(ft_sender_t *s, const uint8_t *canvas, int canvas_w,
                   int src_x, int src_y, int src_w, int src_h)
{
    if (src_w > MAX_W) return -1;
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = s->port, .sin_addr.s_addr = s->addr};
    int failed = 0;
    for (int y = 0; y < src_h; y += TILE_ROWS) {
        int th = src_h - y < TILE_ROWS ? src_h - y : TILE_ROWS;
        int hl = snprintf((char *)s->pkt, 64, "P6\n#FT: %d %d\n%d %d\n255\n", 0, y, src_w, th);
        uint8_t *p = s->pkt + hl;
        for (int r = 0; r < th; r++) {
            memcpy(p, canvas + ((size_t)(src_y + y + r) * canvas_w + src_x) * 3, (size_t)src_w * 3);
            p += src_w * 3;
        }
        int len = (int)(p - s->pkt);
        // lwIP answers ENOMEM when its pbuf pool / the hosted TX queue is
        // full; that is back-pressure, so yield briefly and retry a few times.
        int tries = 0;
        while (sendto(s->sock, s->pkt, len, 0, (struct sockaddr *)&to, sizeof to) < 0) {
            if ((errno != ENOMEM && errno != EAGAIN) || ++tries > 20) {
                failed++;
                s->send_errors++;
                break;
            }
            s->retries++;
            vTaskDelay(1);
        }
        s->packets++;
    }
    s->frames++;
    return failed;
}
