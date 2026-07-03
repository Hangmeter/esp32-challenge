#include <stdbool.h>

#include "freertos/FreeRTOS.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "bh1750_sensor.h"
#include "dht_sensor.h"
#include "fsm.h"
#include "led.h"
#include "monitor.h"
#include "wifi_manager.h"

static const char *TAG = "APP";

static void app_init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32 challenge stage 01 starting");

    app_init_nvs();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    led_init();
    led_start();

    fsm_start();
    dht_sensor_start();
    bh1750_sensor_start();
    monitor_start();
    wifi_manager_start();
}
