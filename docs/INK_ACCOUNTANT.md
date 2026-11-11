# Ink Accountant

Read-only ink/maintenance monitoring for SpoolWisp. It never writes to the
printer: only EEPROM **read** service actions (0x41 / 0x51 bulk read) are used,
never 0x42 (write), resets, cleaning or adjustment commands.

## What is REAL

Read directly from the Epson L1800 EEPROM over USB (action 0x51):

| Data | Address | Notes |
|---|---|---|
| Waste ink pad counter 1 | `0x1C` (16-bit LE) | max 12476 |
| Waste ink pad counter 2 | `0x1E` (16-bit LE) | max 6788 |
| Total pages | `0xC8` (32-bit LE) | |
| Total passes | `0xCC` (32-bit LE) | |
| Cleaning counters | `0x93`, `0x94`, `0x95` | manual / power / timer |

Waste percent is computed against the model maximums, with a configurable
warning threshold (default 80%).

## What is ESTIMATED

The L1800 is an EcoTank: Epson states the tanks **cannot be measured
electronically** and the status packet carries only an "ink end" flag, not
levels. Therefore no per-colour percentages are shown. Cost is an estimate:

```
month_cost = month_pages * ml_per_page / 100 * average(price_per_100ml)
```

`ml_per_page` and the six per-colour EUR/100 ml prices are configurable.
Per-colour usage split is not measured and cannot be derived from the printer.

## HTTP API

Base: `http://spoolwisp.local/` (or the device IP).

| Endpoint | Method | Description |
|---|---|---|
| `/` | GET | Minimal dashboard (ochre accent, no frameworks) |
| `/api/status` | GET | `printer_ready`, `name`, `read_only` |
| `/api/ink` | GET | Refreshes hardware counters, returns waste/pages/passes/cleanings |
| `/api/accounting` | GET | Jobs, bytes, month bucket, estimated cost, config |
| `/api/config` | GET/POST | Read or update prices, `ml_per_page`, `waste_alert_percent` |

POST example:

```bash
curl -X POST http://spoolwisp.local/api/config \
  -d '{"price_black":12,"price_cyan":12,"price_magenta":12,"price_yellow":12,
       "price_light_cyan":12,"price_light_magenta":12,
       "ml_per_page":0.10,"waste_alert_percent":80}'
```

JSON is flat and numeric, suitable for Home Assistant REST sensors.

## Accounting storage

NVS namespace `ink`: configuration blob (`cfg`) and counters (`acct`:
jobs, bytes, page/pass totals, current month bucket). One record per raw or
LPD job; page deltas are computed from the printer's total page counter when
the counters are polled (every 5 minutes and on `/api/ink`).

## Caveats

- Job accounting is per connection/file as seen by SpoolWisp, not per OS page.
- Month bucket uses a persisted key (`boot` until a clock is available); it is
  not a wall-clock month on a device without SNTP.
- Reads briefly pause raw forwarding (`usb_printer_set_exclusive`) while the
  service command is in flight; the print queue keeps buffering.
