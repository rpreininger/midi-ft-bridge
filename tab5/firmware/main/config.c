#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "config";

static void defaults(config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->video_w = 288;
    c->video_h = 128;
    c->midi_channel = -1;
    c->stop_note = -1;
    c->volume = 20;
    c->net.ap = true;
    strcpy(c->net.ssid, "FT-TAB5");
    strcpy(c->net.password, "flaschentaschen");
    c->net.channel = 6;
    strcpy(c->net.ap_ip, "192.168.10.1");
}

static int geti(const cJSON *o, const char *k, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

static void jstr(const cJSON *o, const char *k, char *dst, size_t cap)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(v) && v->valuestring) strlcpy(dst, v->valuestring, cap);
}

// "mp4/Intro.mp4" -> "/sdcard/ftc/Intro.ftc"; a name already ending in .ftc
// is taken relative to /sdcard/ftc as is.
static void resolve_clip(const char *in, char *out, size_t cap)
{
    const char *base = strrchr(in, '/');
    base = base ? base + 1 : in;
    char stem[80];
    strlcpy(stem, base, sizeof stem);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = 0;
    snprintf(out, cap, "/sdcard/ftc/%s.ftc", stem);
}

bool config_load(config_t *c, const char *path)
{
    defaults(c);
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "%s not found, using defaults", path);
        return true;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *txt = malloc(n + 1);
    if (!txt) { fclose(f); return false; }
    fread(txt, 1, n, f);
    txt[n] = 0;
    fclose(f);
    cJSON *root = cJSON_Parse(txt);
    free(txt);
    if (!root) {
        ESP_LOGE(TAG, "%s: invalid JSON near '%.20s'", path, cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "?");
        return false;
    }

    c->video_w = geti(root, "video_width", c->video_w);
    c->video_h = geti(root, "video_height", c->video_h);
    c->midi_channel = geti(root, "midi_channel", c->midi_channel);
    c->stop_note = geti(root, "stop_note", c->stop_note);

    const cJSON *p;
    cJSON_ArrayForEach(p, cJSON_GetObjectItemCaseSensitive(root, "panels")) {
        char type[16] = "ft";
        jstr(p, "type", type, sizeof type);
        if (strcmp(type, "ft") != 0) continue;  // BLE panel: not on the Tab5 (yet)
        if (c->npanels >= CFG_MAX_PANELS) break;
        cfg_panel_t *q = &c->panels[c->npanels++];
        jstr(p, "name", q->name, sizeof q->name);
        jstr(p, "ip", q->ip, sizeof q->ip);
        q->port = geti(p, "port", 1337);
        q->src_x = geti(p, "src_x", 0);
        q->src_y = geti(p, "src_y", 0);
        q->src_w = geti(p, "src_w", c->video_w);
        q->src_h = geti(p, "src_h", c->video_h);
    }

    const cJSON *m;
    cJSON_ArrayForEach(m, cJSON_GetObjectItemCaseSensitive(root, "mappings")) {
        int note = geti(m, "note", -1);
        char clip[96] = "";
        jstr(m, "clip", clip, sizeof clip);
        if (note < 0 || note > 127 || !clip[0] || c->nmappings >= CFG_MAX_MAPPINGS) continue;
        c->mappings[c->nmappings].note = note;
        resolve_clip(clip, c->mappings[c->nmappings].clip, sizeof c->mappings[0].clip);
        c->nmappings++;
    }

    const cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "tab5");
    if (cJSON_IsObject(t)) {
        char mode[8] = "";
        jstr(t, "wifi_mode", mode, sizeof mode);
        if (mode[0]) c->net.ap = strcmp(mode, "sta") != 0;
        jstr(t, "ssid", c->net.ssid, sizeof c->net.ssid);
        jstr(t, "password", c->net.password, sizeof c->net.password);
        jstr(t, "ap_ip", c->net.ap_ip, sizeof c->net.ap_ip);
        c->net.channel = geti(t, "channel", c->net.channel);
        c->volume = geti(t, "volume", c->volume);
    }
    cJSON_Delete(root);
    return true;
}

void config_log(const config_t *c)
{
    ESP_LOGI(TAG, "canvas %dx%d, midi ch %d, stop note %d, volume %d", c->video_w, c->video_h,
             c->midi_channel, c->stop_note, c->volume);
    ESP_LOGI(TAG, "wifi %s '%s' ch %d", c->net.ap ? "AP" : "STA", c->net.ssid, c->net.channel);
    for (int i = 0; i < c->npanels; i++)
        ESP_LOGI(TAG, "panel %s %s:%u src %d,%d %dx%d", c->panels[i].name, c->panels[i].ip, c->panels[i].port,
                 c->panels[i].src_x, c->panels[i].src_y, c->panels[i].src_w, c->panels[i].src_h);
    for (int i = 0; i < c->nmappings; i++) ESP_LOGI(TAG, "note %u -> %s", c->mappings[i].note, c->mappings[i].clip);
}
