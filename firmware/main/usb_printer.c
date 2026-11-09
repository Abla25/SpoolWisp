#include "usb_printer.h"
#include "job_queue.h"

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
    xfer->num_bytes = sizeof(usb_setup_packet_t) + wLength;
    xfer->bEndpointAddress = 0;
    xfer->callback = transfer_done_cb;
    xfer->context = NULL;

    esp_err_t err = usb_host_transfer_submit_control(s_printer.client, xfer);
    if (err != ESP_OK) {
        return err;
    }
    if (xSemaphoreTake(s_printer.xfer_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
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

esp_err_t usb_printer_xfer(bool control, uint8_t ep_addr, uint8_t *buf, uint32_t num_bytes,
                           int *actual, int *status)
{
    usb_transfer_t *xfer = control ? s_printer.ctrl_xfer : s_printer.bulk_xfer;
    if (control) {
        memcpy(xfer->data_buffer, buf, num_bytes);
    } else {
        memcpy(xfer->data_buffer, buf, num_bytes);
    }
    xfer->device_handle = s_printer.dev_hdl;
    xfer->bEndpointAddress = control ? 0 : ep_addr;
    xfer->num_bytes = num_bytes;
    xfer->flags = 0;
    xfer->callback = transfer_done_cb;
    xfer->context = NULL;

    esp_err_t err = control ? usb_host_transfer_submit_control(s_printer.client, xfer)
                            : usb_host_transfer_submit(xfer);
    if (err != ESP_OK) {
        return err;
    }
    if (xSemaphoreTake(s_printer.xfer_done, pdMS_TO_TICKS(30000)) != pdTRUE) {
        *status = -110;
        *actual = 0;
        return ESP_ERR_TIMEOUT;
    }
    *actual = xfer->actual_num_bytes;
    *status = status_to_errno(xfer->status);
    return ESP_OK;
}

static esp_err_t find_printer_interface(usb_device_handle_t dev_hdl, uint8_t *intf_num,
                                        uint8_t *ep_out, uint8_t *ep_in)
{
    const usb_config_desc_t *cfg_desc;
    esp_err_t err = usb_host_get_active_config_descriptor(dev_hdl, &cfg_desc);
    if (err != ESP_OK) {
        return err;
    }

    const usb_intf_desc_t *intf = NULL;
    int offset = 0;
    while ((intf = (const usb_intf_desc_t *)usb_parse_next_descriptor_of_type(
                (const usb_standard_desc_t *)intf, cfg_desc->wTotalLength,
                USB_W_VALUE_DT_INTERFACE, &offset)) != NULL) {
        if (intf->bInterfaceClass == PRINTER_CLASS && intf->bInterfaceSubClass == PRINTER_SUBCLASS) {
            const usb_ep_desc_t *ep = NULL;
            int ep_offset = offset;
            bool found_out = false;
            bool found_in = false;
            while ((ep = (const usb_ep_desc_t *)usb_parse_next_descriptor_of_type(
                        (const usb_standard_desc_t *)ep, cfg_desc->wTotalLength,
                        USB_W_VALUE_DT_ENDPOINT, &ep_offset)) != NULL) {
                if ((ep->bmAttributes & 0x03) != EP_ATTR_BULK) {
                    continue;
                }
                if ((ep->bEndpointAddress & 0x80) == 0 && !found_out) {
                    *ep_out = ep->bEndpointAddress;
                    found_out = true;
                } else if ((ep->bEndpointAddress & 0x80) != 0 && !found_in) {
                    *ep_in = ep->bEndpointAddress;
                    found_in = true;
                }
            }
            if (found_out) {
                *intf_num = intf->bInterfaceNumber;
                return ESP_OK;
            }
        }
    }
    return ESP_ERR_NOT_FOUND;
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
        ESP_LOGI(TAG, "printer: %s", (char *)(dev_id + 2));
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
        usb_host_device_close(s_printer.client, s_printer.dev_hdl);
        s_printer.dev_hdl = NULL;
    }
}

static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        printer_connect(event_msg->new_dev.address);
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        printer_disconnect(event_msg->dev_gone.dev_hdl);
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
        usb_host_client_handle_events(s_printer.client, pdMS_TO_TICKS(100));
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
