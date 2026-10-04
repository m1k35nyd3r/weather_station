#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

/* Brightness levels, in percent. The screen is never fully off -- this is a
 * wall display, so the idle state still has to be readable at a glance. */
#define BACKLIGHT_DAY_ACTIVE     100
#define BACKLIGHT_DAY_IDLE        12
#define BACKLIGHT_NIGHT_ACTIVE    25
#define BACKLIGHT_NIGHT_IDLE       4

/* Quiet hours, local time: the screen is fully off from QUIET_START_HOUR up to
 * QUIET_END_HOUR unless touched. A touch lights it at the night-active level
 * and it blanks again after QUIET_WAKE_S without another. */
#define BACKLIGHT_QUIET_START_HOUR  1
#define BACKLIGHT_QUIET_END_HOUR    7
#define BACKLIGHT_QUIET_WAKE_S     60

/* How long after the last touch before settling to the idle level. */
#define BACKLIGHT_IDLE_AFTER_S   600

/* Configure LEDC PWM on the backlight pin. The board wires GPIO23 to the
 * MP3202 boost driver's EN input, and the net is named LCD_PWM: duty-cycle
 * dimming is how the hardware is meant to be driven. */
esp_err_t backlight_init(int gpio);

void backlight_set(int percent);
int backlight_get(void);

/* Today's sun times as UTC epochs, from the forecast. Until this is called
 * -- or if either value is 0 -- the backlight stays on the day levels, so a
 * failed fetch never leaves the screen unexpectedly dark. */
void backlight_set_sun_times(time_t sunrise_utc, time_t sunset_utc);

/* Local offset from UTC in seconds, from the forecast. Until this is called
 * the offset is 0 and quiet hours are not applied. */
void backlight_set_utc_offset(int seconds);

/* True when the clock is trustworthy and local time is inside quiet hours. */
bool backlight_is_quiet_hours(void);

/* True when the clock is trustworthy and we are outside daylight. */
bool backlight_is_night(void);

/* Start the idle watcher. Creates an LVGL timer, so call it with the LVGL
 * lock held. Any touch resets the idle clock. */
void backlight_start_idle_watch(void);
