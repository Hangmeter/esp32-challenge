#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "fsm.h"

typedef struct {
    float temperature;
    float humidity;
    float lux;
    bool dht_valid;
    bool bh1750_valid;
    uint32_t uptime;
    system_state_t state;
} sensor_metrics_t;

esp_err_t json_builder_build_sensor_payload(const sensor_metrics_t *metrics, char *buffer, size_t buffer_size);
