# Firmware

ESP-IDF v5.3 project for ESP32-S3. Verified to build cleanly with
`ESP-IDF v5.3.3` (`idf.py build`, zero warnings).

## Build & flash

```bash
# one-time: install ESP-IDF v5.3.x, then
cd firmware
idf.py set-target esp32s3
idf.py menuconfig        # Project configuration → set WiFi SSID + password
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

`sdkconfig.defaults` already sets: target esp32s3, 8MB flash, octal PSRAM @ 80MHz.

### Alternative: PlatformIO

`platformio.ini` builds the exact same sources with the pioarduino platform
(ESP-IDF 5.5.4). Useful when you don't want a standalone ESP-IDF install:

```bash
cd firmware
pio run              # build
pio run -t upload    # flash (UART port of the board)
pio device monitor   # 115200 baud
```

`tools/serial_monitor.py` at the repository root is a minimal persistent
115200-baud logger (pyserial) that also pulses the auto-reset circuit.


## Configuration (menuconfig → "Project configuration")

| Option | Default | Meaning |
|---|---|---|
| `APP_WIFI_SSID` / `APP_WIFI_PASSWORD` | empty | WiFi credentials |
| `APP_STATIC_IP` | n | use static IP instead of DHCP |
| `APP_RAW_PORT` | 9100 | raw TCP (JetDirect) port |
| `APP_LPD_PORT` | 515 | LPD port |
| `APP_QUEUE_SIZE_KB` | 1024 | PSRAM queue size in KB |
| `APP_PRINTER_VID` | 0x04B8 | accepted USB vendor (Epson); 0 = any |
| `APP_PRINTER_PID` | 0x0000 | accepted USB product; 0 = any |

## Test without a printer attached

```bash
nc <ESP_IP> 9100 < somefile.bin
```

Bytes should appear in the serial log flow into the queue; with a printer
connected they are forwarded over USB.
