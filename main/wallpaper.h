/* Procedural weather wallpaper behind the Today page. See wallpaper.c. */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define WALLPAPER_W 1024
#define WALLPAPER_H 600

typedef enum {
    WALL_CLEAR,
    WALL_PARTLY,
    WALL_OVERCAST,
    WALL_FOG,
    WALL_RAIN,
    WALL_SNOW,
    WALL_STORM,
    WALL_SKY_COUNT,
} wall_sky_t;

/* Map an Open-Meteo WMO weather code to the scene that shows it. */
wall_sky_t wallpaper_sky(int weather_code);

/* Paint one scene into `out`, WALLPAPER_W x WALLPAPER_H RGB565 pixels. Dark
 * themes get a dimmed scene and light themes a paler one, so the theme's text
 * colour stays readable on top. Pure C with no LVGL or ESP-IDF calls; takes a
 * few hundred milliseconds, so call it outside the LVGL lock. Returns false if
 * the scratch buffer could not be allocated, leaving `out` untouched. */
bool wallpaper_render(uint16_t *out, wall_sky_t sky, bool night, bool light_theme);
