# ESP32 MQTT Sensor Hub

ESP-IDF firmware for an ESP32 development board. The device reads a BH1750
light sensor and a DHT22 temperature/humidity sensor, publishes rounded sensor
values to MQTT, and shows system status with an RGB LED.

## Features

- ESP32 target, ESP-IDF v5.5.4
- BH1750 light sensor over I2C
- DHT22 sensor through the ESP32 RMT peripheral
- First-run setup access point with a small web form
- Wi-Fi and MQTT settings stored in NVS
- MQTT JSON publishing when rounded sensor values change
- RGB status LED
- BOOT button long press to reset saved Wi-Fi/MQTT settings

## Hardware

- ESP32 development board
- BH1750 / GY-302 I2C light sensor
- DHT22 temperature and humidity sensor
- RGB LED or three separate LEDs
- 220-330 Ohm resistors for LED channels
- 10k pull-up resistor for DHT22 DATA if the DHT22 is not a ready-made module

## Wiring

BH1750:

```text
BH1750 VCC  -> ESP32 3V3
BH1750 GND  -> ESP32 GND
BH1750 SDA  -> ESP32 GPIO21
BH1750 SCL  -> ESP32 GPIO22
BH1750 ADDR -> GND or not connected
```

DHT22:

```text
DHT22 VCC  -> ESP32 3V3
DHT22 GND  -> ESP32 GND
DHT22 DATA -> ESP32 GPIO27
```

RGB LED, common cathode:

```text
Red channel   -> 220-330 Ohm resistor -> ESP32 GPIO25
Green channel -> 220-330 Ohm resistor -> ESP32 GPIO26
Blue channel  -> 220-330 Ohm resistor -> ESP32 GPIO33
Common cathode -> ESP32 GND
```

The firmware is configured for active-high LED channels:

```c
#define LED_ACTIVE_LEVEL 1
```

For a common-anode RGB LED, connect the common pin to `3V3` and change
`LED_ACTIVE_LEVEL` in `main/rgb_led.c` to `0`.

The ESP32 BOOT button on `GPIO0` is used as a settings reset button. Hold it for
about 3 seconds to clear saved Wi-Fi and MQTT settings.

## Pin Map

| Function | ESP32 GPIO |
| --- | ---: |
| BH1750 SDA | 21 |
| BH1750 SCL | 22 |
| DHT22 DATA | 27 |
| RGB red | 25 |
| RGB green | 26 |
| RGB blue | 33 |
| Settings reset / BOOT | 0 |

## First Boot Setup

On first boot, or after settings reset, the ESP32 starts a setup access point:

```text
SSID: ESP32-Setup
Password: configure123
Setup page: http://192.168.4.1/
```

Open the setup page and fill in:

```text
Wi-Fi network:  your Wi-Fi SSID
Wi-Fi password: your Wi-Fi password
MQTT URI:       mqtt://<broker-ip>:1883
MQTT topic:     esp32/sensors
MQTT username:  esp32
MQTT password:  your MQTT password
```

For the local Mosquitto broker used during development on this PC:

```text
MQTT URI:      mqtt://172.16.101.38:1883
MQTT topic:    esp32/sensors
MQTT username: esp32
```

The MQTT password is not documented here because `mosquitto.passwd` stores only
a password hash. If the password is forgotten, create a new one with
`mosquitto_passwd`.

## MQTT Payload

Sensor values are rounded to integers before logging and publishing. MQTT
messages are published only when Wi-Fi/MQTT are connected, both sensors have
valid readings, and rounded values change.

Topic:

```text
esp32/sensors
```

Example payload:

```json
{"light_lux":791,"temperature_c":24,"humidity_percent":41}
```

Subscribe from PowerShell:

```powershell
& "C:\Program Files\Mosquitto\mosquitto_sub.exe" `
  -h 172.16.101.38 `
  -p 1883 `
  -u esp32 `
  -P "YOUR_MQTT_PASSWORD" `
  -t esp32/sensors `
  -v
```

## LED Status

| Color | Meaning |
| --- | --- |
| Green | Wi-Fi connected, MQTT connected, and sensors are valid |
| Blue | Setup access point is running |
| Red | Wi-Fi/MQTT unavailable or sensor data is invalid |

The blink profiles are defined in `main/rgb_led.c`.

## Local Mosquitto

The project contains an example Mosquitto config:

```text
mosquitto-local.example.conf
```

Copy it to a local config and adjust paths/IP addresses for your machine:

```powershell
Copy-Item mosquitto-local.example.conf mosquitto-local.conf
```

The runtime files below are local machine artifacts and are ignored by Git:

```text
mosquitto-local.conf
mosquitto.passwd
mosquitto.log
mosquitto.err.log
mosquitto.db
mosquitto-lan.out
mosquitto-lan.err
```

Typical broker setup:

```powershell
New-Item -ItemType Directory -Force C:\ProgramData\mosquitto

Copy-Item `
  "D:\IDE\esp32-challenge\participants\fufilius\stage-01\mosquitto-local.conf" `
  "C:\Program Files\Mosquitto\mosquitto.conf" `
  -Force

Start-Service mosquitto
netstat -ano | findstr :1883
```

Create or reset the `esp32` MQTT password:

```powershell
& "C:\Program Files\Mosquitto\mosquitto_passwd.exe" `
  -b "D:\IDE\esp32-challenge\participants\fufilius\stage-01\mosquitto.passwd" `
  esp32 YOUR_NEW_PASSWORD
```

## Build And Flash

Install ESP-IDF tools for the ESP32 target:

```powershell
cd C:\esp\v5.5.4\esp-idf
.\install.ps1 esp32
```

Build and flash:

```powershell
cd D:\IDE\esp32-challenge\participants\fufilius\stage-01
C:\esp\v5.5.4\esp-idf\export.ps1
idf.py set-target esp32
idf.py build
idf.py -p COMx flash monitor
```

The checked-in `sdkconfig` is configured for:

```text
Target: ESP32
Flash size: 4 MB
```

The VS Code ESP-IDF port is local-machine dependent. If needed, update `COM5`
in `.vscode/settings.json` or pass the port explicitly:

```powershell
idf.py -p COMx flash monitor
```

## Runtime Notes

- BH1750 is read once per second.
- DHT22 is sampled every 5 seconds after a startup delay.
- DHT22 uses the RMT peripheral for stable timing while Wi-Fi and MQTT are active.
- DHT22 and network queues keep the latest state/value instead of accumulating
  stale history.
- The serial monitor prints compact sensor summaries only when rounded values or
  sensor status change.
- Press and hold BOOT / `GPIO0` for about 3 seconds to clear saved Wi-Fi/MQTT
  settings and return to setup mode.

Example serial line:

```text
sensors: BH1750=791 lx, DHT22=24 C 41 %
```

## Git Notes

Build output, Mosquitto runtime files, local editor caches, and generated
backups are ignored by Git. Commit only source files, project configuration,
and documentation.
