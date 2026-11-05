#pragma once

#include <stdbool.h>
#include "config.h"

// target_ip NULL = send to the configured panel IPs; otherwise every panel
// stream goes to target_ip on port (panel port + panel index).
void stream_test_run(const config_t *cfg, const char *clip, const char *target_ip, int seconds, bool audio);
