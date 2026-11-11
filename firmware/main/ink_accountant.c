/* Ink Accountant: read-only monitoring for the Epson L1800.
 *
 * REAL data (read from the printer's EEPROM with read-only service commands,
 * action 0x41/0x51 - never 0x42, resets, cleaning or any write):
 *   - waste ink pad counters (0x1C, 0x1E) and their maximum values
 *   - total pages (0xC8) and total passes (0xCC)
 *   - manual/power/timer cleaning counters (0x93..0x95)
 *
 * ESTIMATED data (the L1800 has no ink sensors; Epson itself says tanks cannot
 * be measured electronically): ink cost from pages * ml_per_page and the
 * user-configured EUR/100 ml prices. No fake per-colour levels are exposed.
 */
#include "ink_accountant.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "usb_printer.h"

static const char *TAG = "ink";

#define NVS_NS "ink"
#define POLL_PERIOD_MS (5 * 60 * 1000)
#define WASTE1_MAX 12476
#define WASTE2_MAX 6788
#define MAX_BODY 512

typedef struct {
    float price_per_100ml[6]; /* K, C, M, Y, LC, LM */
    float ml_per_page;
    uint8_t waste_alert_percent;
} ink_config_t;

typedef struct {
    uint32_t jobs;
    uint64_t bytes;
    uint32_t pages_total;
    uint32_t passes_total;
    uint32_t month_pages;
    uint32_t month_jobs;
    uint64_t month_bytes;
    char month[8];
} ink_acct_t;

typedef struct {
    uint16_t waste1;
    uint16_t waste2;
    uint32_t pages;
    uint32_t passes;
    uint8_t clean_manual;
    uint8_t clean_power;
    uint8_t clean_timer;
    bool valid;
    uint32_t age_s;
} ink_state_t;

static SemaphoreHandle_t s_mux;
static ink_config_t s_cfg;
static ink_acct_t s_acct;
static ink_state_t s_state;
static uint32_t s_boot_s;
static uint8_t s_ep_out;
static uint8_t s_ep_in;
static bool s_started;

static void set_defaults(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.ml_per_page = 0.10f;
    s_cfg.waste_alert_percent = 80;
    memset(&s_acct, 0, sizeof(s_acct));
    memset(&s_state, 0, sizeof(s_state));
}

static void load_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_cfg);
    if (nvs_get_blob(h, "cfg", &s_cfg, &len) != ESP_OK || len != sizeof(s_cfg) ||
        s_cfg.ml_per_page < 0 || s_cfg.ml_per_page > 50) {
        s_cfg.ml_per_page = 0.10f;
        s_cfg.waste_alert_percent = 80;
    }
    len = sizeof(s_acct);
    if (nvs_get_blob(h, "acct", &s_acct, &len) != ESP_OK || len != sizeof(s_acct)) {
        memset(&s_acct, 0, sizeof(s_acct));
    }
    nvs_close(h);
}

static void save_cfg(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "cfg", &s_cfg, sizeof(s_cfg));
        nvs_commit(h);
        nvs_close(h);
    }
}

static void save_acct(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "acct", &s_acct, sizeof(s_acct));
        nvs_commit(h);
        nvs_close(h);
    }
}

static void roll_month_if_needed(void)
{
    if (s_acct.month[0] == '\0') {
        strlcpy(s_acct.month, "boot", sizeof(s_acct.month));
    }
}

/* Parse the "EE:<4 hex addr><2n hex data>;" answer of a read-only EEPROM read. */
static esp_err_t parse_ee(const uint8_t *resp, int len, uint8_t size, uint8_t *out)
{
    for (int i = 0; i + 3 < len; i++) {
        if (resp[i] != 'E' || resp[i + 1] != 'E' || resp[i + 2] != ':') {
            continue;
        }
        const char *hex = (const char *)resp + i + 3;
        int remain = len - (i + 3);
        if (remain < 4 + 2 * size) {
            return ESP_ERR_INVALID_SIZE;
        }
        for (int b = 0; b < size; b++) {
            char hi = hex[4 + 2 * b];
            char lo = hex[4 + 2 * b + 1];
            int hv = (hi >= '0' && hi <= '9') ? hi - '0'
                     : (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10
                     : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10 : -1;
            int lv = (lo >= '0' && lo <= '9') ? lo - '0'
                     : (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10
                     : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10 : -1;
            if (hv < 0 || lv < 0) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            out[b] = (uint8_t)((hv << 4) | lv);
        }
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

/* Read `size` bytes from EEPROM address `addr`; caller holds the USB lock. */
static esp_err_t ee_read(uint16_t addr, uint8_t size, uint8_t *out)
{
    uint8_t req[16];
    uint8_t resp[64];
    int actual = 0;
    int status = 0;

    req[0] = 0x7C;
    req[1] = 0x7C;
    req[2] = 0x0A; /* payload length LE16 = 10 */
    req[3] = 0x00;
    memcpy(req + 4, "L18X", 4);
    req[8] = 0x51; /* bulk read action */
    req[9] = 0xAE;
    req[10] = 0xA8;
    req[11] = (uint8_t)(addr & 0xFF);
    req[12] = (uint8_t)(addr >> 8);
    req[13] = size;

    esp_err_t err = usb_printer_xfer_timeout(false, s_ep_out, req, 14,
                                             &actual, &status, pdMS_TO_TICKS(1500));
    if (err != ESP_OK || status != 0) {
        return ESP_FAIL;
    }
    /* The printer may first emit an unrelated status packet; keep reading
     * until the EE: answer shows up (bounded, read-only, no side effects). */
    for (int attempt = 0; attempt < 4; attempt++) {
        vTaskDelay(pdMS_TO_TICKS(70));
        memset(resp, 0, sizeof(resp));
        actual = 0;
        status = 0;
        err = usb_printer_xfer_timeout(false, s_ep_in, resp, sizeof(resp),
                                       &actual, &status, pdMS_TO_TICKS(400));
        if (err == ESP_OK && actual > 0 && parse_ee(resp, actual, size, out) == ESP_OK) {
            return ESP_OK;
        }
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t ink_accountant_refresh(void)
{
    if (!usb_printer_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (usb_printer_get_endpoints(&s_ep_out, &s_ep_in) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(usb_printer_lock(), pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    /* Pause forwarding while we talk to the printer, then resume. */
    usb_printer_set_exclusive(true);
    ink_state_t st = {0};
    uint8_t b[4];
    int ok = 0;

    if (ee_read(0x1C, 2, b) == ESP_OK) {
        st.waste1 = (uint16_t)(b[0] | (b[1] << 8));
        ok++;
    }
    if (ee_read(0x1E, 2, b) == ESP_OK) {
        st.waste2 = (uint16_t)(b[0] | (b[1] << 8));
        ok++;
    }
    if (ee_read(0xC8, 4, b) == ESP_OK) {
        st.pages = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                   ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
        st.valid = true;
        ok++;
    }
    if (ee_read(0xCC, 4, b) == ESP_OK) {
        st.passes = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                    ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
        ok++;
    }
    if (ee_read(0x93, 3, b) == ESP_OK) {
        st.clean_manual = b[0];
        st.clean_power = b[1];
        st.clean_timer = b[2];
        ok++;
    }

    usb_printer_set_exclusive(false);
    xSemaphoreGive(usb_printer_lock());

    if (ok == 0) {
        return ESP_FAIL;
    }

    st.valid = true;
    st.age_s = (uint32_t)((xTaskGetTickCount() * portTICK_PERIOD_MS) / 1000);

    if (xSemaphoreTake(s_mux, portMAX_DELAY) == pdTRUE) {
        if (st.pages > s_acct.pages_total) {
            s_acct.month_pages += st.pages - s_acct.pages_total;
        }
        s_acct.pages_total = st.pages;
        s_acct.passes_total = st.passes;
        s_state = st;
        save_acct();
        xSemaphoreGive(s_mux);
    }
    ESP_LOGI(TAG, "ink refresh: waste1=%u waste2=%u pages=%u passes=%u",
             (unsigned)st.waste1, (unsigned)st.waste2,
             (unsigned)st.pages, (unsigned)st.passes);
    return ESP_OK;
}

void ink_accountant_note_job(size_t bytes)
{
    if (!s_started) {
        return;
    }
    if (xSemaphoreTake(s_mux, portMAX_DELAY) != pdTRUE) {
        return;
    }
    s_acct.jobs++;
    s_acct.bytes += bytes;
    s_acct.month_jobs++;
    s_acct.month_bytes += bytes;
    roll_month_if_needed();
    save_acct();
    xSemaphoreGive(s_mux);
    ESP_LOGI(TAG, "job accounted: %u bytes", (unsigned)bytes);
}

/* ------------------------------- HTTP UI -------------------------------- */

static void http_send(int conn, const char *status, const char *ctype,
                      const char *body, size_t len)
{
    char hdr[192];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                     "Connection: close\r\nCache-Control: no-store\r\n\r\n",
                     status, ctype, (unsigned)len);
    send(conn, hdr, n, 0);
    if (len > 0) {
        send(conn, body, len, 0);
    }
}

static const char *printer_name(void)
{
    return "EPSON L1800 Series [WIFI]";
}

static int waste_percent(uint16_t raw, uint16_t max)
{
    if (max == 0) {
        return 0;
    }
    return (int)((uint32_t)raw * 100u / max);
}

static float avg_price(void)
{
    float sum = 0;
    for (int i = 0; i < 6; i++) {
        sum += s_cfg.price_per_100ml[i];
    }
    return sum / 6.0f;
}

static float month_cost(void)
{
    if (s_cfg.ml_per_page <= 0) {
        return 0;
    }
    return (float)s_acct.month_pages * s_cfg.ml_per_page / 100.0f * avg_price();
}

static void serve_page(int conn)
{
    char body[4096];
    int w1 = waste_percent(s_state.waste1, WASTE1_MAX);
    int w2 = waste_percent(s_state.waste2, WASTE2_MAX);
    int alert = s_cfg.waste_alert_percent;
    bool maxed = (w1 >= alert || w2 >= alert);
    char *p = body;
    size_t left = sizeof(body);
    int n = snprintf(p, left,
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>SpoolWisp Ink Accountant</title><style>"
        ":root{--ochre:#c9a227}body{margin:0;background:#111;color:#eee;"
        "font:15px/1.5 ui-sans-serif,system-ui,sans-serif}"
        "header{border-bottom:1px solid #2a2a2a;padding:18px 20px}"
        "h1{margin:0;font-size:19px;letter-spacing:.2px}"
        "h1 span{color:var(--ochre)}main{padding:20px;max-width:760px;margin:0 auto}"
        ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px}"
        ".card{border:1px solid #2a2a2a;border-radius:12px;padding:14px}"
        ".k{color:#999;font-size:12px;text-transform:uppercase;letter-spacing:.08em}"
        ".v{font-size:24px;margin-top:4px;color:var(--ochre)}"
        ".warn{border-color:var(--ochre);background:#1c1808}"
        "table{width:100%;border-collapse:collapse;margin-top:16px}"
        "td,th{padding:6px 4px;border-bottom:1px solid #222;text-align:left;font-size:14px}"
        "th{color:#999;font-weight:500}footer{color:#666;font-size:12px;padding:16px 20px}"
        "</style></head><body><header><h1>SpoolWisp <span>Ink Accountant</span></h1>"
        "</header><main><div class='grid'>"
        "<div class='card'><div class='k'>Printer</div><div class='v'>%s</div></div>"
        "<div class='card%s'><div class='k'>Waste pad 1</div><div class='v'>%d%%</div></div>"
        "<div class='card%s'><div class='k'>Waste pad 2</div><div class='v'>%d%%</div></div>"
        "<div class='card'><div class='k'>Pages</div><div class='v'>%u</div></div>"
        "<div class='card'><div class='k'>Est. cost this month</div><div class='v'>&euro; %.2f</div></div>"
        "<div class='card'><div class='k'>Jobs</div><div class='v'>%u</div></div>"
        "</div><table><tr><th>Field</th><th>Value</th></tr>"
        "<tr><td>Waste pad 1 raw / max</td><td>%u / %d</td></tr>"
        "<tr><td>Waste pad 2 raw / max</td><td>%u / %d</td></tr>"
        "<tr><td>Total passes</td><td>%u</td></tr>"
        "<tr><td>Cleanings (manual/power/timer)</td><td>%u / %u / %u</td></tr>"
        "<tr><td>Month</td><td>%s</td></tr>"
        "<tr><td>Month jobs / bytes</td><td>%u / %lu</td></tr>"
        "<tr><td>ml per page (config)</td><td>%.3f</td></tr>"
        "<tr><td>Alert threshold</td><td>%u%%</td></tr></table>"
        "<p style='color:#777;font-size:13px;margin-top:18px'>"
        "Waste counters and page counters are read directly from the printer "
        "(read-only). Ink cost is an estimate: the L1800 has no electronic ink "
        "level sensor. Configure prices via POST /api/config.</p>"
        "</main><footer>read-only monitoring &middot; no counters modified</footer>"
        "</body></html>",
        s_state.valid ? "ready" : (usb_printer_is_ready() ? "ready" : "absent"),
        w1 >= alert ? " warn" : "", w1,
        w2 >= alert ? " warn" : "", w2,
        (unsigned)s_state.pages,
        month_cost(),
        (unsigned)s_acct.jobs,
        (unsigned)s_state.waste1, WASTE1_MAX,
        (unsigned)s_state.waste2, WASTE2_MAX,
        (unsigned)s_state.passes,
        (unsigned)s_state.clean_manual, (unsigned)s_state.clean_power, (unsigned)s_state.clean_timer,
        s_acct.month, (unsigned)s_acct.month_jobs, (unsigned long)s_acct.month_bytes,
        s_cfg.ml_per_page, (unsigned)alert);
    (void)p;
    (void)left;
    (void)maxed;
    http_send(conn, "200 OK", "text/html", body, strlen(body));
}

static void serve_ink(int conn)
{
    char body[512];
    int w1 = waste_percent(s_state.waste1, WASTE1_MAX);
    int w2 = waste_percent(s_state.waste2, WASTE2_MAX);
    snprintf(body, sizeof(body),
             "{\"valid\":%s,\"printer_ready\":%s,"
             "\"waste1\":{\"raw\":%u,\"max\":%d,\"percent\":%d},"
             "\"waste2\":{\"raw\":%u,\"max\":%d,\"percent\":%d},"
             "\"pages\":%u,\"passes\":%u,"
             "\"cleanings\":{\"manual\":%u,\"power\":%u,\"timer\":%u},"
             "\"waste_alert_percent\":%u,\"uptime_s\":%lu}",
             s_state.valid ? "true" : "false",
             usb_printer_is_ready() ? "true" : "false",
             (unsigned)s_state.waste1, WASTE1_MAX, w1,
             (unsigned)s_state.waste2, WASTE2_MAX, w2,
             (unsigned)s_state.pages, (unsigned)s_state.passes,
             (unsigned)s_state.clean_manual, (unsigned)s_state.clean_power,
             (unsigned)s_state.clean_timer, (unsigned)s_cfg.waste_alert_percent,
             (unsigned long)((xTaskGetTickCount() * portTICK_PERIOD_MS) / 1000));
    http_send(conn, "200 OK", "application/json", body, strlen(body));
}

static void serve_accounting(int conn)
{
    char body[768];
    snprintf(body, sizeof(body),
             "{\"jobs\":%u,\"bytes\":%lu,\"month\":{\"key\":\"%s\",\"jobs\":%u,"
             "\"bytes\":%lu,\"pages\":%u,\"estimated_cost_eur\":%.2f},"
             "\"totals\":{\"pages_reported\":%u,\"passes_reported\":%u},"
             "\"config\":{\"ml_per_page\":%.3f,\"waste_alert_percent\":%u,"
             "\"price_per_100ml\":[%.2f,%.2f,%.2f,%.2f,%.2f,%.2f]}}",
             (unsigned)s_acct.jobs, (unsigned long)s_acct.bytes,
             s_acct.month, (unsigned)s_acct.month_jobs,
             (unsigned long)s_acct.month_bytes, (unsigned)s_acct.month_pages,
             month_cost(),
             (unsigned)s_acct.pages_total, (unsigned)s_acct.passes_total,
             s_cfg.ml_per_page, (unsigned)s_cfg.waste_alert_percent,
             s_cfg.price_per_100ml[0], s_cfg.price_per_100ml[1],
             s_cfg.price_per_100ml[2], s_cfg.price_per_100ml[3],
             s_cfg.price_per_100ml[4], s_cfg.price_per_100ml[5]);
    http_send(conn, "200 OK", "application/json", body, strlen(body));
}

static void serve_status(int conn)
{
    char body[256];
    snprintf(body, sizeof(body),
             "{\"printer_ready\":%s,\"name\":\"%s\",\"service\":\"spoolwisp\","
             "\"read_only\":true}",
             usb_printer_is_ready() ? "true" : "false", printer_name());
    http_send(conn, "200 OK", "application/json", body, strlen(body));
}

static float json_num(const char *body, const char *key, float def)
{
    const char *p = strstr(body, key);
    if (p == NULL) {
        return def;
    }
    p += strlen(key);
    while (*p == '"' || *p == ':' || *p == ' ') {
        p++;
    }
    char *end = NULL;
    float v = strtof(p, &end);
    return end == p ? def : v;
}

static void handle_config(int conn, const char *method, const char *body)
{
    if (strcmp(method, "POST") == 0) {
        float v;
        v = json_num(body, "price_black", -1);
        if (v >= 0) s_cfg.price_per_100ml[0] = v;
        v = json_num(body, "price_cyan", -1);
        if (v >= 0) s_cfg.price_per_100ml[1] = v;
        v = json_num(body, "price_magenta", -1);
        if (v >= 0) s_cfg.price_per_100ml[2] = v;
        v = json_num(body, "price_yellow", -1);
        if (v >= 0) s_cfg.price_per_100ml[3] = v;
        v = json_num(body, "price_light_cyan", -1);
        if (v >= 0) s_cfg.price_per_100ml[4] = v;
        v = json_num(body, "price_light_magenta", -1);
        if (v >= 0) s_cfg.price_per_100ml[5] = v;
        v = json_num(body, "ml_per_page", -1);
        if (v > 0 && v < 50) s_cfg.ml_per_page = v;
        v = json_num(body, "waste_alert_percent", -1);
        if (v >= 1 && v <= 100) s_cfg.waste_alert_percent = (uint8_t)v;
        if (xSemaphoreTake(s_mux, portMAX_DELAY) == pdTRUE) {
            save_cfg();
            xSemaphoreGive(s_mux);
        }
        ESP_LOGI(TAG, "config updated (ml/page %.3f)", s_cfg.ml_per_page);
    }
    serve_accounting(conn);
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
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(sock, 2) < 0) {
        ESP_LOGE(TAG, "HTTP bind failed");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Ink Accountant UI on port 80");

    while (1) {
        int conn = accept(sock, NULL, NULL);
        if (conn < 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        char req[1024];
        int total = 0;
        while (total < (int)sizeof(req) - 1) {
            int r = recv(conn, req + total, sizeof(req) - 1 - total, 0);
            if (r <= 0) {
                break;
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
        char *body = strstr(req, "\r\n\r\n");
        body = body ? body + 4 : req + total;

        if (strcmp(path, "/api/status") == 0) {
            serve_status(conn);
        } else if (strcmp(path, "/api/ink") == 0) {
            ink_accountant_refresh();
            serve_ink(conn);
        } else if (strcmp(path, "/api/accounting") == 0) {
            serve_accounting(conn);
        } else if (strcmp(path, "/api/config") == 0) {
            handle_config(conn, method, body);
        } else {
            serve_page(conn);
        }
        close(conn);
    }
}

static void poll_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(3000));
    while (1) {
        ink_accountant_refresh();
        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}

void ink_accountant_start(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {0};
    if (sta == NULL || esp_netif_get_ip_info(sta, &ip) != ESP_OK || ip.ip.addr == 0) {
        ESP_LOGW(TAG, "no station IP, Ink Accountant HTTP disabled (portal mode)");
        return;
    }
    s_mux = xSemaphoreCreateMutex();
    set_defaults();
    load_nvs();
    roll_month_if_needed();
    s_started = true;
    xTaskCreate(http_task, "ink_http", 12288, NULL, 4, NULL);
    xTaskCreate(poll_task, "ink_poll", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "Ink Accountant ready on http://" IPSTR "/", IP2STR(&ip.ip));
}
