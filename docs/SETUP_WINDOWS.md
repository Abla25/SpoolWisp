# Windows setup (keeps the full Epson driver)

1. Install the **Epson L1800 driver** from Epson's support site if not already
   installed.
2. Get the ESP's IP from the serial log (`got IP: ...`).
3. **Settings → Bluetooth & devices → Printers & scanners → Add device → Add
   manually**:
   - "Add a printer using an IP address or hostname" → Next
   - Device type: **TCP/IP Device**, hostname/IP = ESP IP, port = **9100**
   - Uncheck "Query the printer and automatically select the driver"
   - If prompted for the port type, choose **Custom → Raw, port 9100**
4. When asked for the driver, select **EPSON → EPSON L1800 Series** (or "Have
   Disk" and point to the downloaded Epson driver).
5. Print a test page, then open **Printing preferences** — all Epson settings
   (paper, quality, color, borderless) are available and are applied on the PC
   before the job is sent.

The job is sent raw to `ESP_IP:9100` and forwarded byte-for-byte to the printer.
