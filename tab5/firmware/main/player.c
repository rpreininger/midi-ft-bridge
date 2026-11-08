#include "player.h"
#include "usb_audio.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#include "board.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ft_send.h"
#include "ftc.h"
#include "lwip/sockets.h"

static const char *TAG = "player";

enum { CMD_PLAY, CMD_STOP };
typedef struct {
    int cmd;
    char clip[96];
} cmd_t;

static const config_t *s_cfg;
static QueueHandle_t s_cmdq;

// audio
static ftc_clip_t s_clip;
static bool s_clip_open;
static volatile uint64_t s_samples;
static volatile bool s_audio_stop, s_audio_done = true;

// sender: the playback task fills s_canvas[s_fill] and hands it over
static uint8_t *s_canvas[2];
static int s_fill;
static volatile int s_ready = -1;  // index ready to send, -1 none
static TaskHandle_t s_sender;
static ft_sender_t s_snd[CFG_MAX_PANELS];
static char s_test_ip[16];
static volatile bool s_retarget;
static char s_clip_name[48];

static void *psram_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static void *dma_alloc(size_t n) { return heap_caps_aligned_alloc(128, n, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL); }

// The SD read buffers (2 x 32 KB, DMA-capable internal RAM) and the audio
// chunk are allocated once: Wi-Fi/lwIP use the same heap at runtime, and
// freeing/re-allocating per clip fragmented it until a 32 KB block was no
// longer available ("cannot open" after a few clip changes).
#define IO_BUF 32768
#define AUDIO_CHUNK (480 * 4)
static void *s_iobuf[2], *s_audio_buf;
static int s_io_next;
static void *io_alloc(size_t n) { return n <= IO_BUF ? s_iobuf[s_io_next++ & 1] : NULL; }
static void clip_release(void *p)
{
    if (p != s_iobuf[0] && p != s_iobuf[1]) free(p);
}

static void open_senders(void)
{
    for (int i = 0; i < s_cfg->npanels; i++) {
        if (s_snd[i].pkt) {
            closesocket(s_snd[i].sock);
            free(s_snd[i].pkt);
        }
        const char *ip = s_test_ip[0] ? s_test_ip : s_cfg->panels[i].ip;
        ft_sender_init(&s_snd[i], ip, s_cfg->panels[i].port + (s_test_ip[0] ? i : 0));
    }
    ESP_LOGI(TAG, "panel streams -> %s", s_test_ip[0] ? s_test_ip : "configured panel IPs");
}

volatile bool g_diag_nosend;   // console diagnostics
volatile int g_diag_preview_div = 1;

static void sender_task(void *arg)
{
    open_senders();
    int64_t t_stat = esp_timer_get_time();
    uint32_t frames = 0;
    int64_t busy = 0, worst = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        if (s_retarget) {
            s_retarget = false;
            open_senders();
        }
        int idx = s_ready;
        if (idx >= 0 && g_diag_nosend) s_ready = -1;
        else if (idx >= 0) {
            s_ready = -1;
            int64_t a = esp_timer_get_time();
            for (int i = 0; i < s_cfg->npanels; i++) {
                const cfg_panel_t *p = &s_cfg->panels[i];
                ft_sender_send(&s_snd[i], s_canvas[idx], s_cfg->video_w, p->src_x, p->src_y, p->src_w, p->src_h);
            }
            int64_t d = esp_timer_get_time() - a;
            busy += d;
            if (d > worst) worst = d;
            frames++;
        }
        int64_t now = esp_timer_get_time();
        if (now - t_stat > 10000000 && frames) {
            uint32_t err = 0, retry = 0;
            for (int i = 0; i < s_cfg->npanels; i++) { err += s_snd[i].send_errors; retry += s_snd[i].retries; }
            ESP_LOGI(TAG, "sent %.1f fps, send avg %.1f / max %.1f ms, total lost tiles %u, retries %u",
                     frames / ((now - t_stat) / 1e6), busy / 1000.0 / frames, worst / 1000.0, (unsigned)err,
                     (unsigned)retry);
            t_stat = now;
            frames = 0;
            busy = worst = 0;
        }
    }
}

static void submit_canvas(const uint8_t *rgb)
{
    size_t n = (size_t)s_cfg->video_w * s_cfg->video_h * 3;
    if (rgb) memcpy(s_canvas[s_fill], rgb, n);
    else memset(s_canvas[s_fill], 0, n);
    display_preview(s_canvas[s_fill]);
    s_ready = s_fill;  // if the sender is still busy, the older frame is simply replaced
    s_fill ^= 1;
    xTaskNotifyGive(s_sender);
}

static void audio_task(void *arg)
{
    const size_t chunk = AUDIO_CHUNK;
    uint8_t *buf = s_audio_buf;
    size_t n;
    while (!s_audio_stop && (n = ftc_read_audio(&s_clip, buf, chunk)) > 0) {
        if (n < chunk) memset(buf + n, 0, chunk - n);
        usb_audio_tone_replace((int16_t *)buf, chunk / 4);
        if (!usb_audio_active() || !usb_audio_write(buf, chunk)) board_audio_write(buf, chunk);
        s_samples += n / 4;
    }
    s_audio_done = true;
    vTaskDelete(NULL);
}

static void stop_clip(void)
{
    s_audio_stop = true;
    while (!s_audio_done) vTaskDelay(1);
    if (s_clip_open) ftc_close(&s_clip, clip_release);
    s_clip_open = false;
}

static bool start_clip(const char *path)
{
    stop_clip();
    s_io_next = 0;
    if (ftc_open(&s_clip, path, psram_alloc, io_alloc, IO_BUF)) {
        int err = errno;
        struct stat stb;
        ESP_LOGE(TAG, "cannot open %s: errno %d (%s), stat %s, fds v%d a%d, dma free %u largest %u, psram largest %u",
                 path, err, strerror(err), stat(path, &stb) == 0 ? "ok" : "FAILED", s_clip.v.fd, s_clip.a.fd,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        ftc_close(&s_clip, clip_release);
        return false;
    }
    if (s_clip.hdr.width != s_cfg->video_w || s_clip.hdr.height != s_cfg->video_h) {
        ESP_LOGE(TAG, "%s is %ux%u, canvas is %dx%d", path, s_clip.hdr.width, s_clip.hdr.height, s_cfg->video_w,
                 s_cfg->video_h);
        ftc_close(&s_clip, clip_release);
        return false;
    }
    s_clip_open = true;
    const char *b = strrchr(path, '/');
    strlcpy(s_clip_name, b ? b + 1 : path, sizeof s_clip_name);
    char *dot = strrchr(s_clip_name, '.');
    if (dot) *dot = 0;
    s_samples = 0;
    s_audio_stop = false;
    s_audio_done = !s_clip.has_audio;
    if (s_clip.has_audio) xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 20, NULL, 1);
    ESP_LOGI(TAG, "playing %s (%u frames; dma free %u largest %u)", path, (unsigned)s_clip.hdr.frame_count,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    return true;
}

static void play_task(void *arg)
{
    const uint64_t latency = 6 * 480;  // I2S DMA queue ahead of the speaker
    int64_t t0 = 0;
    uint32_t late = 0;
    for (;;) {
        cmd_t c;
        if (xQueueReceive(s_cmdq, &c, s_clip_open ? 0 : portMAX_DELAY) == pdTRUE) {
            if (c.cmd == CMD_PLAY) {
                if (start_clip(c.clip)) {
                    t0 = esp_timer_get_time();
                    late = 0;
                }
            } else {
                stop_clip();
                submit_canvas(NULL);
                ESP_LOGI(TAG, "stopped");
            }
            continue;
        }
        if (!s_clip_open) continue;
        const ftc_header_t *h = &s_clip.hdr;
        uint32_t rate = h->audio_rate ? h->audio_rate : 48000;
        uint64_t played;
        if (s_clip.has_audio) {
            uint64_t o = s_samples;
            uint64_t lat = usb_audio_active() ? usb_audio_latency_frames() : latency;
            played = o > lat ? o - lat : 0;
        } else {
            played = (uint64_t)(esp_timer_get_time() - t0) * rate / 1000000;
        }
        uint32_t target = (uint32_t)(played * h->fps_num / ((uint64_t)rate * h->fps_den));
        bool ended = target >= h->frame_count || (s_clip.has_audio && s_audio_done);
        if (ended) {
            ESP_LOGI(TAG, "clip finished (%u late frames)", (unsigned)late);
            stop_clip();
            submit_canvas(NULL);
            continue;
        }
        if (s_clip.next_frame <= target) {
            if (target > s_clip.next_frame) late++;
            ftc_seek_forward(&s_clip, target);
            ftc_decode_next(&s_clip);
            submit_canvas(s_clip.frame);
        }
        vTaskDelay(1);
    }
}

void player_start(const config_t *cfg)
{
    s_cfg = cfg;
    size_t n = (size_t)cfg->video_w * cfg->video_h * 3;
    s_iobuf[0] = dma_alloc(IO_BUF);
    s_iobuf[1] = dma_alloc(IO_BUF);
    s_audio_buf = dma_alloc(AUDIO_CHUNK);
    s_canvas[0] = psram_alloc(n);
    s_canvas[1] = psram_alloc(n);
    s_cmdq = xQueueCreate(8, sizeof(cmd_t));
    xTaskCreatePinnedToCore(sender_task, "ft_send", 6144, NULL, 12, &s_sender, 0);
    xTaskCreatePinnedToCore(play_task, "play", 6144, NULL, 13, NULL, 0);
}

bool player_note(uint8_t note)
{
    for (int i = 0; i < s_cfg->nmappings; i++) {
        if (s_cfg->mappings[i].note == note) {
            cmd_t c = {.cmd = CMD_PLAY};
            strlcpy(c.clip, s_cfg->mappings[i].clip, sizeof c.clip);
            xQueueSend(s_cmdq, &c, 0);
            return true;
        }
    }
    return false;
}

void player_play_path(const char *path)
{
    cmd_t c = {.cmd = CMD_PLAY};
    strlcpy(c.clip, path, sizeof c.clip);
    xQueueSend(s_cmdq, &c, 0);
}

void player_stop(void)
{
    cmd_t c = {.cmd = CMD_STOP};
    xQueueSend(s_cmdq, &c, 0);
}

void player_set_test_target(const char *ip)
{
    strlcpy(s_test_ip, ip ? ip : "", sizeof s_test_ip);
    s_retarget = true;
    if (s_sender) xTaskNotifyGive(s_sender);
}

void player_get_status(player_status_t *st)
{
    memset(st, 0, sizeof(*st));
    st->playing = s_clip_open;
    if (s_clip_open) {
        strlcpy(st->clip, s_clip_name, sizeof st->clip);
        st->frame = s_clip.next_frame;
        st->frames = s_clip.hdr.frame_count;
        st->fps = s_clip.hdr.fps_den ? s_clip.hdr.fps_num / s_clip.hdr.fps_den : 25;
    }
    st->npanels = s_cfg->npanels;
    for (int i = 0; i < s_cfg->npanels; i++) st->panel_ok[i] = s_snd[i].pkt && !s_snd[i].down && s_snd[i].frames;
    st->mirrored = s_test_ip[0] != 0;
}
