#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "bh1750_sensor.h"
#include "dht_sensor.h"
#include "monitor.h"
#include "sensor_types.h"

static const char *TAG = "MONITOR";

static void monitor_task(void *arg)
{
    (void)arg;

    dht_data_t last_dht = {
        .temperature = -273.15f,
        .humidity = 0.0f,
        .is_valid = false,
        .timestamp = 0,
    };
    bh1750_data_t last_bh1750 = {
        .lux = -1.0f,
        .is_valid = false,
        .timestamp = 0,
    };

    while (true) {
        dht_data_t dht_item;
        while (xQueueReceive(dht_queue, &dht_item, 0) == pdPASS) {
            last_dht = dht_item;
        }

        (void)xQueuePeek(bh1750_queue, &last_bh1750, 0);

        ESP_LOGI(TAG, "DHT: T=%.1fC, H=%.1f%%, valid=%d",
                 last_dht.temperature,
                 last_dht.humidity,
                 last_dht.is_valid ? 1 : 0);
        ESP_LOGI(TAG, "BH1750: Lux=%.1f, valid=%d",
                 last_bh1750.lux,
                 last_bh1750.is_valid ? 1 : 0);

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void monitor_start(void)
{
    BaseType_t task_created = xTaskCreate(monitor_task, "monitor_task", 4096, NULL, tskIDLE_PRIORITY + 1, NULL);
    configASSERT(task_created == pdPASS);
}
