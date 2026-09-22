#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_lcd_jd9165.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs_flash.h"

#include "backlight.h"
#include "radar.h"
#include "ui.h"
#include "weather.h"
#include "wifi_cfg.h"

#define LCD_H_RES 1024
#define LCD_V_RES 600
#define LCD_RST_GPIO 27
#define LCD_BACKLIGHT_GPIO 23
// RGB565 matches the vendor BSP for this JD9165 panel and CONFIG_LV_COLOR_DEPTH_16.
#define LCD_BITS_PER_PIXEL 16
// Bring-up aid: show the DSI controller's own colour bars before LVGL starts.
// It bypasses LVGL and the framebuffer, so bars mean the DSI link, panel and
// backlight are all good and any fault is in LVGL. Leave at 0 normally.
#define DISPLAY_TEST_PATTERN 0
/* Bring-up aid: poll the touch controller directly and log raw coordinates,
 * bypassing LVGL entirely. Leave at 0 in normal use -- the GT911 clears its
 * status register on read, so this probe and LVGL's own input reader steal
 * events from each other. Only turn it on when touch is suspect. */
#define TOUCH_DEBUG 0
/* Ask LVGL what it thinks the pointer is doing. Unlike TOUCH_DEBUG this does
 * not read the GT911 itself, so it cannot steal events from LVGL. */
#define LVGL_INPUT_DEBUG 0

// Touch: TOUCH_INT/TOUCH_RST are confirmed from the schematic (P4 pins 23/24).
// The I2C bus is shared with the RTC; the vendor's Arduino pins_config.h for
// this board gives SDA 7 / SCL 8, which the startup scan below verifies.
#define TOUCH_I2C_SDA_GPIO 7
#define TOUCH_I2C_SCL_GPIO 8
#define TOUCH_INT_GPIO 21
#define TOUCH_RST_GPIO 22

#define WIFI_CONNECTED_BIT BIT0
/* Open-Meteo declares its own cadence in current.interval (900s today), so
 * the schedule follows the data. These only bound it: the fallback is used
 * when a fetch fails or the response omits the field, and the clamps stop a
 * surprising value from hammering the API or stalling the display. */
#define WEATHER_FALLBACK_S 900
#define WEATHER_MIN_S      120
#define WEATHER_MAX_S      3600
/* Land just after the sample publishes rather than racing it. */
#define WEATHER_SKEW_S     20

static const char *TAG = "weather_station";
static EventGroupHandle_t wifi_events;
static weather_location_t location;
static volatile bool zip_request_pending;
static char zip_request[WEATHER_ZIP_LEN];
static volatile bool wifi_scan_pending;
static volatile bool wifi_creds_pending;
static wifi_creds_t wifi_request;
static wifi_creds_t wifi_current;

// Panel initialisation sequence for the HKC 7.0" QD070AS01-1 (JD9165BA, 2-lane),
// transcribed from the manufacturer's device tree:
//   docs/JC1060P470C_I_W/4-Driver_IC_Data_Sheet/
//     MTK_JD9165BA_HKC7.0_IPS(QD070AS01-1)_1024x600_MIPI_1+2Dot_G2.2_20240729_andy_2lane.dtsi.txt
//
// The esp_lcd_jd9165 driver's built-in default is only sleep-out + display-on.
// It never sends the vendor configuration, and critically never sets register
// 0x0B on page 0x01 to 0x11 (2-lane mode), so the panel's MIPI receiver never
// locks to the DSI stream and the screen stays black. 0x30 selects the register
// page; commands below are grouped by the page they belong to.
static const jd9165_lcd_init_cmd_t jd9165_qd070as01_init[] = {
    {0x30, (uint8_t []){0x00}, 1, 0},
    {0xF7, (uint8_t []){0x49, 0x61, 0x02, 0x00}, 4, 0},

    {0x30, (uint8_t []){0x01}, 1, 0},
    {0x04, (uint8_t []){0x0C}, 1, 0},
    {0x05, (uint8_t []){0x00}, 1, 0},
    {0x06, (uint8_t []){0x00}, 1, 0},
    {0x0B, (uint8_t []){0x11}, 1, 0},   // 0x13=4 lanes, 0x12=3, 0x11=2, 0x10=1
    {0x17, (uint8_t []){0x00}, 1, 0},
    {0x20, (uint8_t []){0x04}, 1, 0},
    {0x1F, (uint8_t []){0x05}, 1, 0},   // hs_settle time
    {0x23, (uint8_t []){0x00}, 1, 0},
    {0x25, (uint8_t []){0x19}, 1, 0},
    {0x28, (uint8_t []){0x18}, 1, 0},
    {0x29, (uint8_t []){0x04}, 1, 0},
    {0x2A, (uint8_t []){0x01}, 1, 0},
    {0x2B, (uint8_t []){0x04}, 1, 0},
    {0x2C, (uint8_t []){0x01}, 1, 0},

    {0x30, (uint8_t []){0x02}, 1, 0},
    {0x01, (uint8_t []){0x22}, 1, 0},
    {0x03, (uint8_t []){0x12}, 1, 0},
    {0x04, (uint8_t []){0x00}, 1, 0},
    {0x05, (uint8_t []){0x64}, 1, 0},
    {0x0A, (uint8_t []){0x08}, 1, 0},
    {0x0B, (uint8_t []){0x0A, 0x1A, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x06, 0x08, 0x1F, 0x1D}, 11, 0},
    {0x0C, (uint8_t []){0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x0D, (uint8_t []){0x16, 0x1B, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x07, 0x09, 0x1E, 0x1C}, 11, 0},
    {0x0E, (uint8_t []){0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x0F, (uint8_t []){0x16, 0x1B, 0x0D, 0x0B, 0x0D, 0x11, 0x10, 0x1C, 0x1E, 0x09, 0x07}, 11, 0},
    {0x10, (uint8_t []){0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x11, (uint8_t []){0x0A, 0x1A, 0x0D, 0x0B, 0x0D, 0x11, 0x10, 0x1D, 0x1F, 0x08, 0x06}, 11, 0},
    {0x12, (uint8_t []){0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x14, (uint8_t []){0x00, 0x00, 0x11, 0x11}, 4, 0},   // CKV_OFF
    {0x18, (uint8_t []){0x99}, 1, 0},

    {0x30, (uint8_t []){0x06}, 1, 0},   // gamma
    {0x12, (uint8_t []){0x36, 0x2C, 0x2E, 0x3C, 0x38, 0x35, 0x35, 0x32, 0x2E, 0x1D, 0x2B, 0x21, 0x16, 0x29}, 14, 0},
    {0x13, (uint8_t []){0x36, 0x2C, 0x2E, 0x3C, 0x38, 0x35, 0x35, 0x32, 0x2E, 0x1D, 0x2B, 0x21, 0x16, 0x29}, 14, 0},

    {0x30, (uint8_t []){0x0A}, 1, 0},
    {0x02, (uint8_t []){0x4F}, 1, 0},
    {0x0B, (uint8_t []){0x40}, 1, 0},
    {0x12, (uint8_t []){0x3E}, 1, 0},
    {0x13, (uint8_t []){0x78}, 1, 0},

    {0x30, (uint8_t []){0x0D}, 1, 0},
    {0x0D, (uint8_t []){0x04}, 1, 0},
    {0x10, (uint8_t []){0x0C}, 1, 0},
    {0x11, (uint8_t []){0x0C}, 1, 0},
    {0x12, (uint8_t []){0x0C}, 1, 0},
    {0x13, (uint8_t []){0x0C}, 1, 0},

    {0x30, (uint8_t []){0x00}, 1, 0},   // back to page 0
    {0x11, NULL, 0, 120},               // sleep out
    {0x29, NULL, 0, 20},                // display on
};

static esp_err_t init_display(esp_lcd_panel_handle_t *panel_out)
{
    /* LEDC PWM rather than a plain output: the pin drives the MP3202's EN
     * input and the board net is named LCD_PWM, so duty-cycle dimming is the
     * intended control. Full brightness until the idle watcher says otherwise. */
    ESP_ERROR_CHECK(backlight_init(LCD_BACKLIGHT_GPIO));

    // Set up the internal MIPI DSI power rail
    esp_ldo_channel_handle_t phy_power = NULL;
    esp_ldo_channel_config_t ldo_config = {
        .chan_id = 3,
        .voltage_mv = 2500,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_config, &phy_power));

    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t bus_config = JD9165_PANEL_BUS_DSI_2CH_CONFIG();
    bus_config.lane_bit_rate_mbps = 900;  // vendor BSP uses 900; the macro's 750 is untested here
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &dsi_bus));

    esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_dbi_io_config_t dbi_config = JD9165_PANEL_IO_DBI_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_config, &panel_io));

    esp_lcd_dpi_panel_config_t dpi_config = JD9165_1024_600_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_FMT_RGB565);
    // The driver macro's timings disagree with the panel datasheet: HS 20 vs 24,
    // and VBP/VFP are transposed (12/20 vs the specified 21/12). DOTCLK is 51.2MHz.
    // 1344 x 635 x 60Hz = 51.2MHz, so 51 gives ~59.8Hz.
    dpi_config.dpi_clock_freq_mhz = 51;
    dpi_config.video_timing.hsync_pulse_width = 24;
    dpi_config.video_timing.hsync_back_porch = 136;
    dpi_config.video_timing.hsync_front_porch = 160;
    dpi_config.video_timing.vsync_pulse_width = 2;
    dpi_config.video_timing.vsync_back_porch = 21;
    dpi_config.video_timing.vsync_front_porch = 12;

    jd9165_vendor_config_t vendor_config = {
        .init_cmds = jd9165_qd070as01_init,
        .init_cmds_size = sizeof(jd9165_qd070as01_init) / sizeof(jd9165_lcd_init_cmd_t),
        .mipi_config = {
            .dsi_bus = dsi_bus,
            .dpi_config = &dpi_config,
        },
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BITS_PER_PIXEL,
        .vendor_config = &vendor_config,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_jd9165(panel_io, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

#if DISPLAY_TEST_PATTERN
    ESP_LOGW(TAG, "DSI test pattern: colour bars for 2s (DISPLAY_TEST_PATTERN=1)");
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_set_pattern(panel, MIPI_DSI_PATTERN_BAR_VERTICAL));
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_set_pattern(panel, MIPI_DSI_PATTERN_NONE));
    ESP_LOGW(TAG, "DSI test pattern done, handing over to LVGL");
#endif

    lvgl_port_cfg_t lvgl_config = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_config));
    lvgl_port_display_cfg_t display_config = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .buffer_size = LCD_H_RES * 50,   // vendor demo uses H_RES * 50
        .double_buffer = false,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            // The vendor demo draws into DMA-capable internal RAM, not PSRAM.
            // esp_lvgl_port only permits buff_dma in RGB565, which we now are.
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = false,
        },
    };
    lvgl_port_display_dsi_cfg_t dsi_display_config = {
        .flags = {
            .avoid_tearing = false,
        },
    };
    if (lvgl_port_add_disp_dsi(&display_config, &dsi_display_config) == NULL) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "JD9165 display initialized: %dx%d RGB565", LCD_H_RES, LCD_V_RES);
    (void)phy_power;
    *panel_out = panel;
    return ESP_OK;
}

/* ------------------------------------------------------------------ wifi --- */

#if CONFIG_ESP_WIFI_REMOTE_ENABLED
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    }
}
#endif

static void wifi_start(void)
{
    wifi_events = xEventGroupCreate();
#if CONFIG_ESP_WIFI_REMOTE_ENABLED
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t wifi_error = esp_wifi_init(&wifi_init);
    if (wifi_error == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGE(TAG, "Wi-Fi backend is unavailable. ESP32-P4 requires the ESP32-C6 hosted Wi-Fi stack.");
        ui_set_status("Wi-Fi unavailable");
        return;
    }
    ESP_ERROR_CHECK(wifi_error);
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    /* Stage the credentials before starting: esp_wifi_start() raises
     * STA_START, and the handler connects. Connecting here as well would
     * race it and log an RPC "precondition not met". */
    wifi_cfg_load(&wifi_current);
    if (wifi_cfg_is_set(&wifi_current)) {
        ESP_LOGI(TAG, "Connecting to \"%s\"", wifi_current.ssid);
        wifi_cfg_set_config(&wifi_current);
    }

    /* Start the station even without credentials: scanning needs it running,
     * and the Wi-Fi settings screen is the only way out of that state. */
    ESP_ERROR_CHECK(esp_wifi_start());

    if (!wifi_cfg_is_set(&wifi_current)) {
        ESP_LOGW(TAG, "No Wi-Fi credentials stored; opening settings");
        ui_set_status("No Wi-Fi configured");
        ui_set_wifi_status("Tap Scan to choose a network", false);
        ui_show_wifi_settings();
    }
#else
    ESP_LOGE(TAG, "Wi-Fi backend disabled in sdkconfig; no network access.");
    ui_set_status("Wi-Fi disabled");
#endif
}

static void clock_start(void)
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&config);
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP sync timed out; hour labels may be off until it lands");
    }
}

/* ----------------------------------------------------------------- touch --- */

/* Log every device that ACKs, so the touch bus can be confirmed against the
 * schematic rather than assumed. GT911 answers at 0x5D or 0x14; the RTC that
 * shares this bus is at 0x32. */
static void i2c_scan(i2c_master_bus_handle_t bus)
{
    char found[96] = { 0 };
    size_t used = 0;
    for (uint8_t address = 0x08; address < 0x78; ++address) {
        if (i2c_master_probe(bus, address, 50) == ESP_OK && used < sizeof(found) - 8) {
            used += snprintf(found + used, sizeof(found) - used, "0x%02X ", address);
        }
    }
    ESP_LOGI(TAG, "I2C scan (SDA %d, SCL %d): %s", TOUCH_I2C_SDA_GPIO, TOUCH_I2C_SCL_GPIO,
             used ? found : "no devices responded");
}

static esp_lcd_touch_handle_t touch_handle;
static esp_lcd_panel_io_handle_t touch_io_handle;
static lv_indev_t *touch_indev;

#if LVGL_INPUT_DEBUG
/* Reports LVGL's own view of the pointer: whether it sees presses, where it
 * puts them, and which object is under that point. */
static void lvgl_input_debug_task(void *arg)
{
    LV_UNUSED(arg);
    lv_indev_state_t previous = LV_INDEV_STATE_RELEASED;
    while (true) {
        if (touch_indev != NULL && lvgl_port_lock(200)) {
            lv_indev_state_t state = lv_indev_get_state(touch_indev);
            if (state != previous) {
                lv_point_t point = { 0 };
                lv_indev_get_point(touch_indev, &point);
                lv_obj_t *hit = lv_indev_get_active_obj();
                ESP_LOGW(TAG, "LVGL pointer %s at (%d,%d), active obj=%p",
                         state == LV_INDEV_STATE_PRESSED ? "PRESSED" : "released",
                         (int)point.x, (int)point.y, hit);
                previous = state;
            }
            lvgl_port_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
#endif

#if TOUCH_DEBUG
static void touch_debug_task(void *arg)
{
    LV_UNUSED(arg);
    ESP_LOGW(TAG, "TOUCH_DEBUG on: press the panel, raw coordinates follow");
    uint16_t x[1], y[1], strength[1];
    uint8_t count = 0;
    bool was_down = false;
    int ticks = 0;
    while (true) {
        if (touch_handle != NULL && esp_lcd_touch_read_data(touch_handle) == ESP_OK) {
            bool down = esp_lcd_touch_get_coordinates(touch_handle, x, y, strength, &count, 1);
            if (down && count > 0) {
                ESP_LOGW(TAG, "touch: x=%u y=%u strength=%u points=%u",
                         x[0], y[0], strength[0], count);
                was_down = true;
            } else if (was_down) {
                ESP_LOGW(TAG, "touch: released");
                was_down = false;
            }
        }

        /* Every 2s, read the GT911's own status register (0x814E). Bit 7 is
         * "coordinates ready", bits 3:0 are the touch count. A byte that never
         * changes means the controller is not scanning, which is a different
         * problem from coordinates we are mapping wrongly. */
        if (++ticks >= 40) {
            ticks = 0;
            uint8_t status = 0xFF;
            esp_err_t err = ESP_FAIL;
            if (touch_io_handle != NULL) {
                err = esp_lcd_panel_io_rx_param(touch_io_handle, 0x814E, &status, 1);
            }
            ESP_LOGW(TAG, "GT911 status reg 0x814E = 0x%02X (read %s), INT pin = %d",
                     status, esp_err_to_name(err), gpio_get_level(TOUCH_INT_GPIO));
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
#endif

/* The GT911's configured output resolution need not match the panel it is
 * bonded to. This board's controller reports 800x480 while the display is
 * 1024x600, which squeezed every touch into the top-left ~78% of the screen.
 * Read its own X/Y maximum (config registers 0x8048..0x804B) and let
 * esp_lvgl_port scale, rather than hard-coding a ratio. */
static void touch_scale_from_controller(float *scale_x, float *scale_y)
{
    *scale_x = 1.0f;
    *scale_y = 1.0f;

    uint8_t cfg[4] = { 0 };
    if (touch_io_handle == NULL ||
        esp_lcd_panel_io_rx_param(touch_io_handle, 0x8048, cfg, sizeof(cfg)) != ESP_OK) {
        ESP_LOGW(TAG, "Could not read GT911 resolution; assuming it matches the panel");
        return;
    }

    int native_x = cfg[0] | (cfg[1] << 8);
    int native_y = cfg[2] | (cfg[3] << 8);
    if (native_x < 240 || native_y < 240 || native_x > 4096 || native_y > 4096) {
        ESP_LOGW(TAG, "GT911 reports implausible resolution %dx%d; leaving unscaled",
                 native_x, native_y);
        return;
    }

    *scale_x = (float)LCD_H_RES / (float)native_x;
    *scale_y = (float)LCD_V_RES / (float)native_y;
    ESP_LOGI(TAG, "GT911 native %dx%d -> panel %dx%d, scaling %.3f x %.3f",
             native_x, native_y, LCD_H_RES, LCD_V_RES, *scale_x, *scale_y);
}

/* Non-fatal: a display without touch is still useful, so log and carry on. */
static void init_touch(esp_lcd_panel_handle_t panel)
{
    LV_UNUSED(panel);
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = TOUCH_I2C_SDA_GPIO,
        .scl_io_num = TOUCH_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_new_master_bus(&bus_config, &bus) != ESP_OK) {
        ESP_LOGW(TAG, "Touch I2C bus init failed; continuing without touch");
        return;
    }
    i2c_scan(bus);

    esp_lcd_panel_io_handle_t touch_io = NULL;
    esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    if (esp_lcd_new_panel_io_i2c(bus, &io_config, &touch_io) != ESP_OK) {
        ESP_LOGW(TAG, "Touch panel IO init failed; continuing without touch");
        return;
    }
    touch_io_handle = touch_io;

    /* driver_data is what makes esp_lcd_touch_gt911 run its reset and I2C
     * address-selection sequence. Without it the controller is never reset,
     * and it answers ID reads while never reporting a touch. */
    static const esp_lcd_touch_io_gt911_config_t gt911_config = {
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
    };
    esp_lcd_touch_config_t touch_config = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = TOUCH_RST_GPIO,
        .int_gpio_num = TOUCH_INT_GPIO,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
        .driver_data = (void *)&gt911_config,
    };
    if (esp_lcd_touch_new_i2c_gt911(touch_io, &touch_config, &touch_handle) != ESP_OK) {
        ESP_LOGW(TAG, "GT911 not found; continuing without touch");
        return;
    }

#if TOUCH_DEBUG
    xTaskCreate(touch_debug_task, "touch_dbg", 4096, NULL, 4, NULL);
#endif

    float scale_x, scale_y;
    touch_scale_from_controller(&scale_x, &scale_y);

    lv_display_t *display = lv_display_get_default();
    lvgl_port_touch_cfg_t lvgl_touch = {
        .disp = display,
        .handle = touch_handle,
        .scale = { .x = scale_x, .y = scale_y },
    };
    touch_indev = lvgl_port_add_touch(&lvgl_touch);
    if (touch_indev == NULL) {
        ESP_LOGW(TAG, "Adding touch to LVGL failed");
        return;
    }
    /* esp_lvgl_port switches the indev to LV_INDEV_MODE_EVENT whenever an
     * int_gpio_num is configured, so LVGL stops polling and reads only when
     * the GT911 interrupt fires. That interrupt does not reach us here, so
     * LVGL saw nothing while direct polling of the controller worked fine.
     * We still want int_gpio_num set -- the driver needs it for the reset and
     * address-selection sequence -- so put the indev back into timer mode. */
    if (lvgl_port_lock(1000)) {
        lv_indev_set_mode(touch_indev, LV_INDEV_MODE_TIMER);
        lvgl_port_unlock();
    }
    ESP_LOGI(TAG, "GT911 touch ready (display=%p indev=%p, polled)", display, touch_indev);

#if LVGL_INPUT_DEBUG
    xTaskCreate(lvgl_input_debug_task, "lv_in_dbg", 4096, NULL, 4, NULL);
#endif
}

/* ------------------------------------------------------------------- app --- */

/* All three run in the LVGL task and must not block it, so they only record
 * the request; the weather task does the network work. */
static void on_zip_submit(const char *zip)
{
    snprintf(zip_request, sizeof(zip_request), "%s", zip);
    zip_request_pending = true;
}

static void on_wifi_scan(void)
{
    wifi_scan_pending = true;
}

static void on_wifi_submit(const char *ssid, const char *password)
{
    snprintf(wifi_request.ssid, sizeof(wifi_request.ssid), "%s", ssid);
    snprintf(wifi_request.password, sizeof(wifi_request.password), "%s", password);
    wifi_creds_pending = true;
}

/* Seconds until the next fetch, taken from the source when it says. */
static int seconds_until_next(const weather_data_t *data)
{
    time_t now = time(NULL);
    int wait = WEATHER_FALLBACK_S;

    if (data->valid && data->next_update_utc > 0 && now > 1700000000) {
        wait = (int)(data->next_update_utc + WEATHER_SKEW_S - now);
        if (wait < WEATHER_MIN_S) {
            /* Sample already due or just missed: step forward whole intervals
             * so we stay phase-locked instead of drifting. */
            int interval = data->interval_seconds > 0 ? data->interval_seconds
                                                      : WEATHER_FALLBACK_S;
            while (wait < WEATHER_MIN_S) {
                wait += interval;
            }
        }
    }
    if (wait > WEATHER_MAX_S) {
        wait = WEATHER_MAX_S;
    }
    return wait;
}

/* Held for as long as the image is on screen: LVGL decodes lazily and
 * re-reads these bytes on redraw, so it cannot be freed at the end of a
 * refresh. Replaced wholesale on the next successful fetch. */
static radar_image_t radar_image;

static void refresh_radar(void)
{
    radar_image_t fresh = { 0 };
    if (radar_fetch(&location, &fresh) != ESP_OK) {
        ESP_LOGW(TAG, "radar refresh failed; keeping previous image");
        return;
    }
    /* Point LVGL at the new bytes before releasing the old ones. */
    radar_image_t previous = radar_image;
    radar_image = fresh;
    ui_set_radar(radar_image.png, radar_image.png_len, location.place);
    radar_free(&previous);
}

static int refresh_now(weather_data_t *data)
{
    if (!weather_location_is_set(&location)) {
        ui_set_status("Tap the gear to set your location");
        return WEATHER_FALLBACK_S;
    }
    ui_set_status("Updating...");
    if (weather_fetch(&location, data) == ESP_OK) {
        ui_set_weather(data);
        /* Feed the backlight so it can switch levels at dusk and dawn on its
         * own, rather than only when a fetch happens to land. */
        backlight_set_sun_times(data->sunrise_utc, data->sunset_utc);
        ui_set_status("Updated just now");
    } else {
        ui_set_status("Update failed");
        data->valid = false;
    }
    refresh_radar();
    int wait = seconds_until_next(data);
    ESP_LOGI(TAG, "Next refresh in %d s", wait);
    return wait;
}

static void weather_task(void *arg)
{
    LV_UNUSED(arg);
    weather_data_t *data = calloc(1, sizeof(weather_data_t));
    if (data == NULL) {
        ESP_LOGE(TAG, "Unable to allocate weather data");
        ui_set_status("Out of memory");
        vTaskDelete(NULL);
        return;
    }

    ui_set_status("Waiting for Wi-Fi");
    xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    clock_start();

    /* Without a location there is nothing sensible to fetch -- blank Kconfig
     * coordinates would otherwise request weather for 0N 0E. Wait for a ZIP. */
    while (!weather_location_is_set(&location)) {
        ui_set_status("Tap the gear to set your location");
        if (zip_request_pending) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    bool link_was_up = true;   /* we only get here once associated */
    ui_set_wifi_current(wifi_current.ssid, true);

    TickType_t next_refresh = xTaskGetTickCount() + pdMS_TO_TICKS(refresh_now(data) * 1000);
    while (true) {
        bool link_up = (xEventGroupGetBits(wifi_events) & WIFI_CONNECTED_BIT) != 0;
        if (link_up != link_was_up) {
            link_was_up = link_up;
            ui_set_wifi_current(wifi_current.ssid, link_up);
        }

        if (wifi_scan_pending) {
            wifi_scan_pending = false;
            wifi_ap_t networks[WIFI_SCAN_MAX];
            int found = 0;
            if (wifi_cfg_scan(networks, WIFI_SCAN_MAX, &found) == ESP_OK) {
                ui_set_wifi_list(networks, found);
                ui_set_wifi_status(found ? "Tap a network to connect"
                                         : "No networks found", found == 0);
            } else {
                ui_set_wifi_status("Scan failed", true);
            }
        }

        if (wifi_creds_pending) {
            wifi_creds_pending = false;
            ui_set_wifi_status("Connecting...", false);
            wifi_current = wifi_request;
            bool was_connected =
                (xEventGroupGetBits(wifi_events) & WIFI_CONNECTED_BIT) != 0;
            if (wifi_cfg_apply(&wifi_current, was_connected) == ESP_OK) {
                /* Only save once the association actually succeeds, so a typo
                 * does not lock the device onto a network it cannot join. */
                EventBits_t bits = xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT,
                                                       pdFALSE, pdTRUE, pdMS_TO_TICKS(15000));
                if (bits & WIFI_CONNECTED_BIT) {
                    wifi_cfg_save(&wifi_current);
                    ui_set_wifi_current(wifi_current.ssid, true);
                    link_was_up = true;
                    ui_set_wifi_status("Connected and saved", false);
                } else {
                    ui_set_wifi_status("Could not connect - check the password", true);
                }
            } else {
                ui_set_wifi_status("Could not apply settings", true);
            }
        }

        if (zip_request_pending) {
            zip_request_pending = false;
            weather_location_t resolved;
            if (weather_lookup_zip(zip_request, &resolved) == ESP_OK) {
                location = resolved;
                weather_location_save(&location);
                ui_set_location(location.place, location.zip);
                ui_set_zip_hint("Saved", false);
                next_refresh = xTaskGetTickCount() + pdMS_TO_TICKS(refresh_now(data) * 1000);
            } else {
                ui_set_zip_hint("ZIP not found", true);
            }
        }

        if (xTaskGetTickCount() >= next_refresh) {
            next_refresh = xTaskGetTickCount() + pdMS_TO_TICKS(refresh_now(data) * 1000);
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void app_main(void)
{
    esp_err_t nvs_error = nvs_flash_init();
    if (nvs_error == ESP_ERR_NVS_NO_FREE_PAGES || nvs_error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_error = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_error);

    /* Keep the device clock in UTC so weather.c's offset maths is unambiguous. */
    setenv("TZ", "UTC0", 1);
    tzset();

    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(init_display(&panel));
    init_touch(panel);

    weather_location_load(&location);

    static const ui_callbacks_t ui_cb = {
        .on_zip_submit = on_zip_submit,
        .on_wifi_scan = on_wifi_scan,
        .on_wifi_submit = on_wifi_submit,
    };
    if (lvgl_port_lock(1000)) {
        ui_create(&ui_cb);
        /* LVGL owns the input timestamps, so the idle watcher lives on an
         * LVGL timer inside the lock. */
        backlight_start_idle_watch();
        lvgl_port_unlock();
    }
    ui_set_location(location.place, location.zip);
    ESP_LOGI(TAG, "UI ready");

    wifi_start();
    xTaskCreate(weather_task, "weather_task", 8192, NULL, 5, NULL);
    ESP_LOGI(TAG, "Weather station started");
}
