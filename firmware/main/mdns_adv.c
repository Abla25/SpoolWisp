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
static char s_instance[80] = "SpoolWisp [WIFI]";
static char s_pending_name[80];
static char s_pending_mfg[64];
static char s_pending_mdl[64];

static bool build_printer_name(const char *mfg, const char *mdl, char *out, size_t out_len)
{
    if (mfg != NULL && mfg[0] != '\0' && mdl != NULL && mdl[0] != '\0') {
        snprintf(out, out_len, "%s %s", mfg, mdl);
    } else if (mdl != NULL && mdl[0] != '\0') {
        snprintf(out, out_len, "%s", mdl);
    } else if (mfg != NULL && mfg[0] != '\0') {
        snprintf(out, out_len, "%s", mfg);
    } else {
        return false;
    }
    strlcat(out, " [WIFI]", out_len);
    return true;
}

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
    const char *instance = s_pending_name[0] != '\0' ? s_pending_name : "SpoolWisp [WIFI]";
    mdns_instance_name_set(instance);

    static mdns_txt_item_t raw_txt[] = {
        { "txtvers", "1" },
        { "qtotal", "1" },
        { "rp", "lp" },
        { "ty", "SpoolWisp USB print server" },
        { "note", "USB printer over ESP32-S3" },
    };
    err = mdns_service_add(instance, SVC_RAW, PROTO, CONFIG_APP_RAW_PORT,
                           raw_txt, sizeof(raw_txt) / sizeof(raw_txt[0]));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns raw service add failed: %s", esp_err_to_name(err));
    }

    /* Do NOT advertise _printer._tcp (LPD): macOS creates a dnssd LPD queue
     * from it that fails to connect, while the _pdl-datastream entry below is
     * auto-added as a working socket://host:9100 queue. LPD stays listening on
     * port 515 for manual/Linux use. */

    strlcpy(s_instance, instance, sizeof(s_instance));
    s_started = true;
    if (s_pending_name[0] != '\0') {
        update_printer_txt(s_pending_mfg, s_pending_mdl);
    }
    ESP_LOGI(TAG, "Bonjour: spoolwisp.local | \"%s\" socket:%d", s_instance, CONFIG_APP_RAW_PORT);
}

void mdns_adv_set_printer(const char *manufacturer, const char *model)
{
    char name[80];
    if (!build_printer_name(manufacturer, model, name, sizeof(name))) {
        return;
    }

    if (!s_started) {
        /* Printer already attached at boot: remember it and use the real name
         * for the very first announcement instead of "SpoolWisp". */
        strlcpy(s_pending_name, name, sizeof(s_pending_name));
        strlcpy(s_pending_mfg, manufacturer != NULL ? manufacturer : "", sizeof(s_pending_mfg));
        strlcpy(s_pending_mdl, model != NULL ? model : "", sizeof(s_pending_mdl));
        ESP_LOGI(TAG, "printer seen before mDNS start, will advertise \"%s\"", name);
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
    s_pending_name[0] = '\0';
    s_pending_mfg[0] = '\0';
    s_pending_mdl[0] = '\0';
    if (!s_started || strcmp(s_instance, "SpoolWisp [WIFI]") == 0) {
        return;
    }

    esp_err_t err_raw = mdns_service_instance_name_set(SVC_RAW, PROTO, "SpoolWisp [WIFI]");
    esp_err_t err_lpd = mdns_service_instance_name_set(SVC_LPD, PROTO, "SpoolWisp [WIFI]");
    if (err_raw != ESP_OK && err_lpd != ESP_OK) {
        return;
    }
    strlcpy(s_instance, "SpoolWisp [WIFI]", sizeof(s_instance));

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
