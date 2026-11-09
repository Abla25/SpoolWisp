#include "usbip_server.h"
#include "usb_printer.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lwip/sockets.h"

static const char *TAG = "usbip";

#define USBIP_VERSION       0x0111
#define OP_REQ_DEVLIST      0x8005
#define OP_REP_DEVLIST      0x0005
#define OP_REQ_IMPORT       0x8003
#define OP_REP_IMPORT       0x0003
#define USBIP_CMD_SUBMIT    0x00000001
#define USBIP_CMD_UNLINK    0x00000002
#define USBIP_RET_SUBMIT    0x00000003
#define USBIP_RET_UNLINK    0x00000004
#define USBIP_DIR_OUT       0
#define USBIP_DIR_IN        1

#define OP_COMMON_SIZE      8
#define URB_HDR_SIZE        48
#define DEVLIST_DEV_SIZE    312
#define BUSID_STR           "1-1"
#define BUSNUM              1
#define DEVNUM              1
#define XFER_BUF_SIZE       (16384 + 8)

static uint8_t *s_xfer_buf;

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = v >> 8;
    p[1] = v & 0xFF;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8) & 0xFF;
    p[3] = v & 0xFF;
}

static uint16_t get_u16(const uint8_t *p)
{
    return (p[0] << 8) | p[1];
}

static uint32_t get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int read_full(int sock, uint8_t *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        int r = recv(sock, buf + got, len - got, 0);
        if (r <= 0) {
            return -1;
        }
        got += r;
    }
    return 0;
}

static int write_full(int sock, const uint8_t *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int r = send(sock, buf + sent, len - sent, 0);
        if (r <= 0) {
            return -1;
        }
        sent += r;
    }
    return 0;
}

static void fill_device_record(uint8_t *rec)
{
    memset(rec, 0, DEVLIST_DEV_SIZE);
    snprintf((char *)rec, 256, "/sys/devices/platform/usb/1-1");
    snprintf((char *)rec + 256, 32, BUSID_STR);
    usb_printer_ids_t ids;
    usb_printer_get_ids(&ids);
    put_u32(rec + 0x12C, BUSNUM);
    put_u32(rec + 0x130, DEVNUM);
    put_u32(rec + 0x134, ids.speed);
    put_u16(rec + 0x138, ids.vid);
    put_u16(rec + 0x13A, ids.pid);
    put_u16(rec + 0x13C, ids.bcd_device);
    rec[0x13E] = ids.dev_class;
    rec[0x13F] = ids.dev_subclass;
    rec[0x140] = ids.dev_protocol;
    rec[0x141] = ids.config_value;
    rec[0x142] = ids.num_configs;
    rec[0x143] = ids.num_interfaces;
    for (int i = 0; i < ids.num_interfaces && i < 8; i++) {
        uint8_t *inf = rec + 0x144 + i * 4;
        inf[0] = 0x07;
        inf[1] = 0x01;
        inf[2] = 0x02;
        inf[3] = 0;
    }
}

static void handle_devlist(int sock)
{
    uint8_t rep[OP_COMMON_SIZE + 4 + DEVLIST_DEV_SIZE + 8 * 4];
    memset(rep, 0, sizeof(rep));
    put_u16(rep + 0, USBIP_VERSION);
    put_u16(rep + 2, OP_REP_DEVLIST);
    put_u32(rep + 4, 0);
    bool ready = usb_printer_is_ready();
    put_u32(rep + 8, ready ? 1 : 0);
    size_t len = OP_COMMON_SIZE + 4;
    if (ready) {
        fill_device_record(rep + len);
        len += DEVLIST_DEV_SIZE + 8 * 4;
    }
    write_full(sock, rep, len);
}

static void handle_urb_loop(int sock)
{
    usb_printer_set_exclusive(true);
    ESP_LOGI(TAG, "USB/IP client attached, printer in exclusive mode");

    while (1) {
        uint8_t hdr[URB_HDR_SIZE];
        if (read_full(sock, hdr, URB_HDR_SIZE) < 0) {
            break;
        }
        uint32_t command = get_u32(hdr + 0);
        uint32_t seqnum = get_u32(hdr + 4);
        uint32_t direction = get_u32(hdr + 12);
        uint32_t ep = get_u32(hdr + 16);

        if (command == USBIP_CMD_UNLINK) {
            uint8_t rep[URB_HDR_SIZE];
            memset(rep, 0, sizeof(rep));
            put_u32(rep + 0, USBIP_RET_UNLINK);
            put_u32(rep + 4, seqnum);
            put_u32(rep + 0x14, 0);
            if (write_full(sock, rep, URB_HDR_SIZE) < 0) {
                break;
            }
            continue;
        }
        if (command != USBIP_CMD_SUBMIT) {
            ESP_LOGW(TAG, "unknown command 0x%08X", (unsigned)command);
            break;
        }

        uint32_t xfer_flags = get_u32(hdr + 0x14);
        int32_t xfer_len = (int32_t)get_u32(hdr + 0x18);
        int32_t num_packets = (int32_t)get_u32(hdr + 0x20);
        (void)xfer_flags;
        if (num_packets != -1) {
            ESP_LOGW(TAG, "isochronous transfers not supported");
        }
        if (xfer_len < 0 || (uint32_t)xfer_len > XFER_BUF_SIZE - 8) {
            ESP_LOGW(TAG, "transfer too large: %d", (int)xfer_len);
        }

        bool is_control = (ep == 0);
        bool dir_in = (direction == USBIP_DIR_IN);
        uint32_t payload = is_control ? (uint32_t)xfer_len : (uint32_t)xfer_len;
        if (xfer_len < 0 || payload + 8 > XFER_BUF_SIZE) {
            uint8_t rep[URB_HDR_SIZE];
            memset(rep, 0, sizeof(rep));
            put_u32(rep + 0, USBIP_RET_SUBMIT);
            put_u32(rep + 4, seqnum);
            put_u32(rep + 0x14, (uint32_t)-90);
            put_u32(rep + 0x18, 0);
            put_u32(rep + 0x20, 0xFFFFFFFF);
            if (write_full(sock, rep, URB_HDR_SIZE) < 0) {
                break;
            }
            continue;
        }

        uint32_t num_bytes;
        if (is_control) {
            memcpy(s_xfer_buf, hdr + 0x28, 8);
            num_bytes = 8 + payload;
            if (!dir_in && payload > 0 && read_full(sock, s_xfer_buf + 8, payload) < 0) {
                break;
            }
        } else {
            num_bytes = payload;
            if (!dir_in && payload > 0 && read_full(sock, s_xfer_buf, payload) < 0) {
                break;
            }
        }

        int actual = 0;
        int status = 0;
        xSemaphoreTake(usb_printer_lock(), portMAX_DELAY);
        esp_err_t err;
        if (!usb_printer_is_ready()) {
            err = ESP_ERR_NOT_FOUND;
            status = -19;
            actual = 0;
        } else {
            err = usb_printer_xfer(is_control, (uint8_t)(is_control ? 0 : (ep | (dir_in ? 0x80 : 0x00))),
                                   s_xfer_buf, num_bytes, &actual, &status);
        }
        xSemaphoreGive(usb_printer_lock());
        if (err != ESP_OK && status == 0) {
            status = -5;
        }

        int actual_data = is_control ? (actual > 8 ? actual - 8 : 0) : actual;
        uint8_t rep[URB_HDR_SIZE];
        memset(rep, 0, sizeof(rep));
        put_u32(rep + 0, USBIP_RET_SUBMIT);
        put_u32(rep + 4, seqnum);
        put_u32(rep + 0x14, (uint32_t)status);
        put_u32(rep + 0x18, (uint32_t)actual_data);
        put_u32(rep + 0x20, 0xFFFFFFFF);
        if (write_full(sock, rep, URB_HDR_SIZE) < 0) {
            break;
        }
        if (dir_in && actual_data > 0) {
            const uint8_t *data = is_control ? s_xfer_buf + 8 : s_xfer_buf;
            if (write_full(sock, data, actual_data) < 0) {
                break;
            }
        }
    }

    usb_printer_set_exclusive(false);
    ESP_LOGI(TAG, "USB/IP client detached, raw printing resumed");
}

static void handle_import(int sock)
{
    uint8_t busid[32];
    if (read_full(sock, busid, 32) < 0) {
        return;
    }
    uint8_t rep[OP_COMMON_SIZE + 4 + DEVLIST_DEV_SIZE];
    memset(rep, 0, sizeof(rep));
    put_u16(rep + 0, USBIP_VERSION);
    put_u16(rep + 2, OP_REP_IMPORT);
    if (!usb_printer_is_ready()) {
        put_u32(rep + 4, 1);
        write_full(sock, rep, OP_COMMON_SIZE);
        return;
    }
    put_u32(rep + 4, 0);
    fill_device_record(rep + OP_COMMON_SIZE + 4);
    if (write_full(sock, rep, OP_COMMON_SIZE + 4 + DEVLIST_DEV_SIZE) < 0) {
        return;
    }
    handle_urb_loop(sock);
}

static void usbip_task(void *arg)
{
    s_xfer_buf = heap_caps_malloc(XFER_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_ERROR_CHECK(s_xfer_buf == NULL ? ESP_ERR_NO_MEM : ESP_OK);

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_APP_USBIP_PORT),
        .sin_addr = { .s_addr = htonl(INADDR_ANY) },
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(sock, 1) < 0) {
        ESP_LOGE(TAG, "cannot listen on port %d", CONFIG_APP_USBIP_PORT);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "USB/IP server listening on TCP %d (bidirectional mode)", CONFIG_APP_USBIP_PORT);

    while (1) {
        int conn = accept(sock, NULL, NULL);
        if (conn < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        uint8_t op[OP_COMMON_SIZE];
        if (read_full(conn, op, OP_COMMON_SIZE) < 0) {
            close(conn);
            continue;
        }
        uint16_t code = get_u16(op + 2);
        if (code == OP_REQ_DEVLIST) {
            handle_devlist(conn);
            close(conn);
        } else if (code == OP_REQ_IMPORT) {
            handle_import(conn);
            close(conn);
        } else {
            ESP_LOGW(TAG, "unknown op code 0x%04X", code);
            close(conn);
        }
    }
}

void usbip_server_start(void)
{
#ifdef CONFIG_APP_ENABLE_USBIP
    xTaskCreate(usbip_task, "usbip", 8192, NULL, 5, NULL);
#endif
}
