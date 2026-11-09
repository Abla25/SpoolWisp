# Full bidirectional USB (ink levels, status monitor, cleaning)

Raw printing (TCP 9100 / LPD 515) is one-way by design. To use **all** Epson
functions — Status Monitor, ink levels, nozzle check, head cleaning, queue —
the printer must appear to your computer as a locally attached USB device.
This firmware exports it over **USB/IP** (protocol v1.1.1, TCP port 3240).

While a USB/IP client is attached, raw printing is paused automatically.

## Linux

```bash
sudo apt install usbip          # provides usbip + vhci-hcd
sudo modprobe vhci-hcd
sudo usbip list -r <ESP_IP>     # should show busid 1-1, Epson VID 04B8
sudo usbip attach -r <ESP_IP> -b 1-1
```

The printer now appears as a normal local USB device (`lsusb`). Install/use the
Epson L1800 driver exactly as if it were plugged in — everything bidirectional
works. Detach with `sudo usbip detach -p 1-1` (or port of the vhci device).

## Windows

Use the **usbip-win** client (https://github.com/vadimgrn/usbip-win or the
usbip project's Windows port), then attach to `<ESP_IP>` busid `1-1`.
The printer shows up in Device Manager as a local USB printer.

## macOS

There is no mature USB/IP client for macOS. Use raw printing instead
(port 9100 / LPD 515 — see the setup guides): all *print* settings work,
bidirectional status does not.

## Notes

- Only one USB/IP client at a time; `usbip list -r <ESP_IP>` shows the device.
- Isochronous transfers are not supported (printers don't need them).
- Max single transfer 16 KB, which covers printer-class control/bulk traffic.
