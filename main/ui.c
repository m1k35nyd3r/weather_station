#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "misc/cache/instance/lv_image_cache.h"

#include "icons/icons.h"

/* Palette and geometry come straight from the approved 1024x600 mockup. */
#define COL_BG        lv_color_hex(0x102A43)
#define COL_CARD      lv_color_hex(0x17334F)
#define COL_CARD_ALT  lv_color_hex(0x1D3E5E)
#define COL_TEXT      lv_color_hex(0xF0F4F8)
#define COL_MUTED     lv_color_hex(0x9FB3C8)
#define COL_ACCENT    lv_color_hex(0x7CC4FA)
#define COL_WARM      lv_color_hex(0xF0B429)
#define COL_RAIN      lv_color_hex(0x47A3F3)
#define COL_DISABLED  lv_color_hex(0x5A7A96)

#define PAD           24
#define GAP           16
#define NAV_H         68
#define PAGE_W        976      // 1024 - 2 * PAD
#define PAGE_H        468      // 600 - 2 * PAD - NAV_H - GAP
#define HEADER_H      64

#define ZIP_LEN       5

static const char *TAG = "ui";

static void build_wifi(lv_obj_t *page);
static void build_modal(lv_obj_t *screen);

/* LVGL's built-in sprintf compiles out %f unless LV_USE_FLOAT is enabled, and
 * an unhandled conversion is emitted as a literal character -- "%.0f" renders
 * as a bare "f". Every temperature here is a whole number anyway, so round to
 * int and use %d rather than turning on LV_USE_FLOAT, which would also change
 * lv_value_precise_t throughout LVGL. */
static int whole(float value)
{
    return (int)lroundf(value);
}

typedef enum {
    PAGE_TODAY = 0,
    PAGE_HOURLY,
    PAGE_RADAR,
    PAGE_LOCATION,
    PAGE_WIFI,
    PAGE_COUNT,
} ui_page_t;

static void style_settings_tabs(ui_page_t page);

/* Location is not a tab any more -- the gear in each header opens it -- so
 * the nav bar maps its buttons onto a subset of the pages. */
#define NAV_COUNT 3
static const ui_page_t nav_pages[NAV_COUNT] = { PAGE_TODAY, PAGE_HOURLY, PAGE_RADAR };

static lv_obj_t *pages[PAGE_COUNT];
static lv_obj_t *nav_buttons[NAV_COUNT];
static ui_page_t active_page = PAGE_TODAY;

/* Today */
static lv_obj_t *lbl_place;
static lv_obj_t *lbl_subtitle;
static lv_obj_t *lbl_status;
static lv_obj_t *lbl_temp;
static lv_obj_t *lbl_condition;
static lv_obj_t *lbl_high_low;
static lv_obj_t *glyph_condition;
static lv_obj_t *tile_values[4];
static lv_obj_t *day_labels[WEATHER_DAILY_MAX];
static lv_obj_t *day_glyphs[WEATHER_DAILY_MAX];
static lv_obj_t *day_temps[WEATHER_DAILY_MAX];

/* Hourly */
static lv_obj_t *lbl_hourly_sub;
static lv_obj_t *chart;
static lv_chart_series_t *chart_series;
static lv_obj_t *hour_labels[8];
static lv_obj_t *hour_temps[8];
static lv_obj_t *hour_rain[8];

/* Radar */
static lv_obj_t *img_radar;
static lv_obj_t *lbl_radar_status;
static lv_obj_t *lbl_radar_sub;
static lv_image_dsc_t radar_dsc;

/* Location */
static lv_obj_t *lbl_zip;
static lv_obj_t *lbl_zip_hint;
static lv_obj_t *btn_save;
static lv_obj_t *lbl_save;
static lv_obj_t *lbl_current_loc;
static char zip_entry[ZIP_LEN + 1];

/* Wi-Fi settings */
static lv_obj_t *wifi_list;
static lv_obj_t *lbl_wifi_status;
static lv_obj_t *lbl_wifi_current;
/* Remembered so a later scan can mark the row we are joined to. */
static char current_ssid[WIFI_SSID_LEN];
static bool current_connected;
static lv_obj_t *modal;
static lv_obj_t *modal_title;
static lv_obj_t *modal_field;
static char pending_ssid[WIFI_SSID_LEN];

/* Settings tab pairs, one per settings page, restyled on navigation. */
static lv_obj_t *settings_tabs[2][2];

static ui_callbacks_t callbacks;

/* ---------------------------------------------------------------- utils --- */

static lv_color_t condition_color(int code)
{
    if (code <= 1) return COL_WARM;               // clear / mostly clear
    if (code <= 48) return lv_color_hex(0xBAE3FF); // cloud / fog
    if (code <= 86) return COL_RAIN;              // rain / snow
    return lv_color_hex(0xE12D39);                // storms
}

static lv_obj_t *make_card(lv_obj_t *parent, lv_color_t colour)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_style_bg_color(card, colour, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, lv_color_t colour)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, colour, 0);
    return label;
}

/* Weather Icons glyphs are drawn as text, so they tint with the ordinary
 * text colour and need no image objects or recolouring. */
static lv_obj_t *make_glyph(lv_obj_t *parent, const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, "");
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, COL_MUTED, 0);
    return label;
}

static void set_glyph(lv_obj_t *label, int code)
{
    lv_label_set_text(label, weather_glyph(code));
    lv_obj_set_style_text_color(label, condition_color(code), 0);
}

static void show_page(ui_page_t page)
{
    active_page = page;
    for (int i = 0; i < PAGE_COUNT; ++i) {
        if (i == page) {
            lv_obj_clear_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    style_settings_tabs(page);

    /* Settings has no tab, so every button goes inactive while it is open. */
    for (int i = 0; i < NAV_COUNT; ++i) {
        bool selected = (nav_pages[i] == page);
        lv_obj_set_style_bg_color(nav_buttons[i], selected ? COL_ACCENT : COL_CARD, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(nav_buttons[i], 0),
                                    selected ? COL_BG : COL_TEXT, 0);
    }
}

static void nav_event(lv_event_t *event)
{
    show_page((ui_page_t)(intptr_t)lv_event_get_user_data(event));
}

static void settings_event(lv_event_t *event)
{
    LV_UNUSED(event);
    show_page(PAGE_LOCATION);
}

/* ----------------------------------------------------------- header row --- */

#define HOME_BTN_SIZE 56
#define HEADER_TEXT_X (HOME_BTN_SIZE + 16)
/* Right-hand readouts stop short of the gear. */
#define HEADER_RIGHT_X (HOME_BTN_SIZE + 16)

static void home_event(lv_event_t *event)
{
    LV_UNUSED(event);
    show_page(PAGE_TODAY);
}

/* Every page gets the same header: a home button, then a title and subtitle
 * indented past it. LV_SYMBOL_HOME is FontAwesome U+F015, already present in
 * LVGL's built-in Montserrat fonts, so it needs no extra artwork. */
static lv_obj_t *make_header(lv_obj_t *parent, lv_obj_t **title_out, lv_obj_t **sub_out)
{
    lv_obj_t *header = lv_obj_create(parent);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, PAGE_W, HEADER_H);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *home = lv_button_create(header);
    lv_obj_remove_style_all(home);
    lv_obj_set_size(home, HOME_BTN_SIZE, HOME_BTN_SIZE);
    lv_obj_align(home, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(home, COL_CARD, 0);
    lv_obj_set_style_bg_opa(home, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(home, COL_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_radius(home, 14, 0);
    lv_obj_add_event_cb(home, home_event, LV_EVENT_CLICKED, NULL);

    lv_obj_t *glyph = make_label(home, LV_SYMBOL_HOME, &lv_font_montserrat_24, COL_ACCENT);
    lv_obj_set_style_text_color(glyph, COL_BG, LV_STATE_PRESSED);
    lv_obj_center(glyph);

    lv_obj_t *gear = lv_button_create(header);
    lv_obj_remove_style_all(gear);
    lv_obj_set_size(gear, HOME_BTN_SIZE, HOME_BTN_SIZE);
    lv_obj_align(gear, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(gear, COL_CARD, 0);
    lv_obj_set_style_bg_opa(gear, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(gear, COL_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_radius(gear, 14, 0);
    lv_obj_add_event_cb(gear, settings_event, LV_EVENT_CLICKED, NULL);

    lv_obj_t *gear_glyph = make_label(gear, LV_SYMBOL_SETTINGS, &lv_font_montserrat_24, COL_ACCENT);
    lv_obj_set_style_text_color(gear_glyph, COL_BG, LV_STATE_PRESSED);
    lv_obj_center(gear_glyph);

    /* Title and subtitle live in a content-sized flex column so the pair is
     * centred against the home button as one block. Aligning each label
     * individually would need hand-tuned offsets that break whenever a font
     * size changes. */
    lv_obj_t *text_group = lv_obj_create(header);
    lv_obj_remove_style_all(text_group);
    lv_obj_set_size(text_group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(text_group, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(text_group, 2, 0);
    lv_obj_clear_flag(text_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(text_group, LV_ALIGN_LEFT_MID, HEADER_TEXT_X, 0);

    *title_out = make_label(text_group, "", &lv_font_montserrat_24, COL_TEXT);
    *sub_out = make_label(text_group, "", &lv_font_montserrat_16, COL_MUTED);

    return header;
}

/* ------------------------------------------------------------ today page --- */

static void build_today(lv_obj_t *page)
{
    lv_obj_t *header = make_header(page, &lbl_place, &lbl_subtitle);
    lbl_status = make_label(header, "Starting up", &lv_font_montserrat_16, COL_MUTED);
    lv_obj_align(lbl_status, LV_ALIGN_RIGHT_MID, -HEADER_RIGHT_X, 0);

    /* Current conditions card */
    lv_obj_t *now = make_card(page, COL_CARD);
    lv_obj_set_size(now, 380, 240);
    lv_obj_set_pos(now, 0, HEADER_H + GAP);

    lbl_temp = make_label(now, "--" "°", &lv_font_montserrat_48, COL_TEXT);
    lv_obj_align(lbl_temp, LV_ALIGN_TOP_LEFT, 24, 20);

    glyph_condition = make_glyph(now, &weather_font_56);
    lv_obj_align(glyph_condition, LV_ALIGN_TOP_RIGHT, -24, 16);

    lbl_condition = make_label(now, "Waiting for data", &lv_font_montserrat_20, COL_TEXT);
    lv_obj_align(lbl_condition, LV_ALIGN_LEFT_MID, 24, 20);

    lbl_high_low = make_label(now, "", &lv_font_montserrat_18, COL_MUTED);
    lv_obj_align(lbl_high_low, LV_ALIGN_BOTTOM_LEFT, 24, -20);

    /* 2x2 stat grid */
    static const char *tile_names[4] = { "HUMIDITY", "WIND", "RAIN CHANCE", "FEELS LIKE" };
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *tile = make_card(page, COL_CARD);
        lv_obj_set_size(tile, 282, 112);
        lv_obj_set_pos(tile, 396 + (i % 2) * 298, HEADER_H + GAP + (i / 2) * 128);

        lv_obj_t *name = make_label(tile, tile_names[i], &lv_font_montserrat_14, COL_MUTED);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 20, -18);

        tile_values[i] = make_label(tile, "--", &lv_font_montserrat_24,
                                    i == 2 ? COL_RAIN : COL_TEXT);
        lv_obj_align(tile_values[i], LV_ALIGN_LEFT_MID, 20, 14);
    }

    /* 5-day strip */
    for (int i = 0; i < WEATHER_DAILY_MAX; ++i) {
        lv_obj_t *card = make_card(page, COL_CARD);
        lv_obj_set_size(card, 182, 132);
        lv_obj_set_pos(card, i * 198, HEADER_H + GAP + 240 + GAP);

        day_labels[i] = make_label(card, "---", &lv_font_montserrat_16, COL_MUTED);
        lv_obj_align(day_labels[i], LV_ALIGN_TOP_MID, 0, 14);

        day_glyphs[i] = make_glyph(card, &weather_font_32);
        lv_obj_align(day_glyphs[i], LV_ALIGN_CENTER, 0, 0);

        day_temps[i] = make_label(card, "", &lv_font_montserrat_18, COL_TEXT);
        lv_obj_align(day_temps[i], LV_ALIGN_BOTTOM_MID, 0, -14);
    }
}

/* ----------------------------------------------------------- hourly page --- */

static void build_hourly(lv_obj_t *page)
{
    lv_obj_t *title = NULL;
    make_header(page, &title, &lbl_hourly_sub);
    lv_label_set_text(title, "Next 12 hours");

    lv_obj_t *card = make_card(page, COL_CARD);
    lv_obj_set_size(card, PAGE_W, 224);
    lv_obj_set_pos(card, 0, HEADER_H + GAP);

    chart = lv_chart_create(card);
    lv_obj_set_size(chart, PAGE_W - 48, 170);
    lv_obj_align(chart, LV_ALIGN_TOP_MID, 0, 20);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, WEATHER_HOURLY_MAX);
    lv_chart_set_div_line_count(chart, 4, 0);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_line_color(chart, lv_color_hex(0x24405C), LV_PART_MAIN);
    lv_obj_set_style_size(chart, 8, 8, LV_PART_INDICATOR);
    chart_series = lv_chart_add_series(chart, COL_WARM, LV_CHART_AXIS_PRIMARY_Y);

    /* Hour cards */
    for (int i = 0; i < 8; ++i) {
        lv_obj_t *hour_card = make_card(page, COL_CARD);
        lv_obj_set_size(hour_card, 118, 148);
        lv_obj_set_pos(hour_card, i * 124, HEADER_H + GAP + 224 + GAP);

        hour_labels[i] = make_label(hour_card, "--", &lv_font_montserrat_16, COL_MUTED);
        lv_obj_align(hour_labels[i], LV_ALIGN_TOP_MID, 0, 14);

        hour_temps[i] = make_label(hour_card, "--", &lv_font_montserrat_22, COL_TEXT);
        lv_obj_align(hour_temps[i], LV_ALIGN_CENTER, 0, 0);

        hour_rain[i] = make_label(hour_card, "", &lv_font_montserrat_14, COL_RAIN);
        lv_obj_align(hour_rain[i], LV_ALIGN_BOTTOM_MID, 0, -14);
    }
}

/* ------------------------------------------------------------ radar page --- */

/* The radar page is full-bleed: the image is requested at exactly the size of
 * everything above the nav bar, and the transparent PNG lets the dark screen
 * show through. Controls float on top instead of taking a header row. */
static void build_radar(lv_obj_t *page)
{
    img_radar = lv_image_create(page);
    lv_obj_set_size(img_radar, RADAR_W, RADAR_H);
    lv_obj_set_pos(img_radar, 0, 0);
    lv_obj_add_flag(img_radar, LV_OBJ_FLAG_HIDDEN);

    /* Marker sits at the exact centre, which is the saved lat/lon. */
    lv_obj_t *crosshair = lv_obj_create(page);
    lv_obj_remove_style_all(crosshair);
    lv_obj_set_size(crosshair, 16, 16);
    lv_obj_align(crosshair, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(crosshair, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(crosshair, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(crosshair, lv_color_hex(0xE12D39), 0);
    lv_obj_set_style_border_width(crosshair, 3, 0);

    /* Floating home and gear, so the map keeps the full height. */
    lv_obj_t *home = lv_button_create(page);
    lv_obj_remove_style_all(home);
    lv_obj_set_size(home, HOME_BTN_SIZE, HOME_BTN_SIZE);
    lv_obj_align(home, LV_ALIGN_TOP_LEFT, PAD, PAD);
    lv_obj_set_style_bg_color(home, COL_CARD, 0);
    lv_obj_set_style_bg_opa(home, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(home, COL_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_radius(home, 14, 0);
    lv_obj_add_event_cb(home, home_event, LV_EVENT_CLICKED, NULL);
    lv_obj_center(make_label(home, LV_SYMBOL_HOME, &lv_font_montserrat_24, COL_ACCENT));

    lv_obj_t *gear = lv_button_create(page);
    lv_obj_remove_style_all(gear);
    lv_obj_set_size(gear, HOME_BTN_SIZE, HOME_BTN_SIZE);
    lv_obj_align(gear, LV_ALIGN_TOP_RIGHT, -PAD, PAD);
    lv_obj_set_style_bg_color(gear, COL_CARD, 0);
    lv_obj_set_style_bg_opa(gear, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(gear, COL_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_radius(gear, 14, 0);
    lv_obj_add_event_cb(gear, settings_event, LV_EVENT_CLICKED, NULL);
    lv_obj_center(make_label(gear, LV_SYMBOL_SETTINGS, &lv_font_montserrat_24, COL_ACCENT));

    /* Caption strip along the bottom, over the map. */
    lv_obj_t *strip = make_card(page, COL_CARD);
    lv_obj_set_size(strip, LV_SIZE_CONTENT, 40);
    lv_obj_align(strip, LV_ALIGN_BOTTOM_LEFT, PAD, -PAD);
    lv_obj_set_style_bg_opa(strip, LV_OPA_70, 0);
    lv_obj_set_style_pad_hor(strip, 16, 0);

    lbl_radar_sub = make_label(strip, "", &lv_font_montserrat_16, COL_TEXT);
    lv_obj_center(lbl_radar_sub);

    lbl_radar_status = make_label(page, "Waiting for radar", &lv_font_montserrat_20, COL_MUTED);
    lv_obj_align(lbl_radar_status, LV_ALIGN_CENTER, 0, 40);
}

/* ------------------------------------------------------- settings pages --- */

/* The settings pages share a header of their own: home button, then a pair of
 * tabs, because both sections already use the full height below it. */
static void settings_tab_event(lv_event_t *event)
{
    show_page((ui_page_t)(intptr_t)lv_event_get_user_data(event));
}

static void make_settings_header(lv_obj_t *page, int slot)
{
    lv_obj_t *header = lv_obj_create(page);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, PAGE_W, HEADER_H);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *home = lv_button_create(header);
    lv_obj_remove_style_all(home);
    lv_obj_set_size(home, HOME_BTN_SIZE, HOME_BTN_SIZE);
    lv_obj_align(home, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(home, COL_CARD, 0);
    lv_obj_set_style_bg_opa(home, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(home, COL_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_radius(home, 14, 0);
    lv_obj_add_event_cb(home, home_event, LV_EVENT_CLICKED, NULL);
    lv_obj_center(make_label(home, LV_SYMBOL_HOME, &lv_font_montserrat_24, COL_ACCENT));

    static const char *tab_names[2] = { "Location", "Wi-Fi" };
    static const ui_page_t tab_pages[2] = { PAGE_LOCATION, PAGE_WIFI };
    for (int i = 0; i < 2; ++i) {
        lv_obj_t *tab = lv_button_create(header);
        lv_obj_remove_style_all(tab);
        lv_obj_set_size(tab, 160, 48);
        lv_obj_align(tab, LV_ALIGN_LEFT_MID, HEADER_TEXT_X + i * 172, 0);
        lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(tab, 12, 0);
        lv_obj_add_event_cb(tab, settings_tab_event, LV_EVENT_CLICKED,
                            (void *)(intptr_t)tab_pages[i]);
        lv_obj_center(make_label(tab, tab_names[i], &lv_font_montserrat_18, COL_TEXT));
        settings_tabs[slot][i] = tab;
    }
}

/* Highlight whichever settings tab matches the visible page. */
static void style_settings_tabs(ui_page_t page)
{
    for (int slot = 0; slot < 2; ++slot) {
        for (int i = 0; i < 2; ++i) {
            lv_obj_t *tab = settings_tabs[slot][i];
            if (tab == NULL) {
                continue;
            }
            bool selected = (i == 0 && page == PAGE_LOCATION) ||
                            (i == 1 && page == PAGE_WIFI);
            lv_obj_set_style_bg_color(tab, selected ? COL_ACCENT : COL_CARD, 0);
            lv_obj_set_style_text_color(lv_obj_get_child(tab, 0),
                                        selected ? COL_BG : COL_TEXT, 0);
        }
    }
}

/* --------------------------------------------------------- location page --- */

static void refresh_zip_view(void)
{
    char spaced[ZIP_LEN * 2 + 1] = { 0 };
    size_t out = 0;
    for (size_t i = 0; i < strlen(zip_entry); ++i) {
        spaced[out++] = zip_entry[i];
        if (i + 1 < strlen(zip_entry)) {
            spaced[out++] = ' ';
        }
    }
    lv_label_set_text(lbl_zip, spaced);

    bool complete = strlen(zip_entry) == ZIP_LEN;
    lv_obj_set_style_bg_color(btn_save, complete ? COL_ACCENT : COL_CARD_ALT, 0);
    lv_obj_set_style_text_color(lbl_save, complete ? COL_BG : COL_DISABLED, 0);

    if (complete) {
        lv_label_set_text(lbl_zip_hint, "Ready to save");
    } else {
        lv_label_set_text_fmt(lbl_zip_hint, "%d more digits", ZIP_LEN - (int)strlen(zip_entry));
    }
    lv_obj_set_style_text_color(lbl_zip_hint, COL_MUTED, 0);
}

static void key_event(lv_event_t *event)
{
    const char *key = (const char *)lv_event_get_user_data(event);
    size_t length = strlen(zip_entry);

    if (strcmp(key, "<") == 0) {
        if (length > 0) {
            zip_entry[length - 1] = '\0';
        }
    } else if (strcmp(key, "C") == 0) {
        zip_entry[0] = '\0';
    } else if (length < ZIP_LEN) {
        zip_entry[length] = key[0];
        zip_entry[length + 1] = '\0';
    }
    refresh_zip_view();
}

static void save_event(lv_event_t *event)
{
    LV_UNUSED(event);
    if (strlen(zip_entry) != ZIP_LEN || callbacks.on_zip_submit == NULL) {
        return;
    }
    lv_label_set_text(lbl_zip_hint, "Looking up ZIP...");
    lv_obj_set_style_text_color(lbl_zip_hint, COL_ACCENT, 0);
    callbacks.on_zip_submit(zip_entry);
}

static void build_location(lv_obj_t *page)
{
    make_settings_header(page, 0);

    lbl_current_loc = make_label(page, "", &lv_font_montserrat_16, COL_MUTED);
    lv_obj_align(lbl_current_loc, LV_ALIGN_TOP_RIGHT, 0, 22);

    /* ZIP field */
    lv_obj_t *field_label = make_label(page, "ZIP CODE", &lv_font_montserrat_14, COL_MUTED);
    lv_obj_set_pos(field_label, 0, HEADER_H + GAP);

    lv_obj_t *field = make_card(page, COL_CARD);
    lv_obj_set_size(field, 452, 96);
    lv_obj_set_pos(field, 0, HEADER_H + GAP + 28);
    lv_obj_set_style_border_color(field, lv_color_hex(0x2A4A6A), 0);
    lv_obj_set_style_border_width(field, 2, 0);

    lbl_zip = make_label(field, "", &lv_font_montserrat_44, COL_TEXT);
    lv_obj_align(lbl_zip, LV_ALIGN_LEFT_MID, 24, 0);

    lbl_zip_hint = make_label(page, "5 more digits", &lv_font_montserrat_16, COL_MUTED);
    lv_obj_set_pos(lbl_zip_hint, 0, HEADER_H + GAP + 140);

    /* Save button */
    btn_save = lv_button_create(page);
    lv_obj_remove_style_all(btn_save);
    lv_obj_set_size(btn_save, 452, 72);
    lv_obj_set_pos(btn_save, 0, HEADER_H + GAP + 316);
    lv_obj_set_style_bg_color(btn_save, COL_CARD_ALT, 0);
    lv_obj_set_style_bg_opa(btn_save, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_save, 16, 0);
    lv_obj_add_event_cb(btn_save, save_event, LV_EVENT_CLICKED, NULL);
    lbl_save = make_label(btn_save, "Save location", &lv_font_montserrat_20, COL_DISABLED);
    lv_obj_center(lbl_save);

    /* Keypad: 3 x 4 grid of 160 x 88 keys */
    static const char *keys[12] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "C", "0", "<" };
    for (int i = 0; i < 12; ++i) {
        lv_obj_t *key = lv_button_create(page);
        lv_obj_remove_style_all(key);
        lv_obj_set_size(key, 160, 88);
        lv_obj_set_pos(key, 468 + (i % 3) * 172, HEADER_H + GAP + (i / 3) * 100);
        bool is_action = (i == 9 || i == 11);
        lv_obj_set_style_bg_color(key, is_action ? COL_CARD_ALT : COL_CARD, 0);
        lv_obj_set_style_bg_opa(key, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(key, 14, 0);
        lv_obj_add_event_cb(key, key_event, LV_EVENT_CLICKED, (void *)keys[i]);

        const char *face = (i == 9) ? "Clear" : (i == 11) ? LV_SYMBOL_BACKSPACE : keys[i];
        lv_obj_t *label = make_label(key, face,
                                     is_action ? &lv_font_montserrat_18 : &lv_font_montserrat_28,
                                     is_action ? COL_MUTED : COL_TEXT);
        lv_obj_center(label);
    }
}

/* ------------------------------------------------------------- wifi page --- */

static void modal_close(lv_event_t *event)
{
    LV_UNUSED(event);
    lv_obj_add_flag(modal, LV_OBJ_FLAG_HIDDEN);
}

static void modal_connect(lv_event_t *event)
{
    LV_UNUSED(event);
    if (callbacks.on_wifi_submit != NULL) {
        callbacks.on_wifi_submit(pending_ssid, lv_textarea_get_text(modal_field));
    }
    lv_obj_add_flag(modal, LV_OBJ_FLAG_HIDDEN);
}

/* Full-screen so the keyboard has room; hidden until a network is chosen. */
static void build_modal(lv_obj_t *screen)
{
    modal = lv_obj_create(screen);
    lv_obj_remove_style_all(modal);
    lv_obj_set_size(modal, 1024, 600);
    lv_obj_set_pos(modal, 0, 0);
    lv_obj_set_style_bg_color(modal, COL_BG, 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_COVER, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(modal, LV_OBJ_FLAG_HIDDEN);

    modal_title = make_label(modal, "", &lv_font_montserrat_24, COL_TEXT);
    lv_obj_align(modal_title, LV_ALIGN_TOP_LEFT, PAD, PAD + 8);

    lv_obj_t *cancel = lv_button_create(modal);
    lv_obj_remove_style_all(cancel);
    lv_obj_set_size(cancel, 150, 56);
    lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, -PAD - 166, PAD);
    lv_obj_set_style_bg_color(cancel, COL_CARD, 0);
    lv_obj_set_style_bg_opa(cancel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cancel, 14, 0);
    lv_obj_add_event_cb(cancel, modal_close, LV_EVENT_CLICKED, NULL);
    lv_obj_center(make_label(cancel, "Cancel", &lv_font_montserrat_18, COL_TEXT));

    lv_obj_t *connect = lv_button_create(modal);
    lv_obj_remove_style_all(connect);
    lv_obj_set_size(connect, 150, 56);
    lv_obj_align(connect, LV_ALIGN_TOP_RIGHT, -PAD, PAD);
    lv_obj_set_style_bg_color(connect, COL_ACCENT, 0);
    lv_obj_set_style_bg_opa(connect, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(connect, 14, 0);
    lv_obj_add_event_cb(connect, modal_connect, LV_EVENT_CLICKED, NULL);
    lv_obj_center(make_label(connect, "Connect", &lv_font_montserrat_18, COL_BG));

    modal_field = lv_textarea_create(modal);
    lv_obj_set_size(modal_field, 1024 - 2 * PAD, 64);
    lv_obj_align(modal_field, LV_ALIGN_TOP_LEFT, PAD, 96);
    lv_textarea_set_one_line(modal_field, true);
    lv_textarea_set_password_mode(modal_field, true);
    lv_textarea_set_placeholder_text(modal_field, "Network password");
    lv_obj_set_style_text_font(modal_field, &lv_font_montserrat_24, 0);
    lv_obj_set_style_bg_color(modal_field, COL_CARD, 0);
    lv_obj_set_style_text_color(modal_field, COL_TEXT, 0);
    lv_obj_set_style_border_color(modal_field, COL_ACCENT, 0);

    lv_obj_t *keyboard = lv_keyboard_create(modal);
    lv_obj_set_size(keyboard, 1024, 360);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(keyboard, modal_field);
}

static void wifi_pick_event(lv_event_t *event)
{
    const char *ssid = (const char *)lv_event_get_user_data(event);
    snprintf(pending_ssid, sizeof(pending_ssid), "%s", ssid);
    lv_label_set_text_fmt(modal_title, "Connect to %s", pending_ssid);
    lv_textarea_set_text(modal_field, "");
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(modal);
}

static void wifi_scan_event(lv_event_t *event)
{
    LV_UNUSED(event);
    lv_label_set_text(lbl_wifi_status, "Scanning...");
    lv_obj_set_style_text_color(lbl_wifi_status, COL_ACCENT, 0);
    if (callbacks.on_wifi_scan != NULL) {
        callbacks.on_wifi_scan();
    }
}

static void build_wifi(lv_obj_t *page)
{
    make_settings_header(page, 1);

    lv_obj_t *scan = lv_button_create(page);
    lv_obj_remove_style_all(scan);
    lv_obj_set_size(scan, 180, 56);
    lv_obj_align(scan, LV_ALIGN_TOP_RIGHT, 0, 4);
    lv_obj_set_style_bg_color(scan, COL_ACCENT, 0);
    lv_obj_set_style_bg_opa(scan, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(scan, 14, 0);
    lv_obj_add_event_cb(scan, wifi_scan_event, LV_EVENT_CLICKED, NULL);
    lv_obj_center(make_label(scan, "Scan", &lv_font_montserrat_18, COL_BG));

    lbl_wifi_current = make_label(page, LV_SYMBOL_WIFI "  Not connected",
                                  &lv_font_montserrat_18, COL_MUTED);
    lv_obj_set_pos(lbl_wifi_current, 0, HEADER_H + GAP);

    wifi_list = lv_obj_create(page);
    lv_obj_remove_style_all(wifi_list);
    lv_obj_set_size(wifi_list, PAGE_W, PAGE_H - HEADER_H - GAP - 76);
    lv_obj_set_pos(wifi_list, 0, HEADER_H + GAP + 36);
    lv_obj_set_flex_flow(wifi_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(wifi_list, 8, 0);

    lbl_wifi_status = make_label(page, "Tap Scan to find networks",
                                 &lv_font_montserrat_16, COL_MUTED);
    lv_obj_align(lbl_wifi_status, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

/* ------------------------------------------------------------------ api --- */

void ui_create(const ui_callbacks_t *cb)
{
    callbacks = *cb;
    zip_entry[0] = '\0';

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, COL_BG, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < PAGE_COUNT; ++i) {
        pages[i] = lv_obj_create(screen);
        lv_obj_remove_style_all(pages[i]);
        if (i == PAGE_RADAR) {
            /* Full-bleed: no padding, spans everything above the nav bar. */
            lv_obj_set_size(pages[i], RADAR_W, RADAR_H);
            lv_obj_set_pos(pages[i], 0, 0);
        } else {
            lv_obj_set_size(pages[i], PAGE_W, PAGE_H);
            lv_obj_set_pos(pages[i], PAD, PAD);
        }
        lv_obj_clear_flag(pages[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    build_today(pages[PAGE_TODAY]);
    build_hourly(pages[PAGE_HOURLY]);
    build_radar(pages[PAGE_RADAR]);
    build_location(pages[PAGE_LOCATION]);
    build_wifi(pages[PAGE_WIFI]);
    build_modal(screen);

    /* Bottom navigation */
    static const char *nav_names[NAV_COUNT] = { "Today", "Hourly", "Radar" };
    const int nav_w = (PAGE_W - (NAV_COUNT - 1) * GAP) / NAV_COUNT;
    for (int i = 0; i < NAV_COUNT; ++i) {
        nav_buttons[i] = lv_button_create(screen);
        lv_obj_remove_style_all(nav_buttons[i]);
        lv_obj_set_size(nav_buttons[i], nav_w, NAV_H);
        lv_obj_set_pos(nav_buttons[i], PAD + i * (nav_w + GAP), PAD + PAGE_H + GAP);
        lv_obj_set_style_bg_color(nav_buttons[i], COL_CARD, 0);
        lv_obj_set_style_bg_opa(nav_buttons[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(nav_buttons[i], 14, 0);
        lv_obj_add_event_cb(nav_buttons[i], nav_event, LV_EVENT_CLICKED,
                            (void *)(intptr_t)nav_pages[i]);

        lv_obj_t *label = make_label(nav_buttons[i], nav_names[i], &lv_font_montserrat_18, COL_TEXT);
        lv_obj_center(label);
    }

    show_page(PAGE_TODAY);
    refresh_zip_view();
    ESP_LOGI(TAG, "UI built: 3 pages at %dx%d", PAGE_W, PAGE_H);
}

void ui_set_status(const char *text)
{
    if (lvgl_port_lock(1000)) {
        lv_label_set_text(lbl_status, text);
        lv_obj_align(lbl_status, LV_ALIGN_RIGHT_MID, -HEADER_RIGHT_X, 0);
        lvgl_port_unlock();
    }
}

void ui_set_location(const char *place, const char *zip)
{
    if (lvgl_port_lock(1000)) {
        lv_label_set_text(lbl_place, place);
        lv_label_set_text(lbl_hourly_sub, place);

        /* ZIP sits under the place name on Today. Hidden rather than blanked
         * when unset (first boot uses the Kconfig coordinates and has no ZIP),
         * because a hidden item drops out of the flex column entirely and lets
         * the place name centre on its own against the home button. */
        if (zip && zip[0]) {
            lv_label_set_text(lbl_subtitle, zip);
            lv_obj_clear_flag(lbl_subtitle, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lbl_subtitle, LV_OBJ_FLAG_HIDDEN);
        }

        if (zip && zip[0]) {
            /* LVGL's built-in Montserrat carries only U+00B0, U+2022 and the
             * FontAwesome symbols outside ASCII -- U+00B7 MIDDLE DOT is not in
             * the font and renders as a missing-glyph box. */
            lv_label_set_text_fmt(lbl_current_loc, "Currently %s " LV_SYMBOL_BULLET " %s",
                                  zip, place);
        } else {
            lv_label_set_text(lbl_current_loc, place);
        }
        lv_obj_align(lbl_current_loc, LV_ALIGN_RIGHT_MID, -HEADER_RIGHT_X, 0);
        lvgl_port_unlock();
    }
}

void ui_set_radar(const uint8_t *png, size_t png_len, const char *caption)
{
    if (!lvgl_port_lock(1000)) {
        return;
    }
    if (png == NULL || png_len == 0) {
        lv_obj_add_flag(img_radar, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lbl_radar_status, "Radar unavailable");
    } else {
        /* LVGL caches decoded images against the source pointer. We reuse one
         * descriptor with fresh bytes each refresh, so the stale entry has to
         * go or the old frame keeps being drawn. */
        lv_image_cache_drop(&radar_dsc);

        radar_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        radar_dsc.header.cf = LV_COLOR_FORMAT_RAW;   /* lodepng sniffs the real format */
        radar_dsc.header.w = RADAR_W;
        radar_dsc.header.h = RADAR_H;
        radar_dsc.data = png;
        radar_dsc.data_size = png_len;

        lv_image_set_src(img_radar, &radar_dsc);
        lv_obj_clear_flag(img_radar, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lbl_radar_status, "");
    }
    if (caption != NULL) {
        lv_label_set_text(lbl_radar_sub, caption);
    }
    lvgl_port_unlock();
}

/* Each row owns a copy of its SSID so the caller's array can go away. */
void ui_set_wifi_list(const wifi_ap_t *networks, int count)
{
    if (!lvgl_port_lock(1000)) {
        return;
    }
    lv_obj_clean(wifi_list);

    for (int i = 0; i < count; ++i) {
        static char ssid_store[WIFI_SCAN_MAX][WIFI_SSID_LEN];
        if (i >= WIFI_SCAN_MAX) {
            break;
        }
        snprintf(ssid_store[i], WIFI_SSID_LEN, "%s", networks[i].ssid);

        lv_obj_t *row = lv_button_create(wifi_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, PAGE_W - 8, 56);
        lv_obj_set_style_bg_color(row, COL_CARD, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, COL_ACCENT, LV_STATE_PRESSED);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_add_event_cb(row, wifi_pick_event, LV_EVENT_CLICKED, ssid_store[i]);

        /* Mark the network we are actually joined to. */
        bool is_current = current_connected &&
                          strcmp(ssid_store[i], current_ssid) == 0;
        if (is_current) {
            lv_obj_set_style_border_color(row, COL_ACCENT, 0);
            lv_obj_set_style_border_width(row, 2, 0);
        }

        lv_obj_t *name = make_label(row, ssid_store[i], &lv_font_montserrat_20,
                                    is_current ? COL_ACCENT : COL_TEXT);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 16, 0);

        if (is_current) {
            lv_obj_t *tick = make_label(row, LV_SYMBOL_OK, &lv_font_montserrat_18, COL_ACCENT);
            lv_obj_align(tick, LV_ALIGN_LEFT_MID, 16 + lv_obj_get_width(name) + 12, 0);
        }

        lv_obj_t *meta = make_label(row, networks[i].open ? "open" : "", 
                                    &lv_font_montserrat_16, COL_MUTED);
        lv_obj_align(meta, LV_ALIGN_RIGHT_MID, -96, 0);

        /* RSSI as dBm; -50 is excellent, -80 is marginal. */
        lv_obj_t *signal = make_label(row, "", &lv_font_montserrat_16, COL_MUTED);
        lv_label_set_text_fmt(signal, "%d dBm", networks[i].rssi);
        lv_obj_align(signal, LV_ALIGN_RIGHT_MID, -16, 0);
    }

    if (count == 0) {
        lv_obj_t *empty = make_label(wifi_list, "No networks found",
                                     &lv_font_montserrat_18, COL_MUTED);
        lv_obj_set_style_pad_all(empty, 16, 0);
    }
    lvgl_port_unlock();
}

void ui_set_wifi_status(const char *text, bool is_error)
{
    if (!lvgl_port_lock(1000)) {
        return;
    }
    lv_label_set_text(lbl_wifi_status, text);
    lv_obj_set_style_text_color(lbl_wifi_status,
                                is_error ? lv_color_hex(0xE12D39) : COL_MUTED, 0);
    lv_obj_align(lbl_wifi_status, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lvgl_port_unlock();
}

void ui_set_wifi_current(const char *ssid, bool connected)
{
    if (!lvgl_port_lock(1000)) {
        return;
    }
    current_connected = connected;
    snprintf(current_ssid, sizeof(current_ssid), "%s", ssid ? ssid : "");

    if (connected && current_ssid[0] != '\0') {
        lv_label_set_text_fmt(lbl_wifi_current, LV_SYMBOL_WIFI "  Connected to %s",
                              current_ssid);
        lv_obj_set_style_text_color(lbl_wifi_current, COL_ACCENT, 0);
    } else {
        lv_label_set_text(lbl_wifi_current, LV_SYMBOL_WIFI "  Not connected");
        lv_obj_set_style_text_color(lbl_wifi_current, COL_MUTED, 0);
    }
    lvgl_port_unlock();
}

/* Used at boot when no credentials are stored yet. */
void ui_show_wifi_settings(void)
{
    if (lvgl_port_lock(1000)) {
        show_page(PAGE_WIFI);
        lvgl_port_unlock();
    }
}

void ui_set_zip_hint(const char *text, bool is_error)
{
    if (lvgl_port_lock(1000)) {
        lv_label_set_text(lbl_zip_hint, text);
        lv_obj_set_style_text_color(lbl_zip_hint,
                                    is_error ? lv_color_hex(0xE12D39) : COL_ACCENT, 0);
        lvgl_port_unlock();
    }
}

void ui_set_weather(const weather_data_t *data)
{
    if (!data->valid || !lvgl_port_lock(1000)) {
        return;
    }

    const weather_current_t *now = &data->current;
    lv_label_set_text_fmt(lbl_temp, "%d°", whole(now->temperature));
    lv_label_set_text(lbl_condition, weather_describe(now->weather_code));
    set_glyph(glyph_condition, now->weather_code);
    lv_label_set_text_fmt(lbl_high_low, "%d° high   %d° low",
                          whole(now->today_high), whole(now->today_low));

    lv_label_set_text_fmt(tile_values[0], "%d%%", now->humidity);
    lv_label_set_text_fmt(tile_values[1], "%d mph", whole(now->wind_speed));
    lv_label_set_text_fmt(tile_values[2], "%d%%", now->today_rain_chance);
    lv_label_set_text_fmt(tile_values[3], "%d°", whole(now->feels_like));

    for (int i = 0; i < WEATHER_DAILY_MAX; ++i) {
        if (i < data->day_count) {
            const weather_day_t *day = &data->days[i];
            lv_label_set_text(day_labels[i], day->label);
            lv_label_set_text_fmt(day_temps[i], "%d° / %d°", whole(day->high), whole(day->low));
            set_glyph(day_glyphs[i], day->weather_code);
        } else {
            lv_label_set_text(day_labels[i], "--");
            lv_label_set_text(day_temps[i], "");
        }
    }

    /* Hourly chart: scale the y axis to the actual range so the line uses
     * the full height instead of hugging the top or bottom. */
    if (data->hour_count > 0) {
        float low = data->hours[0].temperature;
        float high = low;
        for (int i = 1; i < data->hour_count; ++i) {
            if (data->hours[i].temperature < low) low = data->hours[i].temperature;
            if (data->hours[i].temperature > high) high = data->hours[i].temperature;
        }
        if (high - low < 4.0f) {
            high = low + 4.0f;   // avoid a flat line filling the whole card
        }
        lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, (int32_t)low - 2, (int32_t)high + 2);
        lv_chart_set_point_count(chart, data->hour_count);
        for (int i = 0; i < data->hour_count; ++i) {
            lv_chart_set_value_by_id(chart, chart_series, i, (int32_t)data->hours[i].temperature);
        }
        lv_chart_refresh(chart);
    }

    for (int i = 0; i < 8; ++i) {
        if (i < data->hour_count) {
            const weather_hour_t *hour = &data->hours[i];
            lv_label_set_text(hour_labels[i], hour->label);
            lv_label_set_text_fmt(hour_temps[i], "%d°", whole(hour->temperature));
            lv_label_set_text_fmt(hour_rain[i], "%d%%", hour->rain_chance);
        } else {
            lv_label_set_text(hour_labels[i], "--");
            lv_label_set_text(hour_temps[i], "");
            lv_label_set_text(hour_rain[i], "");
        }
    }

    lvgl_port_unlock();
}
