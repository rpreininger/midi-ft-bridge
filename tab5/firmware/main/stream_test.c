// Full-load test: play a clip from SD with audio (audio = master clock) and
// stream the panel regions as FT/UDP at the clip's frame rate, exactly like
// the Mac engine does. Prints per-5-s stats: frames sent, tiles, send
// errors, ENOMEM back-pressure retries, send time per frame, late frames.
//
// target_ip: where all panel streams go. With the Tab5 as hotspot this is
// any joined client (a phone is enough: unicast frames are sent and ACKed
// at the Wi-Fi layer whether or not something listens on the port).
#include "stream_test.h"

#include <string.h>

#include "board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ft_send.h"
#include "ftc.h"

static const char *TAG = "stream";

static ftc_clip_t s_clip;
static volatile uint64_t s_samples;
static volatile bool s_audio_stop, s_audio_done;

static void *psram_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static void *dma_alloc(size_t n) { return heap_caps_aligned_alloc(128, n, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL); }

static void audio_task(void *arg)
{
    const size_t chunk = 480 * 4;
    uint8_t *buf = dma_alloc(chunk);
    size_t n;
    while (!s_audio_stop && (n = ftc_read_audio(&s_clip, buf, chunk)) > 0) {
        if (n < chunk) memset(buf + n, 0, chunk - n);
        board_audio_write(buf, chunk);
        s_samples += n / 4;
    }
    free(buf);
    s_audio_done = true;
    vTaskDelete(NULL);
}

void stream_test_run(const config_t *cfg, const char *clip, const char *target_ip, int seconds, bool audio)
{
    if (ftc_open(&s_clip, clip, psram_alloc, dma_alloc, 32 * 1024)) {
        ESP_LOGE(TAG, "open %s failed", clip);
        ftc_close(&s_clip, free);
        return;
    }
    const ftc_header_t *h = &s_clip.hdr;
    int np = cfg->npanels;
    ft_sender_t snd[CFG_MAX_PANELS];
    for (int i = 0; i < np; i++) {
        // same port per panel as configured, but all to the test target
        const char *ip = target_ip ? target_ip : cfg->panels[i].ip;
        ft_sender_init(&snd[i], ip, cfg->panels[i].port + (target_ip ? i : 0));
    }
    size_t bytes_per_frame = 0;
    for (int i = 0; i < np; i++) bytes_per_frame += (size_t)cfg->panels[i].src_w * cfg->panels[i].src_h * 3;
    ESP_LOGI(TAG, "%s -> %d panel stream(s) to %s, %u KB/frame = %.1f Mbit/s at %u fps", clip, np,
             target_ip ? target_ip : "configured IPs", (unsigned)(bytes_per_frame / 1024),
             bytes_per_frame * 8.0 * h->fps_num / h->fps_den / 1e6, h->fps_num / h->fps_den);

    s_samples = 0;
    s_audio_stop = s_audio_done = false;
    if (audio && h->audio_rate) xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 20, NULL, 1);
    const uint32_t rate = h->audio_rate ? h->audio_rate : 48000;
    const uint64_t latency = 6 * 480;

    int64_t t0 = esp_timer_get_time(), tstat = t0;
    uint32_t sent = 0, late = 0, skipped = 0;
    int64_t send_us = 0, send_max = 0, dec_max = 0;
    uint32_t err0 = 0, retry0 = 0, pk0 = 0;
    while (esp_timer_get_time() - t0 < (int64_t)seconds * 1000000) {
        uint64_t played;
        if (audio && h->audio_rate) {
            uint64_t o = s_samples;
            played = o > latency ? o - latency : 0;
            if (s_audio_done) break;
        } else {
            played = (uint64_t)(esp_timer_get_time() - t0) * rate / 1000000;
        }
        uint32_t target = (uint32_t)(played * h->fps_num / ((uint64_t)rate * h->fps_den));
        if (target >= h->frame_count) break;
        if (s_clip.next_frame <= target) {
            if (target > s_clip.next_frame) { late++; skipped += target - s_clip.next_frame; }
            int64_t a = esp_timer_get_time();
            ftc_seek_forward(&s_clip, target);
            ftc_decode_next(&s_clip);
            int64_t b = esp_timer_get_time();
            if (b - a > dec_max) dec_max = b - a;
            for (int i = 0; i < np; i++) {
                const cfg_panel_t *p = &cfg->panels[i];
                ft_sender_send(&snd[i], s_clip.frame, h->width, p->src_x, p->src_y, p->src_w, p->src_h);
            }
            int64_t d = esp_timer_get_time() - b;
            send_us += d;
            if (d > send_max) send_max = d;
            sent++;
        }
        int64_t now = esp_timer_get_time();
        if (now - tstat >= 5000000) {
            uint32_t err = 0, retry = 0, pk = 0;
            for (int i = 0; i < np; i++) { err += snd[i].send_errors; retry += snd[i].retries; pk += snd[i].packets; }
            double s = (now - tstat) / 1e6;
            ESP_LOGI(TAG, "%5.1f s: %.1f fps sent, %.1f Mbit/s, tiles %u, lost %u, retries %u, send avg %.1f / max %.1f ms, "
                          "decode max %.1f ms, late %u (skipped %u), free int %u KB",
                     (now - t0) / 1e6, sent / s, (pk - pk0) * 1.18e3 * 8 / s / 1e6, (unsigned)(pk - pk0),
                     (unsigned)(err - err0), (unsigned)(retry - retry0), sent ? send_us / 1000.0 / sent : 0,
                     send_max / 1000.0, dec_max / 1000.0, (unsigned)late, (unsigned)skipped,
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
            sent = late = skipped = 0;
            send_us = send_max = dec_max = 0;
            err0 = err; retry0 = retry; pk0 = pk;
            tstat = now;
        }
        vTaskDelay(1);
    }
    s_audio_stop = true;
    while (audio && h->audio_rate && !s_audio_done) vTaskDelay(10);
    ftc_close(&s_clip, free);
    ESP_LOGI(TAG, "stream test done");
}
