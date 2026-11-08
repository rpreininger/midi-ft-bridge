// Tab5 playout - stage 1 bench.
//
// Answers "is the Tab5 fast enough" before anything else gets built:
//   1. raw SD sequential read throughput
//   2. FTC decode cost per clip (SD read + LZ4 + XOR), avg and worst frame
//   3. real-time playback of the heaviest clip with audio out of the ES8388,
//      audio clock as master (like the Mac engine), counting late/skipped
//      frames.
// Results go to the USB-Serial-JTAG console (idf.py monitor).
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "board.h"
#include "config.h"
#include "net.h"
#include "stream_test.h"
#include "web.h"
#include "midi_host.h"
#include "usb_audio.h"
#include "console.h"
#include "player.h"
#include "display.h"
#include "esp_timer.h"
#include "power.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ftc.h"
#include "esp_partition.h"

static const char *TAG = "bench";

#define MAX_CLIPS 32
static char s_clips[MAX_CLIPS][128];
static int s_nclips;

static void *psram_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static void *dma_alloc(size_t n) { return heap_caps_aligned_alloc(128, n, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL); }

// Clip in the "testclip" flash partition (no SD card needed). Mapped once,
// read through ftc_open_mem (video and audio keep their own position).
static const void *s_flash_clip;
static size_t s_flash_clip_len;
#define FLASH_CLIP "flash:testclip"

static bool map_flash_clip(void)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, "testclip");
    if (!p) { ESP_LOGW(TAG, "no testclip partition"); return false; }
    uint8_t hdr[64];
    if (esp_partition_read(p, 0, hdr, sizeof hdr) != ESP_OK || memcmp(hdr, "FTC1", 4) != 0) {
        ESP_LOGW(TAG, "testclip partition holds no FTC1 clip (esptool write_flash 0x%lx clip.ftc)", (unsigned long)p->address);
        return false;
    }
    // map only what the clip uses: end of index = index_offset + 4*frames
    uint64_t idx;
    uint32_t frames;
    memcpy(&idx, hdr + 40, 8);
    memcpy(&frames, hdr + 12, 4);
    size_t len = (size_t)(idx + 4ull * frames);
    if (len > p->size) { ESP_LOGW(TAG, "testclip truncated"); return false; }
    esp_partition_mmap_handle_t h;
    esp_err_t e = esp_partition_mmap(p, 0, len, ESP_PARTITION_MMAP_DATA, &s_flash_clip, &h);
    if (e != ESP_OK) { ESP_LOGE(TAG, "testclip mmap of %u bytes failed: %s", (unsigned)len, esp_err_to_name(e)); return false; }
    s_flash_clip_len = len;
    ESP_LOGI(TAG, "testclip: %u bytes mapped from flash", (unsigned)len);
    return true;
}

static int open_clip(ftc_clip_t *c, const char *path)
{
    if (strcmp(path, FLASH_CLIP) == 0) {
        return ftc_open_mem(c, s_flash_clip, s_flash_clip_len, psram_alloc);
    }
    return ftc_open(c, path, psram_alloc, dma_alloc, 32 * 1024);
}

static void scan_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && s_nclips < MAX_CLIPS) {
        char full[300];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        if (stat(full, &st) == 0)
            ESP_LOGI(TAG, "  %s%s  %.1f MB", full, S_ISDIR(st.st_mode) ? "/" : "", st.st_size / 1e6);
        size_t l = strlen(e->d_name);
        if (e->d_name[0] == '.') continue;  // macOS "._x.ftc" AppleDouble junk
        if (l > 4 && strcasecmp(e->d_name + l - 4, ".ftc") == 0)
            snprintf(s_clips[s_nclips++], sizeof s_clips[0], "%s/%s", dir, e->d_name);
    }
    closedir(d);
}

// Sequential read via POSIX read(). Note: newlib's fread() on an _IONBF
// stream costs ~6 ms per 512 B sector here (80 KB/s); never use unbuffered
// stdio on the SD card.
static void bench_sd_read(const char *path)
{
    const size_t chunk = 64 * 1024, total = 32 * 1024 * 1024;
    uint8_t *buf = heap_caps_aligned_alloc(128, chunk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    int fd = open(path, O_RDONLY);
    if (fd < 0 || !buf) { ESP_LOGE(TAG, "sd read bench: open/alloc failed"); goto out; }
    int64_t t0 = esp_timer_get_time();
    size_t got = 0;
    ssize_t n;
    while (got < total && (n = read(fd, buf, chunk)) > 0) got += n;
    double s = (esp_timer_get_time() - t0) / 1e6;
    ESP_LOGI(TAG, "SD sequential read: %.1f MB in %.2f s = %.2f MB/s (64 KB reads)", got / 1e6, s, got / 1e6 / s);
out:
    if (fd >= 0) close(fd);
    free(buf);
}

// Decode up to max_frames as fast as possible.
static void bench_decode(const char *path, uint32_t max_frames)
{
    ftc_clip_t c;
    if (open_clip(&c, path)) {
        ESP_LOGE(TAG, "%s: open failed", path);
        ftc_close(&c, free);
        return;
    }
    uint32_t n = c.hdr.frame_count < max_frames ? c.hdr.frame_count : max_frames;
    int64_t worst = 0, t0 = esp_timer_get_time();
    uint64_t pos0 = c.v.pos;
    for (uint32_t i = 0; i < n; i++) {
        int64_t a = esp_timer_get_time();
        if (ftc_decode_next(&c) != 0) break;
        int64_t d = esp_timer_get_time() - a;
        if (d > worst) worst = d;
    }
    double s = (esp_timer_get_time() - t0) / 1e6;
    double mb = (c.v.pos - pos0) / 1e6;
    double budget_ms = 1000.0 * c.hdr.fps_den / c.hdr.fps_num;
    ESP_LOGI(TAG, "%-28s %4u fr: avg %.2f ms, worst %.2f ms (budget %.0f ms), %.2f MB/s video, %.0fx realtime, err %u",
             strrchr(path, '/') ? strrchr(path, '/') + 1 : path, (unsigned)n, s * 1000 / n, worst / 1000.0, budget_ms, mb / s,
             (n * budget_ms / 1000.0) / s, (unsigned)c.errors);
    ftc_close(&c, free);
}

// ---- real-time playback with audio as master clock ----

static ftc_clip_t s_play;
static volatile uint64_t s_samples_out;  // stereo frames handed to I2S
static volatile bool s_audio_done;

static void audio_task(void *arg)
{
    const size_t chunk = 480 * 4;  // 10 ms of s16 stereo @48k
    uint8_t *buf = heap_caps_malloc(chunk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    size_t n;
    while ((n = ftc_read_audio(&s_play, buf, chunk)) > 0) {
        if (n < chunk) memset(buf + n, 0, chunk - n);
        board_audio_write(buf, chunk);
        s_samples_out += n / 4;
    }
    free(buf);
    s_audio_done = true;
    vTaskDelete(NULL);
}

static void bench_realtime(const char *path, int seconds, bool with_audio)
{
    if (open_clip(&s_play, path)) {
        ESP_LOGE(TAG, "%s: open failed", path);
        ftc_close(&s_play, free);
        return;
    }
    ftc_header_t *h = &s_play.hdr;
    ESP_LOGI(TAG, "realtime: %s, %d s, audio %s", path, seconds, with_audio ? "on" : "off (timer clock)");
    s_samples_out = 0;
    s_audio_done = false;
    const uint32_t rate = h->audio_rate ? h->audio_rate : 48000;
    // I2S has 6 x 10 ms queued ahead of what is audible.
    const uint64_t latency = 6 * 480;
    if (with_audio) xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 20, NULL, 1);

    int64_t t0 = esp_timer_get_time();
    uint32_t shown = 0, skipped = 0, late_events = 0;
    int64_t worst_decode = 0, busy = 0;
    while (esp_timer_get_time() - t0 < (int64_t)seconds * 1000000) {
        uint64_t played;
        if (with_audio) {
            uint64_t o = s_samples_out;
            played = o > latency ? o - latency : 0;
        } else {
            played = (uint64_t)(esp_timer_get_time() - t0) * rate / 1000000;
        }
        uint32_t target = (uint32_t)(played * h->fps_num / ((uint64_t)rate * h->fps_den));
        if (target >= h->frame_count || s_play.next_frame >= h->frame_count) break;
        if (with_audio && s_audio_done) break;
        if (s_play.next_frame <= target) {
            uint32_t behind = target - s_play.next_frame;
            if (behind) { skipped += behind; late_events++; }
            int64_t a = esp_timer_get_time();
            ftc_seek_forward(&s_play, target);  // decodes frames we skip, deltas need them
            ftc_decode_next(&s_play);
            int64_t d = esp_timer_get_time() - a;
            busy += d;
            if (d > worst_decode) worst_decode = d;
            shown++;
            // here the frame would go to panels/display
        }
        vTaskDelay(1);
    }
    double s = (esp_timer_get_time() - t0) / 1e6;
    ESP_LOGI(TAG, "realtime result: %u frames shown in %.1f s (%.2f fps), %u skipped in %u late events, "
                  "worst decode %.2f ms, decode load %.1f%%, audio %llu samples (%.0f Hz effective), errors %u",
             (unsigned)shown, s, shown / s, (unsigned)skipped, (unsigned)late_events, worst_decode / 1000.0,
             100.0 * busy / 1e6 / s, (unsigned long long)s_samples_out, s_samples_out / s, (unsigned)s_play.errors);
    // stop audio by exhausting it
    s_play.audio_pos = s_play.hdr.audio_bytes;
    while (with_audio && !s_audio_done) vTaskDelay(10);
    ftc_close(&s_play, free);
}

static void run_bench(void)
{
    bool have_flash = map_flash_clip();
    if (have_flash && s_nclips < MAX_CLIPS) strcpy(s_clips[s_nclips++], FLASH_CLIP);
    if (!s_nclips) { ESP_LOGE(TAG, "no clips at all"); return; }
    int big = -1;
    off_t bigsz = 0;
    for (int i = 0; i < s_nclips; i++) {
        struct stat st;
        if (strcmp(s_clips[i], FLASH_CLIP) && stat(s_clips[i], &st) == 0 && st.st_size > bigsz) { bigsz = st.st_size; big = i; }
    }
    if (big >= 0) bench_sd_read(s_clips[big]);
    for (int i = 0; i < s_nclips; i++) bench_decode(s_clips[i], 750);
    int rt = big >= 0 ? big : s_nclips - 1;
    for (int i = 0; i < s_nclips; i++)
        if (strcasestr(s_clips[i], "neutrino")) rt = i;
    bench_realtime(s_clips[rt], 30, true);
    ESP_LOGI(TAG, "bench done");
}

static config_t s_cfg;

#define COL_TXT 0xE0E0E0
#define COL_DIM 0x808080
#define COL_OK 0x40D060
#define COL_BAD 0xE04040
#define COL_ACC 0xF0C040

// Top band: what plays and where. Bottom band: links (MIDI, Wi-Fi, panels).
static void show_status(const config_t *cfg)
{
    player_status_t st;
    player_get_status(&st);
    char a[96], b[48], v[48];
    if (st.playing) {
        unsigned p = st.frame / (st.fps ? st.fps : 25), t = st.frames / (st.fps ? st.fps : 25);
        snprintf(a, sizeof a, "> %s", st.clip);
        snprintf(b, sizeof b, "%u:%02u / %u:%02u", p / 60, p % 60, t / 60, t % 60);
    } else {
        snprintf(a, sizeof a, "gestoppt");
        b[0] = 0;
    }
    bool jack = board_audio_poll_jack();
    if (usb_audio_active()) snprintf(v, sizeof v, "USB %s", usb_audio_name());
    else snprintf(v, sizeof v, "%s %d", jack ? "Klinke" : "Lautspr.", board_audio_get_volume());
    power_state_t pw;
    char bat[32] = "";
    uint32_t bat_col = COL_DIM;
    if (power_read(&pw) && pw.present) {
        snprintf(bat, sizeof bat, "Akku %d%%", pw.percent);
        bat_col = pw.percent > 40 ? COL_OK : pw.percent > 20 ? COL_ACC : COL_BAD;
        static int64_t next_log;
        int64_t now = esp_timer_get_time();
        if (now >= next_log) {
            next_log = now + 30000000;
            ESP_LOGI(TAG, "battery %.3f V, %.3f A, %d%%", pw.volts, pw.amps, pw.percent);
        }
    }
    display_span_t top[] = {
        {64, a, st.playing ? COL_ACC : COL_DIM},
        {520, b, COL_TXT},
        {-260, bat, bat_col},
        {-64, v, COL_TXT},
    };
    display_status(0, top, 4);

    char wifi[64];
    if (cfg->net.ap) snprintf(wifi, sizeof wifi, "WLAN %s: %d Client%s", cfg->net.ssid, net_ap_clients(),
                              net_ap_clients() == 1 ? "" : "s");
    else snprintf(wifi, sizeof wifi, "WLAN %s: %s", cfg->net.ssid, net_ip());
    display_span_t bot[3 + CFG_MAX_PANELS];
    int n = 0;
    static char midi[48];
    if (midi_host_connected()) snprintf(midi, sizeof midi, "MIDI: %.30s", midi_host_names());
    else snprintf(midi, sizeof midi, "MIDI: --");
    bot[n++] = (display_span_t){64, midi, midi_host_connected() ? COL_OK : COL_BAD};
    bot[n++] = (display_span_t){420, wifi, COL_TXT};
    static char pn[CFG_MAX_PANELS][24];
    int x = -64 - 0;
    // panels right-aligned, last panel at the right edge
    int right = 64;
    for (int i = st.npanels - 1; i >= 0; i--) {
        snprintf(pn[i], sizeof pn[i], "%c", cfg->panels[i].name[0] ? cfg->panels[i].name[0] : '1' + i);
        bot[n++] = (display_span_t){-right, pn[i], st.mirrored ? COL_ACC : st.panel_ok[i] ? COL_OK : COL_BAD};
        right += 40;
    }
    (void)x;
    if (st.mirrored) bot[n++] = (display_span_t){-(right + 10), "Test:", COL_ACC};
    else bot[n++] = (display_span_t){-(right + 10), "Panels:", COL_TXT};
    display_status(1, bot, n);
}
static bool s_have_config;

void app_main(void)
{
    ESP_LOGI(TAG, "Tab5 playout. PSRAM free %u KB, internal free %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    ESP_ERROR_CHECK(board_init());
    power_init(board_i2c());
    esp_err_t de = display_init(board_i2c(), 288, 128);
    if (de != ESP_OK) ESP_LOGE(TAG, "display: %s", esp_err_to_name(de));
    bool sd = board_sd_mount() == ESP_OK;
    if (sd) {
        scan_dir(BOARD_SD_MOUNT "/ftc");
        ESP_LOGI(TAG, "%d clip(s) on SD", s_nclips);
    } else {
        ESP_LOGW(TAG, "no SD card");
    }
    struct stat cst;
    s_have_config = sd && stat(BOARD_SD_MOUNT "/config.json", &cst) == 0;
    config_load(&s_cfg, BOARD_SD_MOUNT "/config.json");
    if (s_cfg.nmappings == 0) {
        // the Fantom pad layout from the Mac config.json (notes 36..49, stop 51)
        static const char *names[] = {"Intro", "Fly", "Moon", "Pixel", "OYM", "Tennis", "Neutrino",
                                      "ZX89", "Magneto", "Jiffy", "Black", "Shrink", "Shin", "Axel"};
        for (int i = 0; i < 14; i++) {
            s_cfg.mappings[i].note = 36 + i;
            snprintf(s_cfg.mappings[i].clip, sizeof s_cfg.mappings[i].clip, "/sdcard/ftc/%s.ftc", names[i]);
        }
        s_cfg.nmappings = 14;
        if (s_cfg.stop_note < 0) s_cfg.stop_note = 51;
    }
    if (s_cfg.npanels == 0) {
        // the stage rig from the Mac config.json
        static const cfg_panel_t rig[] = {
            {"A_bigpanel", "192.168.10.21", 1337, 0, 0, 128, 128},
            {"B_ericpanel", "192.168.10.22", 1337, 128, 0, 128, 64},
            {"C_ralfpanel", "192.168.10.20", 1337, 128, 64, 128, 64},
        };
        memcpy(s_cfg.panels, rig, sizeof rig);
        s_cfg.npanels = 3;
    }
    config_log(&s_cfg);

    esp_err_t ae = board_audio_init(48000);
    if (ae == ESP_OK) board_audio_default_volumes(s_cfg.volume, s_cfg.speaker_volume);
    else ESP_LOGE(TAG, "audio init failed: %s", esp_err_to_name(ae));

    struct stat st;
    if (sd && stat(BOARD_SD_MOUNT "/bench", &st) == 0) {  // touch /bench on the card
        run_bench();
        return;
    }

    ESP_ERROR_CHECK(net_start(&s_cfg.net));
    if (!net_wait_ready(20000)) { ESP_LOGE(TAG, "Wi-Fi did not come up"); return; }
    ESP_LOGI(TAG, "Wi-Fi up, %s %s", s_cfg.net.ap ? "AP" : "STA", net_ip());
    web_start(&s_cfg);

    if (sd && stat(BOARD_SD_MOUNT "/streamtest", &st) == 0) {  // touch /streamtest: old load test
        const char *clip = NULL;
        for (int i = 0; i < s_nclips; i++)
            if (strcasestr(s_clips[i], "neutrino")) clip = s_clips[i];
        for (;;) {
            const char *target = NULL;
            while (s_cfg.net.ap && !(target = net_ap_client_ip())) vTaskDelay(pdMS_TO_TICKS(500));
            stream_test_run(&s_cfg, clip, target, 60, ae == ESP_OK);
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }

    player_start(&s_cfg);
    esp_err_t me = midi_host_start(s_cfg.midi_device);
    if (me != ESP_OK) ESP_LOGE(TAG, "USB host: %s", esp_err_to_name(me));
    else usb_audio_start(s_cfg.usb_audio);
    console_start();

    // Without a config.json the panels are not reachable anyway (the rig is
    // not on this hotspot), so mirror the streams to the first client that
    // joins - lets a phone stand in as the load for testing.
    bool mirror = !s_have_config;
    const char *mirrored = NULL;
    int64_t next_status = 0;
    for (;;) {
        int64_t now = esp_timer_get_time();
        if (now >= next_status) {
            next_status = now + 500000;
            board_audio_poll_jack();
            show_status(&s_cfg);
        }
        midi_msg_t m;
        if (midi_host_get(&m, 100)) {
            uint8_t type = m.status & 0xF0, ch = m.status & 0x0F;
            ESP_LOGI(TAG, "MIDI cable %u ch %u: %02x %u %u", m.cable, ch + 1, m.status, m.d1, m.d2);
            if (s_cfg.midi_channel >= 0 && ch != s_cfg.midi_channel) continue;
            if (type == 0x90 && m.d2 > 0) {
                if (m.d1 == s_cfg.stop_note) player_stop();
                else if (!player_note(m.d1)) ESP_LOGI(TAG, "note %u not mapped", m.d1);
            } else if (type == 0xB0 && m.d1 == 7) {
                board_audio_set_volume(m.d2 * 100 / 127);
            }
        }
        if (mirror && s_cfg.net.ap) {
            const char *c = net_ap_client_ip();
            if (c && c != mirrored) {
                mirrored = c;
                player_set_test_target(c);
            }
        }
    }
}
