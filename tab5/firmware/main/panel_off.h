// Shut the FT panels down over their HTTP endpoint (token in config.json or NVS).
#pragma once

#include <stdbool.h>
#include "config.h"

void panel_off_init(const config_t *cfg);
bool panel_off_has_token(void);
bool panel_off_set_token(const char *token);  // "" forgets it
// Both return how many panels answered 200 and write one line per panel.
int panel_off_ping(char *report, int cap);
int panel_off_all(char *report, int cap);
