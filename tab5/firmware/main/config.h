// /sdcard/config.json - same schema as the Mac app's config.json, plus a
// "tab5" block. Clip paths of the Mac config ("mp4/Intro.mp4") map to
// /sdcard/ftc/<basename>.ftc, so one config can drive both rigs.
//
//   "tab5": {
//     "wifi_mode": "ap" | "sta",     default "ap"
//     "ssid": "...", "password": "...",
//     "channel": 6, "ap_ip": "192.168.10.1",
//     "volume": 20                   0..100
//   }
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "net.h"

#define CFG_MAX_PANELS 8
#define CFG_MAX_MAPPINGS 64

typedef struct {
    char name[32];
    char ip[16];
    uint16_t port;
    int src_x, src_y, src_w, src_h;
} cfg_panel_t;

typedef struct {
    uint8_t note;
    char clip[96];  // resolved path on the SD card
} cfg_mapping_t;

typedef struct {
    int video_w, video_h;
    int midi_channel;  // 0..15, -1 = any
    int stop_note;     // -1 = none
    int volume;
    char midi_device[32];  // USB name filter, default "FANTOM"
    net_config_t net;
    int npanels;
    cfg_panel_t panels[CFG_MAX_PANELS];
    int nmappings;
    cfg_mapping_t mappings[CFG_MAX_MAPPINGS];
} config_t;

// Fills defaults, then overrides from path if it exists. false only if the
// file exists but is not valid JSON (defaults stay in place).
bool config_load(config_t *c, const char *path);
void config_log(const config_t *c);
