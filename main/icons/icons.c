#include "icons.h"

/* WMO weather codes, per Open-Meteo:
 *   0        clear
 *   1-2      mainly clear / partly cloudy
 *   3        overcast
 *   45,48    fog
 *   51-57    drizzle (incl. freezing)
 *   61-67    rain (incl. freezing)
 *   71-77    snow
 *   80-82    rain showers
 *   85-86    snow showers
 *   95-99    thunderstorm
 *
 * The night variants in wi_glyphs.h are unused until we track sunrise and
 * sunset; Open-Meteo's `daily` block can supply both when we want them.
 */
const char *weather_glyph(int code)
{
    if (code == 0)  return WI_DAY_SUNNY;
    if (code <= 2)  return WI_DAY_CLOUDY;
    if (code == 3)  return WI_CLOUDY;
    if (code <= 48) return WI_FOG;
    if (code <= 57) return WI_SPRINKLE;
    if (code <= 67) return WI_RAIN;
    if (code <= 77) return WI_SNOW;
    if (code <= 82) return WI_SHOWERS;
    if (code <= 86) return WI_SNOW;
    return WI_THUNDERSTORM;
}
