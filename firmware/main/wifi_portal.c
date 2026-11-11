#include "wifi_portal.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "usb_printer.h"

static const char *TAG = "portal";

#define PORTAL_IP       "192.168.4.1"
#define NVS_NS          "wifi"
#define MAX_APS         16

bool wifi_portal_load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t len1 = ssid_len;
    size_t len2 = pass_len;
    esp_err_t e1 = nvs_get_str(nvs, "ssid", ssid, &len1);
    esp_err_t e2 = nvs_get_str(nvs, "pass", pass, &len2);
    nvs_close(nvs);
    return e1 == ESP_OK && e2 == ESP_OK && ssid[0] != '\0';
}

static void portal_save_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, "ssid", ssid));
    ESP_ERROR_CHECK(nvs_set_str(nvs, "pass", pass));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);
}

static int url_decode(char *dst, size_t dst_cap, const char *src, size_t src_len)
{
    size_t o = 0;
    for (size_t i = 0; i < src_len && o + 1 < dst_cap; i++) {
        if (src[i] == '%' && i + 2 < src_len) {
            char hex[3] = { src[i + 1], src[i + 2], 0 };
            dst[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (src[i] == '+') {
            dst[o++] = ' ';
        } else {
            dst[o++] = src[i];
        }
    }
    dst[o] = '\0';
    return (int)o;
}

static void form_get(const char *body, const char *key, char *out, size_t out_cap)
{
    out[0] = '\0';
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t flen = amp ? (size_t)(amp - p) : strlen(p);
        if (flen > klen + 1 && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            url_decode(out, out_cap, p + klen + 1, flen - klen - 1);
            return;
        }
        p = amp ? amp + 1 : NULL;
    }
}

static void scan_networks(wifi_ap_record_t *aps, uint16_t *count)
{
    *count = 0;
    wifi_scan_config_t sc = {0};
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        /* scanning needs the station interface; the portal keeps AP+STA
         * enabled so this is only reached if the driver is really stuck */
        ESP_LOGW(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        return;
    }
    uint16_t num = MAX_APS;
    if (esp_wifi_scan_get_ap_records(&num, aps) == ESP_OK) {
        *count = num;
    }
}

static void send_response(int conn, int code, const char *ctype, const char *body, size_t body_len)
{
    char hdr[160];
    const char *text = code == 200 ? "OK" : "Error";
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                     "Connection: close\r\n\r\n", code, text, ctype, (unsigned)body_len);
    send(conn, hdr, n, 0);
    if (body_len > 0) {
        send(conn, body, body_len, 0);
    }
}

static void handle_http(int conn)
{
    static char req[2048];
    int total = 0;
    while (total < (int)sizeof(req) - 1) {
        int r = recv(conn, req + total, sizeof(req) - 1 - total, 0);
        if (r <= 0) {
            return;
        }
        total += r;
        req[total] = '\0';
        if (strstr(req, "\r\n\r\n")) {
            break;
        }
    }
    req[total] = '\0';

    char method[8] = {0};
    char path[64] = {0};
    sscanf(req, "%7s %63s", method, path);

    if (strcmp(path, "/scan") == 0) {
        wifi_ap_record_t aps[MAX_APS];
        uint16_t count;
        scan_networks(aps, &count);
        static char body[2048];
        int off = snprintf(body, sizeof(body), "[");
        for (int i = 0; i < count && off < (int)sizeof(body) - 80; i++) {
            char esc[33];
            size_t j = 0;
            for (const uint8_t *s = aps[i].ssid; *s && j < 32; s++) {
                esc[j++] = (*s >= 32 && *s < 127) ? *s : '?';
            }
            esc[j] = 0;
            off += snprintf(body + off, sizeof(body) - off, "%s{\"ssid\":\"%s\",\"rssi\":%d}",
                            i ? "," : "", esc, aps[i].rssi);
        }
        snprintf(body + off, sizeof(body) - off, "]");
        send_response(conn, 200, "application/json", body, strlen(body));
        return;
    }

    if (strcmp(path, "/status") == 0) {
        usb_printer_ids_t ids;
        usb_printer_get_ids(&ids);
        static char body[256];
        int n = snprintf(body, sizeof(body),
                         "{\"printer_ready\":%s,\"vid\":\"0x%04X\",\"pid\":\"0x%04X\"}",
                         usb_printer_is_ready() ? "true" : "false", ids.vid, ids.pid);
        send_response(conn, 200, "application/json", body, n);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/forget") == 0) {
        nvs_handle_t nvs;
        if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_erase_key(nvs, "ssid");
            nvs_erase_key(nvs, "pass");
            nvs_commit(nvs);
            nvs_close(nvs);
        }
        send_response(conn, 200, "text/plain", "Forgotten. Rebooting...", 22);
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/save") == 0) {
        char *body = strstr(req, "\r\n\r\n");
        if (!body) {
            send_response(conn, 400, "text/plain", "bad request", 11);
            return;
        }
        body += 4;
        char ssid[33], ssid_manual[33], pass[64];
        form_get(body, "ssid", ssid, sizeof(ssid));
        form_get(body, "ssid2", ssid_manual, sizeof(ssid_manual));
        form_get(body, "pass", pass, sizeof(pass));
        const char *final_ssid = ssid_manual[0] ? ssid_manual : ssid;
        if (!final_ssid[0]) {
            send_response(conn, 400, "text/plain", "missing ssid", 12);
            return;
        }
        portal_save_credentials(final_ssid, pass);
        ESP_LOGI(TAG, "credentials saved for \"%s\", restarting", final_ssid);
        send_response(conn, 200, "text/plain", "Saved. Rebooting...", 19);
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
        return;
    }

    wifi_ap_record_t aps[MAX_APS];
    uint16_t count;
    scan_networks(aps, &count);
    static char page[4096];
    int off = snprintf(page, sizeof(page),
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>SpoolWisp setup</title>"
        "<style>body{background:#111;color:#eee;font:15px/1.5 monospace;display:flex;"
        "justify-content:center;padding-top:8vh}div{width:min(92vw,340px)}h1{font-size:18px}"
        "select,input{width:100%%;background:#222;color:#eee;border:1px solid #444;padding:8px;"
        "margin:6px 0;font:inherit;box-sizing:border-box}button{width:100%%;padding:10px;"
        "background:#eee;color:#111;border:0;font:inherit;cursor:pointer}</style>"
        "</head><body><div><h1>SpoolWisp</h1>"
        "<form method='POST' action='/save'><select name='ssid'>");
    for (int i = 0; i < count && off < (int)sizeof(page) - 160; i++) {
        char esc[33];
        size_t j = 0;
        for (const uint8_t *s = aps[i].ssid; *s && j < 32; s++) {
            esc[j++] = (*s >= 32 && *s < 127) ? *s : '?';
        }
        esc[j] = 0;
        off += snprintf(page + off, sizeof(page) - off,
                        "<option value='%s'>%s (%d dBm)</option>", esc, esc, aps[i].rssi);
    }
    off += snprintf(page + off, sizeof(page) - off,
        "</select><input name='ssid2' maxlength='32' placeholder='hidden SSID (optional)'>"
        "<input type='password' name='pass' maxlength='63' placeholder='password'>"
        "<button type='submit'>connect</button></form></div></body></html>");
    send_response(conn, 200, "text/html", page, off);
}

static void http_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(80),
        .sin_addr = { .s_addr = htonl(INADDR_ANY) },
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(sock, 2) < 0) {
        ESP_LOGE(TAG, "http listen failed");
        vTaskDelete(NULL);
        return;
    }
    while (1) {
        int conn = accept(sock, NULL, NULL);
        if (conn >= 0) {
            handle_http(conn);
            close(conn);
        }
    }
}

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr = { .s_addr = htonl(INADDR_ANY) },
    };
    bind(sock, (struct sockaddr *)&addr, sizeof(addr));
    uint8_t buf[512];
    while (1) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int r = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
        if (r < 12 || r > (int)sizeof(buf) - 16 || (buf[2] & 0x80)) {
            continue;
        }
        uint8_t resp[512];
        memcpy(resp, buf, r);
        resp[2] = 0x81;
        resp[3] = 0x80;
        resp[6] = 0;
        resp[7] = 1;
        resp[r++] = 0xC0;
        resp[r++] = 0x0C;
        resp[r++] = 0; resp[r++] = 1;
        resp[r++] = 0; resp[r++] = 1;
        resp[r++] = 0; resp[r++] = 0; resp[r++] = 0; resp[r++] = 60;
        resp[r++] = 0; resp[r++] = 4;
        resp[r++] = 192; resp[r++] = 168; resp[r++] = 4; resp[r++] = 1;
        sendto(sock, resp, r, 0, (struct sockaddr *)&from, flen);
    }
}

void wifi_portal_run(void)
{
    ESP_LOGI(TAG, "starting provisioning portal");

    /* Reached from two paths: first boot (WiFi driver not initialised yet) and
     * failed STA connect (driver initialised and running). Sequence is valid
     * for both, and portal mode no longer blocks app_main, so the print/USB/IP
     * servers also come up while the portal is active. */
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    char ap_ssid[33];
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(ap_ssid, sizeof(ap_ssid), "%s-%02X%02X%02X", CONFIG_APP_PORTAL_AP_PREFIX,
             mac[3], mac[4], mac[5]);

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (ap_netif == NULL) {
        ESP_LOGE(TAG, "cannot create AP network interface");
        return;
    }
    /* AP-only mode cannot scan for networks (the scan API needs a station
     * interface), which is why the portal used to show an empty list. Keep an
     * STA netif and run AP+STA while provisioning; nothing connects until the
     * saved credentials are used on the next boot. */
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL) {
        esp_netif_create_default_wifi_sta();
    }

    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_ERR_WIFI_NOT_INIT) {
        wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    } else {
        /* already initialised: stop the previous (STA) mode first. Stopping
         * an initialised-but-not-started driver just returns an error we can
         * ignore, which is the case on the failed-connect path. */
        err = esp_wifi_stop();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_stop: %s", esp_err_to_name(err));
        }
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    /* device-specific WPA2 password so a random passer-by cannot reconfigure
     * the device: "cfg" + last 3 MAC bytes (9 chars, printed on serial only) */
    char ap_pass[16];
    snprintf(ap_pass, sizeof(ap_pass), "cfg%02X%02X%02X", mac[3], mac[4], mac[5]);

    wifi_config_t ap_cfg = {
        .ap = {
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strlcpy((char *)ap_cfg.ap.ssid, ap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(ap_ssid);
    strlcpy((char *)ap_cfg.ap.password, ap_pass, sizeof(ap_cfg.ap.password));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* the STA side only exists so the portal can scan; do not let it keep
     * retrying the old (unreachable) network while the portal is active */
    err = esp_wifi_disconnect();
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "esp_wifi_disconnect: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "connect to AP \"%s\" (password: %s) and open http://%s",
             ap_ssid, ap_pass, PORTAL_IP);
    xTaskCreate(http_task, "portal_http", 8192, NULL, 5, NULL);
    xTaskCreate(dns_task, "portal_dns", 3072, NULL, 3, NULL);
}
