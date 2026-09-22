#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define WIFI_SSID_LEN 33   /* 32 + NUL */
#define WIFI_PASS_LEN 65   /* 64 + NUL */
#define WIFI_SCAN_MAX 12

typedef struct {
    char ssid[WIFI_SSID_LEN];
    int8_t rssi;
    bool open;             /* no passphrase required */
} wifi_ap_t;

typedef struct {
    char ssid[WIFI_SSID_LEN];
    char password[WIFI_PASS_LEN];
} wifi_creds_t;

/* Load saved credentials, falling back to the Kconfig values. */
void wifi_cfg_load(wifi_creds_t *out);

/* True once an SSID exists from either source. */
bool wifi_cfg_is_set(const wifi_creds_t *creds);

esp_err_t wifi_cfg_save(const wifi_creds_t *creds);

/* Blocking scan. Returns networks sorted strongest first, duplicates removed. */
esp_err_t wifi_cfg_scan(wifi_ap_t *out, int max, int *found);

/* Stage credentials without connecting. Use at boot, before esp_wifi_start():
 * STA_START then triggers the connect through the event handler, so nothing
 * races it. */
esp_err_t wifi_cfg_set_config(const wifi_creds_t *creds);

/* Apply credentials to the running station and connect. Pass disconnect_first
 * only when already associated: both esp_wifi_disconnect() and
 * esp_wifi_sta_get_ap_info() go over the hosted RPC and log a "precondition
 * not met" error when there is no connection to drop. */
esp_err_t wifi_cfg_apply(const wifi_creds_t *creds, bool disconnect_first);
