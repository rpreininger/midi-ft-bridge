// Battery monitor: INA226 at I2C 0x41 on the battery rail, 5 mOhm shunt
// (as in M5's Tab5 demo). NP-F550 style 2S Li-ion pack.
#include "power.h"

#include "esp_log.h"

static const char *TAG = "power";
static i2c_master_dev_handle_t s_ina;

static esp_err_t wr16(i2c_master_dev_handle_t d, uint8_t reg, uint16_t v)
{
    uint8_t b[3] = {reg, v >> 8, v & 0xFF};
    return i2c_master_transmit(d, b, 3, 50);
}

static esp_err_t rd16(i2c_master_dev_handle_t d, uint8_t reg, uint16_t *v)
{
    uint8_t b[2];
    esp_err_t e = i2c_master_transmit_receive(d, &reg, 1, b, 2, 50);
    *v = (uint16_t)(b[0] << 8 | b[1]);
    return e;
}

static i2c_master_dev_handle_t add(i2c_master_bus_handle_t bus, uint8_t addr)
{
    i2c_device_config_t dc = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = 400000};
    i2c_master_dev_handle_t d = NULL;
    if (i2c_master_bus_add_device(bus, &dc, &d) != ESP_OK) return NULL;
    // 16x averaging, 1.1 ms bus + shunt conversion, continuous shunt+bus
    wr16(d, 0x00, (2 << 9) | (4 << 6) | (4 << 3) | 7);
    return d;
}

static bool read(i2c_master_dev_handle_t d, float *volts, float *amps)
{
    uint16_t vb, vs;
    if (!d || rd16(d, 0x02, &vb) != ESP_OK || rd16(d, 0x01, &vs) != ESP_OK) return false;
    *volts = vb * 1.25e-3f;                // bus voltage LSB 1.25 mV
    *amps = (int16_t)vs * 2.5e-6f / 0.005f; // shunt LSB 2.5 uV over 5 mOhm
    return true;
}

void power_init(i2c_master_bus_handle_t bus)
{
    s_ina = add(bus, 0x41);  // (0x40 on this bus is not an INA226)
    vTaskDelay(pdMS_TO_TICKS(40));  // let it average once
    float v, a;
    if (read(s_ina, &v, &a)) ESP_LOGI(TAG, "battery %.3f V, %.3f A", v, a);
}

bool power_read(power_state_t *st)
{
    if (!read(s_ina, &st->volts, &st->amps)) return false;
    // 2S Li-ion: ~6.4 V empty under load, ~8.3 V full. Linear is good
    // enough for a stage warning.
    float p = (st->volts - 6.4f) / (8.3f - 6.4f) * 100.0f;
    st->percent = p < 0 ? 0 : p > 100 ? 100 : (int)p;
    st->present = st->volts > 5.0f;
    return true;
}
