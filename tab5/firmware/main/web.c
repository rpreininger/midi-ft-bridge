// Control page on port 80 (http://192.168.10.1/ on the Tab5 hotspot):
// live status, clip list with play/stop, volume, clip upload and delete.
//
//   GET  /                 page
//   GET  /status           JSON status
//   GET  /clips            JSON list of /sdcard/ftc/*.ftc with sizes
//   GET  /play?clip=Name   play /sdcard/ftc/Name.ftc
//   GET  /stop
//   GET  /volume?v=0..100  (applies to the active output: jack or speaker)
//   POST /upload?name=Name.ftc   raw body = the .ftc file
//   GET  /delete?name=Name.ftc
#include "web.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "board.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "midi_host.h"
#include "usb_audio.h"
#include "net.h"
#include "player.h"
#include "power.h"

static const char *TAG = "web";
static const config_t *s_cfg;

#define CLIP_DIR "/sdcard/ftc"

static const char PAGE[] =
    "<!DOCTYPE html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'><title>Tab5 Playout</title><style>"
    "body{font-family:-apple-system,sans-serif;margin:1em;background:#111;color:#ddd}"
    "h2{margin:.2em 0}.box{background:#1c1c1c;border-radius:8px;padding:.8em;margin:.6em 0}"
    ".ok{color:#4c6}.bad{color:#e44}.acc{color:#fc4}button{font-size:1.05em;margin:.15em;padding:.45em .8em;"
    "border:0;border-radius:6px;background:#333;color:#eee}button.play{background:#264}button.stop{background:#622}"
    "input[type=range]{width:100%}table{width:100%;border-collapse:collapse}td{padding:.25em}"
    "progress{width:100%}</style></head><body>"
    "<h2>Tab5 Playout</h2>"
    "<div class=box><div id=now>...</div><div id=pos></div></div>"
    "<div class=box><div id=links></div></div>"
    "<div class=box>Lautst&auml;rke <span id=out></span>: <b id=vol></b>"
    "<input id=vr type=range min=0 max=100 oninput=\"vol.textContent=this.value\" "
    "onchange=\"fetch('/volume?v='+this.value)\"></div>"
    "<div class=box><button class=stop onclick=\"fetch('/stop')\">&#9632; Stop</button>"
    "<table id=clips></table></div>"
    "<div class=box><b>Clip hochladen</b> (.ftc aus tools/ftc_encode.py)<br>"
    "<input type=file id=f accept='.ftc' multiple> <button onclick=up()>Hochladen</button>"
    "<progress id=pr value=0 max=1></progress><div id=um></div></div>"
    "<script>"
    "function t(s){var m=Math.floor(s/60),r=Math.floor(s%60);return m+':'+(r<10?'0':'')+r}"
    "var drag=false;vr.onpointerdown=()=>drag=true;vr.onpointerup=()=>drag=false;"
    "async function st(){try{var j=await (await fetch('/status')).json();"
    "now.innerHTML=j.playing?'<span class=acc>&#9654; '+j.clip+'</span>':'<span>gestoppt</span>';"
    "pos.textContent=j.playing?t(j.frame/j.fps)+' / '+t(j.frames/j.fps):'';"
    "var p=j.panels.map(x=>'<span class='+(j.mirrored?'acc':x.ok?'ok':'bad')+'>'+x.name+'</span>').join(' ');"
    "links.innerHTML='MIDI: <span class='+(j.midi?'ok':'bad')+'>'+(j.midi||'--')+'</span><br>WLAN '+j.ssid+': '"
    "+j.clients+' Client(s)<br>'+(j.mirrored?'Test-Modus (Streams an Client): ':'Panels: ')+p"
    "+(j.battery>=0?'<br>Akku: '+j.battery+'% ('+j.volts.toFixed(2)+' V)':'');"
    "out.textContent=j.usb?'(USB '+j.usb+' - Pegel am Mixer)':j.jack?'(Klinke)':'(Lautsprecher)';vol.textContent=j.volume;if(!drag)vr.value=j.volume;"
    "}catch(e){now.textContent='keine Verbindung'}}"
    "async function cl(){var j=await (await fetch('/clips')).json();"
    "clips.innerHTML=j.map(c=>'<tr><td><button class=play onclick=\"fetch(\\'/play?clip='+encodeURIComponent(c.name)"
    "+'\\')\">&#9654;</button></td><td>'+c.name+'</td><td>'+(c.size/1e6).toFixed(0)+' MB</td>"
    "<td><button onclick=\"del(\\''+c.name+'\\')\">&#10005;</button></td></tr>').join('')}"
    "async function del(n){if(confirm(n+' l\\u00f6schen?')){await fetch('/delete?name='+encodeURIComponent(n+'.ftc'));cl()}}"
    "function send(file){return new Promise((ok,bad)=>{var x=new XMLHttpRequest();"
    "x.open('POST','/upload?name='+encodeURIComponent(file.name));"
    "x.upload.onprogress=e=>{pr.value=e.loaded/e.total};x.onload=()=>x.status==200?ok(x.responseText):bad(x.responseText);"
    "x.onerror=()=>bad('Verbindung abgebrochen');x.send(file)})}"
    "async function up(){for(var file of f.files){um.textContent=file.name+' ...';var t0=Date.now();"
    "try{await send(file);um.textContent=file.name+' ok ('+(file.size/1e6/((Date.now()-t0)/1e3)).toFixed(1)+' MB/s)'}"
    "catch(e){um.textContent=file.name+': '+e}}cl()}"
    "st();cl();setInterval(st,1000);"
    "</script></body></html>";

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_get(httpd_req_t *req)
{
    player_status_t st;
    player_get_status(&st);
    power_state_t pw = {0};
    bool bat = power_read(&pw) && pw.present;
    char buf[1024];
    int n = snprintf(buf, sizeof buf,
                     "{\"playing\":%s,\"clip\":\"%s\",\"frame\":%u,\"frames\":%u,\"fps\":%u,\"volume\":%d,"
                     "\"jack\":%s,\"usb\":\"%s\",\"midi\":\"%s\",\"ssid\":\"%s\",\"clients\":%d,\"mirrored\":%s,"
                     "\"battery\":%d,\"volts\":%.2f,\"panels\":[",
                     st.playing ? "true" : "false", st.clip, (unsigned)st.frame, (unsigned)st.frames,
                     st.fps ? st.fps : 25, board_audio_get_volume(), board_audio_poll_jack() ? "true" : "false",
                     usb_audio_name(), midi_host_names(), s_cfg->net.ssid, net_ap_clients(), st.mirrored ? "true" : "false",
                     bat ? pw.percent : -1, bat ? pw.volts : 0.0);
    for (int i = 0; i < st.npanels && n < (int)sizeof buf - 64; i++)
        n += snprintf(buf + n, sizeof buf - n, "%s{\"name\":\"%s\",\"ok\":%s}", i ? "," : "", s_cfg->panels[i].name,
                      st.panel_ok[i] ? "true" : "false");
    n += snprintf(buf + n, sizeof buf - n, "]}");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

static bool valid_name(const char *n)
{
    size_t l = strlen(n);
    if (!l || l > 60 || n[0] == '.') return false;
    for (size_t i = 0; i < l; i++)
        if (n[i] == '/' || n[i] == '\\' || (unsigned char)n[i] < 32) return false;
    return true;
}

static bool query(httpd_req_t *req, const char *key, char *val, size_t cap)
{
    char q[160];
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK) return false;
    if (httpd_query_key_value(q, key, val, cap) != ESP_OK) return false;
    // decode %XX and '+'
    char *o = val;
    for (char *p = val; *p; p++) {
        if (*p == '%' && p[1] && p[2]) {
            char h[3] = {p[1], p[2], 0};
            *o++ = (char)strtol(h, NULL, 16);
            p += 2;
        } else {
            *o++ = *p == '+' ? ' ' : *p;
        }
    }
    *o = 0;
    return true;
}

static esp_err_t clips_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    DIR *d = opendir(CLIP_DIR);
    bool first = true;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (e->d_name[0] == '.' || l < 5 || strcasecmp(e->d_name + l - 4, ".ftc")) continue;
        char path[300], item[160];
        struct stat st;
        snprintf(path, sizeof path, CLIP_DIR "/%s", e->d_name);
        if (stat(path, &st)) continue;
        snprintf(item, sizeof item, "%s{\"name\":\"%.*s\",\"size\":%ld}", first ? "" : ",", (int)(l - 4), e->d_name,
                 (long)st.st_size);
        httpd_resp_sendstr_chunk(req, item);
        first = false;
    }
    if (d) closedir(d);
    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t play_get(httpd_req_t *req)
{
    char name[72], path[100];
    if (!query(req, "clip", name, sizeof name) || !valid_name(name)) return httpd_resp_send_err(req, 400, "clip?");
    snprintf(path, sizeof path, CLIP_DIR "/%s.ftc", name);
    player_play_path(path);
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t stop_get(httpd_req_t *req)
{
    player_stop();
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t volume_get(httpd_req_t *req)
{
    char val[8];
    if (query(req, "v", val, sizeof val)) {
        int v = atoi(val);
        board_audio_set_volume(v < 0 ? 0 : v > 100 ? 100 : v);
    }
    char out[16];
    snprintf(out, sizeof out, "%d\n", board_audio_get_volume());
    return httpd_resp_sendstr(req, out);
}

// Streams the body to a temp file next to the clips, checks the FTC1 magic,
// then renames over the target. Clips can be replaced while another plays.
static esp_err_t upload_post(httpd_req_t *req)
{
    char name[72];
    if (!query(req, "name", name, sizeof name) || !valid_name(name)) return httpd_resp_send_err(req, 400, "name?");
    size_t l = strlen(name);
    if (l < 5 || strcasecmp(name + l - 4, ".ftc")) return httpd_resp_send_err(req, 400, "only .ftc files");
    mkdir(CLIP_DIR, 0777);
    char tmp[100], dst[100];
    snprintf(tmp, sizeof tmp, CLIP_DIR "/.upload.tmp");
    snprintf(dst, sizeof dst, CLIP_DIR "/%s", name);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return httpd_resp_send_err(req, 500, "cannot create file (SD card?)");
    const size_t cap = 32 * 1024;
    char *buf = heap_caps_aligned_alloc(128, cap, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    size_t left = req->content_len, got_total = 0;
    bool ok = buf != NULL, magic_checked = false;
    while (ok && left > 0) {
        int r = httpd_req_recv(req, buf, left < cap ? left : cap);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { ok = false; break; }
        if (!magic_checked) {
            magic_checked = true;
            if (r < 4 || memcmp(buf, "FTC1", 4)) { ok = false; break; }
        }
        if (write(fd, buf, r) != r) { ok = false; break; }
        left -= r;
        got_total += r;
    }
    free(buf);
    close(fd);
    if (!ok || got_total == 0) {
        unlink(tmp);
        ESP_LOGW(TAG, "upload %s failed after %u bytes", name, (unsigned)got_total);
        return httpd_resp_send_err(req, 400, magic_checked ? "upload failed (not an FTC1 file or write error)"
                                                           : "upload failed");
    }
    unlink(dst);
    if (rename(tmp, dst)) {
        unlink(tmp);
        return httpd_resp_send_err(req, 500, "rename failed");
    }
    ESP_LOGI(TAG, "uploaded %s (%u bytes)", name, (unsigned)got_total);
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t delete_get(httpd_req_t *req)
{
    char name[72], path[100];
    if (!query(req, "name", name, sizeof name) || !valid_name(name)) return httpd_resp_send_err(req, 400, "name?");
    snprintf(path, sizeof path, CLIP_DIR "/%s", name);
    if (unlink(path)) return httpd_resp_send_err(req, 404, "not found");
    ESP_LOGI(TAG, "deleted %s", name);
    return httpd_resp_sendstr(req, "ok");
}

void web_start(const config_t *cfg)
{
    s_cfg = cfg;
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.core_id = 0;
    c.stack_size = 8192;
    c.max_uri_handlers = 12;
    c.recv_wait_timeout = 20;
    c.lru_purge_enable = true;
    httpd_handle_t h;
    if (httpd_start(&h, &c) != ESP_OK) {
        ESP_LOGE(TAG, "http server failed");
        return;
    }
    static const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_get},
        {.uri = "/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/clips", .method = HTTP_GET, .handler = clips_get},
        {.uri = "/play", .method = HTTP_GET, .handler = play_get},
        {.uri = "/stop", .method = HTTP_GET, .handler = stop_get},
        {.uri = "/volume", .method = HTTP_GET, .handler = volume_get},
        {.uri = "/upload", .method = HTTP_POST, .handler = upload_post},
        {.uri = "/delete", .method = HTTP_GET, .handler = delete_get},
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) httpd_register_uri_handler(h, &uris[i]);
    ESP_LOGI(TAG, "control page on port 80");
}
