idf_component_register(SRCS
    "main.c"
    "wifi_sta.c"
    "job_queue.c"
    "usb_printer.c"
    "print_server.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event nvs_flash usb lwip
)
