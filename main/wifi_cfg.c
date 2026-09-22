#include "wifi_cfg.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"

#define NVS_NAMESPACE "weather"
#define NVS_KEY_SSID  "wifi_ssid"
#define NVS_KEY_PASS  "wifi_pass"

static const char *TAG = "wifi_cfg";

void wifi_cfg_load(wifi_creds_t *out)
{
    memset(out, 0, sizeof(*out));
    /* Kconfig values are the fallback and are normally blank -- credentials
     * belong in NVS so nothing sensitive sits in the build. */
    snprintf(out->ssid, sizeof(out->ssid), "%s", CONFIG_WEATHER_WIFI_SSID);
    snprintf(out->password, sizeof(out->password), "%s", CONFIG_WEATHER_WIFI_PASSWORD);

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    char ssid[WIFI_SSID_LEN] = { 0 };
    char pass[WIFI_PASS_LEN] = { 0 };
    size_t ssid_len = sizeof(ssid);
    size_t pass_len = sizeof(pass);
    if (nvs_get_str(handle, NVS_KEY_SSID, ssid, &ssid_len) == ESP_OK && ssid[0] != '\0') {
        snprintf(out->ssid, sizeof(out->ssid), "%s", ssid);
        if (nvs_get_str(handle, NVS_KEY_PASS, pass, &pass_len) == ESP_OK) {
            snprintf(out->password, sizeof(out->password), "%s", pass);
        } else {
            out->password[0] = '\0';
        }
        ESP_LOGI(TAG, "Loaded stored credentials for \"%s\"", out->ssid);
    }
    nvs_close(handle);
}

bool wifi_cfg_is_set(const wifi_creds_t *creds)
{
    return creds->ssid[0] != '\0';
}

esp_err_t wifi_cfg_save(const wifi_creds_t *creds)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, NVS_KEY_SSID, creds->ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_PASS, creds->password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        /* Never log the passphrase. */
        ESP_LOGI(TAG, "Saved credentials for \"%s\"", creds->ssid);
    }
    return err;
}

#if CONFIG_ESP_WIFI_REMOTE_ENABLED

esp_err_t wifi_cfg_scan(wifi_ap_t *out, int max, int *found)
{
    *found = 0;

    wifi_scan_config_t scan = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_err_t err = esp_wifi_scan_start(&scan, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan failed: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count == 0) {
        return ESP_OK;
    }
    /* The co-processor can report many more APs than we can show. */
    if (count > 40) {
        count = 40;
    }

    wifi_ap_record_t *records = calloc(count, sizeof(wifi_ap_record_t));
    if (records == NULL) {
        return ESP_ERR_NO_MEM;
    }
    err = esp_wifi_scan_get_ap_records(&count, records);
    if (err != ESP_OK) {
        free(records);
        return err;
    }

    /* Records arrive strongest-first; keep the first sighting of each SSID so
     * a mesh with several APs on one name does not fill the list. */
    for (int i = 0; i < count && *found < max; ++i) {
        const char *ssid = (const char *)records[i].ssid;
        if (ssid[0] == '\0') {
            continue;
        }
        bool duplicate = false;
        for (int j = 0; j < *found; ++j) {
            if (strcmp(out[j].ssid, ssid) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        snprintf(out[*found].ssid, WIFI_SSID_LEN, "%s", ssid);
        out[*found].rssi = records[i].rssi;
        out[*found].open = (records[i].authmode == WIFI_AUTH_OPEN);
        (*found)++;
    }
    free(records);
    ESP_LOGI(TAG, "scan found %d networks", *found);
    return ESP_OK;
}

esp_err_t wifi_cfg_set_config(const wifi_creds_t *creds)
{
    /* sta.ssid and sta.password are fixed-width and not NUL-terminated: a
     * full-length SSID fills the array exactly. Copy an explicit length into
     * the zero-initialised struct rather than using a string function, which
     * would either truncate by one or warn about not terminating. */
    wifi_config_t config = { 0 };
    memcpy(config.sta.ssid, creds->ssid,
           strnlen(creds->ssid, sizeof(config.sta.ssid)));
    memcpy(config.sta.password, creds->password,
           strnlen(creds->password, sizeof(config.sta.password)));
    /* Allow open networks as well as WPA2. */
    config.sta.threshold.authmode = creds->password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    return esp_wifi_set_config(WIFI_IF_STA, &config);
}

esp_err_t wifi_cfg_apply(const wifi_creds_t *creds, bool disconnect_first)
{
    if (disconnect_first) {
        esp_wifi_disconnect();
    }
    esp_err_t err = wifi_cfg_set_config(creds);
    if (err != ESP_OK) {
        return err;
    }
    return esp_wifi_connect();
}

#else  /* Wi-Fi backend disabled */

esp_err_t wifi_cfg_scan(wifi_ap_t *out, int max, int *found)
{
    (void)out;
    (void)max;
    *found = 0;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t wifi_cfg_set_config(const wifi_creds_t *creds)
{
    (void)creds;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t wifi_cfg_apply(const wifi_creds_t *creds, bool disconnect_first)
{
    (void)creds;
    (void)disconnect_first;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
