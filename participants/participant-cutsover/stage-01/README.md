# Stage 01 - ESP32 Challenge

ESP-IDF 5.5.4 project for `participant-cutsover`.

## Hardware

- RGB LED: red GPIO 23, green GPIO 22, blue GPIO 21, active HIGH
- DHT22: GPIO 4
- BH1750: I2C SDA GPIO 18, SCL GPIO 19, address 0x23, 100 kHz

## Build

Set ESP-IDF 5.5.4 environment first, then run from this directory:

```powershell
idf.py set-target esp32
idf.py build
idf.py -p COMx flash monitor
```

Wi-Fi credentials can be passed at configure time:

```powershell
idf.py -DWIFI_SSID=YourNetworkSSID -DWIFI_PASS=YourNetworkPassword build
```

Default values are defined in `main/app_config.h`.
