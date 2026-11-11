#include "mdns_adv.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "mdns.h"

static const char *TAG = "mdns";

#define SVC_RAW  "_pdl-datastream"
#define SVC_LPD  "_printer"
#define PROTO    "_tcp"

static bool s_started;
static char s_instance[80] = "SpoolWisp";

/* CUPS uses usb_MFG/usb_MDL to build the IEEE-1284 device ID and can then
 * preselect the matching driver; ty is what humans see as the model. */
static void update_printer_txt(const char *mfg, const char *mdl)
{
    if (mdl != NULL && mdl[0] != '\0') {
        char product[96];
        snprintf(product, sizeof(product), "(%s)", mdl);
        mdns_service_txt_item_set(SVC_RAW, PROTO, "ty", mdl);
        mdns_service_txt_item_set(SVC_LPD, PROTO, "ty", mdl);
        mdns_service_txt_item_set(SVC_RAW, PROTO, "usb_MDL", mdl);
        mdns_service_txt_item_set(SVC_LPD, PROTO, "usb_MDL", mdl);
        mdns_service_txt_item_set(SVC_RAW, PROTO, "product", product);
        mdns_service_txt_item_set(SVC_LPD, PROTO, "product", product);
    }
    if (mfg != NULL && mfg[0] != '\0') {
        mdns_service_txt_item_set(SVC_RAW, PROTO, "usb_MFG", mfg);
        mdns_service_txt_item_set(SVC_LPD, PROTO, "usb_MFG", mfg);
    }
}

void mdns_adv_start(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return;
    }
    if ((err = mdns_hostname_set("spoolwisp")) != ESP_OK) {
        ESP_LOGW(TAG, "mdns_hostname_set: %s", esp_err_to_name(err));
    }
    mdns_instance_name_set("SpoolWisp");

    static mdns_txt_item_t raw_txt[] = {
        { "txtvers", "1" },
        { "qtotal", "1" },
        { "rp", "lp" },
        { "ty", "SpoolWisp USB print server" },
        { "note", "USB printer over ESP32-S3" },
    };
    err = mdns_service_add("SpoolWisp", SVC_RAW, PROTO, CONFIG_APP_RAW_PORT,
                           raw_txt, sizeof(raw_txt) / sizeof(raw_txt[0]));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns raw service add failed: %s", esp_err_to_name(err));
    }

    static mdns_txt_item_t lpd_txt[] = {
        { "txtvers", "1" },
        { "qtotal", "1" },
        { "rp", "lp" },
        { "ty", "SpoolWisp USB print server" },
        { "note", "USB printer over ESP32-S3" },
    };
    err = mdns_service_add("SpoolWisp", SVC_LPD, PROTO, CONFIG_APP_LPD_PORT,
                           lpd_txt, sizeof(lpd_txt) / sizeof(lpd_txt[0]));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns LPD service add failed: %s", esp_err_to_name(err));
    }

    s_started = true;
    ESP_LOGI(TAG, "Bonjour: spoolwisp.local | raw _pdl-datastream._tcp:%d | LPD _printer._tcp:%d",
             CONFIG_APP_RAW_PORT, CONFIG_APP_LPD_PORT);
}

void mdns_adv_set_printer(const char *manufacturer, const char *model)
{
    if (!s_started) {
        ESP_LOGW(TAG, "printer detected but mDNS is not running (no IP yet)");
        return;
    }

    char name[80];
    if (manufacturer != NULL && manufacturer[0] != '\0' &&
        model != NULL && model[0] != '\0') {
        snprintf(name, sizeof(name), "%s %s", manufacturer, model);
    } else if (model != NULL && model[0] != '\0') {
        snprintf(name, sizeof(name), "%s", model);
    } else if (manufacturer != NULL && manufacturer[0] != '\0') {
        snprintf(name, sizeof(name), "%s", manufacturer);
    } else {
        return;
    }

    if (strcmp(name, s_instance) == 0) {
        return;
    }

    esp_err_t err_raw = mdns_service_instance_name_set(SVC_RAW, PROTO, name);
    esp_err_t err_lpd = mdns_service_instance_name_set(SVC_LPD, PROTO, name);
    if (err_raw != ESP_OK && err_lpd != ESP_OK) {
        ESP_LOGW(TAG, "cannot rename Bonjour services to \"%s\": %s",
                 name, esp_err_to_name(err_raw));
        return;
    }
    strlcpy(s_instance, name, sizeof(s_instance));
    update_printer_txt(manufacturer, model);
    ESP_LOGI(TAG, "Bonjour printer name: \"%s\" (usb_MFG=\"%s\" usb_MDL=\"%s\")",
             name,
             manufacturer != NULL ? manufacturer : "",
             model != NULL ? model : "");
}

void mdns_adv_clear_printer(void)
{
    if (!s_started || strcmp(s_instance, "SpoolWisp") == 0) {
        return;
    }

    esp_err_t err_raw = mdns_service_instance_name_set(SVC_RAW, PROTO, "SpoolWisp");
    esp_err_t err_lpd = mdns_service_instance_name_set(SVC_LPD, PROTO, "SpoolWisp");
    if (err_raw != ESP_OK && err_lpd != ESP_OK) {
        return;
    }
    strlcpy(s_instance, "SpoolWisp", sizeof(s_instance));

    mdns_service_txt_item_remove(SVC_RAW, PROTO, "usb_MFG");
    mdns_service_txt_item_remove(SVC_LPD, PROTO, "usb_MFG");
    mdns_service_txt_item_remove(SVC_RAW, PROTO, "usb_MDL");
    mdns_service_txt_item_remove(SVC_LPD, PROTO, "usb_MDL");
    mdns_service_txt_item_remove(SVC_RAW, PROTO, "product");
    mdns_service_txt_item_remove(SVC_LPD, PROTO, "product");
    mdns_service_txt_item_set(SVC_RAW, PROTO, "ty", "SpoolWisp USB print server");
    mdns_service_txt_item_set(SVC_LPD, PROTO, "ty", "SpoolWisp USB print server");
    ESP_LOGI(TAG, "printer unplugged, Bonjour name back to \"SpoolWisp\"");
}
