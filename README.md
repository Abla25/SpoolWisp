# SpoolWisp

**Wireless USB print server for the Epson L1800 (and any USB printer) on an ESP32-S3.**
Spool + wisp: your print queue, set free over WiFi.

Turns a USB printer (built and tested for the Epson L1800) into a wireless network
printer using an ESP32-S3 dual-USB board, **without losing the Epson driver or any
of its settings**.

```
PC / Mac / Linux                     ESP32-S3                          Epson L1800
┌──────────────────────┐   WiFi     ┌──────────────┐   USB-C→USB-B   ┌──────────┐
│ Epson L1800 driver   │  ────────► │ raw TCP 9100 │ ──────────────► │ printer  │
│ (all settings, ICC,  │   or LPD   │ LPD 515      │  USB host       │          │
│  quality, paper ...) │            │ PSRAM queue  │  bulk OUT       │          │
└──────────────────────┘            └──────────────┘                 └──────────┘
```

## Why this keeps full Epson quality

The Epson driver stays installed **on your computer** and does all rendering
(color management, paper type, print quality, ICC profiles). It sends the finished
raster data over the network; the ESP32-S3 only forwards those bytes verbatim to the
printer over USB. Nothing is re-rendered or converted on the ESP, so output is
identical to a direct USB connection.

## Features

- **WiFi setup portal**: on first boot (or if the saved network is unreachable)
  the ESP opens a WPA2-protected AP (`SpoolWisp-XXXXXX`, password printed on the
  serial log) with a minimal web page at 192.168.4.1 to scan networks and enter
  the password — no reflashing needed. Credentials are stored in NVS.
  (`docs/PROVISIONING.md`)
- Raw TCP printing on port **9100** (Windows Standard TCP/IP port, Linux `socket://`)
- Minimal **LPD (RFC 1179)** server on port **515** (macOS/Linux LPD)
- **USB/IP server (TCP 3240)**: exports the printer as a remote USB device, so on
  Linux/Windows it can be attached as if locally plugged in — **ink levels,
  Status Monitor, nozzle check, head cleaning, all bidirectional functions work**.
  (`docs/BIDIRECTIONAL.md`)
- **Bonjour/mDNS auto-discovery**: macOS and Linux find the printer by
  themselves — no IP address to type. The advertised name is the real printer
  model read from its USB IEEE-1284 ID (e.g. `Epson L1800 Series`), and the
  host name is always `spoolwisp.local`, so DHCP changes do not break the
  queue.
- USB host driver for USB printer-class devices (VID filter: Epson `0x04B8`
  by default; set `APP_PRINTER_VID=0` for **any** USB printer)
- PSRAM ring-buffer queue with TCP flow control — jobs of any size stream through
- DHCP or static IP (configurable in menuconfig)

## Repository layout

```
firmware/          ESP-IDF v5.3 project (builds clean for esp32s3)
docs/ARCHITECTURE.md   how the data path works and why quality is preserved
docs/HARDWARE.md       wiring, 5V VBUS feed, cable notes
docs/PROVISIONING.md   WiFi setup portal (AP + web page, NVS storage)
docs/BIDIRECTIONAL.md  USB/IP: full bidirectional USB (ink levels, status, cleaning)
docs/SETUP_WINDOWS.md  add the printer in Windows keeping the Epson driver
docs/SETUP_MACOS.md    add the printer in macOS keeping the Epson driver
docs/SETUP_LINUX.md    CUPS setup on Linux keeping the Epson driver
docs/SECURITY.md       threat model and hardening notes
```

## Quick start

```bash
cd firmware
idf.py set-target esp32s3
idf.py menuconfig        # set WiFi SSID/password (Project config menu)
idf.py build flash monitor
```

Watch the log for `got IP: ...` and `printer ready: ...`, then configure your OS
(see the docs above) to print to the ESP's IP on port 9100.

## Known limitations

- Raw mode (9100/515) is one-way: use USB/IP mode for bidirectional status.
  On macOS (no USB/IP client) only printing works, not ink-level reporting.
- One raw print job is accepted at a time; one USB/IP client at a time.
- The L1800 is a USB Full-Speed device (~12 Mbit/s effective) — fine for this
  printer, which is the bottleneck anyway.

## License

MIT — see [LICENSE](LICENSE).
