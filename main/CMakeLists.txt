# Weather artwork lives in icons/ as LVGL font C files built with lv_font_conv.
# CONFIGURE_DEPENDS re-runs cmake when the set of files changes, so adding or
# regenerating artwork never needs a manual reconfigure. ESP-IDF evaluates this
# file in script mode first to gather requirements, and CONFIGURE_DEPENDS is
# rejected there, so it is only passed during the real configure pass.
if(CMAKE_SCRIPT_MODE_FILE)
    file(GLOB ICON_SRCS ${CMAKE_CURRENT_LIST_DIR}/icons/*.c)
else()
    file(GLOB ICON_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/icons/*.c)
endif()

idf_component_register(
    SRCS "main.c" "ui.c" "weather.c" "radar.c" "wifi_cfg.c" "backlight.c" ${ICON_SRCS}
    INCLUDE_DIRS "."
    REQUIRES esp_lcd_jd9165 esp_lcd_touch_gt911 esp_lvgl_port lvgl nvs_flash esp_wifi esp_wifi_remote esp_hosted esp_http_client mbedtls cjson
)
