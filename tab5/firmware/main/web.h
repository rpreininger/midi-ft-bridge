#pragma once

#include "config.h"

// Control page and API on port 80, see web.c for the endpoints.
void web_start(const config_t *cfg);
