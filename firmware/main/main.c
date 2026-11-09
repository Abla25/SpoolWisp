#include "esp_log.h"
#include "nvs_flash.h"

#include "wifi_sta.h"
#include "job_queue.h"
#include "usb_printer.h"
#include "print_server.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "ESP32-S3 WiFi print server starting");

    job_queue_init();
    ESP_ERROR_CHECK(wifi_sta_start());
    usb_printer_start();
    print_server_start();

    ESP_LOGI(TAG, "ready: raw TCP %d, LPD %d", CONFIG_APP_RAW_PORT, CONFIG_APP_LPD_PORT);
}
