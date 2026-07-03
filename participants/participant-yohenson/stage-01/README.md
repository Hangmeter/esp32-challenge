# ESP32-C3 Super Mini sensor hub

ESP-IDF firmware for an ESP32-C3 Super Mini board. The application reads
temperature and humidity from a DHT22 sensor and displays the current controller
state on an RGB LED.

The firmware is organized around FreeRTOS tasks and queues:

- DHT22 worker publishes temperature/humidity readings through a regular queue.
- RGB worker blinks one LED channel according to the controller state.
- The main controller task validates sensor data and drives the RGB state.
- Wi-Fi manager starts a first-run setup access point and serves a small web UI.
- The IP event handler converts ESP-IDF `IP_EVENT` notifications into a small
  internal network-event queue.

## Hardware

- ESP32-C3 Super Mini
- DHT22 temperature and humidity sensor
- RGB LED or three separate LEDs

DHT22 wiring:

```text
DHT22 VCC  -> 3V3
DHT22 GND  -> GND
DHT22 DATA -> GPIO10
```

If the DHT22 is not a ready-made module, add a pull-up resistor of about `10k`
between DATA and `3V3`.

RGB LED wiring used by the firmware:

```text
Red   -> GPIO0
Green -> GPIO2
Blue  -> GPIO1
```

If the LED is common-anode or otherwise active-low, update this macro in
`main/rgb_led.c`:

```c
#define LED_ACTIVE_LEVEL 0
```

## Pin Map

| Function | GPIO |
| --- | ---: |
| DHT22 DATA | 10 |
| RGB red | 0 |
| RGB green | 2 |
| RGB blue | 1 |

## Wi-Fi Setup

On first boot, or when no saved Wi-Fi credentials are available, the firmware
starts a setup access point:

```text
SSID:     ESP32-Setup
Password: configure123
```

Connect to this access point and open:

```text
http://192.168.4.1/
```

The setup page scans nearby Wi-Fi networks, lets you choose an SSID, and saves
the password in NVS. On the next boot the firmware reuses the saved credentials.

After the ESP32-C3 connects to your Wi-Fi network, the serial monitor prints the
assigned IP address. Open that address in a browser to see the latest DHT22
reading, or use:

```text
http://<device-ip>/api/sensors
```

## Behavior

The DHT22 is sampled every 3 seconds. The main controller waits for the latest
DHT22 reading, validates it, and converts the result into these RGB states:

| State | Meaning | LED indication |
| --- | --- | --- |
| `SYSTEM_STATE_OK` | Wi-Fi connected and DHT22 data is valid | green, 1 Hz |
| `SYSTEM_STATE_WARNING` | setup AP is running or Wi-Fi is connecting | blue, 2 Hz |
| `SYSTEM_STATE_CRITICAL` | DHT22 timeout/error or Wi-Fi unavailable | red, 4 Hz |

Valid DHT22 readings are printed to the serial monitor. If the DHT22 read fails
or no reading arrives before the controller timeout, the RGB indicator switches
to the critical state. Values outside the DHT22 operating range (`-40..80 C`,
`0..100 %`) are also rejected. The next valid reading returns the indicator to
`OK`.

The network module registers an asynchronous ESP-IDF `IP_EVENT` handler. It
maps IP events to:

- `NETWORK_EVENT_CONNECTING`
- `NETWORK_EVENT_CONNECTED`
- `NETWORK_EVENT_LOST`

This project does not yet configure Wi-Fi or Ethernet by itself. The handler is
ready for integration with a networking module that creates a station,
Ethernet, or other network interface and emits ESP-IDF IP events.

## Main controller

The main application logic runs in a dedicated FreeRTOS task. It switches
between these states:

- `ST_INIT`: sends the initial critical RGB state while the system starts.
- `ST_CONNECTING`: waits briefly for network/IP events before continuing the
  sensor loop.
- `ST_WAIT_SENSOR_DATA`: waits for the latest DHT22 reading and drains stale
  queued DHT22 readings.
- `ST_PROCESS_SENSOR_DATA`: validates DHT22 data and calculates the RGB state.
- `ST_UPDATE_OUTPUT`: sends the calculated RGB state to the RGB overwrite queue.
- `ST_RECOVERY`: enters network recovery mode after an IP loss event.
- `ST_ERROR`: switches the RGB indicator to the critical state when sensor data
  is invalid.

The DHT22 reading structure contains an `is_valid` flag. If the sensor reports
invalid data, the controller logs the error and enters `ST_ERROR`. On the next
loop the controller returns to waiting for fresh sensor data, so a temporary
sensor failure can recover automatically when valid readings appear again.

When the network event handler receives an IP loss event, the controller moves
to `ST_RECOVERY`, switches the RGB indicator to the critical state, waits for a
short recovery delay, and then returns to `ST_CONNECTING`.

## Build and flash

Requirements:

- ESP-IDF v5.5.4
- ESP32-C3 target
- 4 MB flash configuration

```powershell
idf.py set-target esp32c3
idf.py build
idf.py -p COMx flash monitor
```

The checked-in `sdkconfig` is already set for `esp32c3`, a 4 MB flash chip, and
the default single-app partition table.

## Development Environment

This project keeps VS Code ESP-IDF settings in Git. Install ESP-IDF v5.5.4 on
each Windows PC to the same path:

```text
C:\esp\v5.5.4\esp-idf
```

When opening the project in VS Code, the default terminal profile runs
`export.ps1` automatically. After opening a new terminal, `idf.py` should be
ready to use without manually activating a Python virtual environment.

The serial port can differ between PCs. If needed, update `COM4` in
`.vscode/settings.json` or pass a port explicitly:

```powershell
idf.py -p COMx flash monitor
```

## Git Workflow

The `main` branch is protected. Make changes in a feature branch, push it to
GitHub, and merge through a pull request:

```powershell
git switch -c feature/my-change
git add .
git commit -m "Describe change"
git push -u origin feature/my-change
```
