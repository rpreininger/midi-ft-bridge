// The playout engine (Tab5 counterpart of src/engine.cpp): one clip at a
// time, audio as master clock, panel regions sent as FT/UDP from a separate
// task on the other core.
#pragma once

#include "config.h"

void player_start(const config_t *cfg);
// Play the clip mapped to note (replaces the running one). false if unmapped.
bool player_note(uint8_t note);
// Play a clip by path (web UI).
void player_play_path(const char *path);
// Stop and black the panels.
void player_stop(void);
// Optional: redirect all panel streams to one IP (test receiver), NULL = off.
void player_set_test_target(const char *ip);

typedef struct {
    bool playing;
    char clip[48];          // clip name without path/extension, "" if none
    uint32_t frame, frames; // position / length
    uint16_t fps;
    int npanels;
    bool panel_ok[CFG_MAX_PANELS];  // last frame got through (ARP answered, no send error)
    bool mirrored;          // streams go to a test client instead of the panels
} player_status_t;

void player_get_status(player_status_t *st);
