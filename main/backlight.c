#include "backlight.h"

#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "lvgl.h"

/* Matches the vendor BSP: 5kHz is well above anything visible and low enough
 * that the boost converter tracks it cleanly. 10-bit gives 1024 steps. */
#define BL_TIMER      LEDC_TIMER_1
#define BL_CHANNEL    LEDC_CHANNEL_1
#define BL_MODE       LEDC_LOW_SPEED_MODE
#define BL_RESOLUTION LEDC_TIMER_10_BIT
#define BL_FREQ_HZ    5000
#define BL_MAX_DUTY   ((1 << 10) - 1)

/* Long enough to read as a deliberate transition rather than a glitch. */
#define BL_FADE_MS    400

static const char *TAG = "backlight";

static int current_percent = BACKLIGHT_DAY_ACTIVE;
static bool fade_ready;
static time_t sunrise_utc;
static time_t sunset_utc;
static bool was_night;

esp_err_t backlight_init(int gpio)
{
    const ledc_timer_config_t timer = {
        .speed_mode = BL_MODE,
        .duty_resolution = BL_RESOLUTION,
        .timer_num = BL_TIMER,
        .freq_hz = BL_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_FALSE(ledc_timer_config(&timer) == ESP_OK, ESP_FAIL, TAG,
                        "LEDC timer config failed");

    const ledc_channel_config_t channel = {
        .gpio_num = gpio,
        .speed_mode = BL_MODE,
        .channel = BL_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BL_TIMER,
        .duty = BL_MAX_DUTY,
        .hpoint = 0,
    };
    ESP_RETURN_ON_FALSE(ledc_channel_config(&channel) == ESP_OK, ESP_FAIL, TAG,
                        "LEDC channel config failed");

    /* Hardware fading is optional: without it we still dim, just abruptly. */
    fade_ready = (ledc_fade_func_install(0) == ESP_OK);
    if (!fade_ready) {
        ESP_LOGW(TAG, "LEDC fade unavailable; brightness will step");
    }

    current_percent = BACKLIGHT_DAY_ACTIVE;
    ESP_LOGI(TAG, "backlight on GPIO%d, PWM %dHz", gpio, BL_FREQ_HZ);
    return ESP_OK;
}

void backlight_set(int percent)
{
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    if (percent == current_percent) {
        return;
    }
    current_percent = percent;

    uint32_t duty = (uint32_t)((BL_MAX_DUTY * percent) / 100);
    if (fade_ready) {
        ledc_set_fade_with_time(BL_MODE, BL_CHANNEL, duty, BL_FADE_MS);
        ledc_fade_start(BL_MODE, BL_CHANNEL, LEDC_FADE_NO_WAIT);
    } else {
        ledc_set_duty(BL_MODE, BL_CHANNEL, duty);
        ledc_update_duty(BL_MODE, BL_CHANNEL);
    }
}

int backlight_get(void)
{
    return current_percent;
}

void backlight_set_sun_times(time_t sunrise, time_t sunset)
{
    sunrise_utc = sunrise;
    sunset_utc = sunset;
}

bool backlight_is_night(void)
{
    time_t now = time(NULL);
    /* Same guard the hourly strip uses: before SNTP lands the clock is not
     * worth trusting, and guessing wrong would darken the screen in daylight. */
    if (now < 1700000000 || sunrise_utc == 0 || sunset_utc == 0) {
        return false;
    }
    return now < sunrise_utc || now > sunset_utc;
}

/* LVGL timestamps every input event, so inactivity needs no separate touch
 * hook. The GT911 keeps scanning at any brightness, so the screen always
 * responds -- and since it never goes fully dark, it stays glanceable. */
static void idle_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    uint32_t idle_ms = lv_display_get_inactive_time(NULL);
    bool night = backlight_is_night();
    bool idle = idle_ms >= BACKLIGHT_IDLE_AFTER_S * 1000U;

    int wanted;
    if (night) {
        wanted = idle ? BACKLIGHT_NIGHT_IDLE : BACKLIGHT_NIGHT_ACTIVE;
    } else {
        wanted = idle ? BACKLIGHT_DAY_IDLE : BACKLIGHT_DAY_ACTIVE;
    }

    if (night != was_night) {
        was_night = night;
        ESP_LOGI(TAG, "%s", night ? "sunset: night levels" : "sunrise: day levels");
    }
    if (wanted != current_percent) {
        ESP_LOGI(TAG, "%s, idle %us -> %d%%", night ? "night" : "day",
                 (unsigned)(idle_ms / 1000), wanted);
        backlight_set(wanted);
    }
}

void backlight_start_idle_watch(void)
{
    lv_timer_create(idle_timer_cb, 1000, NULL);
    ESP_LOGI(TAG, "idle watch: settle after %ds (day %d%%/%d%%, night %d%%/%d%%)",
             BACKLIGHT_IDLE_AFTER_S, BACKLIGHT_DAY_ACTIVE, BACKLIGHT_DAY_IDLE,
             BACKLIGHT_NIGHT_ACTIVE, BACKLIGHT_NIGHT_IDLE);
}
