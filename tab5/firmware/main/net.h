// Wi-Fi via the Tab5's ESP32-C6 co-processor (esp_hosted over SDIO, used
// through esp_wifi_remote, so the normal esp_wifi API applies).
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define NET_MAX_LEASES 8

typedef struct {
    char mac[18];  // "88:a2:9e:75:ff:eb"
    char ip[16];
} net_lease_t;

typedef struct {
    bool ap;              // true: Tab5 is the hotspot, false: join a network
    char ssid[33];
    char password[65];
    uint8_t channel;      // AP only (1..13)
    char ap_ip[16];       // AP only, e.g. "192.168.10.1"
    int nleases;          // AP only: fixed DHCP leases (the panels)
    net_lease_t leases[NET_MAX_LEASES];
} net_config_t;

// SSID/password saved in NVS (set over the console, never in git). Loaded
// over the built-in defaults; config.json still wins.
void net_load_saved(net_config_t *cfg);
esp_err_t net_save_wifi(const char *ssid, const char *password);  // "" ssid = forget

esp_err_t net_start(const net_config_t *cfg);
// Blocks until we have an IPv4 (STA) or the AP is up. false on timeout.
bool net_wait_ready(int timeout_ms);
const char *net_ip(void);
int net_ap_clients(void);
// IP the AP handed out last (NULL until a client has joined).
const char *net_ap_client_ip(void);
