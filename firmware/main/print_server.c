#include "print_server.h"
#include "job_queue.h"
#include "usb_printer.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

static const char *TAG = "print_server";

#define RECV_BUF_SIZE 4096

bool client_allowed(const struct sockaddr_in *peer)
{
#ifdef CONFIG_APP_RESTRICT_LOCAL_SUBNET
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta == NULL || peer == NULL) {
        return true;
    }
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(sta, &ip_info) != ESP_OK) {
        return true;
    }
    uint32_t mask = ip_info.netmask.addr;
    uint32_t local = ip_info.ip.addr;
    uint32_t remote = peer->sin_addr.s_addr;
    if ((remote & mask) != (local & mask)) {
        esp_ip4_addr_t r = { .addr = remote };
        ESP_LOGW(TAG, "rejected client " IPSTR " (outside local subnet)", IP2STR(&r));
        return false;
    }
#endif
    return true;
}

static int listen_on(uint16_t port)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket failed for port %d", port);
        return -1;
    }
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr = { .s_addr = htonl(INADDR_ANY) },
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind failed on port %d", port);
        close(sock);
        return -1;
    }
    if (listen(sock, 2) < 0) {
        ESP_LOGE(TAG, "listen failed on port %d", port);
        close(sock);
        return -1;
    }
    return sock;
}

static void raw_session(int conn)
{
    static uint8_t buf[RECV_BUF_SIZE];
    ESP_LOGI(TAG, "raw client connected (port %d)", CONFIG_APP_RAW_PORT);
    int r;
    while ((r = recv(conn, buf, sizeof(buf), 0)) > 0) {
        job_queue_write(buf, r);
    }
    close(conn);
    ESP_LOGI(TAG, "raw client disconnected, job flushed to queue");
}

static void raw_server_task(void *arg)
{
    int sock = listen_on(CONFIG_APP_RAW_PORT);
    if (sock < 0) {
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "raw print server listening on TCP %d", CONFIG_APP_RAW_PORT);
    while (1) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int conn = accept(sock, (struct sockaddr *)&peer, &plen);
        if (conn < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (!client_allowed(&peer)) {
            close(conn);
            continue;
        }
        raw_session(conn);
    }
}

static int lpd_read_line(int conn, char *buf, size_t max_len)
{
    size_t i = 0;
    while (i + 1 < max_len) {
        char c;
        int r = recv(conn, &c, 1, 0);
        if (r <= 0) {
            return -1;
        }
        if (c == '\n') {
            buf[i] = '\0';
            return (int)i;
        }
        buf[i++] = c;
    }
    buf[max_len - 1] = '\0';
    return (int)i;
}

static int lpd_read_exact(int conn, uint8_t *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        int r = recv(conn, buf + got, len - got, 0);
        if (r <= 0) {
            return -1;
        }
        got += r;
    }
    return 0;
}

static void lpd_ack(int conn)
{
    uint8_t zero = 0;
    send(conn, &zero, 1, 0);
}

static void lpd_receive_job(int conn)
{
    char line[128];
    if (lpd_read_line(conn, line, sizeof(line)) < 0) {
        return;
    }
    lpd_ack(conn);
    ESP_LOGI(TAG, "LPD job on queue \"%s\"", line);

    while (1) {
        uint8_t sub;
        int r = recv(conn, &sub, 1, 0);
        if (r <= 0) {
            break;
        }
        if (lpd_read_line(conn, line, sizeof(line)) < 0) {
            break;
        }
        if (sub == 0x02 || sub == 0x03) {
            char *sp = strchr(line, ' ');
            long size = sp ? atol(sp + 1) : 0;
            lpd_ack(conn);
            if (sub == 0x02) {
                /* RFC 1179: 0x02 is the control file (job metadata), the
                 * printable payload is the 0x03 data file. Discard control. */
                uint8_t tmp[512];
                long left = size;
                while (left > 0) {
                    size_t chunk = left > (long)sizeof(tmp) ? sizeof(tmp) : (size_t)left;
                    if (lpd_read_exact(conn, tmp, chunk) < 0) {
                        return;
                    }
                    left -= chunk;
                }
                ESP_LOGI(TAG, "LPD control file (%ld bytes) discarded", size);
            } else {
                static uint8_t buf[RECV_BUF_SIZE];
                long left = size;
                while (left > 0) {
                    size_t chunk = left > (long)sizeof(buf) ? sizeof(buf) : (size_t)left;
                    if (lpd_read_exact(conn, buf, chunk) < 0) {
                        return;
                    }
                    job_queue_write(buf, chunk);
                    left -= chunk;
                }
                ESP_LOGI(TAG, "LPD data file (%ld bytes) queued", size);
            }
            lpd_ack(conn);
        } else if (sub == 0x04 || sub == 0x05) {
            break;
        } else {
            break;
        }
    }
}

static void lpd_session(int conn)
{
    uint8_t cmd;
    int r = recv(conn, &cmd, 1, 0);
    if (r <= 0) {
        close(conn);
        return;
    }
    char line[128];
    switch (cmd) {
    case 0x01:
        if (lpd_read_line(conn, line, sizeof(line)) >= 0) {
            uint8_t zero = 0;
            send(conn, &zero, 1, 0);
        }
        break;
    case 0x02:
        lpd_receive_job(conn);
        break;
    case 0x03:
    case 0x04:
        if (lpd_read_line(conn, line, sizeof(line)) >= 0) {
            uint8_t zero = 0;
            send(conn, &zero, 1, 0);
        }
        break;
    default:
        break;
    }
    close(conn);
}

static void lpd_server_task(void *arg)
{
    int sock = listen_on(CONFIG_APP_LPD_PORT);
    if (sock < 0) {
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "LPD server listening on TCP %d", CONFIG_APP_LPD_PORT);
    while (1) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int conn = accept(sock, (struct sockaddr *)&peer, &plen);
        if (conn < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (!client_allowed(&peer)) {
            close(conn);
            continue;
        }
        lpd_session(conn);
    }
}

void print_server_start(void)
{
    xTaskCreate(raw_server_task, "raw_srv", 4096, NULL, 5, NULL);
    xTaskCreate(lpd_server_task, "lpd_srv", 6144, NULL, 5, NULL);
}
