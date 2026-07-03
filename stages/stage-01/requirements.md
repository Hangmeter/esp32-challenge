ШАГ 1: «Железный старт» (GPIO и Таймеры)
Цель
Реализовать управление RGB светодиодом с динамической частотой мигания в зависимости от "состояния системы".

Аппаратные требования
RGB светодиод подключен к GPIO:

Красный: GPIO_NUM_23
Зеленый: GPIO_NUM_22
Синий: GPIO_NUM_21
Активный уровень: HIGH (1 - включен, 0 - выключен)

Функциональные требования
1. Создать задачу led_control_task:

Приоритет: tskIDLE_PRIORITY + 2
Стек: 2048 байт
Имя: "led_task"

2. Логика мигания:

Реализовать три режима мигания RGB:
Режим 1 (1 Гц): Красный - 500ms вкл, 500ms выкл
Режим 2 (2 Гц): Зеленый - 250ms вкл, 250ms выкл
Режим 3 (4 Гц): Синий - 125ms вкл, 125ms выкл

Переключение режимов происходит через глобальную переменную-флаг (имитация состояния системы):

c
typedef enum {
    LED_MODE_SLOW = 1,    // 1 Гц - Красный
    LED_MODE_MEDIUM = 2,  // 2 Гц - Зеленый
    LED_MODE_FAST = 4     // 4 Гц - Синий
} led_mode_t;

volatile led_mode_t g_system_mode = LED_MODE_SLOW;
3. Таймер для смены режимов:

Использовать аппаратный таймер (ESP_TIMER) или софт-таймер FreeRTOS
Каждые 10 секунд автоматически менять режим: SLOW → MEDIUM → FAST → SLOW → ...

При смене режима выводить лог: ESP_LOGI("LED", "Switching to mode: %d Hz", frequency)


ШАГ 2: «Сбор данных» (Датчики и Очереди)
Цель
Реализовать сбор данных с двух датчиков и отправку в очереди.

Аппаратные требования
DHT22 (температура/влажность):
GPIO: GPIO_NUM_4
Протокол: 1-Wire (использовать компонент dht из ESP-IDF)
BH1750 (освещенность):
I2C: SDA = GPIO_NUM_18, SCL = GPIO_NUM_19
Адрес: 0x23 (стандартный)
Частота I2C: 100 kHz

Структуры данных
c
// Данные с DHT22
typedef struct {
    float temperature;  // в градусах Цельсия
    float humidity;     // в процентах
    bool is_valid;      // true если данные корректны
    TickType_t timestamp;
} dht_data_t;

// Данные с BH1750
typedef struct {
    float lux;          // освещенность в люксах
    bool is_valid;      // true если данные корректны
    TickType_t timestamp;
} bh1750_data_t;
Функциональные требования
1. Создать воркер для DHT22:

Задача: dht_task

Приоритет: tskIDLE_PRIORITY + 1

Стек: 4096 байт

Период опроса: 2000 мс

Отправка данных в обычную очередь (xQueueCreate):

c
QueueHandle_t dht_queue;
dht_queue = xQueueCreate(10, sizeof(dht_data_t));
2. Создать воркер для BH1750:

Задача: bh1750_task

Приоритет: tskIDLE_PRIORITY + 1

Стек: 4096 байт

Период опроса: 1000 мс

Отправка данных в очередь с перезаписью (xQueueOverwrite):

c
QueueHandle_t bh1750_queue;
bh1750_queue = xQueueCreate(1, sizeof(bh1750_data_t));
3. Обработка ошибок датчиков:

Если DHT22 не отвечает: is_valid = false, установить температуру = -273.15, влажность = 0

Если BH1750 не отвечает: is_valid = false, установить lux = -1

При ошибке выводить ESP_LOGW("DHT", "Sensor read failed")

4. Проверка работы:

Создать третью задачу-монитор (monitor_task), которая читает обе очереди и выводит данные в лог каждые 5 секунд:

text
I (1234) MONITOR: DHT: T=25.5°C, H=60.2%, valid=1
I (1234) MONITOR: BH1750: Lux=342.5, valid=1
Критерии проверки
DHT22 отправляет данные каждые 2 секунды

BH1750 отправляет данные каждую секунду

Правильное использование очередей (xQueueSend для DHT, xQueueOverwrite для BH1750)

Обработка невалидных данных

Модули в отдельных файлах: dht_sensor.c, bh1750_sensor.c, monitor.c

ШАГ 3: «Железная логика» (Архитектура FSM)
Цель
Реализовать конечный автомат для управления системой на основе данных датчиков.

Состояния FSM
c
typedef enum {
    ST_INIT = 0,        // Начальная инициализация
    ST_IDLE = 1,        // Ожидание данных
    ST_READING = 2,     // Активный сбор данных
    ST_PROCESSING = 3,  // Обработка данных
    ST_ERROR = 4        // Критическая ошибка
} system_state_t;
Функциональные требования
1. Создать задачу FSM:

Задача: fsm_task

Приоритет: tskIDLE_PRIORITY + 3 (выше датчиков)

Стек: 4096 байт

Имя: "fsm_task"

2. Логика переходов:

Текущее состояние	Событие	Новое состояние	Действие
ST_INIT	Система инициализирована	ST_IDLE	ESP_LOGI
ST_IDLE	Получены данные от DHT22	ST_READING	Сохранить данные
ST_READING	Получены данные от BH1750	ST_PROCESSING	Объединить данные
ST_PROCESSING	Данные обработаны	ST_IDLE	ESP_LOGI
ЛЮБОЕ	is_valid == false (у любого датчика)	ST_ERROR	ESP_LOGE
ST_ERROR	Прошло 5 секунд	ST_INIT	Сброс системы
3. Обработка невалидных данных:

Если приходит is_valid == false от DHT22 → переход в ST_ERROR

Если приходит is_valid == false от BH1750 → переход в ST_ERROR

В состоянии ST_ERROR все датчики отключаются (не опрашиваются)

Через 5 секунд в ST_ERROR автоматический переход в ST_INIT (перезапуск)

4. Визуализация:

Использовать ESP_LOGI("FSM", "State transition: %s -> %s", old_state, new_state)

Определить функцию const char* state_to_string(system_state_t state) для вывода имен состояний

5. Интеграция с LED (из Шага 1):

В состоянии ST_ERROR: LED мигает красным с частотой 1 Гц (аварийный режим)

В остальных состояниях: LED работает по расписанию из Шага 1

Критерии проверки
Реализован полный конечный автомат с 5 состояниями

Корректная обработка невалидных данных (переход в ST_ERROR)

Автоматическое восстановление из ST_ERROR

Состояния логируются в консоль

FSM реализован в отдельном модуле: fsm.c, fsm.h

ШАГ 4: «Выход в сеть» (Wi-Fi и LwIP)
Цель
Реализовать подключение к Wi-Fi с обработкой событий.

Параметры сети
c
#define WIFI_SSID      "YourNetworkSSID"
#define WIFI_PASS      "YourNetworkPassword"
#define WIFI_MAX_RETRY 5
Функциональные требования
1. Расширить FSM новыми состояниями:

c
typedef enum {
    // ... предыдущие состояния
    ST_CONNECTING = 5,   // Попытка подключения к Wi-Fi
    ST_CONNECTED = 6,    // Подключено к Wi-Fi, получен IP
    ST_RECOVERY = 7      // Режим восстановления (потеря сети)
} system_state_t;
2. Wi-Fi задача:

Создать задачу wifi_task (приоритет: +4, стек: 4096)

Использовать ESP-IDF Wi-Fi API (режим STA)

Подключение к сети с SSID и паролем из дефайнов

3. Асинхронный обработчик событий LwIP:

c
esp_event_handler_instance_t instance_any_id;
esp_event_handler_instance_t instance_got_ip;

// Регистрация обработчиков
ESP_ERROR_CHECK(esp_event_handler_instance_register(
    WIFI_EVENT,
    ESP_EVENT_ANY_ID,
    &wifi_event_handler,
    NULL,
    &instance_any_id
));

ESP_ERROR_CHECK(esp_event_handler_instance_register(
    IP_EVENT,
    IP_EVENT_STA_GOT_IP,
    &got_ip_event_handler,
    NULL,
    &instance_got_ip
));
4. Обработчики событий:

wifi_event_handler:

WIFI_EVENT_STA_START → попытка подключения

WIFI_EVENT_STA_CONNECTED → переход в ST_CONNECTING

WIFI_EVENT_STA_DISCONNECTED → переход в ST_RECOVERY, попытка переподключения

got_ip_event_handler:

Получен IP → переход в ST_CONNECTED

Логировать IP: ESP_LOGI("WIFI", "Got IP: " IPSTR, IP2STR(&event->ip_info.ip))

5. Логика восстановления:

При потере сети (WIFI_EVENT_STA_DISCONNECTED):

Переход в ST_RECOVERY

Попытка переподключения каждые 5 секунд (максимум 5 попыток)

После 5 неудачных попыток → переход в ST_ERROR

При успешном переподключении → переход в ST_CONNECTED

6. Интеграция с FSM:

Из ST_INIT → ST_CONNECTING (автоматически)

Из ST_CONNECTING → ST_CONNECTED (при получении IP)

Из ST_CONNECTED → ST_IDLE (переход к работе датчиков)

Из любого состояния → ST_RECOVERY (при потере сети)

Критерии проверки
ESP32 подключается к Wi-Fi

Обработчик событий LwIP работает асинхронно

FSM переходит в ST_CONNECTING и ST_CONNECTED

Реализован режим восстановления при потере сети

Код в отдельных файлах: wifi_manager.c, wifi_manager.h

ШАГ 5: «Финальный аккорд» (MQTT и JSON)
Цель
Отправка данных датчиков на MQTT брокер в формате JSON.

Параметры MQTT
c
#define MQTT_BROKER_URI   "mqtt://test.mosquitto.org"
#define MQTT_PORT         1883
#define MQTT_TOPIC        "esp32/sensors/data"
#define MQTT_CLIENT_ID    "esp32_challenge_001"
Функциональные требования
1. MQTT клиент:

Использовать компонент esp_mqtt из ESP-IDF

Инициализация ТОЛЬКО после получения IP (ST_CONNECTED)

Параметры подключения:

c
esp_mqtt_client_config_t mqtt_cfg = {
    .broker.address.uri = MQTT_BROKER_URI,
    .broker.address.port = MQTT_PORT,
    .credentials.client_id = MQTT_CLIENT_ID,
};
2. Задача MQTT:

Создать задачу mqtt_task (приоритет: +2, стек: 4096)

Запускается только при переходе в ST_CONNECTED

При переходе в ST_ERROR или ST_RECOVERY → отключение MQTT

3. Формирование JSON пакета:

c
// Структура для отправки
typedef struct {
    float temperature;    // из DHT22
    float humidity;       // из DHT22
    float lux;           // из BH1750
    bool dht_valid;      // валидность DHT
    bool bh1750_valid;   // валидность BH1750
    uint32_t uptime;     // время работы в секундах
    system_state_t state; // текущее состояние FSM
} sensor_metrics_t;
JSON формат:

json
{
    "device_id": "esp32_challenge_001",
    "timestamp": 1234567890,
    "metrics": {
        "temperature": 25.5,
        "humidity": 60.2,
        "lux": 342.5,
        "uptime": 3600,
        "state": "ST_CONNECTED"
    },
    "status": {
        "dht_ok": true,
        "bh1750_ok": true
    }
}
4. Отправка данных:

Период отправки: 10 секунд

Если датчик невалидный → отправить null в JSON:

json
{
    "metrics": {
        "temperature": null,
        "humidity": null,
        "lux": 342.5
    }
}
5. Обработка ошибок:

MQTT_CONNECT_ERROR → переход в ST_RECOVERY

MQTT_DISCONNECTED → попытка переподключения (3 попытки)

После 3 неудач → переход в ST_ERROR

6. Обработчики событий MQTT:

c
esp_mqtt_client_register_event(client, MQTT_EVENT_CONNECTED, mqtt_event_handler, NULL);
esp_mqtt_client_register_event(client, MQTT_EVENT_DISCONNECTED, mqtt_event_handler, NULL);
esp_mqtt_client_register_event(client, MQTT_EVENT_ERROR, mqtt_event_handler, NULL);
Критерии проверки
MQTT запускается строго после получения IP

JSON формируется корректно (валидный JSON)

При невалидных датчиках в JSON отправляется null

Обработка ошибок подключения к брокеру

Код в отдельных файлах: mqtt_publisher.c, mqtt_publisher.h, json_builder.c

