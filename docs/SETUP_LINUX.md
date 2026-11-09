# Linux (CUPS) setup (keeps the full Epson driver)

1. Install the Epson driver package for the L1800 (e.g. `epson-inkjet-printer-escpr`
   or Epson's proprietary `.ppd`), so CUPS lists "EPSON L1800" as a driver.
2. Add the printer as a raw network printer pointing at the ESP:

   ```bash
   sudo lpadmin -p L1800 -E -v socket://<ESP_IP>:9100 -P /path/to/epson-l1800.ppd
   ```

   or via `lpadmin -p L1800 -E -v socket://<ESP_IP>:9100 -m <epson-l1800-model>`
   if the driver is registered in the CUPS model list.
3. Print a test page:

   ```bash
   lp -d L1800 /usr/share/cups/data/testprint
   ```

CUPS sends the driver-rendered job raw to `ESP_IP:9100`; the ESP forwards it
byte-for-byte to the printer. All Epson driver options in the print dialog
(quality, media, color mode) are applied by the driver on the PC.

LPD on port 515 also works (`-v lpd://<ESP_IP>/lp`) if you prefer.
