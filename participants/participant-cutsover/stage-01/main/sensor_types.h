#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"

typedef struct {
    float temperature;
    float humidity;
    bool is_valid;
    TickType_t timestamp;
} dht_data_t;

typedef struct {
    float lux;
    bool is_valid;
    TickType_t timestamp;
} bh1750_data_t;
