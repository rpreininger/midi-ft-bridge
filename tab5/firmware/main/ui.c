#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "font.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "player.h"
#include "touch.h"

static const char *TAG = "ui";

// The canvas area right of B/C (x 128..256) and below D (256,0 32x16) is not
// sent to any panel in the stage rig: the song list goes there (on screen
// 128 x 448 px). With a rig that uses it, there is no list, only the picker.
#define LIST_CX 256
#define LIST_CY 16
#define LIST_CW 32
#define LIST_CH 112
#define LIST_ROW_MAX 36  // px; 14 clips -> 32 px rows

#define PICKER_US (8 * 1000 * 1000)
#define MAX_ITEMS 29  // picker: up to 5 rows x 6 columns including STOP
#define GAP 8

#define COL_BG 0x000000
#define COL_TXT 0xF0F0F0
#define COL_DIM 0x9A9A9A
#define COL_ACC 0xF0C040
#define COL_ACC_BG 0x3A3008
#define COL_LINE 0x303030
#define COL_BTN 0x2A2A2A
#define COL_STOP 0x8A1C1C

static int s_n;  // clips, sorted by note (pad order)
static char s_names[MAX_ITEMS][32];
static const char *s_paths[MAX_ITEMS];
static bool s_list, s_touch;
static int s_lx, s_ly, s_lw, s_lh;  // song list (landscape)
static int s_px, s_py, s_pw, s_ph;  // preview = picker area
static int s_rows, s_cols, s_cell_w, s_cell_h;
static uint16_t *s_list_buf, *s_pick_buf;
static QueueHandle_t s_taps;  // console taps {x, y}

static void fill(uint16_t *buf, int bw, int x, int y, int w, int h, uint32_t rgb)
{
    const uint16_t c = display_rgb565(rgb);
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) buf[(size_t)j * bw + i] = c;
}

static bool inside(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

// Index of the clip that plays, -1 if none (or one that is not in the list).
static int playing_index(void)
{
    player_status_t st;
    player_get_status(&st);
    if (!st.playing) return -1;
    for (int i = 0; i < s_n; i++)
        if (!strcmp(st.clip, s_names[i])) return i;
    return -1;
}

// ---- song list ----

static void draw_list(int playing)
{
    if (!s_list) return;
    const font_t *f = &FONT_LIST;
    fill(s_list_buf, s_lw, 0, 0, s_lw, s_lh, COL_BG);
    fill(s_list_buf, s_lw, 0, 0, 1, s_lh, COL_LINE);  // the list is not part of the picture
    int fit = s_lh / (f->height + 2);
    int n = s_n < fit ? s_n : fit;
    if (n == 0) return;
    int rh = s_lh / n < LIST_ROW_MAX ? s_lh / n : LIST_ROW_MAX;
    for (int i = 0; i < n; i++) {
        const bool on = i == playing;
        const int y = i * rh;
        if (on) fill(s_list_buf, s_lw, 2, y, s_lw - 2, rh, COL_ACC_BG);
        display_text(s_list_buf, s_lw, s_lh, f, 10, y + (rh - f->height) / 2, s_lw - 4, s_names[i],
                     on ? COL_ACC : COL_DIM, on ? COL_ACC_BG : COL_BG);
    }
    display_overlay(s_list_buf);
}

// ---- picker ----

static void picker_layout(void)
{
    int cells = s_n + 1;  // + STOP
    s_rows = cells <= 15 ? 3 : cells <= 24 ? 4 : 5;
    s_cols = (cells + s_rows - 1) / s_rows;
    s_cell_w = (s_pw - GAP * (s_cols + 1)) / s_cols;
    s_cell_h = (s_ph - GAP * (s_rows + 1)) / s_rows;
}

static void cell_pos(int k, int *x, int *y)
{
    *x = GAP + (k % s_cols) * (s_cell_w + GAP);
    *y = GAP + (k / s_cols) * (s_cell_h + GAP);
}

// Button under landscape (x, y): 0..s_n-1 a clip, s_n STOP, -1 none.
static int picker_hit(int x, int y)
{
    for (int k = 0; k <= s_n; k++) {
        int cx, cy;
        cell_pos(k, &cx, &cy);
        if (inside(x - s_px, y - s_py, cx, cy, s_cell_w, s_cell_h)) return k;
    }
    return -1;
}

static void draw_picker(int playing)
{
    const font_t *f = &FONT_STATUS;
    fill(s_pick_buf, s_pw, 0, 0, s_pw, s_ph, COL_BG);
    for (int k = 0; k <= s_n; k++) {
        int x, y;
        cell_pos(k, &x, &y);
        const bool stop = k == s_n, on = k == playing;
        const uint32_t back = stop ? COL_STOP : on ? COL_ACC : COL_BTN;
        const char *label = stop ? "STOP" : s_names[k];
        fill(s_pick_buf, s_pw, x, y, s_cell_w, s_cell_h, back);
        int tw = display_text_width(f, label);
        if (tw > s_cell_w - 16) tw = s_cell_w - 16;
        display_text(s_pick_buf, s_pw, s_ph, f, x + (s_cell_w - tw) / 2, y + (s_cell_h - f->height) / 2,
                     x + s_cell_w - 8, label, on ? 0x000000 : COL_TXT, back);
    }
    display_draw_land(s_px, s_py, s_pw, s_ph, s_pick_buf);
}

// ---- task ----

static void ui_task(void *arg)
{
    bool open = false, was_down = false;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0, playing = -2;
    int64_t close_at = 0, next_poll = 0;
    for (;;) {
        const int64_t now = esp_timer_get_time();
        // a tap = finger down at (x0, y0), up at (x1, y1)
        int x, y, q[2];
        bool tap = false;
        const bool down = s_touch && touch_read(&x, &y);
        if (down) {
            if (!was_down) x0 = x, y0 = y;
            x1 = x, y1 = y;
        } else if (was_down) {
            tap = true;
        }
        was_down = down;
        if (!tap && xQueueReceive(s_taps, q, 0)) x0 = x1 = q[0], y0 = y1 = q[1], tap = true;

        if (now >= next_poll) {
            next_poll = now + 200 * 1000;
            int p = playing_index();
            if (p != playing) {
                playing = p;
                draw_list(p);
                if (open) draw_picker(p);
            }
        }

        if (tap && !open) {
            if (inside(x0, y0, s_px, s_py, s_pw, s_ph)) {
                ESP_LOGI(TAG, "tap %d,%d: picker", x0, y0);
                display_pause_preview(true);
                draw_picker(playing);
                open = true;
                close_at = now + PICKER_US;
            }
        } else if (tap) {
            const int k = picker_hit(x0, y0);
            if (k >= 0 && k == picker_hit(x1, y1)) {
                if (k == s_n) {
                    ESP_LOGI(TAG, "tap %d,%d: STOP", x0, y0);
                    player_stop();
                } else {
                    ESP_LOGI(TAG, "tap %d,%d: %s", x0, y0, s_names[k]);
                    player_play_path(s_paths[k]);
                }
                open = false;
            } else if (k < 0) {
                ESP_LOGI(TAG, "tap %d,%d: beside the buttons, picker closed", x0, y0);
                open = false;
            } else {
                close_at = now + PICKER_US;  // slid off the button: nothing, stays open
            }
            if (!open) display_pause_preview(false);
        } else if (open && now >= close_at) {
            ESP_LOGI(TAG, "picker closed (no tap)");
            open = false;
            display_pause_preview(false);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void ui_tap(int x, int y)
{
    if (!s_taps) return;
    int q[2] = {x, y};
    xQueueSend(s_taps, q, 0);
}

void ui_start(const config_t *cfg)
{
    // clips in pad order (by note)
    int order[CFG_MAX_MAPPINGS];
    int n = cfg->nmappings < CFG_MAX_MAPPINGS ? cfg->nmappings : CFG_MAX_MAPPINGS;
    for (int i = 0; i < n; i++) {
        int j = i;
        for (; j > 0 && cfg->mappings[order[j - 1]].note > cfg->mappings[i].note; j--) order[j] = order[j - 1];
        order[j] = i;
    }
    if (n > MAX_ITEMS) {
        ESP_LOGW(TAG, "%d clips mapped, the picker shows the first %d", n, MAX_ITEMS);
        n = MAX_ITEMS;
    }
    for (int i = 0; i < n; i++) {
        const char *path = cfg->mappings[order[i]].clip;
        const char *base = strrchr(path, '/');
        snprintf(s_names[i], sizeof s_names[i], "%s", base ? base + 1 : path);
        char *dot = strrchr(s_names[i], '.');
        if (dot) *dot = 0;
        s_paths[i] = path;
    }
    s_n = n;

    display_preview_rect(&s_px, &s_py, &s_pw, &s_ph);
    bool used = false;  // does a panel take the list area?
    for (int i = 0; i < cfg->npanels; i++) {
        const cfg_panel_t *p = &cfg->panels[i];
        if (p->src_x < LIST_CX + LIST_CW && LIST_CX < p->src_x + p->src_w && p->src_y < LIST_CY + LIST_CH &&
            LIST_CY < p->src_y + p->src_h)
            used = true;
    }
    s_list = !used && display_set_overlay(LIST_CX, LIST_CY, LIST_CW, LIST_CH);
    if (s_list) {
        display_canvas_rect(LIST_CX, LIST_CY, LIST_CW, LIST_CH, &s_lx, &s_ly, &s_lw, &s_lh);
        s_list_buf = heap_caps_malloc((size_t)s_lw * s_lh * 2, MALLOC_CAP_SPIRAM);
        s_list = s_list_buf != NULL;
    }
    s_pick_buf = heap_caps_malloc((size_t)s_pw * s_ph * 2, MALLOC_CAP_SPIRAM);
    if (!s_pick_buf) {
        ESP_LOGE(TAG, "no memory for the picker");
        return;
    }
    picker_layout();
    s_taps = xQueueCreate(4, sizeof(int[2]));
    s_touch = touch_init(board_i2c()) == ESP_OK;
    ESP_LOGI(TAG, "%d clips, song list %s, picker %dx%d buttons of %dx%d px, touch %s", s_n,
             s_list ? "on" : "off (area used by a panel)", s_cols, s_rows, s_cell_w, s_cell_h,
             s_touch ? "on" : "NOT FOUND");
    xTaskCreatePinnedToCore(ui_task, "ui", 4096, NULL, 2, NULL, 0);
}
