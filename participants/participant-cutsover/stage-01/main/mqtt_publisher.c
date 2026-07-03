#include <stdbool.h>
#include <inttypes.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_mqtt_client.h"
#include "esp_timer.h"

#include "app_config.h"
#include "fsm.h"
#include "json_builder.h"
#include "mqtt_publisher.h"
#include "sensor_types.h"

static const char *TAG = "MQTT";

static esp_mqtt_client_handle_t s_client;
static TaskHandle_t s_mqtt_task_handle;
static bool s_started;
static bool s_connected;
static int s_disconnect_count;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;

    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        s_disconnect_count = 0;
        ESP_LOGI(TAG, "MQTT connected");
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ++s_disconnect_count;
        ESP_LOGW(TAG, "MQTT disconnected");
        if (s_disconnect_count >= 3) {
            fsm_post_mqtt_error();
        }
        break;
    case MQTT_EVENT_ERROR:
        s_connected = false;
        ESP_LOGE(TAG, "MQTT error");
        fsm_post_wifi_recovery();
        break;
    default:
        ESP_LOGD(TAG, "MQTT event id=%" PRId32, event->event_id);
        break;
    }
}

static void mqtt_task(void *arg)
{
    (void)arg;

    char payload[512];

    while (true) {
        if (s_started && s_connected) {
            dht_data_t dht_data;
            bh1750_data_t bh1750_data;

            if (fsm_get_latest_sensor_data(&dht_data, &bh1750_data)) {
                sensor_metrics_t metrics = {
                    .temperature = dht_data.temperature,
                    .humidity = dht_data.humidity,
                    .lux = bh1750_data.lux,
                    .dht_valid = dht_data.is_valid,
                    .bh1750_valid = bh1750_data.is_valid,
                    .uptime = (uint32_t)(esp_timer_get_time() / 1000000LL),
                    .state = fsm_get_state(),
                };

                if (json_builder_build_sensor_payload(&metrics, payload, sizeof(payload)) != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to build JSON payload");
                    vTaskDelay(pdMS_TO_TICKS(10000));
                    continue;
                }

                int msg_id = esp_mqtt_client_publish(s_client, MQTT_TOPIC, payload, 0, 1, 0);
                ESP_LOGI(TAG, "Published metrics msg_id=%d", msg_id);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void mqtt_publisher_start(void)
{
    if (s_client == NULL) {
        esp_mqtt_client_config_t mqtt_cfg = {
            .broker.address.uri = MQTT_BROKER_URI,
            .broker.address.port = MQTT_PORT,
            .credentials.client_id = MQTT_CLIENT_ID,
        };

        s_client = esp_mqtt_client_init(&mqtt_cfg);
        configASSERT(s_client != NULL);
        ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_client, MQTT_EVENT_CONNECTED, mqtt_event_handler, NULL));
        ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_client, MQTT_EVENT_DISCONNECTED, mqtt_event_handler, NULL));
        ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_client, MQTT_EVENT_ERROR, mqtt_event_handler, NULL));
    }

    if (!s_started) {
        ESP_ERROR_CHECK(esp_mqtt_client_start(s_client));
        s_started = true;
        s_disconnect_count = 0;
    }

    if (s_mqtt_task_handle == NULL) {
        BaseType_t task_created = xTaskCreate(mqtt_task, "mqtt_task", 4096, NULL, tskIDLE_PRIORITY + 2, &s_mqtt_task_handle);
        configASSERT(task_created == pdPASS);
    }
}

void mqtt_publisher_stop(void)
{
    if (s_client != NULL && s_started) {
        esp_err_t err = esp_mqtt_client_stop(s_client);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "MQTT stop failed: %s", esp_err_to_name(err));
        }
    }

    s_started = false;
    s_connected = false;
}
