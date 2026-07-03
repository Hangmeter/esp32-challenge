#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "fsm.h"
#include "led.h"
#include "mqtt_publisher.h"
#include "wifi_manager.h"

static const char *TAG = "FSM";

typedef enum {
    FSM_EVENT_SYSTEM_READY,
    FSM_EVENT_DHT_DATA,
    FSM_EVENT_BH1750_DATA,
    FSM_EVENT_WIFI_CONNECTING,
    FSM_EVENT_WIFI_CONNECTED,
    FSM_EVENT_WIFI_RECOVERY,
    FSM_EVENT_WIFI_FAILED,
    FSM_EVENT_MQTT_ERROR,
    FSM_EVENT_ERROR_TIMEOUT,
    FSM_EVENT_RECOVERY_TIMEOUT,
    FSM_EVENT_PROCESSING_DONE,
} fsm_event_type_t;

typedef struct {
    fsm_event_type_t type;
    union {
        dht_data_t dht;
        bh1750_data_t bh1750;
    } payload;
} fsm_event_t;

static QueueHandle_t s_fsm_queue;
static system_state_t s_state = ST_INIT;
static dht_data_t s_latest_dht;
static bh1750_data_t s_latest_bh1750;
static bool s_has_dht;
static bool s_has_bh1750;
static portMUX_TYPE s_data_lock = portMUX_INITIALIZER_UNLOCKED;

const char *state_to_string(system_state_t state)
{
    switch (state) {
    case ST_INIT:
        return "ST_INIT";
    case ST_IDLE:
        return "ST_IDLE";
    case ST_READING:
        return "ST_READING";
    case ST_PROCESSING:
        return "ST_PROCESSING";
    case ST_ERROR:
        return "ST_ERROR";
    case ST_CONNECTING:
        return "ST_CONNECTING";
    case ST_CONNECTED:
        return "ST_CONNECTED";
    case ST_RECOVERY:
        return "ST_RECOVERY";
    default:
        return "ST_UNKNOWN";
    }
}

static void fsm_transition(system_state_t new_state)
{
    system_state_t old_state = s_state;

    if (old_state == new_state) {
        return;
    }

    s_state = new_state;
    ESP_LOGI(TAG, "State transition: %s -> %s", state_to_string(old_state), state_to_string(new_state));

    if (new_state == ST_ERROR) {
        led_set_error_mode(true);
        mqtt_publisher_stop();
    } else if (old_state == ST_ERROR) {
        led_set_error_mode(false);
    }

    if (new_state == ST_RECOVERY) {
        mqtt_publisher_stop();
    }

    if (new_state == ST_CONNECTED) {
        mqtt_publisher_start();
    }
}

static void post_event(const fsm_event_t *event)
{
    if (s_fsm_queue != NULL) {
        (void)xQueueSend(s_fsm_queue, event, 0);
    }
}

static void handle_invalid_sensor_data(void)
{
    ESP_LOGE(TAG, "Invalid sensor data received");
    fsm_transition(ST_ERROR);
}

static void handle_sensor_event(const fsm_event_t *event)
{
    if (s_state == ST_ERROR || s_state == ST_RECOVERY || s_state == ST_CONNECTING) {
        return;
    }

    if (event->type == FSM_EVENT_DHT_DATA) {
        portENTER_CRITICAL(&s_data_lock);
        s_latest_dht = event->payload.dht;
        s_has_dht = true;
        portEXIT_CRITICAL(&s_data_lock);

        if (!s_latest_dht.is_valid) {
            handle_invalid_sensor_data();
            return;
        }

        if (s_state == ST_IDLE || s_state == ST_CONNECTED) {
            fsm_transition(ST_READING);
        }
    }

    if (event->type == FSM_EVENT_BH1750_DATA) {
        portENTER_CRITICAL(&s_data_lock);
        s_latest_bh1750 = event->payload.bh1750;
        s_has_bh1750 = true;
        portEXIT_CRITICAL(&s_data_lock);

        if (!s_latest_bh1750.is_valid) {
            handle_invalid_sensor_data();
            return;
        }

        if (s_state == ST_READING && s_has_dht) {
            fsm_transition(ST_PROCESSING);
            ESP_LOGI(TAG, "Processed sensor frame: T=%.1fC H=%.1f%% Lux=%.1f",
                     s_latest_dht.temperature,
                     s_latest_dht.humidity,
                     s_latest_bh1750.lux);
            fsm_event_t done_event = {.type = FSM_EVENT_PROCESSING_DONE};
            post_event(&done_event);
        }
    }
}

static void handle_event(const fsm_event_t *event)
{
    switch (event->type) {
    case FSM_EVENT_SYSTEM_READY:
        fsm_transition(ST_CONNECTING);
        break;
    case FSM_EVENT_WIFI_CONNECTING:
        fsm_transition(ST_CONNECTING);
        break;
    case FSM_EVENT_WIFI_CONNECTED:
        fsm_transition(ST_CONNECTED);
        fsm_transition(ST_IDLE);
        break;
    case FSM_EVENT_WIFI_RECOVERY:
        fsm_transition(ST_RECOVERY);
        break;
    case FSM_EVENT_WIFI_FAILED:
    case FSM_EVENT_MQTT_ERROR:
        fsm_transition(ST_ERROR);
        break;
    case FSM_EVENT_DHT_DATA:
    case FSM_EVENT_BH1750_DATA:
        handle_sensor_event(event);
        break;
    case FSM_EVENT_PROCESSING_DONE:
        if (s_state == ST_PROCESSING) {
            fsm_transition(ST_IDLE);
        }
        break;
    case FSM_EVENT_ERROR_TIMEOUT:
        if (s_state == ST_ERROR) {
            portENTER_CRITICAL(&s_data_lock);
            s_has_dht = false;
            s_has_bh1750 = false;
            portEXIT_CRITICAL(&s_data_lock);
            fsm_transition(ST_INIT);
            if (wifi_manager_is_connected()) {
                fsm_transition(ST_CONNECTED);
                fsm_transition(ST_IDLE);
            } else {
                fsm_transition(ST_CONNECTING);
                wifi_manager_reconnect_now();
            }
        }
        break;
    case FSM_EVENT_RECOVERY_TIMEOUT:
        if (s_state == ST_RECOVERY) {
            if (wifi_manager_is_connected()) {
                fsm_transition(ST_CONNECTED);
                fsm_transition(ST_IDLE);
            } else {
                wifi_manager_reconnect_now();
            }
        }
        break;
    default:
        break;
    }
}

static void fsm_task(void *arg)
{
    (void)arg;

    fsm_event_t ready_event = {.type = FSM_EVENT_SYSTEM_READY};
    post_event(&ready_event);

    TickType_t error_entered_at = 0;
    TickType_t recovery_entered_at = 0;

    while (true) {
        fsm_event_t event;
        if (xQueueReceive(s_fsm_queue, &event, pdMS_TO_TICKS(250)) == pdPASS) {
            system_state_t before = s_state;
            handle_event(&event);
            if (before != ST_ERROR && s_state == ST_ERROR) {
                error_entered_at = xTaskGetTickCount();
            }
            if (before != ST_RECOVERY && s_state == ST_RECOVERY) {
                recovery_entered_at = xTaskGetTickCount();
            }
        }

        if (s_state == ST_ERROR && (xTaskGetTickCount() - error_entered_at) >= pdMS_TO_TICKS(5000)) {
            fsm_event_t timeout_event = {.type = FSM_EVENT_ERROR_TIMEOUT};
            handle_event(&timeout_event);
        }

        if (s_state == ST_RECOVERY && (xTaskGetTickCount() - recovery_entered_at) >= pdMS_TO_TICKS(5000)) {
            recovery_entered_at = xTaskGetTickCount();
            fsm_event_t timeout_event = {.type = FSM_EVENT_RECOVERY_TIMEOUT};
            handle_event(&timeout_event);
        }
    }
}

void fsm_start(void)
{
    s_fsm_queue = xQueueCreate(16, sizeof(fsm_event_t));
    configASSERT(s_fsm_queue != NULL);

    BaseType_t task_created = xTaskCreate(fsm_task, "fsm_task", 4096, NULL, tskIDLE_PRIORITY + 3, NULL);
    configASSERT(task_created == pdPASS);
}

system_state_t fsm_get_state(void)
{
    return s_state;
}

bool fsm_are_sensors_enabled(void)
{
    system_state_t state = s_state;
    return state == ST_IDLE || state == ST_READING || state == ST_PROCESSING || state == ST_CONNECTED;
}

bool fsm_get_latest_sensor_data(dht_data_t *dht_data, bh1750_data_t *bh1750_data)
{
    if (dht_data == NULL || bh1750_data == NULL) {
        return false;
    }

    portENTER_CRITICAL(&s_data_lock);
    bool has_data = s_has_dht && s_has_bh1750;
    if (has_data) {
        *dht_data = s_latest_dht;
        *bh1750_data = s_latest_bh1750;
    }
    portEXIT_CRITICAL(&s_data_lock);

    return has_data;
}

void fsm_post_dht_data(const dht_data_t *data)
{
    fsm_event_t event = {
        .type = FSM_EVENT_DHT_DATA,
        .payload.dht = *data,
    };
    post_event(&event);
}

void fsm_post_bh1750_data(const bh1750_data_t *data)
{
    fsm_event_t event = {
        .type = FSM_EVENT_BH1750_DATA,
        .payload.bh1750 = *data,
    };
    post_event(&event);
}

void fsm_post_wifi_connected(void)
{
    fsm_event_t event = {.type = FSM_EVENT_WIFI_CONNECTED};
    post_event(&event);
}

void fsm_post_wifi_connecting(void)
{
    fsm_event_t event = {.type = FSM_EVENT_WIFI_CONNECTING};
    post_event(&event);
}

void fsm_post_wifi_recovery(void)
{
    fsm_event_t event = {.type = FSM_EVENT_WIFI_RECOVERY};
    post_event(&event);
}

void fsm_post_wifi_failed(void)
{
    fsm_event_t event = {.type = FSM_EVENT_WIFI_FAILED};
    post_event(&event);
}

void fsm_post_mqtt_error(void)
{
    fsm_event_t event = {.type = FSM_EVENT_MQTT_ERROR};
    post_event(&event);
}
