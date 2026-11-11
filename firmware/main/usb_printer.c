#include "usb_printer.h"
#include "job_queue.h"
#include "mdns_adv.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"

static const char *TAG = "usb_printer";

#define PRINTER_CLASS           0x07
#define PRINTER_SUBCLASS        0x01
#define EP_ATTR_BULK            0x02
#define XFER_BUF_SIZE           16384
#define CTRL_XFER_SIZE          (sizeof(usb_setup_packet_t) + 256)

#define PRINTER_REQ_GET_DEVICE_ID   0x00
#define PRINTER_REQ_IN              0xA1

#define USBIP_SPEED_FULL        2

typedef struct {
    usb_host_client_handle_t client;
    usb_device_handle_t dev_hdl;
    uint8_t dev_addr;
    uint8_t intf_num;
    uint8_t ep_out;
    uint8_t ep_in;
    bool connected;
    bool exclusive;
    bool pending_new;
    bool pending_gone;
    uint8_t pending_dev_addr;
    usb_device_handle_t pending_gone_hdl;
    usb_printer_ids_t ids;
    SemaphoreHandle_t xfer_done;
    SemaphoreHandle_t mux;
    usb_transfer_t *bulk_xfer;
    usb_transfer_t *ctrl_xfer;
} printer_state_t;

static printer_state_t s_printer;

static void transfer_done_cb(usb_transfer_t *transfer)
{
    xSemaphoreGive(s_printer.xfer_done);
}

/* Transfer completions are only delivered from usb_host_client_handle_events(),
 * so a blocking wait must keep pumping that function, otherwise the completion
 * never reaches the callback and the wait always times out. */
static esp_err_t wait_transfer(TickType_t timeout_ticks)
{
    TickType_t start = xTaskGetTickCount();
    do {
        if (xSemaphoreTake(s_printer.xfer_done, 0) == pdTRUE) {
            return ESP_OK;
        }
        usb_host_client_handle_events(s_printer.client, pdMS_TO_TICKS(1));
    } while ((xTaskGetTickCount() - start) < timeout_ticks);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t control_request(uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                                 uint16_t wLength, uint8_t *data_out)
{
    usb_transfer_t *xfer = s_printer.ctrl_xfer;
    usb_setup_packet_t *setup = (usb_setup_packet_t *)xfer->data_buffer;
    setup->bmRequestType = PRINTER_REQ_IN;
    setup->bRequest = bRequest;
    setup->wValue = wValue;
    setup->wIndex = wIndex;
    setup->wLength = wLength;
    xfer->device_handle = s_printer.dev_hdl;
    xfer->num_bytes = sizeof(usb_setup_packet_t) + wLength;
    xfer->bEndpointAddress = 0;
    xfer->callback = transfer_done_cb;
    xfer->context = NULL;

    while (xSemaphoreTake(s_printer.xfer_done, 0) == pdTRUE) {
        /* drain stale completions */
    }
    esp_err_t err = usb_host_transfer_submit_control(s_printer.client, xfer);
    if (err != ESP_OK) {
        return err;
    }
    if (wait_transfer(pdMS_TO_TICKS(2000)) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        return ESP_FAIL;
    }
    if (data_out != NULL && wLength > 0) {
        int n = xfer->actual_num_bytes - (int)sizeof(usb_setup_packet_t);
        if (n > 0) {
            memcpy(data_out, xfer->data_buffer + sizeof(usb_setup_packet_t), n);
        }
    }
    return ESP_OK;
}

static int status_to_errno(usb_transfer_status_t status)
{
    switch (status) {
    case USB_TRANSFER_STATUS_COMPLETED:
        return 0;
    case USB_TRANSFER_STATUS_STALL:
        return -32;
    case USB_TRANSFER_STATUS_NO_DEVICE:
        return -19;
    case USB_TRANSFER_STATUS_TIMED_OUT:
        return -110;
    default:
        return -5;
    }
}

static esp_err_t xfer_impl(bool control, uint8_t ep_addr, uint8_t *buf, uint32_t num_bytes,
                           int *actual, int *status, TickType_t timeout_ticks)
{
    usb_transfer_t *xfer = control ? s_printer.ctrl_xfer : s_printer.bulk_xfer;
    if (control && num_bytes > CTRL_XFER_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (num_bytes > XFER_BUF_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(xfer->data_buffer, buf, num_bytes);
    xfer->device_handle = s_printer.dev_hdl;
    xfer->bEndpointAddress = control ? 0 : ep_addr;
    xfer->num_bytes = num_bytes;
    xfer->flags = 0;
    xfer->callback = transfer_done_cb;
    xfer->context = NULL;

    while (xSemaphoreTake(s_printer.xfer_done, 0) == pdTRUE) {
        /* drain stale completions */
    }
    esp_err_t err = control ? usb_host_transfer_submit_control(s_printer.client, xfer)
                            : usb_host_transfer_submit(xfer);
    if (err != ESP_OK) {
        return err;
    }
    if (wait_transfer(timeout_ticks) != ESP_OK) {
        *status = -110;
        *actual = 0;
        return ESP_ERR_TIMEOUT;
    }
    /* IN data is written by the host controller into the transfer buffer;
     * mirror it back into the caller's buffer (USB/IP returns it to the OS). */
    if (xfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        if (control) {
            int setup_len = (int)sizeof(usb_setup_packet_t);
            int data_len = (int)xfer->actual_num_bytes - setup_len;
            if (data_len > 0 && (uint32_t)data_len <= num_bytes - (uint32_t)setup_len) {
                memcpy(buf + setup_len, xfer->data_buffer + setup_len, (size_t)data_len);
            }
        } else if ((ep_addr & 0x80) != 0) {
            if (xfer->actual_num_bytes > 0 && (uint32_t)xfer->actual_num_bytes <= num_bytes) {
                memcpy(buf, xfer->data_buffer, (size_t)xfer->actual_num_bytes);
            }
        }
    }
    *actual = xfer->actual_num_bytes;
    *status = status_to_errno(xfer->status);
    return ESP_OK;
}

esp_err_t usb_printer_xfer(bool control, uint8_t ep_addr, uint8_t *buf, uint32_t num_bytes,
                           int *actual, int *status)
{
    return xfer_impl(control, ep_addr, buf, num_bytes, actual, status, pdMS_TO_TICKS(30000));
}

esp_err_t usb_printer_xfer_timeout(bool control, uint8_t ep_addr, uint8_t *buf,
                                   uint32_t num_bytes, int *actual, int *status,
                                   TickType_t timeout_ticks)
{
    return xfer_impl(control, ep_addr, buf, num_bytes, actual, status, timeout_ticks);
}

static esp_err_t find_printer_interface(usb_device_handle_t dev_hdl, uint8_t *intf_num,
                                        uint8_t *ep_out, uint8_t *ep_in)
{
    const usb_config_desc_t *cfg_desc;
    esp_err_t err = usb_host_get_active_config_descriptor(dev_hdl, &cfg_desc);
    if (err != ESP_OK) {
        return err;
    }

    /* Walk the configuration descriptors by hand: the usb_parse_* helpers
     * require a non-NULL starting descriptor and would otherwise also scan
     * past the end of a printer interface into the next one. */
    const uint8_t *p = (const uint8_t *)cfg_desc;
    const uint8_t *end = p + cfg_desc->wTotalLength;
    bool in_printer_intf = false;
    bool found_out = false;
    uint8_t out_addr = 0;
    uint8_t in_addr = 0;

    while ((size_t)(end - p) >= sizeof(usb_standard_desc_t)) {
        const usb_standard_desc_t *desc = (const usb_standard_desc_t *)p;
        if (desc->bLength < sizeof(usb_standard_desc_t) || p + desc->bLength > end) {
            break;
        }
        if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            if (found_out) {
                break;  /* endpoints of the first matching interface are enough */
            }
            const usb_intf_desc_t *intf = (const usb_intf_desc_t *)desc;
            in_printer_intf = (intf->bInterfaceClass == PRINTER_CLASS &&
                               intf->bInterfaceSubClass == PRINTER_SUBCLASS);
            if (in_printer_intf) {
                *intf_num = intf->bInterfaceNumber;
            }
        } else if (in_printer_intf && desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            const usb_ep_desc_t *ep = (const usb_ep_desc_t *)desc;
            if ((ep->bmAttributes & 0x03) == EP_ATTR_BULK) {
                if ((ep->bEndpointAddress & 0x80) == 0 && !found_out) {
                    out_addr = ep->bEndpointAddress;
                    found_out = true;
                } else if ((ep->bEndpointAddress & 0x80) != 0 && in_addr == 0) {
                    in_addr = ep->bEndpointAddress;
                }
            }
        }
        p += desc->bLength;
    }

    if (!found_out) {
        return ESP_ERR_NOT_FOUND;
    }
    *ep_out = out_addr;
    *ep_in = in_addr;
    return ESP_OK;
}

static void printer_connect(uint8_t dev_addr)
{
    esp_err_t err = usb_host_device_open(s_printer.client, dev_addr, &s_printer.dev_hdl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "device_open failed: %s", esp_err_to_name(err));
        return;
    }
    s_printer.dev_addr = dev_addr;

    const usb_device_desc_t *dev_desc;
    err = usb_host_get_device_descriptor(s_printer.dev_hdl, &dev_desc);
    if (err != ESP_OK) {
        goto fail;
    }
    ESP_LOGI(TAG, "USB device VID=0x%04X PID=0x%04X", dev_desc->idVendor, dev_desc->idProduct);
    if (CONFIG_APP_PRINTER_VID != 0 && dev_desc->idVendor != CONFIG_APP_PRINTER_VID) {
        ESP_LOGW(TAG, "VID 0x%04X not accepted, ignoring device", dev_desc->idVendor);
        goto fail;
    }
    if (CONFIG_APP_PRINTER_PID != 0 && dev_desc->idProduct != CONFIG_APP_PRINTER_PID) {
        ESP_LOGW(TAG, "PID 0x%04X not accepted, ignoring device", dev_desc->idProduct);
        goto fail;
    }

    usb_device_info_t dev_info;
    if (usb_host_device_info(s_printer.dev_hdl, &dev_info) == ESP_OK) {
        s_printer.ids.speed = (dev_info.speed == USB_SPEED_FULL) ? USBIP_SPEED_FULL : dev_info.speed;
    }
    s_printer.ids.vid = dev_desc->idVendor;
    s_printer.ids.pid = dev_desc->idProduct;
    s_printer.ids.bcd_device = dev_desc->bcdDevice;
    s_printer.ids.dev_class = dev_desc->bDeviceClass;
    s_printer.ids.dev_subclass = dev_desc->bDeviceSubClass;
    s_printer.ids.dev_protocol = dev_desc->bDeviceProtocol;
    s_printer.ids.config_value = dev_info.bConfigurationValue;
    s_printer.ids.num_configs = dev_desc->bNumConfigurations;

    err = find_printer_interface(s_printer.dev_hdl, &s_printer.intf_num, &s_printer.ep_out, &s_printer.ep_in);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no USB printer-class interface found");
        goto fail;
    }
    err = usb_host_interface_claim(s_printer.client, s_printer.dev_hdl, s_printer.intf_num, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "interface claim failed: %s", esp_err_to_name(err));
        goto fail;
    }

    const usb_config_desc_t *cfg_desc;
    if (usb_host_get_active_config_descriptor(s_printer.dev_hdl, &cfg_desc) == ESP_OK) {
        s_printer.ids.num_interfaces = cfg_desc->bNumInterfaces;
    }

    uint8_t dev_id[256] = {0};
    if (control_request(PRINTER_REQ_GET_DEVICE_ID, 0, s_printer.intf_num, 255, dev_id) == ESP_OK) {
        const char *id_str = (const char *)(dev_id + 2);
        ESP_LOGI(TAG, "printer: %s", id_str);

        /* IEEE-1284 device ID, e.g. "MFG:Epson;MDL:L1800 Series;CMD:...;".
         * Advertise the real printer name over Bonjour so the OS shows the
         * printer itself (and can preselect the matching driver). */
        char mfg[64] = {0};
        char mdl[64] = {0};
        const char *field = strstr(id_str, "MFG:");
        if (field != NULL) {
            sscanf(field + 4, "%63[^;]", mfg);
        }
        field = strstr(id_str, "MDL:");
        if (field != NULL) {
            sscanf(field + 4, "%63[^;]", mdl);
        }
        mdns_adv_set_printer(mfg, mdl);
    }
    s_printer.connected = true;
    ESP_LOGI(TAG, "printer ready: intf=%d ep_out=0x%02X ep_in=0x%02X",
             s_printer.intf_num, s_printer.ep_out, s_printer.ep_in);
    return;

fail:
    usb_host_device_close(s_printer.client, s_printer.dev_hdl);
    s_printer.dev_hdl = NULL;
}

static void printer_disconnect(usb_device_handle_t dev_hdl)
{
    if (s_printer.dev_hdl == dev_hdl) {
        s_printer.connected = false;
        ESP_LOGW(TAG, "printer disconnected");
        mdns_adv_clear_printer();
        usb_host_device_close(s_printer.client, s_printer.dev_hdl);
        s_printer.dev_hdl = NULL;
    }
}

static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    /* Runs inside usb_host_client_handle_events(): never block or start
     * transfers here, just remember what happened and let the printer task
     * process it outside the event pumping loop. */
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        s_printer.pending_new = true;
        s_printer.pending_dev_addr = event_msg->new_dev.address;
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        s_printer.pending_gone = true;
        s_printer.pending_gone_hdl = event_msg->dev_gone.dev_hdl;
        break;
    default:
        break;
    }
}

static void usb_lib_task(void *arg)
{
    usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskNotifyGive(arg);

    while (1) {
        uint32_t event_flags;
        ESP_ERROR_CHECK(usb_host_lib_handle_events(portMAX_DELAY, &event_flags));
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            break;
        }
    }
    vTaskSuspend(NULL);
}

static void usb_printer_task(void *arg)
{
    uint8_t *xfer_buf = heap_caps_malloc(XFER_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_ERROR_CHECK(xfer_buf == NULL ? ESP_ERR_NO_MEM : ESP_OK);

    usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        },
    };
    ESP_ERROR_CHECK(usb_host_client_register(&client_config, &s_printer.client));

    ESP_ERROR_CHECK(usb_host_transfer_alloc(XFER_BUF_SIZE, 0, &s_printer.bulk_xfer));
    ESP_ERROR_CHECK(usb_host_transfer_alloc(CTRL_XFER_SIZE, 0, &s_printer.ctrl_xfer));
    s_printer.xfer_done = xSemaphoreCreateBinary();
    s_printer.mux = xSemaphoreCreateMutex();

    ESP_LOGI(TAG, "USB host printer driver running");

    while (1) {
        xSemaphoreTake(s_printer.mux, portMAX_DELAY);
        usb_host_client_handle_events(s_printer.client, pdMS_TO_TICKS(20));

        if (s_printer.pending_gone) {
            s_printer.pending_gone = false;
            printer_disconnect(s_printer.pending_gone_hdl);
        }
        if (s_printer.pending_new) {
            s_printer.pending_new = false;
            printer_connect(s_printer.pending_dev_addr);
        }
        if (s_printer.connected && !s_printer.exclusive && job_queue_available() > 0) {
            size_t n = job_queue_read(xfer_buf, XFER_BUF_SIZE);
            if (n > 0) {
                int actual, status;
                if (usb_printer_xfer(false, s_printer.ep_out, xfer_buf, n, &actual, &status) != ESP_OK
                        || status != 0) {
                    ESP_LOGW(TAG, "raw write failed (status=%d), clearing endpoint", status);
                    usb_host_endpoint_halt(s_printer.dev_hdl, s_printer.ep_out);
                    usb_host_endpoint_clear(s_printer.dev_hdl, s_printer.ep_out);
                    xSemaphoreGive(s_printer.mux);
                    vTaskDelay(pdMS_TO_TICKS(500));
                    continue;
                }
            }
        }
        xSemaphoreGive(s_printer.mux);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

bool usb_printer_is_ready(void)
{
    return s_printer.connected;
}

void usb_printer_get_ids(usb_printer_ids_t *ids)
{
    xSemaphoreTake(s_printer.mux, portMAX_DELAY);
    *ids = s_printer.ids;
    xSemaphoreGive(s_printer.mux);
}

esp_err_t usb_printer_get_endpoints(uint8_t *ep_out, uint8_t *ep_in)
{
    if (!s_printer.connected || s_printer.dev_hdl == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ep_out != NULL) {
        *ep_out = s_printer.ep_out;
    }
    if (ep_in != NULL) {
        *ep_in = s_printer.ep_in;
    }
    return ESP_OK;
}

void usb_printer_set_exclusive(bool on)
{
    s_printer.exclusive = on;
}

SemaphoreHandle_t usb_printer_lock(void)
{
    return s_printer.mux;
}

void usb_printer_start(void)
{
    memset(&s_printer, 0, sizeof(s_printer));

    TaskHandle_t lib_task;
    xTaskCreate(usb_lib_task, "usb_lib", 4096, xTaskGetCurrentTaskHandle(), 2, &lib_task);
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));

    xTaskCreate(usb_printer_task, "usb_printer", 8192, NULL, 5, NULL);
}
