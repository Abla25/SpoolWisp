# WiFi provisioning (no reflashing needed)

You don't need to hard-code WiFi credentials. On first boot (or whenever the
saved network can't be reached), the ESP32-S3 switches to AP mode:

1. Connect your phone/laptop to the AP named **`ESP32-PrintServer-XXXXXX`**
   (open network, suffix = last MAC bytes).
2. Open **http://192.168.4.1** — a tiny DNS responder redirects any address
   there, so captive-portal popups usually open it automatically.
3. Pick your network from the scanned list (or type a hidden SSID), enter the
   password, press **Connect**.
4. Credentials are stored in NVS and the device reboots into station mode.

To change networks later, erase NVS (`idf.py erase_flash`) or flash with
`APP_WIFI_SSID` set in menuconfig (menuconfig values take priority).

If you prefer compile-time credentials, keep using menuconfig
(`APP_WIFI_SSID` / `APP_WIFI_PASSWORD`) and disable the portal with
`APP_ENABLE_PORTAL=n`.
