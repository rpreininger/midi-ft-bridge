// Line commands on the USB-C console (for tests from the dev machine):
//   play <Name>      start /sdcard/ftc/<Name>.ftc
//   stop
//   heap             memory + open state
//   stress <s> [n]   n rounds (default 50): random clip every <s> seconds,
//                    a stop every 5th round
#include "console.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "player.h"
#include "net.h"
#include "usb_audio.h"
#include "esp_system.h"

static const char *TAG = "console";

static void heap(void)
{
    ESP_LOGI(TAG, "dma free %u largest %u, internal free %u, psram free %u largest %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

static void stress(int secs, int rounds)
{
    static char names[32][64];
    int n = 0;
    DIR *d = opendir("/sdcard/ftc");
    struct dirent *e;
    while (d && n < 32 && (e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (e->d_name[0] == '.' || l < 5 || strcasecmp(e->d_name + l - 4, ".ftc")) continue;
        snprintf(names[n++], sizeof names[0], "/sdcard/ftc/%s", e->d_name);
    }
    if (d) closedir(d);
    if (!n) return;
    for (int r = 0; r < rounds; r++) {
        if (r % 5 == 4) player_stop();
        else player_play_path(names[esp_random() % n]);
        vTaskDelay(pdMS_TO_TICKS(secs * 1000));
        if (r % 10 == 9) heap();
    }
    ESP_LOGI(TAG, "stress done (%d rounds)", rounds);
}

static void task(void *arg)
{
    char line[96];
    for (;;) {
        if (!fgets(line, sizeof line, stdin)) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "play ", 5)) {
            char path[100];
            snprintf(path, sizeof path, "/sdcard/ftc/%.60s.ftc", line + 5);
            player_play_path(path);
        } else if (!strcmp(line, "stop")) {
            player_stop();
        } else if (!strcmp(line, "heap")) {
            heap();
        } else if (!strncmp(line, "stress", 6)) {
            int s = 3, k = 50;
            sscanf(line + 6, "%d %d", &s, &k);
            stress(s < 1 ? 1 : s, k);
        } else if (!strncmp(line, "wifi", 4)) {
            char ssid[33] = "", pw[65] = "";
            int k = sscanf(line + 4, " %32s %64[^\n]", ssid, pw);
            if (k >= 1 && strcmp(ssid, "forget")) {
                net_save_wifi(ssid, pw);
                ESP_LOGI(TAG, "saved Wi-Fi '%s' (%d chars password) - 'reboot' to use it", ssid, (int)strlen(pw));
            } else if (k == 1) {
                net_save_wifi("", "");
                ESP_LOGI(TAG, "saved Wi-Fi forgotten (defaults after reboot)");
            } else {
                ESP_LOGI(TAG, "wifi <ssid> <password> | wifi forget");
            }
        } else if (!strncmp(line, "diag ", 5)) {  // diag <nosend 0|1> <nopreview 0|1>
            extern volatile bool g_diag_nosend;
            extern volatile int g_diag_preview_div;
            int a = 0, b = 1;
            sscanf(line + 5, "%d %d", &a, &b);
            g_diag_nosend = a, g_diag_preview_div = b;
            ESP_LOGI(TAG, "diag: sending %s, preview every %d. frame (0 = off)", a ? "OFF" : "on", b);
        } else if (!strncmp(line, "cushion ", 8)) {
            usb_audio_set_cushion(atoi(line + 8));
        } else if (!strncmp(line, "toneclip ", 9)) {
            char name[64] = "Intro";
            int s = 60;
            sscanf(line + 9, "%63s %d", name, &s);
            char path[100];
            snprintf(path, sizeof path, "/sdcard/ftc/%s.ftc", name);
            usb_audio_tone_load(true);
            player_play_path(path);
            vTaskDelay(pdMS_TO_TICKS(s * 1000));
            usb_audio_tone_load(false);
            player_stop();
        } else if (!strncmp(line, "tone", 4)) {
            int s = 60;
            sscanf(line + 4, "%d", &s);
            player_stop();
            vTaskDelay(pdMS_TO_TICKS(300));
            usb_audio_tone_test(s);
        } else if (!strncmp(line, "usbgain ", 8)) {
            usb_audio_set_gain_db(strtof(line + 8, NULL));
        } else if (!strncmp(line, "rec", 3)) {
            int s = 8;
            sscanf(line + 3, "%d", &s);
            usb_audio_record(s < 1 ? 1 : s > 20 ? 20 : s);
        } else if (!strcmp(line, "reboot")) {
            esp_restart();
        } else if (line[0]) {
            ESP_LOGI(TAG, "commands: play <Name> | stop | heap | stress <s> [n] | wifi <ssid> <pw> | wifi forget | reboot | tone <s> | toneclip <Name> <s> | rec <s> | usbgain <dB> | diag <a> <b>");
        }
    }
}

void console_start(void)
{
    usb_serial_jtag_driver_config_t c = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    if (usb_serial_jtag_driver_install(&c) != ESP_OK) return;
    usb_serial_jtag_vfs_use_driver();
    setvbuf(stdin, NULL, _IONBF, 0);
    xTaskCreatePinnedToCore(task, "console", 6144, NULL, 2, NULL, 0);
}
