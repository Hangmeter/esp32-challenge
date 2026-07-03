#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "driver/i2c.h"
#include "esp_err.h"
#include "esp_log.h"

#include "app_config.h"
#include "bh1750_sensor.h"
#include "fsm.h"
#include "sensor_types.h"

static const char *TAG = "BH1750";

#define BH1750_POWER_ON 0x01
#define BH1750_RESET 0x07
#define BH1750_CONTINUOUS_HIGH_RES_MODE 0x10

QueueHandle_t bh1750_queue;

static esp_err_t bh1750_write_command(uint8_t command)
{
    return i2c_master_write_to_device(
        BH1750_I2C_PORT,
        BH1750_ADDRESS,
        &command,
        sizeof(command),
        pdMS_TO_TICKS(100));
}

static esp_err_t bh1750_read_lux(float *lux)
{
    uint8_t raw[2] = {0};
    esp_err_t err = i2c_master_read_from_device(
        BH1750_I2C_PORT,
        BH1750_ADDRESS,
        raw,
        sizeof(raw),
        pdMS_TO_TICKS(100));

    if (err != ESP_OK) {
        return err;
    }

    uint16_t level = ((uint16_t)raw[0] << 8) | raw[1];
    *lux = (float)level / 1.2f;

    return ESP_OK;
}

static void bh1750_init_hw(void)
{
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BH1750_SDA_GPIO,
        .scl_io_num = BH1750_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = BH1750_I2C_FREQ_HZ,
        .clk_flags = 0,
    };

    ESP_ERROR_CHECK(i2c_param_config(BH1750_I2C_PORT, &i2c_conf));
    esp_err_t err = i2c_driver_install(BH1750_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    err = bh1750_write_command(BH1750_POWER_ON);
    if (err == ESP_OK) {
        err = bh1750_write_command(BH1750_RESET);
    }
    if (err == ESP_OK) {
        err = bh1750_write_command(BH1750_CONTINUOUS_HIGH_RES_MODE);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Sensor init failed: %s", esp_err_to_name(err));
    }
}

static bh1750_data_t read_bh1750(void)
{
    bh1750_data_t data = {
        .lux = -1.0f,
        .is_valid = false,
        .timestamp = xTaskGetTickCount(),
    };

    if (bh1750_read_lux(&data.lux) != ESP_OK) {
        ESP_LOGW(TAG, "Sensor read failed");
        return data;
    }

    data.is_valid = true;
    return data;
}

static void bh1750_task(void *arg)
{
    (void)arg;

    while (true) {
        if (fsm_are_sensors_enabled()) {
            bh1750_data_t data = read_bh1750();
            (void)xQueueOverwrite(bh1750_queue, &data);
            fsm_post_bh1750_data(&data);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void bh1750_sensor_start(void)
{
    bh1750_queue = xQueueCreate(1, sizeof(bh1750_data_t));
    configASSERT(bh1750_queue != NULL);

    bh1750_init_hw();

    BaseType_t task_created = xTaskCreate(bh1750_task, "bh1750_task", 4096, NULL, tskIDLE_PRIORITY + 1, NULL);
    configASSERT(task_created == pdPASS);
}
