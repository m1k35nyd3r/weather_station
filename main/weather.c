#include "weather.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define RESPONSE_SIZE   24576
#define NVS_NAMESPACE   "weather"
#define NVS_KEY_LOC     "location"

static const char *TAG = "weather";

const char *weather_describe(int code)
{
    if (code == 0) return "Clear sky";
    if (code <= 2) return "Partly cloudy";
    if (code == 3) return "Overcast";
    if (code <= 48) return "Foggy";
    if (code <= 57) return "Drizzle";
    if (code <= 67) return "Rain";
    if (code <= 77) return "Snow";
    if (code <= 82) return "Rain showers";
    if (code <= 86) return "Snow showers";
    return "Storms";
}

/* ---------------------------------------------------------------- HTTP --- */

/* esp_http_client_perform() consumes the body itself, so drive the request
 * manually and read it into our own buffer. */
static esp_err_t http_get(const char *url, char *response, size_t response_size)
{
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    ESP_RETURN_ON_FALSE(client, ESP_ERR_NO_MEM, TAG, "HTTP client allocation failed");

    int total = 0;
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    // Negative means the headers could not be read; 0 is legal and means chunked.
    int64_t content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (content_length < 0 || status != 200) {
        ESP_LOGE(TAG, "HTTP request failed: status %d, content length %lld", status, content_length);
        err = ESP_FAIL;
        goto cleanup;
    }

    while (total < (int)response_size - 1) {
        int length = esp_http_client_read(client, response + total, response_size - 1 - total);
        if (length < 0) {
            ESP_LOGE(TAG, "HTTP read failed after %d bytes", total);
            err = ESP_FAIL;
            goto cleanup;
        }
        if (length == 0) {
            break;
        }
        total += length;
    }
    response[total] = '\0';
    if (total == 0) {
        ESP_LOGE(TAG, "HTTP response body was empty");
        err = ESP_FAIL;
    }

cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

/* ------------------------------------------------------------ location --- */

void weather_location_load(weather_location_t *out)
{
    memset(out, 0, sizeof(*out));
    // Kconfig defaults, used until the user sets a ZIP on the Location screen.
    snprintf(out->place, sizeof(out->place), "%s", "No location set");
    out->latitude = strtof(CONFIG_WEATHER_LATITUDE, NULL);
    out->longitude = strtof(CONFIG_WEATHER_LONGITUDE, NULL);

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    weather_location_t stored;
    size_t length = sizeof(stored);
    if (nvs_get_blob(handle, NVS_KEY_LOC, &stored, &length) == ESP_OK && length == sizeof(stored)) {
        stored.zip[sizeof(stored.zip) - 1] = '\0';
        stored.place[sizeof(stored.place) - 1] = '\0';
        *out = stored;
        ESP_LOGI(TAG, "Loaded location %s (%s) %.4f,%.4f",
                 out->zip, out->place, out->latitude, out->longitude);
    }
    nvs_close(handle);
}

bool weather_location_is_set(const weather_location_t *location)
{
    return location->zip[0] != '\0' ||
           location->latitude != 0.0f || location->longitude != 0.0f;
}

esp_err_t weather_location_save(const weather_location_t *location)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, NVS_KEY_LOC, location, sizeof(*location));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved location %s (%s)", location->zip, location->place);
    }
    return err;
}

/* Open-Meteo's geocoding API searches by place name, not US ZIP, so use
 * Zippopotam for the ZIP -> coordinates step. No API key, plain JSON. */
esp_err_t weather_lookup_zip(const char *zip, weather_location_t *out)
{
    char *response = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(response, ESP_ERR_NO_MEM, TAG, "ZIP lookup buffer allocation failed");

    char url[128];
    snprintf(url, sizeof(url), "https://api.zippopotam.us/us/%s", zip);

    esp_err_t err = http_get(url, response, 4096);
    if (err != ESP_OK) {
        free(response);
        return err;
    }

    cJSON *root = cJSON_Parse(response);
    free(response);
    ESP_RETURN_ON_FALSE(root, ESP_FAIL, TAG, "ZIP lookup returned invalid JSON");

    cJSON *places = cJSON_GetObjectItem(root, "places");
    cJSON *place = cJSON_IsArray(places) ? cJSON_GetArrayItem(places, 0) : NULL;
    cJSON *name = place ? cJSON_GetObjectItem(place, "place name") : NULL;
    cJSON *state = place ? cJSON_GetObjectItem(place, "state abbreviation") : NULL;
    cJSON *lat = place ? cJSON_GetObjectItem(place, "latitude") : NULL;
    cJSON *lon = place ? cJSON_GetObjectItem(place, "longitude") : NULL;

    if (!cJSON_IsString(name) || !cJSON_IsString(lat) || !cJSON_IsString(lon)) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "ZIP %s not found", zip);
        return ESP_ERR_NOT_FOUND;
    }

    memset(out, 0, sizeof(*out));
    snprintf(out->zip, sizeof(out->zip), "%s", zip);
    if (cJSON_IsString(state)) {
        snprintf(out->place, sizeof(out->place), "%s, %s", name->valuestring, state->valuestring);
    } else {
        snprintf(out->place, sizeof(out->place), "%s", name->valuestring);
    }
    out->latitude = strtof(lat->valuestring, NULL);
    out->longitude = strtof(lon->valuestring, NULL);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "ZIP %s -> %s (%.4f, %.4f)", zip, out->place, out->latitude, out->longitude);
    return ESP_OK;
}

/* ------------------------------------------------------------- forecast --- */

static void format_day_label(const char *iso_date, char *out, size_t out_size)
{
    // iso_date is "YYYY-MM-DD".
    struct tm tm_value = { 0 };
    int year, month, day;
    if (sscanf(iso_date, "%d-%d-%d", &year, &month, &day) != 3) {
        snprintf(out, out_size, "---");
        return;
    }
    tm_value.tm_year = year - 1900;
    tm_value.tm_mon = month - 1;
    tm_value.tm_mday = day;
    tm_value.tm_isdst = -1;
    mktime(&tm_value);
    strftime(out, out_size, "%a", &tm_value);
    for (char *c = out; *c; ++c) {
        if (*c >= 'a' && *c <= 'z') {
            *c -= 32;
        }
    }
}

static void format_hour_label(const char *iso_time, char *out, size_t out_size)
{
    // iso_time is "YYYY-MM-DDTHH:MM".
    const char *t = strchr(iso_time, 'T');
    int hour = 0;
    if (!t || sscanf(t + 1, "%d", &hour) != 1) {
        snprintf(out, out_size, "--");
        return;
    }
    const char *suffix = hour < 12 ? "AM" : "PM";
    int display = hour % 12;
    if (display == 0) {
        display = 12;
    }
    snprintf(out, out_size, "%d %s", display, suffix);
}

/* With timezone=auto every timestamp in the response is local to the forecast
 * location, while the device clock is UTC (TZ is pinned to UTC0 in app_main).
 * Convert using the utc_offset_seconds the response carries rather than
 * comparing the two directly -- that mismatch previously made the hourly strip
 * start several hours late. */
static time_t iso_to_utc(const char *iso, int utc_offset)
{
    struct tm parsed = { 0 };
    int year, month, day, hour = 0, minute = 0;
    int fields = sscanf(iso, "%d-%d-%dT%d:%d", &year, &month, &day, &hour, &minute);
    if (fields < 3) {
        return 0;
    }
    parsed.tm_year = year - 1900;
    parsed.tm_mon = month - 1;
    parsed.tm_mday = day;
    parsed.tm_hour = hour;
    parsed.tm_min = minute;
    parsed.tm_isdst = 0;
    time_t as_utc = mktime(&parsed);   // TZ is UTC, so this is a plain conversion
    return as_utc == (time_t)-1 ? 0 : as_utc - utc_offset;
}

/* Index of the first hourly sample at or after now, so the strip starts at
 * the current hour rather than midnight. */
static int first_future_hour(cJSON *times, int utc_offset)
{
    time_t now = time(NULL);
    if (now < 1700000000) {
        return 0;   // clock not set yet; just start at the top of the array
    }
    int count = cJSON_GetArraySize(times);
    for (int i = 0; i < count; ++i) {
        cJSON *entry = cJSON_GetArrayItem(times, i);
        if (!cJSON_IsString(entry)) {
            continue;
        }
        time_t sample = iso_to_utc(entry->valuestring, utc_offset);
        if (sample != 0 && sample >= now) {
            return i;
        }
    }
    return 0;
}

static float json_number(cJSON *array, int index, float fallback)
{
    cJSON *entry = cJSON_GetArrayItem(array, index);
    return cJSON_IsNumber(entry) ? (float)entry->valuedouble : fallback;
}

esp_err_t weather_fetch(const weather_location_t *location, weather_data_t *out)
{
    char *response = heap_caps_malloc(RESPONSE_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(response, ESP_ERR_NO_MEM, TAG, "forecast buffer allocation failed");

    char url[640];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m"
             "&hourly=temperature_2m,precipitation_probability"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset"
             "&temperature_unit=fahrenheit&wind_speed_unit=mph&precipitation_unit=inch"
             "&timezone=auto&forecast_days=%d",
             location->latitude, location->longitude, WEATHER_DAILY_MAX);

    esp_err_t err = http_get(url, response, RESPONSE_SIZE);
    if (err != ESP_OK) {
        free(response);
        return err;
    }

    cJSON *root = cJSON_Parse(response);
    free(response);
    ESP_RETURN_ON_FALSE(root, ESP_FAIL, TAG, "forecast returned invalid JSON");

    memset(out, 0, sizeof(*out));

    cJSON *offset = cJSON_GetObjectItem(root, "utc_offset_seconds");
    int utc_offset = cJSON_IsNumber(offset) ? offset->valueint : 0;

    cJSON *current = cJSON_GetObjectItem(root, "current");
    cJSON *daily = cJSON_GetObjectItem(root, "daily");
    cJSON *hourly = cJSON_GetObjectItem(root, "hourly");
    if (!current || !daily) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "forecast missing current/daily");
        return ESP_FAIL;
    }

    cJSON *temperature = cJSON_GetObjectItem(current, "temperature_2m");
    cJSON *apparent = cJSON_GetObjectItem(current, "apparent_temperature");
    cJSON *humidity = cJSON_GetObjectItem(current, "relative_humidity_2m");
    cJSON *code = cJSON_GetObjectItem(current, "weather_code");
    cJSON *wind = cJSON_GetObjectItem(current, "wind_speed_10m");
    if (!cJSON_IsNumber(temperature) || !cJSON_IsNumber(humidity) ||
        !cJSON_IsNumber(code) || !cJSON_IsNumber(wind)) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "forecast current block incomplete");
        return ESP_FAIL;
    }
    out->current.temperature = (float)temperature->valuedouble;
    out->current.feels_like = cJSON_IsNumber(apparent) ? (float)apparent->valuedouble
                                                       : (float)temperature->valuedouble;
    out->current.humidity = humidity->valueint;
    out->current.weather_code = code->valueint;
    out->current.wind_speed = (float)wind->valuedouble;

    /* The source declares its own cadence: current.interval is the number of
     * seconds between samples, and current.time is the one we just got. Line
     * the next fetch up with the next sample rather than polling blindly. */
    cJSON *interval = cJSON_GetObjectItem(current, "interval");
    cJSON *sample_time = cJSON_GetObjectItem(current, "time");
    if (cJSON_IsNumber(interval) && cJSON_IsString(sample_time)) {
        out->interval_seconds = interval->valueint;
        time_t sampled = iso_to_utc(sample_time->valuestring, utc_offset);
        if (sampled != 0 && out->interval_seconds > 0) {
            out->next_update_utc = sampled + out->interval_seconds;
        }
    }

    cJSON *dates = cJSON_GetObjectItem(daily, "time");
    cJSON *codes = cJSON_GetObjectItem(daily, "weather_code");
    cJSON *highs = cJSON_GetObjectItem(daily, "temperature_2m_max");
    cJSON *lows = cJSON_GetObjectItem(daily, "temperature_2m_min");
    cJSON *rain = cJSON_GetObjectItem(daily, "precipitation_probability_max");

    /* Index 0 is today, which is all the night test needs: before sunrise and
     * after sunset both resolve against today's pair. */
    cJSON *sunrise = cJSON_GetObjectItem(daily, "sunrise");
    cJSON *sunset = cJSON_GetObjectItem(daily, "sunset");
    cJSON *sunrise_today = cJSON_IsArray(sunrise) ? cJSON_GetArrayItem(sunrise, 0) : NULL;
    cJSON *sunset_today = cJSON_IsArray(sunset) ? cJSON_GetArrayItem(sunset, 0) : NULL;
    if (cJSON_IsString(sunrise_today) && cJSON_IsString(sunset_today)) {
        out->sunrise_utc = iso_to_utc(sunrise_today->valuestring, utc_offset);
        out->sunset_utc = iso_to_utc(sunset_today->valuestring, utc_offset);
    }
    int day_count = cJSON_IsArray(dates) ? cJSON_GetArraySize(dates) : 0;
    for (int i = 0; i < day_count && out->day_count < WEATHER_DAILY_MAX; ++i) {
        cJSON *date = cJSON_GetArrayItem(dates, i);
        cJSON *day_code = cJSON_GetArrayItem(codes, i);
        if (!cJSON_IsString(date) || !cJSON_IsNumber(day_code)) {
            continue;
        }
        weather_day_t *day = &out->days[out->day_count++];
        format_day_label(date->valuestring, day->label, sizeof(day->label));
        day->weather_code = day_code->valueint;
        day->high = json_number(highs, i, 0.0f);
        day->low = json_number(lows, i, 0.0f);
    }
    if (out->day_count > 0) {
        out->current.today_high = out->days[0].high;
        out->current.today_low = out->days[0].low;
        cJSON *today_rain = cJSON_GetArrayItem(rain, 0);
        out->current.today_rain_chance = cJSON_IsNumber(today_rain) ? today_rain->valueint : 0;
    }

    if (hourly) {
        cJSON *times = cJSON_GetObjectItem(hourly, "time");
        cJSON *temps = cJSON_GetObjectItem(hourly, "temperature_2m");
        cJSON *chances = cJSON_GetObjectItem(hourly, "precipitation_probability");
        int hour_total = cJSON_IsArray(times) ? cJSON_GetArraySize(times) : 0;
        int start = first_future_hour(times, utc_offset);
        for (int i = start; i < hour_total && out->hour_count < WEATHER_HOURLY_MAX; ++i) {
            cJSON *entry = cJSON_GetArrayItem(times, i);
            cJSON *temp = cJSON_GetArrayItem(temps, i);
            if (!cJSON_IsString(entry) || !cJSON_IsNumber(temp)) {
                continue;
            }
            weather_hour_t *hour = &out->hours[out->hour_count++];
            format_hour_label(entry->valuestring, hour->label, sizeof(hour->label));
            hour->temperature = (float)temp->valuedouble;
            cJSON *chance = cJSON_GetArrayItem(chances, i);
            hour->rain_chance = cJSON_IsNumber(chance) ? chance->valueint : 0;
        }
    }

    cJSON_Delete(root);
    out->valid = true;
    ESP_LOGI(TAG, "Forecast: %.0fF, %d days, %d hours; source interval %ds",
             out->current.temperature, out->day_count, out->hour_count,
             out->interval_seconds);
    if (out->sunrise_utc && out->sunset_utc) {
        ESP_LOGI(TAG, "Sun: rise %lld, set %lld (UTC epoch)",
                 (long long)out->sunrise_utc, (long long)out->sunset_utc);
    }
    return ESP_OK;
}
