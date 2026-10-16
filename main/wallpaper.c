/* Procedural weather wallpaper.
 *
 * Each scene is painted from a handful of primitives -- a vertical sky
 * gradient, soft glows and discs for the sun and moon, cloud banks built from
 * overlapping circles, and streaks or dots for rain and snow -- into a float
 * RGB scratch buffer, then dithered down to RGB565. Nothing is stored in
 * flash, and a theme switch simply re-renders with a different tint.
 *
 * Positions are in screen pixels and were placed against the Today layout:
 * the sun and moon sit in the open middle of the header band (y 12-76), where
 * no card covers them. */

#include "wallpaper.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define W WALLPAPER_W
#define H WALLPAPER_H

typedef struct {
    float r, g, b;
} rgb_t;

static rgb_t *px;        /* scratch, W*H, freed after each render */
static uint8_t *mask;    /* scratch for cloud coverage, W*H */
static uint32_t seed;

static rgb_t hex(uint32_t c)
{
    return (rgb_t){ ((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f };
}

static rgb_t mix(rgb_t a, rgb_t b, float t)
{
    return (rgb_t){ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t };
}

static rgb_t scale(rgb_t a, float k)
{
    return (rgb_t){ a.r * k, a.g * k, a.b * k };
}

/* xorshift32: deterministic, so a scene looks the same on every render. */
static float rnd(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return (seed & 0xFFFFFF) / (float)0x1000000;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void blend(int x, int y, rgb_t c, float a)
{
    if (x < 0 || x >= W || y < 0 || y >= H || a <= 0.0f) {
        return;
    }
    rgb_t *p = &px[y * W + x];
    *p = mix(*p, c, a > 1.0f ? 1.0f : a);
}

static void gradient(rgb_t top, rgb_t bottom)
{
    for (int y = 0; y < H; ++y) {
        rgb_t c = mix(top, bottom, (float)y / (H - 1));
        for (int x = 0; x < W; ++x) {
            px[y * W + x] = c;
        }
    }
}

/* Gaussian halo around a point; `radius` is where it falls to ~37%. */
static void glow(float cx, float cy, float radius, rgb_t c, float strength)
{
    int reach = (int)(radius * 3);
    for (int y = (int)cy - reach; y <= (int)cy + reach; ++y) {
        for (int x = (int)cx - reach; x <= (int)cx + reach; ++x) {
            float dx = (x - cx) / radius, dy = (y - cy) / radius;
            blend(x, y, c, strength * expf(-(dx * dx + dy * dy)));
        }
    }
}

/* Filled circle with a 1.5 px soft edge. */
static void disc(float cx, float cy, float r, rgb_t c, float a)
{
    for (int y = (int)(cy - r - 2); y <= (int)(cy + r + 2); ++y) {
        for (int x = (int)(cx - r - 2); x <= (int)(cx + r + 2); ++x) {
            float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
            blend(x, y, c, a * clampf((r - d) / 1.5f + 0.5f, 0.0f, 1.0f));
        }
    }
}

/* One cumulus: a fixed cluster of circles scaled by `s`, flattened at the
 * base, lit from above (lighter top, `shade` darker at the bottom). The
 * circles are merged in `mask` first so overlaps do not double the alpha. */
static void cloud(float cx, float cy, float s, rgb_t c, float a, float shade)
{
    static const float puffs[][3] = {   /* x, y, radius, in units of s */
        { -1.55f, 0.20f, 0.55f }, { -0.85f, -0.20f, 0.80f }, { 0.00f, -0.45f, 1.00f },
        { 0.85f, -0.15f, 0.82f }, { 1.60f, 0.20f, 0.58f }, { -0.30f, 0.15f, 0.75f },
        { 0.50f, 0.18f, 0.72f },
    };
    int x0 = (int)(cx - 2.3f * s), x1 = (int)(cx + 2.3f * s);
    int y0 = (int)(cy - 1.5f * s), y1 = (int)(cy + 0.5f * s);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= W) x1 = W - 1;
    if (y1 >= H) y1 = H - 1;
    if (x0 > x1 || y0 > y1) {
        return;
    }
    for (int y = y0; y <= y1; ++y) {
        memset(&mask[y * W + x0], 0, x1 - x0 + 1);
    }

    float base = cy + 0.42f * s;
    for (size_t i = 0; i < sizeof(puffs) / sizeof(puffs[0]); ++i) {
        float pcx = cx + puffs[i][0] * s, pcy = cy + puffs[i][1] * s, r = puffs[i][2] * s;
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                float d = sqrtf((x - pcx) * (x - pcx) + (y - pcy) * (y - pcy));
                float cover = clampf((r - d) / 2.0f + 0.5f, 0.0f, 1.0f);
                cover *= clampf((base - y) / 3.0f, 0.0f, 1.0f);   /* flat bottom */
                uint8_t m = (uint8_t)(cover * 255.0f);
                if (m > mask[y * W + x]) {
                    mask[y * W + x] = m;
                }
            }
        }
    }

    float top = cy - 1.45f * s;
    for (int y = y0; y <= y1; ++y) {
        rgb_t shaded = scale(c, 1.0f - shade * clampf((y - top) / (base - top), 0.0f, 1.0f));
        for (int x = x0; x <= x1; ++x) {
            uint8_t m = mask[y * W + x];
            if (m) {
                blend(x, y, shaded, a * m / 255.0f);
            }
        }
    }
}

/* A bank of overlapping clouds across the top of the sky, back rows darker. */
static void cloud_bank(int count, float y_min, float y_max, float s_min, float s_max,
                       rgb_t back, rgb_t front, float a)
{
    for (int i = 0; i < count; ++i) {
        float t = (float)i / (count - 1);
        float cx = -80.0f + rnd() * (W + 160.0f);
        float cy = y_min + rnd() * (y_max - y_min);
        float s = s_min + rnd() * (s_max - s_min);
        cloud(cx, cy, s, mix(back, front, t), a, 0.18f);
    }
}

/* Slanted streaks; `slant` is the x drift over one streak's length. */
static void rain(int count, float len, float slant, rgb_t c, float a)
{
    for (int i = 0; i < count; ++i) {
        float x = rnd() * (W + 60.0f), y = rnd() * (H + len) - len;
        for (int k = 0; k < (int)len; ++k) {
            float t = (float)k / len;
            float a_k = a * (0.3f + 0.7f * t);   /* fades in towards the head */
            int xi = (int)(x + slant * t), yi = (int)(y + k);
            blend(xi, yi, c, a_k);
            blend(xi + 1, yi, c, a_k * 0.4f);
        }
    }
}

static void snow(int count, rgb_t c)
{
    for (int i = 0; i < count; ++i) {
        float r = 1.2f + rnd() * 2.8f;
        disc(rnd() * W, rnd() * H, r, c, 0.55f + 0.4f * (r / 4.0f));
    }
}

/* Twinkles in the upper sky; brighter ones get a faint halo. */
static void stars(int count, float y_max)
{
    rgb_t white = hex(0xFFFFFF);
    for (int i = 0; i < count; ++i) {
        float x = rnd() * W, y = rnd() * y_max, b = rnd();
        blend((int)x, (int)y, white, 0.35f + 0.6f * b);
        if (b > 0.85f) {
            glow(x, y, 2.5f, white, 0.35f);
        }
    }
}

/* Crescent: a pale disc with a sky-coloured disc offset over it. */
static void moon(float cx, float cy, float r, rgb_t sky)
{
    glow(cx, cy, r * 2.2f, hex(0xC8D6F0), 0.18f);
    disc(cx, cy, r, hex(0xF2F0E2), 1.0f);
    disc(cx + r * 0.45f, cy - r * 0.25f, r * 0.92f, sky, 1.0f);
}

static void sun(float cx, float cy, float r)
{
    glow(cx, cy, r * 4.0f, hex(0xFFE7A8), 0.35f);
    glow(cx, cy, r * 1.6f, hex(0xFFF2C4), 0.55f);
    disc(cx, cy, r, hex(0xFFF6D8), 1.0f);
}

/* Soft horizontal haze bands that drift slightly in x. */
static void fog(rgb_t c, float strength)
{
    static const float bands[][2] = { { 120, 50 }, { 250, 60 }, { 380, 55 }, { 500, 70 } };
    for (int y = 0; y < H; ++y) {
        float band = 0.0f;
        for (size_t b = 0; b < sizeof(bands) / sizeof(bands[0]); ++b) {
            float d = (y - bands[b][0]) / bands[b][1];
            band += expf(-d * d);
        }
        for (int x = 0; x < W; ++x) {
            float wave = 0.75f + 0.25f * sinf(x / 95.0f + y / 40.0f);
            blend(x, y, c, strength * clampf(band, 0.0f, 1.0f) * wave);
        }
    }
}

/* Jagged bolt from a cloud base downwards, with a glow around it. */
static void lightning(float x, float y, float y_end)
{
    rgb_t core = hex(0xFFFFFF), halo = hex(0xC9D7FF);
    glow(x, y + 60, 110, halo, 0.22f);
    while (y < y_end) {
        float nx = x + (rnd() - 0.5f) * 46.0f, ny = y + 18.0f + rnd() * 22.0f;
        int steps = (int)(ny - y);
        for (int k = 0; k <= steps; ++k) {
            float t = (float)k / steps;
            float lx = x + (nx - x) * t, ly = y + (ny - y) * t;
            for (int w = -3; w <= 3; ++w) {
                float a = w == 0 ? 1.0f : (abs(w) == 1 ? 0.75f : 0.18f);
                blend((int)lx + w, (int)ly, abs(w) <= 1 ? core : halo, a);
            }
        }
        x = nx;
        y = ny;
    }
}

/* Sky colours per scene: day top, day bottom, night top, night bottom. */
static const uint32_t sky_colors[WALL_SKY_COUNT][4] = {
    [WALL_CLEAR]    = { 0x2B78D4, 0x9FD4F6, 0x0A1532, 0x26375F },
    [WALL_PARTLY]   = { 0x367DCB, 0xA6CDEA, 0x0F1A36, 0x2B395B },
    [WALL_OVERCAST] = { 0x6C7C8E, 0xAAB5C0, 0x1A202A, 0x38404C },
    [WALL_FOG]      = { 0x8E9AA6, 0xC8CED4, 0x262C34, 0x464C55 },
    [WALL_RAIN]     = { 0x46546A, 0x7C8999, 0x131922, 0x2C3540 },
    [WALL_SNOW]     = { 0x8798AB, 0xD4DCE5, 0x202A39, 0x465369 },
    [WALL_STORM]    = { 0x2A303C, 0x575E6E, 0x0D1016, 0x242933 },
};

wall_sky_t wallpaper_sky(int code)
{
    if (code <= 1) return WALL_CLEAR;
    if (code == 2) return WALL_PARTLY;
    if (code == 3) return WALL_OVERCAST;
    if (code <= 48) return WALL_FOG;
    if (code <= 67) return WALL_RAIN;
    if (code <= 77) return WALL_SNOW;
    if (code <= 82) return WALL_RAIN;
    if (code <= 86) return WALL_SNOW;
    return WALL_STORM;
}

static void paint(wall_sky_t sky, bool night)
{
    const uint32_t *sc = sky_colors[sky];
    rgb_t top = hex(sc[night ? 2 : 0]), bottom = hex(sc[night ? 3 : 1]);
    gradient(top, bottom);
    seed = 0x9E3779B9u ^ (uint32_t)(sky * 7919 + night);

    /* Cloud tones: lit white by day, blue-grey at night. */
    float lum = night ? 0.42f : 1.0f;
    rgb_t white = scale(hex(0xFFFFFF), lum);
    rgb_t grey = scale(hex(0xC9D0D8), lum);
    rgb_t dark = scale(hex(0x7C8592), lum);
    rgb_t sun_moon_sky = mix(top, bottom, 44.0f / H);   /* sky behind the moon */

    switch (sky) {
    case WALL_CLEAR:
        if (night) {
            stars(220, 360);
            moon(560, 44, 26, sun_moon_sky);
        } else {
            sun(560, 44, 28);
        }
        break;
    case WALL_PARTLY:
        if (night) {
            stars(140, 320);
            moon(560, 44, 24, sun_moon_sky);
        } else {
            sun(560, 44, 26);
        }
        cloud(300, 120, 48, white, 0.92f, 0.16f);
        cloud(680, 112, 40, white, 0.88f, 0.16f);
        cloud(880, 300, 58, white, 0.85f, 0.18f);
        cloud(150, 420, 52, white, 0.80f, 0.18f);
        cloud(620, 470, 44, white, 0.75f, 0.18f);
        break;
    case WALL_OVERCAST:
        cloud_bank(14, 20, 360, 55, 95, dark, grey, 0.9f);
        cloud_bank(8, 380, 560, 45, 75, dark, grey, 0.6f);
        break;
    case WALL_FOG:
        cloud_bank(8, 40, 300, 60, 90, grey, white, 0.35f);
        fog(scale(hex(0xE6EAEE), night ? 0.5f : 1.0f), 0.55f);
        break;
    case WALL_RAIN:
        cloud_bank(16, 0, 300, 55, 100, scale(dark, 0.8f), dark, 0.95f);
        rain(520, 30, -7, scale(hex(0xC8D4E2), night ? 0.6f : 1.0f), 0.32f);
        break;
    case WALL_SNOW:
        cloud_bank(12, 0, 260, 55, 95, grey, white, 0.85f);
        snow(320, scale(hex(0xFFFFFF), night ? 0.8f : 1.0f));
        break;
    case WALL_STORM:
        cloud_bank(18, 0, 320, 60, 105, scale(dark, 0.55f), scale(dark, 0.8f), 0.97f);
        lightning(650, 230, 560);
        rain(600, 34, -10, scale(hex(0xB8C4D4), night ? 0.6f : 1.0f), 0.30f);
        break;
    default:
        break;
    }
}

/* 4x4 ordered dither, so the gradients do not band at 5/6/5 bits. */
static const uint8_t bayer[4][4] = {
    { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 },
};

bool wallpaper_render(uint16_t *out, wall_sky_t sky, bool night, bool light_theme)
{
    px = malloc(sizeof(rgb_t) * W * H);
    mask = malloc(W * H);
    if (px == NULL || mask == NULL) {
        free(px);
        free(mask);
        px = NULL;
        mask = NULL;
        return false;
    }
    paint(sky < WALL_SKY_COUNT ? sky : WALL_CLEAR, night);

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            rgb_t c = px[y * W + x];
            /* Dark theme: dim so light text reads. Light theme: wash towards
             * white so dark text reads. */
            if (light_theme) {
                c = mix(c, hex(0xFFFFFF), 0.30f);
            } else {
                c = scale(c, 0.72f);
            }
            float d = (bayer[y & 3][x & 3] + 0.5f) / 16.0f;
            int r = (int)(clampf(c.r, 0, 1) * 31.0f + d);
            int g = (int)(clampf(c.g, 0, 1) * 63.0f + d);
            int b = (int)(clampf(c.b, 0, 1) * 31.0f + d);
            out[y * W + x] = (uint16_t)((r > 31 ? 31 : r) << 11 | (g > 63 ? 63 : g) << 5 | (b > 31 ? 31 : b));
        }
    }

    free(px);
    free(mask);
    px = NULL;
    mask = NULL;
    return true;
}
