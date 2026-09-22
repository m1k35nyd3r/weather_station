#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "weather.h"

/* Full-bleed: the radar page spans the whole width and everything above the
 * nav bar. Requested at exactly this size so nothing is rescaled on device. */
#define RADAR_W 1024
#define RADAR_H 492

/* Half-height of the view in degrees of latitude. 1.2 is roughly 130km from
 * the centre to the top edge. */
#define RADAR_SPAN_DEG 1.2f

typedef struct {
    uint8_t *png;     /* PNG bytes, in PSRAM; NULL when empty */
    size_t png_len;
    bool valid;
} radar_image_t;

/* Fetch a radar image centred on `location`. On success `out` owns a PSRAM
 * buffer that the caller must release with radar_free(). Safe to call with an
 * `out` that already holds an image: the previous one is freed first. */
esp_err_t radar_fetch(const weather_location_t *location, radar_image_t *out);

void radar_free(radar_image_t *image);
