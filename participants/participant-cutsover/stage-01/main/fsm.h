#pragma once

#include <stdbool.h>

#include "freertos/FreeRTOS.h"

#include "sensor_types.h"

typedef enum {
    ST_INIT = 0,
    ST_IDLE = 1,
    ST_READING = 2,
    ST_PROCESSING = 3,
    ST_ERROR = 4,
    ST_CONNECTING = 5,
    ST_CONNECTED = 6,
    ST_RECOVERY = 7,
} system_state_t;

const char *state_to_string(system_state_t state);
void fsm_start(void);
system_state_t fsm_get_state(void);
bool fsm_are_sensors_enabled(void);
bool fsm_get_latest_sensor_data(dht_data_t *dht_data, bh1750_data_t *bh1750_data);
void fsm_post_dht_data(const dht_data_t *data);
void fsm_post_bh1750_data(const bh1750_data_t *data);
void fsm_post_wifi_connected(void);
void fsm_post_wifi_connecting(void);
void fsm_post_wifi_recovery(void);
void fsm_post_wifi_failed(void);
void fsm_post_mqtt_error(void);
