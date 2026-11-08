// Minimal DHCP server for the Tab5 hotspot with fixed leases (MAC -> IP),
// replacing ESP-IDF's dhcps, which has no reservations. The panels keep the
// same addresses as on the Mango/Cudy (.20/.21/.22), so every tool (Mac app,
// ssh, shutdown) works unchanged. Other clients get .100-.199.
#include "dhcpd.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "dhcpd";

#define POOL_FIRST 100
#define POOL_SIZE 100
#define LEASE_SECS 86400

typedef struct {
    uint8_t mac[6];
    uint32_t ip;  // network order
} lease_t;

static lease_t s_fixed[NET_MAX_LEASES];
static int s_nfixed;
static lease_t s_dyn[POOL_SIZE];
static uint32_t s_server, s_mask;
static dhcpd_cb_t s_cb;

static int fixed_lookup(const uint8_t *mac)
{
    for (int i = 0; i < s_nfixed; i++)
        if (!memcmp(s_fixed[i].mac, mac, 6)) return i;
    return -1;
}

static uint32_t assign(const uint8_t *mac, bool *fixed)
{
    int f = fixed_lookup(mac);
    *fixed = f >= 0;
    if (f >= 0) return s_fixed[f].ip;
    int free_slot = -1;
    for (int i = 0; i < POOL_SIZE; i++) {
        if (s_dyn[i].ip && !memcmp(s_dyn[i].mac, mac, 6)) return s_dyn[i].ip;
        if (!s_dyn[i].ip && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) free_slot = mac[5] % POOL_SIZE;  // pool full: reuse a slot
    uint32_t net = ntohl(s_server) & ntohl(s_mask);
    s_dyn[free_slot].ip = htonl(net | (POOL_FIRST + free_slot));
    memcpy(s_dyn[free_slot].mac, mac, 6);
    return s_dyn[free_slot].ip;
}

static const uint8_t *opt_find(const uint8_t *o, int len, uint8_t code, uint8_t *olen)
{
    int i = 0;
    while (i < len && o[i] != 255) {
        if (o[i] == 0) {
            i++;
            continue;
        }
        if (i + 1 >= len || i + 2 + o[i + 1] > len) break;
        if (o[i] == code) {
            *olen = o[i + 1];
            return o + i + 2;
        }
        i += 2 + o[i + 1];
    }
    return NULL;
}

static uint8_t *opt_u32(uint8_t *p, uint8_t code, uint32_t v_net)
{
    *p++ = code;
    *p++ = 4;
    memcpy(p, &v_net, 4);
    return p + 4;
}

static void task(void *arg)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(67), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) {
        ESP_LOGE(TAG, "bind :67 failed");
        close(s);
        vTaskDelete(NULL);
    }
    static uint8_t in[576], out[548];
    for (;;) {
        int n = recv(s, in, sizeof in, 0);
        if (n < 240 || in[0] != 1 || in[1] != 1 || in[2] != 6) continue;
        if (in[236] != 99 || in[237] != 130 || in[238] != 83 || in[239] != 99) continue;
        const uint8_t *opts = in + 240;
        int olen_all = n - 240;
        uint8_t l;
        const uint8_t *t = opt_find(opts, olen_all, 53, &l);
        if (!t || l != 1) continue;
        uint8_t type = t[0], reply;
        const uint8_t *mac = in + 28;
        bool fixed;
        uint32_t ip = assign(mac, &fixed);
        if (type == 1) {  // DISCOVER
            reply = 2;    // OFFER
        } else if (type == 3) {  // REQUEST
            const uint8_t *sid = opt_find(opts, olen_all, 54, &l);
            if (sid && l == 4 && memcmp(sid, &s_server, 4)) continue;  // chose another server
            uint32_t want = 0;
            const uint8_t *rq = opt_find(opts, olen_all, 50, &l);
            if (rq && l == 4) memcpy(&want, rq, 4);
            else memcpy(&want, in + 12, 4);  // ciaddr (renew)
            reply = want == ip ? 5 : 6;      // ACK / NAK
        } else {
            continue;  // RELEASE, DECLINE, INFORM: nothing to do
        }

        memset(out, 0, sizeof out);
        out[0] = 2;
        out[1] = 1;
        out[2] = 6;
        memcpy(out + 4, in + 4, 4);    // xid
        memcpy(out + 10, in + 10, 2);  // flags
        if (reply != 6) {
            memcpy(out + 16, &ip, 4);  // yiaddr
            memcpy(out + 20, &s_server, 4);
        }
        memcpy(out + 28, mac, 16);
        out[236] = 99, out[237] = 130, out[238] = 83, out[239] = 99;
        uint8_t *p = out + 240;
        *p++ = 53, *p++ = 1, *p++ = reply;
        p = opt_u32(p, 54, s_server);
        if (reply != 6) {
            p = opt_u32(p, 51, htonl(LEASE_SECS));
            p = opt_u32(p, 1, s_mask);
            p = opt_u32(p, 3, s_server);
            p = opt_u32(p, 6, s_server);
        }
        *p++ = 255;
        struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(68), .sin_addr.s_addr = htonl(INADDR_BROADCAST)};
        sendto(s, out, 300 > p - out ? 300 : p - out, 0, (struct sockaddr *)&to, sizeof to);

        if (reply == 5) {
            char ipstr[16], host[33] = "";
            const uint8_t *h = opt_find(opts, olen_all, 12, &l);
            if (h) snprintf(host, sizeof host, "%.*s", l, (const char *)h);
            inet_ntoa_r(*(struct in_addr *)&ip, ipstr, sizeof ipstr);
            ESP_LOGI(TAG, "%02x:%02x:%02x:%02x:%02x:%02x %s -> %s%s", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                     host, ipstr, fixed ? " (fixed lease)" : "");
            if (s_cb) s_cb(ipstr, fixed);
        } else if (reply == 6) {
            ESP_LOGI(TAG, "NAK %02x:%02x:%02x:%02x:%02x:%02x (asked for another address)", mac[0], mac[1], mac[2],
                     mac[3], mac[4], mac[5]);
        }
    }
}

bool dhcpd_parse_mac(const char *s, uint8_t mac[6])
{
    unsigned m[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) return false;
    for (int i = 0; i < 6; i++) mac[i] = m[i];
    return true;
}

void dhcpd_start(const char *server_ip, const net_lease_t *leases, int n, dhcpd_cb_t cb)
{
    s_server = ipaddr_addr(server_ip);
    s_mask = htonl(0xFFFFFF00);
    s_cb = cb;
    s_nfixed = 0;
    for (int i = 0; i < n && s_nfixed < NET_MAX_LEASES; i++) {
        lease_t *f = &s_fixed[s_nfixed];
        if (!dhcpd_parse_mac(leases[i].mac, f->mac)) continue;
        f->ip = ipaddr_addr(leases[i].ip);
        ESP_LOGI(TAG, "fixed lease %s -> %s", leases[i].mac, leases[i].ip);
        s_nfixed++;
    }
    xTaskCreatePinnedToCore(task, "dhcpd", 4096, NULL, 5, NULL, 0);
}
