#include "mqtt_manager.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/semphr.h"
#include "mqtt_client.h"
#include "network_events.h"
#include "nvs.h"

#define CFG_NVS_NAMESPACE "net_cfg"
#define CFG_WIFI_SSID_KEY "wifi_ssid"
#define CFG_WIFI_PASS_KEY "wifi_pass"
#define CFG_MQTT_URI_KEY "mqtt_uri"
#define CFG_MQTT_TOPIC_KEY "mqtt_topic"
#define CFG_MQTT_USER_KEY "mqtt_user"
#define CFG_MQTT_PASS_KEY "mqtt_pass"

#define SETUP_AP_SSID "ESP32-Setup"
#define SETUP_AP_PASSWORD "configure123"
#define SETUP_AP_CHANNEL 1
#define SETUP_AP_MAX_CONN 4
#define WIFI_SCAN_MAX_APS 16
#define WIFI_FORM_MAX_LEN 384
#define CONNECT_MAX_RETRIES 8
#define RECOVERY_AP_DELAY_MS 2500

typedef struct {
    char wifi_ssid[33];
    char wifi_password[65];
    char mqtt_uri[129];
    char mqtt_topic[65];
    char mqtt_username[65];
    char mqtt_password[65];
    bool has_settings;
} saved_settings_t;

static const char *TAG = "mqtt_manager";

static QueueHandle_t s_network_event_queue;
static httpd_handle_t s_http_server;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static SemaphoreHandle_t s_client_mutex;
static SemaphoreHandle_t s_settings_mutex;
static SemaphoreHandle_t s_wifi_mutex;
static portMUX_TYPE s_state_spinlock = portMUX_INITIALIZER_UNLOCKED;
static esp_mqtt_client_handle_t s_mqtt_client;
static saved_settings_t s_settings;
static int s_retry_count;
static bool s_setup_ap_running;
static bool s_sta_connected;
static bool s_manual_setup_requested;
static bool s_mqtt_connected;
static bool s_recovery_ap_task_pending;

static void state_lock(void)
{
    taskENTER_CRITICAL(&s_state_spinlock);
}

static void state_unlock(void)
{
    taskEXIT_CRITICAL(&s_state_spinlock);
}

static void client_lock(void)
{
    if (s_client_mutex != NULL) {
        xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    }
}

static void client_unlock(void)
{
    if (s_client_mutex != NULL) {
        xSemaphoreGive(s_client_mutex);
    }
}

static void wifi_lock(void)
{
    if (s_wifi_mutex != NULL) {
        xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
    }
}

static void wifi_unlock(void)
{
    if (s_wifi_mutex != NULL) {
        xSemaphoreGive(s_wifi_mutex);
    }
}

static bool settings_lock(void)
{
    return s_settings_mutex != NULL &&
           xSemaphoreTake(s_settings_mutex, pdMS_TO_TICKS(100)) == pdTRUE;
}

static void settings_unlock(void)
{
    if (s_settings_mutex != NULL) {
        xSemaphoreGive(s_settings_mutex);
    }
}

static esp_err_t settings_get_copy(saved_settings_t *settings)
{
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!settings_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    *settings = s_settings;
    settings_unlock();
    return ESP_OK;
}

static esp_err_t settings_set_copy(const saved_settings_t *settings)
{
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!settings_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    s_settings = *settings;
    settings_unlock();
    return ESP_OK;
}

static esp_err_t settings_clear_cached(void)
{
    if (!settings_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    memset(&s_settings, 0, sizeof(s_settings));
    settings_unlock();
    return ESP_OK;
}

static bool settings_have_saved(void)
{
    bool has_settings = false;
    if (settings_lock()) {
        has_settings = s_settings.has_settings;
        settings_unlock();
    }
    return has_settings;
}

static esp_err_t settings_get_topic(char *topic, size_t topic_size)
{
    if (topic == NULL || topic_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!settings_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    strlcpy(topic, s_settings.mqtt_topic, topic_size);
    settings_unlock();
    return ESP_OK;
}

static void queue_network_event(network_event_type_t type, int32_t event_id)
{
    if (s_network_event_queue == NULL) {
        return;
    }

    network_event_t event = {
        .type = type,
        .event_id = event_id,
        .timestamp_us = esp_timer_get_time(),
    };

    xQueueOverwrite(s_network_event_queue, &event);
}

static esp_err_t nvs_get_str_optional(nvs_handle_t nvs, const char *key,
                                      char *value, size_t value_size)
{
    size_t len = value_size;
    esp_err_t err = nvs_get_str(nvs, key, value, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        value[0] = '\0';
        return ESP_OK;
    }
    return err;
}

static esp_err_t load_settings(saved_settings_t *settings)
{
    memset(settings, 0, sizeof(*settings));

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CFG_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_str_optional(nvs, CFG_WIFI_SSID_KEY, settings->wifi_ssid,
                               sizeof(settings->wifi_ssid));
    if (err == ESP_OK) {
        err = nvs_get_str_optional(nvs, CFG_WIFI_PASS_KEY, settings->wifi_password,
                                   sizeof(settings->wifi_password));
    }
    if (err == ESP_OK) {
        err = nvs_get_str_optional(nvs, CFG_MQTT_URI_KEY, settings->mqtt_uri,
                                   sizeof(settings->mqtt_uri));
    }
    if (err == ESP_OK) {
        err = nvs_get_str_optional(nvs, CFG_MQTT_TOPIC_KEY, settings->mqtt_topic,
                                   sizeof(settings->mqtt_topic));
    }
    if (err == ESP_OK) {
        err = nvs_get_str_optional(nvs, CFG_MQTT_USER_KEY, settings->mqtt_username,
                                   sizeof(settings->mqtt_username));
    }
    if (err == ESP_OK) {
        err = nvs_get_str_optional(nvs, CFG_MQTT_PASS_KEY, settings->mqtt_password,
                                   sizeof(settings->mqtt_password));
    }
    nvs_close(nvs);

    settings->has_settings = err == ESP_OK &&
                             settings->wifi_ssid[0] != '\0' &&
                             settings->mqtt_uri[0] != '\0';
    if (settings->mqtt_topic[0] == '\0') {
        strlcpy(settings->mqtt_topic, "esp32/sensors", sizeof(settings->mqtt_topic));
    }

    return err;
}

static esp_err_t save_settings(const saved_settings_t *settings)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CFG_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    if (err == ESP_OK) {
        err = nvs_set_str(nvs, CFG_WIFI_SSID_KEY, settings->wifi_ssid);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, CFG_WIFI_PASS_KEY, settings->wifi_password);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, CFG_MQTT_URI_KEY, settings->mqtt_uri);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, CFG_MQTT_TOPIC_KEY, settings->mqtt_topic);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, CFG_MQTT_USER_KEY, settings->mqtt_username);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, CFG_MQTT_PASS_KEY, settings->mqtt_password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return err;
}

static esp_err_t clear_settings(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CFG_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_all(nvs);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static int hex_to_int(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void url_decode(char *value)
{
    char *src = value;
    char *dst = value;

    while (*src != '\0') {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && isxdigit((unsigned char)src[1]) &&
                   isxdigit((unsigned char)src[2])) {
            *dst++ = (char)((hex_to_int(src[1]) << 4) | hex_to_int(src[2]));
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }

    *dst = '\0';
}

static bool form_get_value(const char *body, const char *key, char *out, size_t out_size)
{
    const size_t key_len = strlen(key);
    const char *cursor = body;

    while (cursor != NULL && *cursor != '\0') {
        if (strncmp(cursor, key, key_len) == 0 && cursor[key_len] == '=') {
            const char *value_start = cursor + key_len + 1;
            const char *value_end = strchr(value_start, '&');
            const size_t value_len = value_end == NULL ? strlen(value_start)
                                                       : (size_t)(value_end - value_start);
            const size_t copy_len = value_len < out_size - 1 ? value_len : out_size - 1;
            memcpy(out, value_start, copy_len);
            out[copy_len] = '\0';
            url_decode(out);
            return true;
        }

        cursor = strchr(cursor, '&');
        if (cursor != NULL) {
            cursor++;
        }
    }

    return false;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    (void)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        state_lock();
        s_mqtt_connected = true;
        state_unlock();
        ESP_LOGI(TAG, "MQTT connected");
        break;
    case MQTT_EVENT_DISCONNECTED:
        bool was_connected;

        state_lock();
        was_connected = s_mqtt_connected;
        s_mqtt_connected = false;
        state_unlock();
        if (was_connected) {
            ESP_LOGW(TAG, "MQTT disconnected");
        }
        break;
    default:
        break;
    }
}

static esp_err_t start_mqtt_client(void)
{
    saved_settings_t settings;

    ESP_RETURN_ON_ERROR(settings_get_copy(&settings), TAG,
                        "failed to read MQTT settings");

    state_lock();
    s_mqtt_connected = false;
    state_unlock();

    client_lock();
    if (s_mqtt_client != NULL) {
        esp_mqtt_client_stop(s_mqtt_client);
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
    }

    esp_mqtt_client_config_t config = {
        .broker.address.uri = settings.mqtt_uri,
        .credentials.username = settings.mqtt_username[0] != '\0'
            ? settings.mqtt_username
            : NULL,
        .credentials.authentication.password = settings.mqtt_password[0] != '\0'
            ? settings.mqtt_password
            : NULL,
    };

    s_mqtt_client = esp_mqtt_client_init(&config);
    if (s_mqtt_client == NULL) {
        client_unlock();
        return ESP_FAIL;
    }

    esp_err_t err = esp_mqtt_client_register_event(s_mqtt_client,
                                                   ESP_EVENT_ANY_ID,
                                                   mqtt_event_handler, NULL);
    if (err == ESP_OK) {
        err = esp_mqtt_client_start(s_mqtt_client);
    }
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
        client_unlock();
        return err;
    }
    client_unlock();

    ESP_LOGI(TAG, "MQTT client starting: %s, topic '%s'",
             settings.mqtt_uri, settings.mqtt_topic);
    return ESP_OK;
}

static esp_err_t start_sta_connect(void)
{
    saved_settings_t settings;
    ESP_RETURN_ON_ERROR(settings_get_copy(&settings), TAG,
                        "failed to read Wi-Fi settings");

    wifi_config_t sta_config = {0};
    strlcpy((char *)sta_config.sta.ssid, settings.wifi_ssid,
            sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password, settings.wifi_password,
            sizeof(sta_config.sta.password));
    sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    state_lock();
    s_manual_setup_requested = false;
    s_retry_count = 0;
    s_recovery_ap_task_pending = false;
    state_unlock();

    wifi_lock();
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    }
    if (err == ESP_OK) {
        err = esp_wifi_connect();
    }
    wifi_unlock();
    ESP_RETURN_ON_ERROR(err, TAG, "failed to start STA connection");

    queue_network_event(NETWORK_EVENT_CONNECTING, WIFI_EVENT_STA_START);
    return ESP_OK;
}

static esp_err_t connect_post_handler(httpd_req_t *req)
{
    char body[WIFI_FORM_MAX_LEN] = {0};
    saved_settings_t settings = {0};
    int received = 0;

    while (received < req->content_len && received < (int)sizeof(body) - 1) {
        const int ret = httpd_req_recv(req, body + received,
                                      sizeof(body) - 1 - received);
        if (ret <= 0) {
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';

    form_get_value(body, "wifi_ssid", settings.wifi_ssid, sizeof(settings.wifi_ssid));
    form_get_value(body, "wifi_password", settings.wifi_password,
                   sizeof(settings.wifi_password));
    form_get_value(body, "mqtt_uri", settings.mqtt_uri, sizeof(settings.mqtt_uri));
    form_get_value(body, "mqtt_topic", settings.mqtt_topic, sizeof(settings.mqtt_topic));
    form_get_value(body, "mqtt_username", settings.mqtt_username,
                   sizeof(settings.mqtt_username));
    form_get_value(body, "mqtt_password", settings.mqtt_password,
                   sizeof(settings.mqtt_password));

    if (settings.wifi_ssid[0] == '\0' || settings.mqtt_uri[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Wi-Fi SSID and MQTT URI are required");
        return ESP_OK;
    }
    if (settings.mqtt_topic[0] == '\0') {
        strlcpy(settings.mqtt_topic, "esp32/sensors", sizeof(settings.mqtt_topic));
    }

    ESP_LOGI(TAG, "saving settings for Wi-Fi SSID '%s'", settings.wifi_ssid);
    esp_err_t err = save_settings(&settings);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to save settings: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Failed to save settings");
        return ESP_OK;
    }

    settings.has_settings = true;
    ESP_RETURN_ON_ERROR(settings_set_copy(&settings), TAG,
                        "failed to cache saved settings");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
                       "<!doctype html><html><body><h1>Saved</h1>"
                       "<p>ESP32 is connecting to Wi-Fi and MQTT.</p></body></html>");

    start_sta_connect();
    return ESP_OK;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    wifi_ap_record_t *aps = calloc(WIFI_SCAN_MAX_APS, sizeof(wifi_ap_record_t));
    if (aps == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Not enough memory");
        return ESP_OK;
    }

    uint16_t ap_count = WIFI_SCAN_MAX_APS;
    wifi_scan_config_t scan_config = {
        .show_hidden = false,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err == ESP_OK) {
        err = esp_wifi_scan_get_ap_records(&ap_count, aps);
    }
    if (err != ESP_OK) {
        ap_count = 0;
        ESP_LOGW(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(err));
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req,
                             "<!doctype html><html><head><meta charset=\"utf-8\">"
                             "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                             "<title>ESP32 setup</title>"
                             "<style>body{font-family:sans-serif;max-width:560px;margin:24px auto;padding:0 16px}"
                             "label,input,select,button{display:block;width:100%;box-sizing:border-box;margin:10px 0}"
                             "input,select,button{font-size:16px;padding:10px}</style></head><body>"
                             "<h1>ESP32 setup</h1><form method=\"post\" action=\"/connect\">"
                             "<label>Wi-Fi network</label><select name=\"wifi_ssid\">");

    for (uint16_t i = 0; i < ap_count; ++i) {
        char option[128];
        snprintf(option, sizeof(option), "<option value=\"%s\">%s (%d dBm)</option>",
                 (const char *)aps[i].ssid, (const char *)aps[i].ssid, aps[i].rssi);
        httpd_resp_sendstr_chunk(req, option);
    }

    httpd_resp_sendstr_chunk(req,
                             "</select><label>Wi-Fi password</label>"
                             "<input name=\"wifi_password\" type=\"password\">"
                             "<label>MQTT URI</label>"
                             "<input name=\"mqtt_uri\" placeholder=\"mqtt://192.168.1.10:1883\">"
                             "<label>MQTT topic</label>"
                             "<input name=\"mqtt_topic\" value=\"esp32/sensors\">"
                             "<label>MQTT username</label>"
                             "<input name=\"mqtt_username\">"
                             "<label>MQTT password</label>"
                             "<input name=\"mqtt_password\" type=\"password\">"
                             "<button type=\"submit\">Save and connect</button>"
                             "</form></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    free(aps);
    return ESP_OK;
}

static esp_err_t start_http_server(void)
{
    if (s_http_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.stack_size = 8192;

    ESP_RETURN_ON_ERROR(httpd_start(&s_http_server, &config), TAG,
                        "failed to start HTTP server");

    const httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    const httpd_uri_t connect_uri = {
        .uri = "/connect",
        .method = HTTP_POST,
        .handler = connect_post_handler,
    };

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &root_uri), TAG,
                        "failed to register root handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &connect_uri), TAG,
                        "failed to register connect handler");
    return ESP_OK;
}

static esp_err_t start_setup_ap(void)
{
    wifi_config_t ap_config = {
        .ap = {
            .ssid = SETUP_AP_SSID,
            .ssid_len = strlen(SETUP_AP_SSID),
            .channel = SETUP_AP_CHANNEL,
            .password = SETUP_AP_PASSWORD,
            .max_connection = SETUP_AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
        },
    };

    wifi_lock();
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    }
    if (err == ESP_OK) {
        err = start_http_server();
    }
    wifi_unlock();
    ESP_RETURN_ON_ERROR(err, TAG, "failed to start setup AP");

    state_lock();
    s_setup_ap_running = true;
    s_recovery_ap_task_pending = false;
    state_unlock();
    queue_network_event(NETWORK_EVENT_CONNECTING, WIFI_EVENT_AP_START);
    ESP_LOGI(TAG, "setup AP started: SSID '%s', password '%s'",
             SETUP_AP_SSID, SETUP_AP_PASSWORD);
    return ESP_OK;
}

static bool recovery_ap_still_needed(void)
{
    bool should_start;

    state_lock();
    should_start = s_recovery_ap_task_pending &&
                   !s_sta_connected &&
                   !s_setup_ap_running;
    if (!should_start) {
        s_recovery_ap_task_pending = false;
    }
    state_unlock();

    return should_start;
}

static void delayed_setup_ap_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(RECOVERY_AP_DELAY_MS));
    if (!recovery_ap_still_needed()) {
        ESP_LOGI(TAG, "setup AP recovery skipped; STA recovered");
        vTaskDelete(NULL);
        return;
    }

    if (start_setup_ap() != ESP_OK) {
        ESP_LOGE(TAG, "failed to start setup AP after Wi-Fi failure");
        state_lock();
        s_recovery_ap_task_pending = false;
        state_unlock();
    }
    vTaskDelete(NULL);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        saved_settings_t settings;
        if (load_settings(&settings) == ESP_OK && settings.has_settings) {
            if (settings_set_copy(&settings) != ESP_OK) {
                ESP_LOGE(TAG, "failed to cache loaded settings");
                start_setup_ap();
                return;
            }
            start_sta_connect();
        } else {
            start_setup_ap();
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        int retry_count;
        const bool has_settings = settings_have_saved();
        bool should_retry = false;
        bool should_schedule_ap = false;

        state_lock();
        s_mqtt_connected = false;
        if (s_manual_setup_requested) {
            s_sta_connected = false;
            s_retry_count = 0;
            state_unlock();
            return;
        }

        s_sta_connected = false;
        if (s_retry_count < CONNECT_MAX_RETRIES && has_settings) {
            s_retry_count++;
            retry_count = s_retry_count;
            should_retry = true;
        } else if (!s_recovery_ap_task_pending && !s_setup_ap_running) {
            s_recovery_ap_task_pending = true;
            should_schedule_ap = true;
            retry_count = s_retry_count;
        } else {
            retry_count = s_retry_count;
        }
        state_unlock();

        queue_network_event(NETWORK_EVENT_LOST, event_id);

        if (should_retry) {
            ESP_LOGW(TAG, "Wi-Fi disconnected; retry %d/%d", retry_count,
                     CONNECT_MAX_RETRIES);
            wifi_lock();
            esp_wifi_connect();
            wifi_unlock();
        } else if (should_schedule_ap) {
            ESP_LOGW(TAG, "saved Wi-Fi is unavailable; starting setup AP");
            if (xTaskCreate(delayed_setup_ap_task, "setup_ap_delay", 3072,
                            NULL, 4, NULL) != pdPASS) {
                ESP_LOGE(TAG, "failed to create setup AP recovery task");
                state_lock();
                s_recovery_ap_task_pending = false;
                state_unlock();
            }
        }
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "connected, IP address: " IPSTR, IP2STR(&event->ip_info.ip));

        bool has_settings;
        bool was_setup_ap_running;

        has_settings = settings_have_saved();

        state_lock();
        s_sta_connected = true;
        s_retry_count = 0;
        s_recovery_ap_task_pending = false;
        was_setup_ap_running = s_setup_ap_running;
        if (s_setup_ap_running) {
            s_setup_ap_running = false;
        }
        state_unlock();

        if (was_setup_ap_running) {
            wifi_lock();
            esp_wifi_set_mode(WIFI_MODE_STA);
            wifi_unlock();
        }
        if (has_settings && start_mqtt_client() != ESP_OK) {
            ESP_LOGE(TAG, "failed to start MQTT client");
            return;
        }
    }
}

esp_err_t mqtt_manager_init(QueueHandle_t network_event_queue)
{
    if (network_event_queue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_network_event_queue = network_event_queue;
    if (s_client_mutex == NULL) {
        s_client_mutex = xSemaphoreCreateMutex();
        if (s_client_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_settings_mutex == NULL) {
        s_settings_mutex = xSemaphoreCreateMutex();
        if (s_settings_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_wifi_mutex == NULL) {
        s_wifi_mutex = xSemaphoreCreateMutex();
        if (s_wifi_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "failed to init Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   wifi_event_handler, NULL),
                        TAG, "failed to register Wi-Fi event handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   ip_event_handler, NULL),
                        TAG, "failed to register IP event handler");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG,
                        "failed to set Wi-Fi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG,
                        "failed to set STA mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "failed to start Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG,
                        "failed to disable Wi-Fi power save");
    return ESP_OK;
}

esp_err_t mqtt_manager_publish_sensor(const mqtt_sensor_payload_t *payload)
{
    if (payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char topic[sizeof(s_settings.mqtt_topic)];
    ESP_RETURN_ON_ERROR(settings_get_topic(topic, sizeof(topic)), TAG,
                        "failed to read MQTT topic");

    state_lock();
    const bool is_connected = s_mqtt_connected;
    state_unlock();

    if (!is_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    client_lock();
    if (s_mqtt_client == NULL) {
        client_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    char message[160];
    snprintf(message, sizeof(message),
             "{\"light_lux\":%d,\"temperature_c\":%d,\"humidity_percent\":%d}",
             payload->light_lux, payload->temperature_c, payload->humidity_percent);

    const int msg_id = esp_mqtt_client_enqueue(s_mqtt_client, topic,
                                               message, 0, 0, 0, true);
    client_unlock();
    if (msg_id < 0) {
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "published sensor data to '%s'", topic);
    return ESP_OK;
}

esp_err_t mqtt_manager_reset_settings(void)
{
    ESP_LOGW(TAG, "manual settings reset requested");

    esp_err_t err = clear_settings();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to clear settings: %s", esp_err_to_name(err));
        return err;
    }

    client_lock();
    if (s_mqtt_client != NULL) {
        esp_mqtt_client_stop(s_mqtt_client);
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
    }
    client_unlock();

    ESP_RETURN_ON_ERROR(settings_clear_cached(), TAG, "failed to clear settings cache");

    state_lock();
    s_manual_setup_requested = true;
    s_sta_connected = false;
    s_mqtt_connected = false;
    s_retry_count = 0;
    s_recovery_ap_task_pending = false;
    state_unlock();

    wifi_lock();
    esp_wifi_disconnect();
    wifi_unlock();

    return start_setup_ap();
}

bool mqtt_manager_is_setup_ap_running(void)
{
    state_lock();
    const bool is_running = s_setup_ap_running;
    state_unlock();
    return is_running;
}

bool mqtt_manager_is_mqtt_connected(void)
{
    state_lock();
    const bool is_connected = s_mqtt_connected;
    state_unlock();
    return is_connected;
}
