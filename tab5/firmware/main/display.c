// Tab5 display: live preview of the 288x128 canvas, 4x nearest-neighbour,
// rotated so it fills the screen with the Tab5 held in landscape.
// Panel bring-up (DSI 2 lanes @965 Mbps, DPI 70 MHz, timings) follows
// M5Stack's Tab5 BSP for the ST7123/ST7121 panel variants.
#include "display.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7121.h"
#include "esp_lcd_st7123.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "display";

#define LCD_W 720
#define LCD_H 1280
#define BACKLIGHT_GPIO 22
#define SCALE 4

extern const st7123_lcd_init_cmd_t st7123_vendor_specific_init_default[];
extern const size_t st7123_vendor_specific_init_default_count;

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_stage;  // preview region, RGB565, portrait orientation
static int s_cw, s_ch;     // canvas size
static int s_px, s_py, s_pw, s_ph;  // preview rect on the panel (portrait coords)
static const uint8_t *volatile s_pending;
static uint8_t *s_copy;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_draw_done;

// draw_bitmap with use_dma2d is asynchronous; one transfer at a time.
static bool IRAM_ATTR on_trans_done(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *e, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_draw_done, &woken);
    return woken == pdTRUE;
}

static void draw(int x0, int y0, int x1, int y1, const void *px)
{
    xSemaphoreTake(s_draw_done, portMAX_DELAY);
    if (esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x1, y1, px) != ESP_OK) xSemaphoreGive(s_draw_done);
}

static void backlight_init(int percent)
{
    ledc_timer_config_t t = {.speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_12_BIT,
                             .timer_num = LEDC_TIMER_0, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK};
    ledc_timer_config(&t);
    ledc_channel_config_t c = {.gpio_num = BACKLIGHT_GPIO, .speed_mode = LEDC_LOW_SPEED_MODE,
                               .channel = LEDC_CHANNEL_1, .timer_sel = LEDC_TIMER_0, .duty = 0};
    ledc_channel_config(&c);
    display_backlight(percent);
}

void display_backlight(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 4095 * percent / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

// Touch controller at 0x55 reports the panel variant: fw 1 = ST7121, else ST7123.
static bool is_st7121(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t dc = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x55, .scl_speed_hz = 100000};
    i2c_master_dev_handle_t d;
    if (i2c_master_bus_add_device(bus, &dc, &d) != ESP_OK) return false;
    uint8_t reg[2] = {0, 0}, fw = 0;
    esp_err_t e = i2c_master_transmit_receive(d, reg, 2, &fw, 1, 50);
    i2c_master_bus_rm_device(d);
    ESP_LOGI(TAG, "touch fw version %u (%s)", fw, e == ESP_OK ? "ok" : esp_err_to_name(e));
    return e == ESP_OK && fw == 1;
}

static void preview_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const uint8_t *src = s_pending;
        if (!src) continue;
        // Canvas (cx, cy) -> portrait panel: x runs along canvas rows
        // (rotated 90 degrees), each canvas pixel becomes SCALE x SCALE.
        for (int cx = 0; cx < s_cw; cx++) {
            // row order and column order both reversed = landscape the right way up
            uint16_t *row0 = s_stage + (size_t)((s_cw - 1 - cx) * SCALE) * s_pw;
            for (int cy = 0; cy < s_ch; cy++) {
                const uint8_t *p = src + ((size_t)cy * s_cw + cx) * 3;
                uint16_t c = (uint16_t)((p[0] & 0xF8) << 8 | (p[1] & 0xFC) << 3 | p[2] >> 3);
                uint16_t *o = row0 + cy * SCALE;
                o[0] = o[1] = o[2] = o[3] = c;
            }
            for (int r = 1; r < SCALE; r++) memcpy(row0 + (size_t)r * s_pw, row0, (size_t)s_pw * 2);
        }
        draw(s_px, s_py, s_px + s_pw, s_py + s_ph, s_stage);
        // the staging buffer is read by DMA: wait before overwriting it
        xSemaphoreTake(s_draw_done, portMAX_DELAY);
        xSemaphoreGive(s_draw_done);
    }
}

esp_err_t display_init(i2c_master_bus_handle_t bus, int canvas_w, int canvas_h)
{
    s_cw = canvas_w;
    s_ch = canvas_h;
    s_pw = canvas_h * SCALE;  // 512
    s_ph = canvas_w * SCALE;  // 1152
    if (s_pw > LCD_W || s_ph > LCD_H) return ESP_ERR_INVALID_SIZE;
    s_px = (LCD_W - s_pw) / 2;
    s_py = (LCD_H - s_ph) / 2;

    bool st7121 = is_st7121(bus);
    backlight_init(0);

    static esp_ldo_channel_handle_t phy;
    esp_ldo_channel_config_t ldo = {.chan_id = 3, .voltage_mv = 2500};  // VDD_MIPI_DPHY
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo, &phy));

    esp_lcd_dsi_bus_handle_t dsi;
    esp_lcd_dsi_bus_config_t bc = {.bus_id = 0, .num_data_lanes = 2, .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
                                   .lane_bit_rate_mbps = 965};
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bc, &dsi));
    esp_lcd_panel_io_handle_t io;
    esp_lcd_dbi_io_config_t dbi = {.virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8};
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi, &dbi, &io));

    esp_lcd_dpi_panel_config_t dpi = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 70,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs = 1,
        .video_timing = {
            .h_size = LCD_W,
            .v_size = LCD_H,
            .hsync_pulse_width = 2,
            .hsync_back_porch = 40,
            .hsync_front_porch = 40,
            .vsync_pulse_width = st7121 ? 20 : 2,
            .vsync_back_porch = st7121 ? 24 : 8,
            .vsync_front_porch = st7121 ? 200 : 220,
        },
        .flags.use_dma2d = true,
    };
    esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 24,
    };
    if (st7121) {
        static st7121_vendor_config_t v;
        v.mipi_config.dsi_bus = dsi;
        v.mipi_config.dpi_config = &dpi;
        dev.vendor_config = &v;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7121(io, &dev, &s_panel));
    } else {
        static st7123_vendor_config_t v;
        v.init_cmds = st7123_vendor_specific_init_default;
        v.init_cmds_size = st7123_vendor_specific_init_default_count;
        v.mipi_config.dsi_bus = dsi;
        v.mipi_config.dpi_config = &dpi;
        v.mipi_config.lane_num = 2;
        dev.vendor_config = &v;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7123(io, &dev, &s_panel));
    }
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    s_draw_done = xSemaphoreCreateBinary();
    xSemaphoreGive(s_draw_done);
    esp_lcd_dpi_panel_event_callbacks_t cbs = {.on_color_trans_done = on_trans_done};
    esp_lcd_dpi_panel_register_event_callbacks(s_panel, &cbs, NULL);

    // clear the whole screen to black once
    uint16_t *line = heap_caps_calloc(LCD_W * 64, 2, MALLOC_CAP_SPIRAM);
    for (int y = 0; y < LCD_H; y += 64) draw(0, y, LCD_W, y + 64, line);
    xSemaphoreTake(s_draw_done, portMAX_DELAY);
    xSemaphoreGive(s_draw_done);
    free(line);

    s_stage = heap_caps_malloc((size_t)s_pw * s_ph * 2, MALLOC_CAP_SPIRAM);
    s_copy = heap_caps_malloc((size_t)canvas_w * canvas_h * 3, MALLOC_CAP_SPIRAM);
    xTaskCreatePinnedToCore(preview_task, "preview", 4096, NULL, 5, &s_task, 0);
    display_backlight(60);
    ESP_LOGI(TAG, "%s panel up, preview %dx%d at %d,%d", st7121 ? "ST7121" : "ST7123", s_pw, s_ph, s_px, s_py);
    return ESP_OK;
}

void display_preview(const uint8_t *rgb)
{
    if (!s_task) return;
    // Copy so the player can reuse its buffer; if the preview task is still
    // drawing the previous frame this one is simply skipped.
    if (eTaskGetState(s_task) != eBlocked) return;
    if (rgb) memcpy(s_copy, rgb, (size_t)s_cw * s_ch * 3);
    else memset(s_copy, 0, (size_t)s_cw * s_ch * 3);
    s_pending = s_copy;
    xTaskNotifyGive(s_task);
}
