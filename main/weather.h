#pragma once

#include <stdbool.h>
#include <time.h>

#include "esp_err.h"

#define WEATHER_DAILY_MAX  5
#define WEATHER_HOURLY_MAX 12
#define WEATHER_PLACE_LEN  48
#define WEATHER_ZIP_LEN    6

typedef struct {
    float temperature;
    float feels_like;
    float wind_speed;
    int humidity;
    int weather_code;
    float today_high;
    float today_low;
    int today_rain_chance;
} weather_current_t;

typedef struct {
    char label[8];        // "MON"
    float high;
    float low;
    int weather_code;
} weather_day_t;

typedef struct {
    char label[8];        // "4 PM"
    float temperature;
    int rain_chance;
} weather_hour_t;

typedef struct {
    weather_current_t current;
    /* When the source says the next sample lands (UTC epoch), derived from
     * current.time + current.interval, and the interval itself in seconds.
     * Zero means the response did not say and the caller should fall back. */
    time_t next_update_utc;
    int interval_seconds;
    /* Today's sun times as UTC epochs; 0 when the response omitted them. */
    time_t sunrise_utc;
    time_t sunset_utc;
    weather_day_t days[WEATHER_DAILY_MAX];
    int day_count;
    weather_hour_t hours[WEATHER_HOURLY_MAX];
    int hour_count;
    bool valid;
} weather_data_t;

/* Location persisted in NVS. */
typedef struct {
    char zip[WEATHER_ZIP_LEN];
    char place[WEATHER_PLACE_LEN];   // "Mebane, NC"
    float latitude;
    float longitude;
} weather_location_t;

/* Short description for an Open-Meteo WMO weather code. */
const char *weather_describe(int code);

/* Load the stored location, falling back to the Kconfig defaults. */
void weather_location_load(weather_location_t *out);

/* False when no ZIP has been saved and the Kconfig fallback coordinates are
 * blank -- fetching would otherwise ask for weather at 0N 0E. Derived rather
 * than stored so the NVS blob layout does not change. */
bool weather_location_is_set(const weather_location_t *location);

/* Persist a location to NVS. */
esp_err_t weather_location_save(const weather_location_t *location);

/* Resolve a US ZIP to coordinates and a place name. Network required. */
esp_err_t weather_lookup_zip(const char *zip, weather_location_t *out);

/* Fetch and parse the forecast for a location. Network required. */
esp_err_t weather_fetch(const weather_location_t *location, weather_data_t *out);
