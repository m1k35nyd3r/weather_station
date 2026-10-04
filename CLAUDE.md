# Weather Station

## Purpose

Display current weather for the configured location on a wall-mounted panel.

## Hardware

- Guiffion **JC1060P470C** board
- **ESP32-P4**, silicon revision **v1.0**
- 7" **1024x600** JD9165 MIPI-DSI display
- **GT911** capacitive touch
- **ESP32-C6** co-processor provides Wi-Fi over SDIO (the P4 has no radio)

## Screens

Three tabs along the bottom, plus settings behind a gear icon:

- **Today** — current conditions, stat tiles, 5-day strip; tapping a day opens **Day**
- **Next 12 Hours** — 12-hour temperature chart and per-hour cards
- **Current Radar** — full-bleed precipitation radar centred on the location
- **Day** — all 24 hours of one forecast day (not a tab; opened from Today)
- **Settings** — two tabs behind the gear: Location (ZIP keypad) and Wi-Fi (scan, password entry)

Page headers have home, refresh and gear buttons. The screen is off from
1:00 to 7:00 local unless touched (`main/backlight.c`).

## Build

ESP-IDF **5.4.x**, not 6.x — see `README.md` for both reasons (v1.0 silicon,
and the ESP-Hosted version pin). Activate with:

```sh
source ~/.espressif/tools/activate_idf_v5.4.4.sh
```

Flash over the **"Full Speed USB"** connector only; the other two USB-C ports
either have no console or drop off the bus on reset.

## Conventions

- Credentials live in NVS, set from the Wi-Fi settings screen on the device.
  `main/Kconfig.projbuild` ships blank defaults and is only a bench fallback;
  nothing secret belongs in a tracked file.
- After editing `sdkconfig.defaults`, delete `sdkconfig` and rebuild — the
  defaults file is ignored otherwise, and the build still reports success.
- `lv_label_set_text_fmt()` cannot take `%f` in this configuration; it prints
  a literal `f`. Round to int, or use `snprintf` first.
- Labels can only render ASCII, `°` and `LV_SYMBOL_*`. Anything else is a box.
- Generated files (`main/icons/weather_font_*.c`, `wi_glyphs.h`) carry a
  "do not edit" banner; change `tools/weather_icons.txt` and regenerate.
- The device clock is UTC. Local time comes from the forecast's
  `utc_offset_seconds` (`weather_data_t`), never from `TZ`.
- Weather and radar are fetched in parallel in `refresh_now()`; the radar wait
  is bounded and its job is static. Do not make it unbounded or stack-allocated.
- Verify changes reached the device, not just that the build succeeded:
  check the symbol in `build/config/sdkconfig.h` and the
  `Hash of data verified` lines from esptool.

`README.md` carries the full pin map, data sources and the hardware gotchas.
