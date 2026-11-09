# Hardware setup

## Parts

- ESP32-S3 board with **two USB ports** (e.g. ESP32-S3-DevKitC-1 N8R8: one
  USB-UART port for flashing/logs, one native USB-OTG port for the printer)
- USB-C → USB-B cable (data cable, not charge-only) between the OTG port and the
  Epson L1800
- Good 5V USB power supply for the ESP board

## Wiring

1. Flash and power the board through the **USB-UART** port as usual.
2. Plug the USB-C end of the cable into the board's **OTG / native USB** port.
3. Plug the USB-B end into the **printer's USB-B port**.
4. Turn the printer on.

## Important: 5V VBUS on the OTG port

The printer is self-powered, but the USB host port must still present 5V on VBUS
for the printer's USB transceiver to enumerate. Many ESP32-S3 dev boards do **not**
feed 5V to the OTG port by default. On common boards:

- ESP32-S3-DevKitC-1: solder/close the **JP1 / 5V-EN** jumper (or feed 5V to the
  `5V` pin) so the OTG port gets VBUS. Check your board's schematic.
- If your board can't supply VBUS, use a **powered USB hub** between the ESP and
  the printer (hub provides VBUS; ESP still controls the data path).

## Cable quality

Use a short, shielded USB-C→USB-B data cable. The L1800 is USB Full-Speed, so
signal integrity over 1–2 m is normally fine.

## LEDs / logs

Open the serial monitor (`idf.py monitor`, 115200 baud) on the UART port. You
should see:

```
wifi: got IP: 192.168.x.x
usb_printer: printer ready: intf=0 ep_out=0x01 ep_in=0x82
print_server: raw print server listening on TCP 9100
print_server: LPD server listening on TCP 515
```
