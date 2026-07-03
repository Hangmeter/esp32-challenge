#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_timer.h"
#include "cJSON.h"

#include "app_config.h"
#include "json_builder.h"

static void add_number_or_null(cJSON *object, const char *name, float value, bool is_valid)
{
    if (is_valid) {
        cJSON_AddNumberToObject(object, name, value);
    } else {
        cJSON_AddNullToObject(object, name);
    }
}

esp_err_t json_builder_build_sensor_payload(const sensor_metrics_t *metrics, char *buffer, size_t buffer_size)
{
    if (metrics == NULL || buffer == NULL || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *metrics_object = cJSON_CreateObject();
    cJSON *status_object = cJSON_CreateObject();

    if (root == NULL || metrics_object == NULL || status_object == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(metrics_object);
        cJSON_Delete(status_object);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "device_id", MQTT_CLIENT_ID);
    cJSON_AddNumberToObject(root, "timestamp", (double)(esp_timer_get_time() / 1000000LL));

    add_number_or_null(metrics_object, "temperature", metrics->temperature, metrics->dht_valid);
    add_number_or_null(metrics_object, "humidity", metrics->humidity, metrics->dht_valid);
    add_number_or_null(metrics_object, "lux", metrics->lux, metrics->bh1750_valid);
    cJSON_AddNumberToObject(metrics_object, "uptime", metrics->uptime);
    cJSON_AddStringToObject(metrics_object, "state", state_to_string(metrics->state));
    cJSON_AddItemToObject(root, "metrics", metrics_object);

    cJSON_AddBoolToObject(status_object, "dht_ok", metrics->dht_valid);
    cJSON_AddBoolToObject(status_object, "bh1750_ok", metrics->bh1750_valid);
    cJSON_AddItemToObject(root, "status", status_object);

    bool printed = cJSON_PrintPreallocated(root, buffer, (int)buffer_size, false);
    cJSON_Delete(root);

    return printed ? ESP_OK : ESP_ERR_NO_MEM;
}
