# macOS setup (keeps the full Epson driver)

The firmware advertises itself over **Bonjour/mDNS**, so macOS finds the
printer by itself — no IP address to type. The advertised name is taken from
the USB printer's own IEEE-1284 ID (`MFG:`/`MDL:`) as soon as it is attached,
e.g. **"Epson L1800 Series"**; the host name is always `spoolwisp.local`, so a
new DHCP lease does not break the queue.

1. Install the **Epson L1800 driver** (Epson Printer Driver / ESC/P-R or the
   model-specific package from Epson support) so the driver appears in the
   printer list.
2. Connect the printer to the ESP32's **USB-OTG port** and power it on; the
   serial log shows `printer ready` and the Bonjour name switches from
   `SpoolWisp` to the real printer model.
3. **System Settings → Printers & Scanners → Add Printer**
4. In the **Default** (Bonjour) list the printer appears automatically, with
   the printer's own model name.
5. In "Use:" choose **Select Software…** and pick **EPSON L1800** explicitly.
   Do not leave it on "Generic" — that would bypass the Epson driver.
6. Add the printer. All Epson driver settings (quality, paper, color management)
   render locally on the Mac; only the rendered job travels to the ESP.

Fallback if the Bonjour entry does not appear: use the **IP** tab with address
`spoolwisp.local` (or the ESP's IP), protocol **HP Jetdirect – Socket** port
**9100**, same Epson driver — or protocol **LPD** with queue `lp`.
