#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#include "app_config.h"
#include "dht_sensor.h"
#include "fsm.h"
#include "sensor_types.h"

static const char *TAG = "DHT";

QueueHandle_t dht_queue;

static esp_err_t wait_for_level(int level, uint32_t timeout_us, uint32_t *duration_us)
{
    uint32_t count = 0;

    while (gpio_get_level(DHT22_GPIO) == level) {
        if (count++ >= timeout_us) {
            return ESP_ERR_TIMEOUT;
        }
        esp_rom_delay_us(1);
    }

    if (duration_us != NULL) {
        *duration_us = count;
    }

    return ESP_OK;
}

static esp_err_t dht22_read_raw(uint8_t data[5])
{
    for (size_t i = 0; i < 5; ++i) {
        data[i] = 0;
    }

    gpio_set_direction(DHT22_GPIO, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(DHT22_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(DHT22_GPIO, 1);
    esp_rom_delay_us(30);
    gpio_set_direction(DHT22_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(DHT22_GPIO, GPIO_PULLUP_ONLY);

    if (wait_for_level(1, 100, NULL) != ESP_OK ||
        wait_for_level(0, 100, NULL) != ESP_OK ||
        wait_for_level(1, 100, NULL) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }

    for (size_t bit = 0; bit < 40; ++bit) {
        uint32_t high_duration = 0;

        if (wait_for_level(0, 70, NULL) != ESP_OK ||
            wait_for_level(1, 100, &high_duration) != ESP_OK) {
            return ESP_ERR_TIMEOUT;
        }

        data[bit / 8] <<= 1;
        if (high_duration > 40) {
            data[bit / 8] |= 1;
        }
    }

    uint8_t checksum = data[0] + data[1] + data[2] + data[3];
    if (checksum != data[4]) {
        return ESP_ERR_INVALID_CRC;
    }

    return ESP_OK;
}

static dht_data_t read_dht22(void)
{
    uint8_t raw[5];
    dht_data_t data = {
        .temperature = -273.15f,
        .humidity = 0.0f,
        .is_valid = false,
        .timestamp = xTaskGetTickCount(),
    };

    esp_err_t err = dht22_read_raw(raw);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Sensor read failed");
        return data;
    }

    uint16_t humidity_raw = ((uint16_t)raw[0] << 8) | raw[1];
    uint16_t temperature_raw = ((uint16_t)(raw[2] & 0x7F) << 8) | raw[3];

    data.humidity = (float)humidity_raw / 10.0f;
    data.temperature = (float)temperature_raw / 10.0f;
    if ((raw[2] & 0x80) != 0) {
        data.temperature = -data.temperature;
    }
    data.is_valid = true;

    return data;
}

static void dht_task(void *arg)
{
    (void)arg;

    while (true) {
        if (fsm_are_sensors_enabled()) {
            dht_data_t data = read_dht22();
            if (xQueueSend(dht_queue, &data, 0) != pdPASS) {
                dht_data_t dropped;
                (void)xQueueReceive(dht_queue, &dropped, 0);
                (void)xQueueSend(dht_queue, &data, 0);
            }
            fsm_post_dht_data(&data);
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void dht_sensor_start(void)
{
    dht_queue = xQueueCreate(10, sizeof(dht_data_t));
    configASSERT(dht_queue != NULL);

    BaseType_t task_created = xTaskCreate(dht_task, "dht_task", 4096, NULL, tskIDLE_PRIORITY + 1, NULL);
    configASSERT(task_created == pdPASS);
}
