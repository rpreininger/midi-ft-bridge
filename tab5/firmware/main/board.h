// Tab5 board bring-up: I2C, the two PI4IOE5V6416 IO expanders, microSD,
// ES8388 audio out. Pins and init sequences follow M5Stack's Tab5 BSP
// (M5Tab5-UserDemo, components/m5stack_tab5, Apache-2.0).
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "sd_protocol_types.h"

#define BOARD_SD_MOUNT "/sdcard"

esp_err_t board_init(void);               // I2C + IO expanders (power rails)
esp_err_t board_sd_mount(void);           // 4-bit SDMMC, 40 MHz, FAT
sdmmc_card_t *board_sd_card(void);
esp_err_t board_audio_init(uint32_t rate); // I2S1 TX stereo s16 + ES8388
esp_err_t board_audio_set_volume(int vol); // 0..100

// Blocking write of interleaved s16 stereo PCM; returns when DMA took it.
esp_err_t board_audio_write(const void *pcm, size_t bytes);
