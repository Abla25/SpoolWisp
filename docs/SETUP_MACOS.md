# macOS setup (keeps the full Epson driver)

1. Install the **Epson L1800 driver** (Epson Printer Driver / ESC/P-R or the
   model-specific package from Epson support) so the driver appears in the
   printer list.
2. **System Settings → Printers & Scanners → Add Printer**
3. Click the **IP** tab:
   - Address: ESP's IP (from the serial log)
   - Protocol: **Line Printer Daemon - LPD**
   - Queue: `lp` (any name works; the firmware accepts any queue)
4. In "Use:" choose **Select Software…** and pick **EPSON L1800** explicitly.
   Do not leave it on "Generic" — that would bypass the Epson driver.
5. Add the printer. All Epson driver settings (quality, paper, color management)
   render locally on the Mac; only the rendered job travels to the ESP.

Alternative: use protocol **IPP** with `ipp://ESP_IP/ipp/print` only if you run a
full IPP server — this firmware exposes raw TCP 9100 and LPD 515, so prefer LPD
(or add the printer via a local CUPS raw queue pointing at `socket://ESP_IP:9100`).
