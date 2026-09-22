#include "radar.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

/* NOAA's GeoServer composites layers server-side, so one request returns the
 * radar mosaic already drawn over state boundaries -- no tile arithmetic and
 * no second source to align. conus_bref_qcd is quality-controlled base
 * reflectivity; nws:state_boundary supplies the outlines. */
#define RADAR_HOST   "https://opengeo.ncep.noaa.gov/geoserver/ows"
/* Colons and commas are legal unencoded in a query string, and must stay
 * that way here: this string is a printf format, where %3A and %2C would
 * be read as conversion specifiers. */
#define RADAR_LAYERS "conus:conus_bref_qcd,nws:state_boundary"
/* Per-layer styles: none for the radar, boundary_gray for the outlines. The
 * default boundary style draws black lines, which vanish on the dark UI. */
#define RADAR_STYLES "styles=,boundary_gray"

/* A 384x384 PNG of radar plus vectors runs ~60-130KB; cap well clear of that
 * and bail out rather than truncate into a half image the decoder will reject. */
#define RADAR_MAX_BYTES (512 * 1024)

static const char *TAG = "radar";

void radar_free(radar_image_t *image)
{
    if (image->png != NULL) {
        free(image->png);
    }
    image->png = NULL;
    image->png_len = 0;
    image->valid = false;
}

esp_err_t radar_fetch(const weather_location_t *location, radar_image_t *out)
{
    radar_free(out);

    /* EPSG:4326 is plate carree, so equal spans in degrees are not equal
     * distances on the ground. Widen the longitude span by 1/cos(lat) to keep
     * the map from looking horizontally stretched. */
    float lat_span = RADAR_SPAN_DEG;
    float cos_lat = cosf(location->latitude * (float)M_PI / 180.0f);
    if (cos_lat < 0.2f) {
        cos_lat = 0.2f;   /* guard against the poles */
    }
    /* Widen longitude for the projection, then again for the image's own
     * aspect so the map is not stretched to fit a wide frame. */
    float lon_span = (lat_span / cos_lat) * ((float)RADAR_W / (float)RADAR_H);

    char url[512];
    snprintf(url, sizeof(url),
             RADAR_HOST "?service=WMS&version=1.3.0&request=GetMap"
             "&layers=" RADAR_LAYERS
             "&" RADAR_STYLES
             "&crs=EPSG:4326"
             "&bbox=%.4f,%.4f,%.4f,%.4f"
             "&width=%d&height=%d&format=image/png&transparent=true",
             location->latitude - lat_span, location->longitude - lon_span,
             location->latitude + lat_span, location->longitude + lon_span,
             RADAR_W, RADAR_H);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach,
        /* GeoServer renders on demand and can be slow under load. */
        .timeout_ms = 30000,
        .buffer_size = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    ESP_RETURN_ON_FALSE(client, ESP_ERR_NO_MEM, TAG, "HTTP client allocation failed");

    uint8_t *buffer = NULL;
    int total = 0;
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int64_t content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (content_length < 0 || status != 200) {
        ESP_LOGE(TAG, "radar request failed: status %d, length %lld", status, content_length);
        err = ESP_FAIL;
        goto cleanup;
    }

    /* Chunked responses report 0, so allocate the cap and shrink afterwards. */
    size_t capacity = (content_length > 0 && content_length < RADAR_MAX_BYTES)
                      ? (size_t)content_length : RADAR_MAX_BYTES;
    buffer = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "could not allocate %u bytes for radar image", (unsigned)capacity);
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    while ((size_t)total < capacity) {
        int length = esp_http_client_read(client, (char *)buffer + total, capacity - total);
        if (length < 0) {
            ESP_LOGE(TAG, "radar read failed after %d bytes", total);
            err = ESP_FAIL;
            goto cleanup;
        }
        if (length == 0) {
            break;
        }
        total += length;
    }

    /* Reject anything that is not a PNG rather than handing the decoder junk
     * -- GeoServer reports errors as an XML body with a 200 status. */
    static const uint8_t png_magic[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    if (total < (int)sizeof(png_magic) || memcmp(buffer, png_magic, sizeof(png_magic)) != 0) {
        ESP_LOGE(TAG, "radar response is not a PNG (%d bytes)", total);
        err = ESP_FAIL;
        goto cleanup;
    }

    out->png = buffer;
    out->png_len = (size_t)total;
    out->valid = true;
    buffer = NULL;   /* ownership handed over */
    ESP_LOGI(TAG, "radar %dx%d for %.3f,%.3f: %d bytes",
             RADAR_W, RADAR_H, location->latitude, location->longitude, total);

cleanup:
    if (buffer != NULL) {
        free(buffer);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}
