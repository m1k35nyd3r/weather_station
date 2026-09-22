#pragma once

/* Glyph defines and LV_FONT_DECLAREs are generated from the icon list:
 *   tools/weather_icons.txt  ->  ./tools/gen_weather_icons.py  ->  wi_glyphs.h
 * Edit the list, not the header. This file holds only the hand-written
 * mapping from weather data to those glyphs. */
#include "wi_glyphs.h"

/* Map an Open-Meteo WMO weather code to a glyph. */
const char *weather_glyph(int code);
