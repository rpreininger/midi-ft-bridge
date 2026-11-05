// The playout engine (Tab5 counterpart of src/engine.cpp): one clip at a
// time, audio as master clock, panel regions sent as FT/UDP from a separate
// task on the other core.
#pragma once

#include "config.h"

void player_start(const config_t *cfg);
// Play the clip mapped to note (replaces the running one). false if unmapped.
bool player_note(uint8_t note);
// Stop and black the panels.
void player_stop(void);
// Optional: redirect all panel streams to one IP (test receiver), NULL = off.
void player_set_test_target(const char *ip);
