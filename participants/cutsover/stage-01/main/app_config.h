#pragma once

#include "driver/gpio.h"

#define LED_RED_GPIO GPIO_NUM_23
#define LED_GREEN_GPIO GPIO_NUM_22
#define LED_BLUE_GPIO GPIO_NUM_21

#define DHT22_GPIO GPIO_NUM_4

#define BH1750_I2C_PORT I2C_NUM_0
#define BH1750_SDA_GPIO GPIO_NUM_18
#define BH1750_SCL_GPIO GPIO_NUM_19
#define BH1750_ADDRESS 0x23
#define BH1750_I2C_FREQ_HZ 100000

#ifndef WIFI_SSID
#define WIFI_SSID "YourNetworkSSID"
#endif

#ifndef WIFI_PASS
#define WIFI_PASS "YourNetworkPassword"
#endif

#define WIFI_MAX_RETRY 5

#define MQTT_BROKER_URI "mqtt://test.mosquitto.org"
#define MQTT_PORT 1883
#define MQTT_TOPIC "esp32/sensors/data"
#define MQTT_CLIENT_ID "esp32_challenge_001"
