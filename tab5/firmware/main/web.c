// Tiny control page on port 80: volume now, more later (clip upload,
// status). Open http://192.168.10.1/ from any device on the Tab5 hotspot.
#include "web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "web";
static int s_volume;

static const char PAGE[] =
    "<!DOCTYPE html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Tab5</title><style>body{font-family:sans-serif;margin:2em;background:#111;color:#eee}"
    "input{width:100%%}button{font-size:1.4em;margin:.3em;padding:.4em 1em}</style></head><body>"
    "<h2>Tab5 Playout</h2><p>Lautst&auml;rke: <b id=v>%d</b></p>"
    "<input type=range min=0 max=100 value=%d oninput=\"v.textContent=this.value\" "
    "onchange=\"fetch('/volume?v='+this.value)\">"
    "<p><button onclick=\"fetch('/volume?v=0').then(()=>location.reload())\">Stumm</button></p>"
    "</body></html>";

static esp_err_t root_get(httpd_req_t *req)
{
    char buf[sizeof PAGE + 16];
    snprintf(buf, sizeof buf, PAGE, s_volume, s_volume);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t volume_get(httpd_req_t *req)
{
    char q[32], val[8];
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK &&
        httpd_query_key_value(q, "v", val, sizeof val) == ESP_OK) {
        int v = atoi(val);
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        s_volume = v;
        board_audio_set_volume(v);
        ESP_LOGI(TAG, "volume %d", v);
    }
    char out[16];
    snprintf(out, sizeof out, "%d\n", s_volume);
    return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
}

void web_start(int initial_volume)
{
    s_volume = initial_volume;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.core_id = 0;
    httpd_handle_t h;
    if (httpd_start(&h, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "http server failed");
        return;
    }
    static const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get};
    static const httpd_uri_t vol = {.uri = "/volume", .method = HTTP_GET, .handler = volume_get};
    httpd_register_uri_handler(h, &root);
    httpd_register_uri_handler(h, &vol);
    ESP_LOGI(TAG, "control page on port 80");
}
