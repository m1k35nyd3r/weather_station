# Weather Station

An ESP-IDF application for the Guiffion **JC1060P470C** — an ESP32-P4 with a
7" 1024x600 JD9165 MIPI-DSI display and a GT911 capacitive touch panel.

Five screens, driven by touch. The bottom bar has three tabs:

| Screen | Contents |
| --- | --- |
| **Today** | Current temperature and conditions, humidity / wind / rain chance / feels-like, and a 5-day strip. **Tap a day** to open its hour-by-hour page |
| **Next 12 Hours** | 12-hour temperature chart plus per-hour cards with rain probability |
| **Current Radar** | Full-bleed live precipitation radar centred on your location, with state outlines |
| **Day** | Opened from a Today day card: all 24 hours of that day as a temperature line over two rows of hour cards (temperature and rain chance). The home button returns to Today |
| **Settings** | Two tabs behind the gear icon: **Location** (ZIP keypad) and **Wi-Fi** (network scan and password entry). A **Theme** button at the right of its header switches between dark and light mode; the choice is saved on the device |

The header on Today, Next 12 Hours and Day has a **home** button on the left and,
on the right, a **refresh** button next to the **gear**. Refresh asks for an
immediate weather and radar update; otherwise the device refreshes on the
forecast's own 15-minute cadence.

Weather comes from [Open-Meteo](https://open-meteo.com/), radar from NOAA's
GeoServer, and ZIP-to-coordinates from [Zippopotam](https://www.zippopotam.us/).
None of them need an API key. The saved location persists in NVS across reboots.

---

## Before you start: use the right USB port

The board has **three** USB-C connectors and only one of them is useful for
development. Getting this wrong wastes a lot of time, because flashing appears
to work on two of them while the console stays silent.

| Connector | Enumerates as | Flash | Console |
| --- | --- | --- | --- |
| **"Full Speed USB"** | `/dev/ttyACM*` (12 Mbit) | yes | **yes — use this one** |
| "High speed USB" | `/dev/ttyACM*` via USB-OTG | yes | no, and the port disappears after every reset |
| "USB-TTL" | `/dev/ttyUSB*` (CH340C) | yes | UART0 only, not the app console |

To confirm you are on the right one:

```sh
python -m esptool --chip esp32p4 -p /dev/ttyACM0 chip_id | grep "USB mode"
# want: USB mode: USB-Serial/JTAG      (not USB-OTG)
```

---

## Toolchain: ESP-IDF 5.4.x, not 6.x

Two independent reasons this project does **not** build on ESP-IDF 6.1:

1. **Silicon revision.** This board's ESP32-P4 is rev **v1.0**. IDF 6.1 defaults
   to `ESP32P4_REV_MIN_301` (v3.1+), and per its own Kconfig, pre-3.0 and 3.0+
   silicon are mutually incompatible — a default 6.1 build hangs in the
   bootloader with a watchdog loop and no output. IDF 5.4 defaults to
   `ESP32P4_SELECTS_REV_LESS_V3`, which is correct for this hardware.
2. **ESP-Hosted version match.** See [Wi-Fi](#wi-fi), below.

Activate the toolchain with the EIM script, which carries its own Python
environment:

```sh
source ~/.espressif/tools/activate_idf_v5.4.4.sh
```

Do **not** use `$IDF_PATH/export.sh` for 5.4 if a 6.x install is also present —
it can pick up the wrong Python environment and fail with a missing
`espidf.constraints.v5.4.txt`, which makes a perfectly good install look broken.

---

## First-time setup

Nothing needs configuring before the first flash. Credentials are deliberately
**not** in the repository and `main/Kconfig.projbuild` ships blank defaults.

**Set everything from the device.** On first boot, with no stored credentials,
the Wi-Fi settings screen opens by itself:

1. Tap **Scan**, pick your network, type the password, tap **Connect**
2. Tap the **Location** tab and enter your ZIP code

Both are written to NVS on the device, so they survive reboots *and* reflashes,
and neither ever touches the build. Until a location is set the Today screen
simply says "Tap the gear to set your location" rather than fetching weather
for 0N 0E.

If you would rather bake defaults in for a bench setup, `idf.py menuconfig`
under **Weather Station** still has SSID, password and fallback coordinates.
Those land in `sdkconfig`, which is gitignored — but anything saved on the
device takes precedence.

---

## Build and flash

```sh
source ~/.espressif/tools/activate_idf_v5.4.4.sh
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

If `idf.py flash` fails with
`ImportError: cannot import name 'is_target_supported'`, a 6.x installation is
leaking onto `sys.path`. Either use a shell that has never sourced 6.x, or
flash with esptool directly:

```sh
cd build && python -m esptool --chip esp32p4 -p /dev/ttyACM0 -b 460800 \
    --before default_reset --after hard_reset write_flash @flash_args
```

> **After editing `sdkconfig.defaults`, delete `sdkconfig` and rebuild.**
> The defaults file is read *only* when `sdkconfig` is absent, so edits are
> otherwise silently ignored while the build still reports success.

---

## Wi-Fi

The ESP32-P4 has no radio. Wi-Fi runs on the board's **ESP32-C6** co-processor
over SDIO using ESP-Hosted, with the P4 as host.

The C6 ships with ESP-Hosted **slave firmware 0.0.6**, and ESP-Hosted requires
host and slave versions to match. `main/idf_component.yml` therefore pins:

```yaml
espressif/esp_hosted:
  version: "=0.0.6"
```

A newer host against the factory slave makes `esp_hosted_init()` block forever
in a global constructor — before `app_main` runs, so the display never even
lights up.

Reflashing the C6 instead is possible but needs hardware: its `TXD0`/`RXD0`
reach **only** the 2x3 `CN5` header, its native USB pins are marked no-connect,
the P4 has no electrical path to them, and the C6 ROM has no SDIO download
mode. A 3.3V USB-UART adapter wired to CN5 is the only route.

SDIO pins (set in `sdkconfig.defaults`): CMD 19, CLK 18, D0–D3 14–17,
co-processor reset GPIO 54.

Credentials are staged with `wifi_cfg_set_config()` *before* `esp_wifi_start()`,
so the `STA_START` event handler performs the single connect. Connecting
explicitly as well races it and logs an RPC "precondition not met" error.
The same applies to `esp_wifi_disconnect()` and `esp_wifi_sta_get_ap_info()`
when unassociated — both go over the hosted RPC and log on failure, which is
why `wifi_cfg_apply()` takes an explicit `disconnect_first`.

---

## Dark and light themes

Every colour is read from a runtime palette in `ui.c` (`COL_*`). The Theme button
in the settings header calls `apply_theme()`, which remaps each styled colour in
the live widget tree from the old palette to the new one, so nothing is rebuilt
and no state (Wi-Fi list, selected day, status text) is lost. The choice is saved
in NVS (`ui_prefs`/`light`) and applied at boot. To add a colour, add a field to
`palette_t` and set it in both branches of `load_palette()`. The Wi-Fi password
keyboard uses LVGL's own (light) theme in both modes.

---

## Screen brightness

Brightness is set in `main/backlight.c` from the time of day and from touch. The
clock is kept in UTC, so local time comes from the `utc_offset_seconds` in each
forecast (which also tracks daylight saving).

| Condition | Brightness |
| --- | --- |
| Day, touched in the last 10 min | 100% |
| Day, idle | 12% |
| Night (after sunset, before sunrise), touched | 25% |
| Night, idle | 4% |
| **Quiet hours, 1:00 to 7:00 local** | **0% (off)** |
| Quiet hours, just touched | 25% for 60 s, then off again |

Quiet hours are `BACKLIGHT_QUIET_*` in `backlight.h`. They stay off until SNTP has
set the clock *and* a forecast has supplied the offset, so a failed fetch at boot
can never blank the screen in the daytime. The touch panel keeps scanning while
the screen is dark, so the first touch wakes it and is also delivered to the UI:
a tap that lands on a button will press it.

---

## Refresh behaviour

`refresh_now()` in `main.c` fetches the forecast and the radar **in parallel**:
they go to different hosts and do not depend on each other, so the radar runs on
a short-lived task while the weather request proceeds. Waiting for the radar is
bounded (90 s) so a stalled request cannot freeze weather updates, and a new
radar fetch will not start while an old one is still running.

Each refresh logs its timings, which is the first place to look when something
feels slow:

```text
radar: radar timing: open 369 ms, wait 228 ms, body 404 ms
weather_station: weather fetch took 1461 ms
```

`open` is DNS + TCP + TLS, `wait` is the server rendering the image, `body` is
the download. A refresh button press logs `manual refresh requested`.

---

## Board pin map

Confirmed from the schematics in `docs/JC1060P470C_I_W/5-Schematic/`:

| Function | GPIO | Notes |
| --- | --- | --- |
| LCD backlight | 23 | `LCD_PWM` → MP3202 boost driver EN |
| LCD reset | 27 | `GPIO27_LCD_RST` on the panel FPC |
| Touch I2C SDA / SCL | 7 / 8 | Shared with the on-board RTC |
| Touch INT / RST | 21 / 22 | |
| C6 SDIO | 14–19, 54 | See above |

---

## Project layout

```text
main/
  main.c        hardware bring-up, Wi-Fi, SNTP, refresh task
  ui.c/.h       all five screens, navigation, data binding
  weather.c/.h  Open-Meteo fetch/parse (current, daily, hourly, per-day hours),
                ZIP lookup, NVS persistence
  radar.c/.h    NOAA WMS radar fetch
  backlight.c/.h  PWM brightness: day/night/idle levels and 1-7am quiet hours
  wifi_cfg.c/.h network scan, credential storage in NVS
  icons/        Weather Icons fonts (generated) + WMO code mapping
tools/
  gen_weather_icons.py   regenerates the icon fonts
  weather_icons.txt      which glyphs to include
components/
  esp_lcd_jd9165          vendored panel driver
docs/                     vendor datasheets, schematics and demos
case_drawings/            enclosure models
```

### Weather icons

Glyphs come from [Erik Flowers' Weather Icons](https://github.com/erikflowers/weather-icons)
(SIL OFL 1.1), subset to only the codepoints used and converted to LVGL fonts.
To add one, put its `wi-` name in `tools/weather_icons.txt` and run:

```sh
./tools/gen_weather_icons.py            # regenerate fonts + wi_glyphs.h
./tools/gen_weather_icons.py --list rain   # browse the 584 available icons
```

Then map it to a WMO weather code in `main/icons/icons.c`. The generated files
carry a "do not edit" banner; only `icons.c` is hand-maintained.

---

## Things worth knowing before changing the code

- **`lv_label_set_text_fmt()` cannot use `%f`.** `CONFIG_LV_USE_FLOAT` is off,
  so LVGL's built-in sprintf compiles out float support and prints the literal
  character `f` instead — silently. Round to int and use `%d`, or format with
  the C library's `snprintf` first.
- **Only ASCII, `°` and `LV_SYMBOL_*` render.** LVGL's built-in Montserrat
  carries just 62 glyphs beyond ASCII. A middle dot or en dash draws as a box.
- **The panel needs its manufacturer init sequence.** The stock
  `esp_lcd_jd9165` default is only sleep-out plus display-on and never sets
  2-lane MIPI mode, which leaves the screen black. The full sequence is in
  `main.c`, transcribed from the device tree in `docs/.../4-Driver_IC_Data_Sheet/`.
- **The LVGL draw buffer must be DMA-capable internal RAM**, not PSRAM, or the
  image glitches and decays.
- **Touch needs `driver_data` and `LV_INDEV_MODE_TIMER`.** Without the former
  the GT911 is never reset; without the latter `esp_lvgl_port` waits on an
  interrupt that never arrives. Either one alone looks like "touch is dead".
- Three bring-up switches in `main.c`, all off: `DISPLAY_TEST_PATTERN`,
  `TOUCH_DEBUG`, `LVGL_INPUT_DEBUG`. Do not ship `TOUCH_DEBUG` enabled — it and
  LVGL steal touch events from each other.

## Licences

Application code is this project's own. Weather Icons is SIL OFL 1.1.
`components/esp_lcd_jd9165` and everything under `docs/` are redistributed from
Espressif and Guiffion under their respective terms.
