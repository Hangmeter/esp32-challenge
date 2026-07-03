#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/inet.h"

#include "app_config.h"
#include "fsm.h"
#include "wifi_manager.h"

static const char *TAG = "WIFI";

static TaskHandle_t s_wifi_task_handle;
static int s_retry_count;
static bool s_has_ip;

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;

    if (event_id == WIFI_EVENT_STA_START) {
        s_has_ip = false;
        fsm_post_wifi_connecting();
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
        fsm_post_wifi_connecting();
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_has_ip = false;
        fsm_post_wifi_recovery();
        if (s_wifi_task_handle != NULL) {
            xTaskNotifyGive(s_wifi_task_handle);
        }
    }
}

static void got_ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;

    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    s_retry_count = 0;
    s_has_ip = true;

    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    fsm_post_wifi_connected();
}

static void wifi_task(void *arg)
{
    (void)arg;

    while (true) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (s_retry_count < WIFI_MAX_RETRY) {
            ++s_retry_count;
            ESP_LOGI(TAG, "Reconnect attempt %d/%d", s_retry_count, WIFI_MAX_RETRY);
            vTaskDelay(pdMS_TO_TICKS(5000));
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
                ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
            }
            break;
        }

        if (s_retry_count >= WIFI_MAX_RETRY) {
            ESP_LOGE(TAG, "Wi-Fi reconnect failed after %d attempts", WIFI_MAX_RETRY);
            fsm_post_wifi_failed();
            s_retry_count = 0;
        }
    }
}

void wifi_manager_start(void)
{
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &wifi_event_handler,
        NULL,
        &instance_any_id));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        &got_ip_event_handler,
        NULL,
        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    BaseType_t task_created = xTaskCreate(wifi_task, "wifi_task", 4096, NULL, tskIDLE_PRIORITY + 4, &s_wifi_task_handle);
    configASSERT(task_created == pdPASS);

    ESP_ERROR_CHECK(esp_wifi_start());
}

bool wifi_manager_is_connected(void)
{
    return s_has_ip;
}

void wifi_manager_reconnect_now(void)
{
    s_has_ip = false;
    s_retry_count = 0;
    fsm_post_wifi_connecting();
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "Reconnect request failed: %s", esp_err_to_name(err));
    }
}
