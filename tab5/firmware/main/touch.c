#include "touch.h"

#include "esp_log.h"

#define LAND_W 1280  // landscape width (display.c: portrait 720 x 1280, rotated)

static const char *TAG = "touch";
static i2c_master_dev_handle_t s_dev;

static esp_err_t rd(uint16_t reg, uint8_t *buf, size_t n)
{
    uint8_t r[2] = {reg >> 8, reg & 0xFF};
    return i2c_master_transmit_receive(s_dev, r, 2, buf, n, 20);
}

esp_err_t touch_init(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t dc = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x55, .scl_speed_hz = 400000};
    esp_err_t e = i2c_master_bus_add_device(bus, &dc, &s_dev);
    uint8_t max = 0;
    if (e == ESP_OK) e = rd(0x0009, &max, 1);
    ESP_LOGI(TAG, "touch controller %s, max touches %u", e == ESP_OK ? "ok" : esp_err_to_name(e), max);
    return e;
}

// Same sequence as M5GFX's Touch_ST7123: adv info, max touches, then all
// report slots in one read (reading only the first slot freezes the report).
bool touch_read(int *x, int *y)
{
    uint8_t adv = 0, max = 0, p[10 * 7];
    if (!s_dev || rd(0x0010, &adv, 1) != ESP_OK || !(adv & 0x08)) return false;
    if (rd(0x0009, &max, 1) != ESP_OK) return false;
    if (max > 10) max = 10;
    if (max == 0 || rd(0x0014, p, max * 7) != ESP_OK) return false;
    for (int i = 0; i < max; i++) {
        const uint8_t *r = p + i * 7;
        if (!(r[0] & 0x80)) continue;
        int px = (r[0] & 0x3F) << 8 | r[1], py = r[2] << 8 | r[3];  // portrait 720 x 1280
        // portrait (px, py) -> landscape: display.c maps landscape (lx, ly) to
        // portrait (x = ly, y = 1279 - lx)
        *x = LAND_W - 1 - py;
        *y = px;
        return true;
    }
    return false;
}
