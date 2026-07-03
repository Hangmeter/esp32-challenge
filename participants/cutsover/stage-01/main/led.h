#pragma once

#include <stdbool.h>

#include "freertos/FreeRTOS.h"

typedef enum {
    LED_MODE_SLOW = 1,
    LED_MODE_MEDIUM = 2,
    LED_MODE_FAST = 4,
} led_mode_t;

extern volatile led_mode_t g_system_mode;

void led_init(void);
void led_start(void);
void led_set_error_mode(bool enabled);
