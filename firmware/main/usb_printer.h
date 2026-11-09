#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    uint16_t vid;
    uint16_t pid;
    uint16_t bcd_device;
    uint8_t dev_class;
    uint8_t dev_subclass;
    uint8_t dev_protocol;
    uint8_t config_value;
    uint8_t num_configs;
    uint8_t num_interfaces;
    uint8_t speed;
} usb_printer_ids_t;

void usb_printer_start(void);

bool usb_printer_is_ready(void);

void usb_printer_get_ids(usb_printer_ids_t *ids);

void usb_printer_set_exclusive(bool on);

SemaphoreHandle_t usb_printer_lock(void);

esp_err_t usb_printer_xfer(bool control, uint8_t ep_addr, uint8_t *buf, uint32_t num_bytes,
                           int *actual, int *status);
