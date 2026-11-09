#include "wifi_sta.h"
#include "wifi_portal.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static const char *TAG = "wifi";

static EventGroupHandle_t s_event_group;
static int s_retry_count;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define CONNECT_TIMEOUT_S  20

static void apply_static_ip(esp_netif_t *netif)
{
#ifdef CONFIG_APP_STATIC_IP
    esp_netif_dhcpc_stop(netif);
    esp_netif_ip_info_t ip_info = {0};
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(CONFIG_APP_STATIC_IP_ADDR, &ip_info.ip));
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(CONFIG_APP_STATIC_GW, &ip_info.gw));
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(CONFIG_APP_STATIC_NETMASK, &ip_info.netmask));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(netif, &ip_info));
#else
    (void)netif;
#endif
}

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < 3) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGW(TAG, "retrying WiFi connection (%d)", s_retry_count);
        } else {
            xEventGroupSetBits(s_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_connect(const char *ssid, const char *password)
{
    s_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();

    static bool wifi_initialized;
    if (!wifi_initialized) {
        wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
        wifi_initialized = true;
    }

    esp_event_handler_instance_t any_id;
    esp_event_handler_instance_t got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &got_ip));

    apply_static_ip(netif);

    wifi_config_t wifi_cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strlcpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "connecting to SSID \"%s\"", ssid);
    EventBits_t bits = xEventGroupWaitBits(s_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(CONNECT_TIMEOUT_S * 1000));
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected");
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t wifi_sta_start(void)
{
    char ssid[33] = {0};
    char pass[64] = {0};

    if (strlen(CONFIG_APP_WIFI_SSID) > 0) {
        strlcpy(ssid, CONFIG_APP_WIFI_SSID, sizeof(ssid));
        strlcpy(pass, CONFIG_APP_WIFI_PASSWORD, sizeof(pass));
    } else if (!wifi_portal_load_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
#ifdef CONFIG_APP_ENABLE_PORTAL
        ESP_LOGW(TAG, "no WiFi credentials stored, starting setup portal");
        wifi_portal_run();
#else
        ESP_LOGE(TAG, "no WiFi credentials configured");
        return ESP_ERR_INVALID_STATE;
#endif
    }

    if (wifi_connect(ssid, pass) != ESP_OK) {
#ifdef CONFIG_APP_ENABLE_PORTAL
        ESP_LOGW(TAG, "connection failed, starting setup portal");
        wifi_portal_run();
#else
        return ESP_FAIL;
#endif
    }
    return ESP_OK;
}
