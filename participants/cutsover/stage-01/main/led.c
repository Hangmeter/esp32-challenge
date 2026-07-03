#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "driver/gpio.h"
#include "esp_log.h"

#include "app_config.h"
#include "led.h"

static const char *TAG = "LED";

volatile led_mode_t g_system_mode = LED_MODE_SLOW;

static TimerHandle_t s_mode_timer;
static volatile bool s_error_mode;

static void led_all_off(void)
{
    gpio_set_level(LED_RED_GPIO, 0);
    gpio_set_level(LED_GREEN_GPIO, 0);
    gpio_set_level(LED_BLUE_GPIO, 0);
}

static void led_set_channel(gpio_num_t gpio_num)
{
    led_all_off();
    gpio_set_level(gpio_num, 1);
}

static void mode_timer_callback(TimerHandle_t timer)
{
    (void)timer;

    if (s_error_mode) {
        return;
    }

    switch (g_system_mode) {
    case LED_MODE_SLOW:
        g_system_mode = LED_MODE_MEDIUM;
        break;
    case LED_MODE_MEDIUM:
        g_system_mode = LED_MODE_FAST;
        break;
    case LED_MODE_FAST:
    default:
        g_system_mode = LED_MODE_SLOW;
        break;
    }

    ESP_LOGI(TAG, "Switching to mode: %d Hz", (int)g_system_mode);
}

static TickType_t led_delay_for_mode(led_mode_t mode)
{
    switch (mode) {
    case LED_MODE_FAST:
        return pdMS_TO_TICKS(125);
    case LED_MODE_MEDIUM:
        return pdMS_TO_TICKS(250);
    case LED_MODE_SLOW:
    default:
        return pdMS_TO_TICKS(500);
    }
}

static gpio_num_t led_gpio_for_mode(led_mode_t mode)
{
    switch (mode) {
    case LED_MODE_FAST:
        return LED_BLUE_GPIO;
    case LED_MODE_MEDIUM:
        return LED_GREEN_GPIO;
    case LED_MODE_SLOW:
    default:
        return LED_RED_GPIO;
    }
}

static void led_control_task(void *arg)
{
    (void)arg;

    while (true) {
        led_mode_t mode = s_error_mode ? LED_MODE_SLOW : g_system_mode;
        gpio_num_t gpio_num = s_error_mode ? LED_RED_GPIO : led_gpio_for_mode(mode);
        TickType_t delay = led_delay_for_mode(mode);

        led_set_channel(gpio_num);
        vTaskDelay(delay);
        led_all_off();
        vTaskDelay(delay);
    }
}

void led_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_RED_GPIO) | (1ULL << LED_GREEN_GPIO) | (1ULL << LED_BLUE_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&io_conf));
    led_all_off();
}

void led_start(void)
{
    s_mode_timer = xTimerCreate("led_mode_timer", pdMS_TO_TICKS(10000), pdTRUE, NULL, mode_timer_callback);
    configASSERT(s_mode_timer != NULL);
    configASSERT(xTimerStart(s_mode_timer, 0) == pdPASS);

    BaseType_t task_created = xTaskCreate(led_control_task, "led_task", 2048, NULL, tskIDLE_PRIORITY + 2, NULL);
    configASSERT(task_created == pdPASS);
}

void led_set_error_mode(bool enabled)
{
    s_error_mode = enabled;
}
