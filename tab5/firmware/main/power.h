#pragma once

#include <stdbool.h>
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
    bool present;
    float volts, amps;  // amps: sign tells charge/discharge direction
    int percent;
} power_state_t;

void power_init(i2c_master_bus_handle_t bus);
bool power_read(power_state_t *st);
