// Tab5 board bring-up. Pin map and IO-expander sequences are taken from
// M5Stack's Tab5 BSP (M5Tab5-UserDemo/platforms/tab5/components/m5stack_tab5,
// SPDX Apache-2.0, (c) 2024 Espressif Systems / M5Stack).
#include "board.h"

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/sdmmc_host.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"
#include <string.h>

static const char *TAG = "board";

#define I2C_SDA 31
#define I2C_SCL 32

#define I2S_MCLK 30
#define I2S_BCLK 27
#define I2S_WS 29
#define I2S_DOUT 26
#define I2S_DIN 28

#define SD_CLK 43
#define SD_CMD 44
#define SD_D0 39
#define SD_D1 40
#define SD_D2 41
#define SD_D3 42
#define SD_LDO_CHAN 4  // on-chip LDO VO4 powers the SD IO

#define PI4IOE1 0x43
#define PI4IOE2 0x44
#define PI4IO_REG_CHIP_RESET 0x01
#define PI4IO_REG_IO_DIR 0x03
#define PI4IO_REG_OUT_SET 0x05
#define PI4IO_REG_OUT_H_IM 0x07
#define PI4IO_REG_IN_DEF_STA 0x09
#define PI4IO_REG_PULL_EN 0x0B
#define PI4IO_REG_PULL_SEL 0x0D
#define PI4IO_REG_INT_MASK 0x11

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_io1, s_io2;
static sdmmc_card_t *s_card;
static i2s_chan_handle_t s_tx;
static esp_codec_dev_handle_t s_spk;

static void io_wr(i2c_master_dev_handle_t d, uint8_t reg, uint8_t val)
{
    uint8_t b[2] = {reg, val};
    i2c_master_transmit(d, b, 2, 50);
}

static void io_rd(i2c_master_dev_handle_t d, uint8_t reg)
{
    uint8_t v;
    i2c_master_transmit_receive(d, &reg, 1, &v, 1, 50);
}

esp_err_t board_init(void)
{
    i2c_master_bus_config_t bus = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .i2c_port = 0,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus, &s_i2c), TAG, "i2c bus");

    i2c_device_config_t d1 = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = PI4IOE1, .scl_speed_hz = 400000};
    i2c_device_config_t d2 = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = PI4IOE2, .scl_speed_hz = 400000};
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &d1, &s_io1), TAG, "pi4ioe1");
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &d2, &s_io2), TAG, "pi4ioe2");

    // Expander 1: LCD/touch reset, speaker amp enable (P1), ext 5V, ...
    io_wr(s_io1, PI4IO_REG_CHIP_RESET, 0xFF);
    io_rd(s_io1, PI4IO_REG_CHIP_RESET);
    io_wr(s_io1, PI4IO_REG_PULL_SEL, 0x7F);
    io_wr(s_io1, PI4IO_REG_PULL_EN, 0x7F);
    io_wr(s_io1, PI4IO_REG_OUT_SET, 0x66);
    io_wr(s_io1, PI4IO_REG_OUT_H_IM, 0x00);
    io_wr(s_io1, PI4IO_REG_IO_DIR, 0x7F);
    vTaskDelay(pdMS_TO_TICKS(10));
    io_wr(s_io1, PI4IO_REG_IO_DIR, 0x6F);
    vTaskDelay(pdMS_TO_TICKS(50));

    // Expander 2: P0 WLAN power, P3 USB-A 5V, P7 charge enable.
    io_wr(s_io2, PI4IO_REG_CHIP_RESET, 0xFF);
    io_rd(s_io2, PI4IO_REG_CHIP_RESET);
    io_wr(s_io2, PI4IO_REG_IO_DIR, 0xB9);
    io_wr(s_io2, PI4IO_REG_OUT_H_IM, 0x06);
    io_wr(s_io2, PI4IO_REG_PULL_SEL, 0xB9);
    io_wr(s_io2, PI4IO_REG_PULL_EN, 0xF9);
    io_wr(s_io2, PI4IO_REG_IN_DEF_STA, 0x40);
    io_wr(s_io2, PI4IO_REG_INT_MASK, 0xBF);
    io_wr(s_io2, PI4IO_REG_OUT_SET, 0x09);  // WLAN power + USB 5V on

    char found[128] = "";
    for (int a = 0x08; a < 0x78; a++)
        if (i2c_master_probe(s_i2c, a, 20) == ESP_OK)
            snprintf(found + strlen(found), sizeof found - strlen(found), " %02x", a);
    ESP_LOGI(TAG, "I2C devices:%s", found);
    return ESP_OK;
}

esp_err_t board_sd_mount(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    static sd_pwr_ctrl_handle_t pwr;
    if (!pwr) {
        sd_pwr_ctrl_ldo_config_t ldo = {.ldo_chan_id = SD_LDO_CHAN};
        ESP_RETURN_ON_ERROR(sd_pwr_ctrl_new_on_chip_ldo(&ldo, &pwr), TAG, "sd ldo");
    }
    host.pwr_ctrl_handle = pwr;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = SD_CLK;
    slot.cmd = SD_CMD;
    slot.d0 = SD_D0;
    slot.d1 = SD_D1;
    slot.d2 = SD_D2;
    slot.d3 = SD_D3;

    esp_vfs_fat_sdmmc_mount_config_t mc = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };
    ESP_RETURN_ON_ERROR(esp_vfs_fat_sdmmc_mount(BOARD_SD_MOUNT, &host, &slot, &mc, &s_card), TAG, "sd mount");
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

sdmmc_card_t *board_sd_card(void) { return s_card; }
i2c_master_bus_handle_t board_i2c(void) { return s_i2c; }

esp_err_t board_audio_init(uint32_t rate)
{
    // TX only: we never record, and sharing the controller with the RX TDM
    // config of the BSP is not needed.
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    cc.auto_clear = true;
    cc.dma_desc_num = 6;
    cc.dma_frame_num = 480;  // 10 ms per descriptor at 48 kHz
    ESP_RETURN_ON_ERROR(i2s_new_channel(&cc, &s_tx, NULL), TAG, "i2s chan");

    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std), TAG, "i2s std");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "i2s enable");

    audio_codec_i2s_cfg_t ic = {.port = I2S_NUM_1, .tx_handle = s_tx, .rx_handle = NULL};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&ic);
    audio_codec_i2c_cfg_t i2c_cfg = {.port = 0, .addr = ES8388_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c};
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && ctrl_if && gpio_if, ESP_FAIL, TAG, "codec if");

    es8388_codec_cfg_t es = {
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .master_mode = false,
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .pa_pin = -1,  // amp enable is IO expander 1 P1, already high
    };
    const audio_codec_if_t *codec_if = es8388_codec_new(&es);
    ESP_RETURN_ON_FALSE(codec_if, ESP_FAIL, TAG, "es8388");

    esp_codec_dev_cfg_t dc = {.dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec_if, .data_if = data_if};
    s_spk = esp_codec_dev_new(&dc);
    ESP_RETURN_ON_FALSE(s_spk, ESP_FAIL, TAG, "codec dev");

    esp_codec_dev_sample_info_t fs = {.sample_rate = rate, .channel = 2, .bits_per_sample = 16};
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_spk, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "codec open");
    board_audio_set_volume(20);  // quiet default; config.json will set it
    return ESP_OK;
}

esp_err_t board_audio_set_volume(int vol)
{
    if (!s_spk) return ESP_ERR_INVALID_STATE;
    esp_codec_dev_set_out_mute(s_spk, vol <= 0);
    return esp_codec_dev_set_out_vol(s_spk, vol) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t board_audio_write(const void *pcm, size_t bytes)
{
    size_t done = 0;
    return i2s_channel_write(s_tx, pcm, bytes, &done, portMAX_DELAY);
}
