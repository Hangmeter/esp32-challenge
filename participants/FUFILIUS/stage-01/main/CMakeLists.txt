idf_component_register(SRCS "sensor_store.c" "wifi_manager.c" "main.c" "dht22.c" "rgb_led.c" "network_events.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES driver esp_timer esp_event esp_netif esp_wifi esp_http_server nvs_flash)
