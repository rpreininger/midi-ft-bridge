#include "net.h"
#include "dhcpd.h"
#include "nvs.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_private/wifi.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_wifi_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/ip4_addr.h"
#include "nvs_flash.h"

static const char *TAG = "net";

#define READY_BIT BIT0

static EventGroupHandle_t s_ev;
static char s_ip[16] = "0.0.0.0";
static int s_clients;
static char s_client_ip[16];
static bool s_ap_netif_started;
static const net_config_t *s_cfg;

// Mirror/test target = the last client that is not a panel
static void lease_cb(const char *ip, bool fixed)
{
    if (!fixed) strlcpy(s_client_ip, ip, sizeof s_client_ip);
}

// esp_wifi_remote does not wire the AP netif up on its own; M5's Tab5 demo
// (hal_wifi.cpp, MIT) does it in the AP_START handler. Same here.
static void ap_start_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_netif_t *netif = arg;
    if (s_ap_netif_started || esp_netif_is_netif_up(netif)) return;
    wifi_netif_driver_t driver = esp_netif_get_io_driver(netif);
    uint8_t mac[6];
    if (esp_wifi_get_if_mac(driver, mac) != ESP_OK) return;
    if (esp_wifi_is_if_ready_when_started(driver)) esp_wifi_register_if_rxcb(driver, esp_netif_receive, netif);
    esp_wifi_internal_reg_netstack_buf_cb(esp_netif_netstack_buf_ref, esp_netif_netstack_buf_free);
    esp_netif_set_mac(netif, mac);
    esp_netif_action_start(netif, base, id, data);
    s_ap_netif_started = true;
    // IDF's dhcps has no reservations: replace it with ours (fixed panel IPs)
    esp_netif_dhcps_stop(netif);
    static bool dhcpd_running;
    if (!dhcpd_running) {
        dhcpd_running = true;
        dhcpd_start(s_cfg->ap_ip[0] ? s_cfg->ap_ip : "192.168.10.1", s_cfg->leases, s_cfg->nleases, lease_cb);
    }
    xEventGroupSetBits(s_ev, READY_BIT);
}

static void ap_stop_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_netif_action_stop(arg, base, id, data);
    s_ap_netif_started = false;
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case WIFI_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW(TAG, "STA disconnected (reason %d), reconnecting", d->reason);
        xEventGroupClearBits(s_ev, READY_BIT);
        esp_wifi_connect();
        break;
    }
    case WIFI_EVENT_AP_STACONNECTED:
        s_clients++;
        ESP_LOGI(TAG, "AP: client joined (%d)", s_clients);
        break;
    case WIFI_EVENT_AP_STADISCONNECTED:
        if (s_clients) s_clients--;
        ESP_LOGI(TAG, "AP: client left (%d)", s_clients);
        break;
    }
}

static void ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_AP_STAIPASSIGNED) {
        ip_event_ap_staipassigned_t *e = data;
        snprintf(s_client_ip, sizeof s_client_ip, IPSTR, IP2STR(&e->ip));
        ESP_LOGI(TAG, "AP: client got %s", s_client_ip);
        return;
    }
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "got IP %s", s_ip);
        xEventGroupSetBits(s_ev, READY_BIT);
    }
}

static void nvs_init(void)
{
    static bool done;
    if (done) return;
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        r = nvs_flash_init();
    }
    ESP_ERROR_CHECK(r);
    done = true;
}

void net_load_saved(net_config_t *cfg)
{
    nvs_init();
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return;
    char ssid[33] = "", pw[65] = "";
    size_t l1 = sizeof ssid, l2 = sizeof pw;
    if (nvs_get_str(h, "ssid", ssid, &l1) == ESP_OK && ssid[0]) {
        nvs_get_str(h, "pass", pw, &l2);
        strlcpy(cfg->ssid, ssid, sizeof cfg->ssid);
        strlcpy(cfg->password, pw, sizeof cfg->password);
        ESP_LOGI(TAG, "saved Wi-Fi '%s' from NVS", ssid);
    }
    nvs_close(h);
}

esp_err_t net_save_wifi(const char *ssid, const char *password)
{
    nvs_init();
    nvs_handle_t h;
    esp_err_t e = nvs_open("wifi", NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    if (ssid && ssid[0]) {
        nvs_set_str(h, "ssid", ssid);
        nvs_set_str(h, "pass", password ? password : "");
    } else {
        nvs_erase_all(h);
    }
    e = nvs_commit(h);
    nvs_close(h);
    return e;
}

esp_err_t net_start(const net_config_t *cfg)
{
    nvs_init();
    s_cfg = cfg;
    s_ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, ip_event, NULL));

    esp_netif_t *netif = NULL;
    if (cfg->ap) {
        esp_netif_config_t nc = ESP_NETIF_DEFAULT_WIFI_AP();
        netif = esp_netif_new(&nc);
        ESP_ERROR_CHECK(esp_netif_attach_wifi_ap(netif));
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START, ap_start_handler, netif));
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STOP, ap_stop_handler, netif));
        // our panel subnet instead of the default 192.168.4.1
        esp_netif_ip_info_t ip = {0};
        ip.ip.addr = ipaddr_addr(cfg->ap_ip[0] ? cfg->ap_ip : "192.168.10.1");
        ip.gw.addr = ip.ip.addr;
        ip.netmask.addr = ipaddr_addr("255.255.255.0");
        esp_netif_dhcps_stop(netif);
        esp_netif_set_ip_info(netif, &ip);
        esp_netif_dhcps_start(netif);
        snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&ip.ip));
    } else {
        netif = esp_netif_create_default_wifi_sta();
    }

    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wc));
    wifi_config_t w = {0};
    if (cfg->ap) {
        strlcpy((char *)w.ap.ssid, cfg->ssid, sizeof w.ap.ssid);
        w.ap.ssid_len = strlen(cfg->ssid);
        strlcpy((char *)w.ap.password, cfg->password, sizeof w.ap.password);
        w.ap.channel = cfg->channel ? cfg->channel : 6;
        w.ap.max_connection = 6;
        w.ap.authmode = cfg->password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &w));
    } else {
        strlcpy((char *)w.sta.ssid, cfg->ssid, sizeof w.sta.ssid);
        strlcpy((char *)w.sta.password, cfg->password, sizeof w.sta.password);
        w.sta.threshold.authmode = cfg->password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &w));
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    // Panel traffic is latency-sensitive: no modem sleep (same lesson as the
    // Pi Zero panels, see WISSEN/ / project notes on power_save).
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "%s '%s' starting", cfg->ap ? "AP" : "STA", cfg->ssid);
    return ESP_OK;
}

bool net_wait_ready(int timeout_ms)
{
    return xEventGroupWaitBits(s_ev, READY_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms)) & READY_BIT;
}

const char *net_ip(void) { return s_ip; }
int net_ap_clients(void) { return s_clients; }

const char *net_ap_client_ip(void) { return s_client_ip[0] ? s_client_ip : NULL; }
