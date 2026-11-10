// Panel shutdown over the panels' HTTP endpoint (setup/panel-shutdown-service.sh,
// the same path as the iOS app): GET http://<panel>:<port>/shutdown?token=<t>,
// GET /ping to check that the endpoint answers. The Tab5 has no ssh client,
// so without a token there is no shutdown.
//
// The token is a shared secret and never in git: it comes from config.json
// ("shutdown_token", like the Mac config) or is stored on the device with the
// console command "token <hex>" (NVS namespace "panel").
#include "panel_off.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "paneloff";
static const config_t *s_cfg;
static char s_token[80];
static int s_port = 8081;

void panel_off_init(const config_t *cfg)
{
    s_cfg = cfg;
    s_port = cfg->shutdown_port > 0 ? cfg->shutdown_port : 8081;
    strlcpy(s_token, cfg->shutdown_token, sizeof s_token);
    if (!s_token[0]) {
        nvs_handle_t h;
        size_t n = sizeof s_token;
        if (nvs_open("panel", NVS_READONLY, &h) == ESP_OK) {
            if (nvs_get_str(h, "token", s_token, &n) != ESP_OK) s_token[0] = 0;
            nvs_close(h);
        }
    }
    ESP_LOGI(TAG, "panel shutdown %s (port %d)", s_token[0] ? "armed" : "off - no token ('token <hex>')", s_port);
}

bool panel_off_has_token(void) { return s_token[0] != 0; }

bool panel_off_set_token(const char *t)
{
    nvs_handle_t h;
    if (nvs_open("panel", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = (t[0] ? nvs_set_str(h, "token", t) : nvs_erase_key(h, "token")) == ESP_OK || !t[0];
    nvs_commit(h);
    nvs_close(h);
    if (ok) strlcpy(s_token, t, sizeof s_token);
    return ok;
}

// HTTP status of GET http://ip:port/path, -1 = no answer
static int get(const char *ip, const char *path)
{
    char url[192];
    snprintf(url, sizeof url, "http://%s:%d%s", ip, s_port, path);
    esp_http_client_config_t c = {.url = url, .timeout_ms = 3000, .disable_auto_redirect = true};
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) return -1;
    int code = esp_http_client_perform(h) == ESP_OK ? esp_http_client_get_status_code(h) : -1;
    esp_http_client_cleanup(h);
    return code;
}

static int each(const char *path, const char *ok_word, char *rep, int cap)
{
    int ok = 0, k = 0;
    rep[0] = 0;
    for (int i = 0; i < s_cfg->npanels; i++) {
        const cfg_panel_t *p = &s_cfg->panels[i];
        int code = get(p->ip, path);
        if (code == 200) ok++;
        const char *what = code == 200 ? ok_word : code == 403 || code == 401 ? "Token falsch" : code < 0 ? "keine Antwort" : "Fehler";
        if (k < cap) k += snprintf(rep + k, cap - k, "%s%s (%s): %s", i ? "\n" : "", p->name, p->ip, what);
        ESP_LOGI(TAG, "%s %s%s -> %d", p->name, p->ip, strncmp(path, "/shutdown", 9) ? path : "/shutdown", code);
    }
    return ok;
}

int panel_off_ping(char *rep, int cap) { return each("/ping", "bereit", rep, cap); }

int panel_off_all(char *rep, int cap)
{
    if (!s_token[0]) {
        snprintf(rep, cap, "Kein Token gespeichert (Konsole: token <hex>)");
        return 0;
    }
    char path[128];
    snprintf(path, sizeof path, "/shutdown?token=%s", s_token);
    return each(path, "f\xC3\xA4hrt herunter", rep, cap);
}
