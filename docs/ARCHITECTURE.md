# Architecture

## Data path

1. Your computer's **Epson L1800 driver** renders the page exactly as if the
   printer were plugged in by USB (media type, quality, color mode, borderless,
   ICC profile — everything in the Epson driver dialog).
2. The OS sends the rendered bytes to the ESP32-S3 over WiFi:
   - raw TCP on port **9100** (recommended, used by Windows/macOS/Linux), or
   - LPD on port **515**.
3. The firmware stores the bytes in a **PSRAM ring buffer**. If the buffer fills,
   TCP flow control pauses the sender — no data is ever dropped.
4. A USB host task on the ESP32-S3's native OTG port forwards the bytes to the
   printer's bulk-OUT endpoint, chunk by chunk.

The ESP never interprets the data. It is a transparent pipe: WiFi in, USB out.
That is what guarantees the output is byte-identical to a direct USB connection.

## Firmware modules (`firmware/main/`)

| File | Role |
|---|---|
| `main.c` | boot: NVS, WiFi, queue, USB host, servers |
| `wifi_sta.c` | WiFi station, DHCP or static IP |
| `print_server.c` | TCP 9100 raw server + minimal LPD server on 515 |
| `job_queue.c` | PSRAM ring buffer with flow control |
| `usb_printer.c` | USB host: enumerate printer-class device, claim interface, GET_DEVICE_ID, bulk OUT writes |
| `mdns_adv.c` | Bonjour/mDNS advertisement: raw 9100 (`_pdl-datastream`), LPD 515 (`_printer`), host `spoolwisp.local`; name/TXT updated from the printer's USB device ID |

## USB details

- Interface class `0x07` (printer), subclass `0x01`, bulk OUT endpoint for data,
  optional bulk IN for status.
- `GET_DEVICE_ID` (class request `0x00`) is used at connect time to log the
  printer's IEEE-1284 ID string.
- All USB transfer buffers are allocated with `MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL`
  because the S3's USB peripheral can only DMA from internal SRAM; the queue
  itself lives in PSRAM.
- The vendor ID is filtered to Epson (`0x04B8`) by default; set
  `APP_PRINTER_VID=0` in menuconfig to accept any printer-class device.
