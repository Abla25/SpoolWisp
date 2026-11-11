# WiFi provisioning (no reflashing needed)

You don't need to hard-code WiFi credentials. On first boot (or whenever the
saved network can't be reached), the ESP32-S3 switches to AP mode:

1. Connect your phone/laptop to the AP named **`SpoolWisp-XXXXXX`** (suffix =
   last MAC bytes). It is WPA2-protected: the password is `cfg` + the same 6 hex
   chars as the AP suffix, printed once on the serial log (`idf.py monitor`).
2. Open **http://192.168.4.1** — a tiny DNS responder redirects any address
   there, so captive-portal popups usually open it automatically.
3. Pick your network from the scanned list (or type a hidden SSID), enter the
   password, press **connect**.
4. Credentials are stored in NVS and the device reboots into station mode.
   `POST /forget` on the portal erases them; `/status` shows printer state.

To change networks later, erase NVS (`idf.py erase_flash`) or flash with
`APP_WIFI_SSID` set in menuconfig (menuconfig values take priority).

If you prefer compile-time credentials, keep using menuconfig
(`APP_WIFI_SSID` / `APP_WIFI_PASSWORD`) and disable the portal with
`APP_ENABLE_PORTAL=n`.
