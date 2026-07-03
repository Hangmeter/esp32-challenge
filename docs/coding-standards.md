## Coding Standards

### Naming Conventions
- `snake_case` for functions and variables: `m_mqtt_send_data()`
- `UPPER_CASE` for macros and constants: `M_SERVICE_UUID`
- `kebab-case` for filenames: `m-mqtt-component`
- Prefix component functions with component name: `m_wifi_`, `m_dht22_`

### File Organization
- Components go in `components/` directory
- Each component has: `include/`, `src/`, `CMakeLists.txt`, optional `Kconfig`
- Main application in `main/` directory
- Project configuration: Configuration via Kconfig

### ESP-IDF Specifics
- Use `ESP_LOGx` macros for logging (`ESP_LOGI`, `ESP_LOGE`)
- Use FreeRTOS primitives (`xTaskCreate`, `vTaskDelay`)
- No blocking delays in main loop - use state machines
- For high-precision timing (80 Hz), use `esp_timer` or timer interrupts

### Include instruction Specifics
In the ESP-IDF and FreeRTOS ecosystem, always follow this include order in any files (.h or .c) where tasks, queues, semaphores, or timers are used:
- Standard C system libraries (<stdio.h>, <stdbool.h>)
- #include "freertos/FreeRTOS.h" (always first among FreeRTOS headers)
- Other FreeRTOS header files ("freertos/task.h", "freertos/queue.h")
- ESP-IDF header files ("esp_log.h", "esp_err.h")
- Your own local header files

## Memory Management
- Prefer static allocation over dynamic
- Use `malloc` only when necessary, always check return
- Document memory ownership in comments