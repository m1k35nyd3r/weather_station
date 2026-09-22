#pragma once

#include <stdbool.h>

#include "radar.h"
#include "weather.h"
#include "wifi_cfg.h"

/* Called from the LVGL task when the user saves a 5-digit ZIP. */
typedef void (*ui_zip_submit_cb_t)(const char *zip);
typedef void (*ui_wifi_scan_cb_t)(void);
typedef void (*ui_wifi_submit_cb_t)(const char *ssid, const char *password);

typedef struct {
    ui_zip_submit_cb_t on_zip_submit;
    ui_wifi_scan_cb_t on_wifi_scan;
    ui_wifi_submit_cb_t on_wifi_submit;
} ui_callbacks_t;

/* Build all three screens. Must be called with the LVGL lock held. */
void ui_create(const ui_callbacks_t *callbacks);

/* All of the below take the LVGL lock themselves; call from any task. */
void ui_set_status(const char *text);
void ui_set_location(const char *place, const char *zip);
void ui_set_weather(const weather_data_t *data);
void ui_set_zip_hint(const char *text, bool is_error);

/* Pass NULL to show the "unavailable" state. The buffer must stay alive for
 * as long as it is displayed -- LVGL decodes lazily and re-reads on redraw. */
void ui_set_radar(const uint8_t *png, size_t png_len, const char *caption);

/* Wi-Fi settings screen. */
void ui_set_wifi_list(const wifi_ap_t *networks, int count);
void ui_set_wifi_status(const char *text, bool is_error);

/* Shows which network is in use and marks it in the scan list. Pass
 * connected=false to show the disconnected state. */
void ui_set_wifi_current(const char *ssid, bool connected);
void ui_show_wifi_settings(void);
