idf_component_register(
    SRCS
        "main.c"
        "led.c"
        "dht_sensor.c"
        "bh1750_sensor.c"
        "monitor.c"
        "fsm.c"
        "wifi_manager.c"
        "mqtt_publisher.c"
        "json_builder.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_event
        esp_netif
        esp_timer
        esp_wifi
        json
        mqtt
        nvs_flash
)

if(DEFINED WIFI_SSID)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WIFI_SSID=\"${WIFI_SSID}\")
endif()

if(DEFINED WIFI_PASS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WIFI_PASS=\"${WIFI_PASS}\")
endif()
