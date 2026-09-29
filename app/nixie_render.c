/**
 * @file nixie_render.c
 * @brief Port of the Ulanzi TC002 nixie face (nixie.zig) to 72x16.
 *
 * Everything is derived from the clock: a digit's age is the time since the
 * boundary that changed it, and what it fades from is that digit one second
 * ago. The flicker repeats every 1000 s, and every frequency is a whole number
 * of millihertz, so that many cycles land in the window exactly and the wrap
 * is seamless. Integer throughout, off a sine table: there is no libm in a FAP.
 */
#include "nixie_render.h"

#include <string.h>

#define ONE (1024)
#define TUBE_W (11)
#define GLYPH_W (7)
#define GLYPH_H (11)
#define GLYPH_Y (2)

#define WINDOW_MS (1000000LL)
/* A changed digit is a cathode heating up while the old one cools. The next
 * digit is known ahead of time, so it starts to glow PRE_MS before the
 * boundary and is about half way up when the old one begins to cool; the
 * two cross rather than passing through dark. Everything fits inside the
 * second, so the seconds tube never carries three digits. */
#define PRE_MS (300)
#define HEAT_MS (650) /* from the start of the pre-heat to full glow */
#define COOL_MS (600) /* from the boundary to dark */
/* When several tubes change at once they go as a wave from the right: the
 * n-th tube from the right waits WAVE_MS * n * (n + 1) / 2, so 0, 50, 150,
 * 300, 500, 750 ms. Changes nest (a new hour is also a new minute and a new
 * second), so the wave never has to know which tubes are in it. */
#define WAVE_MS (50)
/* Where the ember sits, in 1/1024 of full: brightness and colour both start here. */
#define EMBER (150)
#define SPUTTER_MS (90)

static const int32_t sine[256] = {
#include "sine_table.inc"
};

/* the three breathing frequencies, the same for every look; only their depth is tuned */
static const int64_t ripple_mhz[3] = {3000, 5600, 9400};

/* ------------------------------------------------------------------- looks */

typedef struct {
    uint8_t core_top[3], core_bot[3], halo[3], corner[3], far[3], knight[3], glass[3],
        stack[3], colon[3];
} NixieColours;

/* Straight from the Ulanzi presets calm, neon, green and red. Blue needs about
 * 3 to light at all where red lights at 1, so its glass and stack sit higher. */
static const NixieColours colours[NixieColourCount] = {
    [NixieColourOrange] =
        {{255, 130, 38}, {255, 89, 26}, {128, 14, 0}, {38, 4, 0}, {0, 0, 0}, {2, 0, 0}, {1, 0, 0}, {1, 0, 0}, {106, 12, 0}},
    [NixieColourBlue] =
        {{90, 200, 255}, {30, 120, 255}, {4, 26, 128}, {0, 8, 38}, {0, 0, 0}, {0, 0, 3}, {0, 0, 3}, {0, 1, 6}, {4, 24, 106}},
    [NixieColourGreen] =
        {{120, 255, 160}, {30, 255, 70}, {6, 128, 22}, {0, 38, 6}, {0, 0, 0}, {0, 2, 0}, {0, 1, 0}, {0, 2, 0}, {6, 106, 18}},
    [NixieColourRed] =
        {{255, 50, 24}, {255, 14, 8}, {128, 5, 0}, {38, 2, 0}, {0, 0, 0}, {2, 0, 0}, {1, 0, 0}, {1, 0, 0}, {106, 4, 0}},
};

typedef struct {
    int32_t ripple[3];
    int32_t core_field, glow_field, sputter_to, sputter_per_min;
} NixieDynamics;

/* The glass in good health, the Ulanzi `calm`: breathing 3.7%, a gentle crawl,
 * no sputter. The Ulanzi `live` was this with the crawl at 900 and three
 * sputters a minute to 21%; the depth control passes through that around 4. */
static const NixieDynamics calm = {{185, 115, 70}, 200, 200, 1004, 0};

void nixie_preset_make(NixiePreset* out, NixieColour colour) {
    if(colour >= NixieColourCount) colour = NixieColourOrange;
    const NixieColours* c = &colours[colour];
    const NixieDynamics* d = &calm;
    memcpy(out->core_top, c->core_top, 3);
    memcpy(out->core_bot, c->core_bot, 3);
    memcpy(out->halo, c->halo, 3);
    memcpy(out->corner, c->corner, 3);
    memcpy(out->far, c->far, 3);
    memcpy(out->knight, c->knight, 3);
    memcpy(out->glass, c->glass, 3);
    memcpy(out->stack, c->stack, 3);
    memcpy(out->colon, c->colon, 3);
    for(int i = 0; i < 3; i++) out->ripple[i] = d->ripple[i];
    out->core_field = d->core_field;
    out->glow_field = d->glow_field;
    out->sputter_to = d->sputter_to;
    out->sputter_per_min = d->sputter_per_min;
}

void nixie_preset_set_depth(NixiePreset* preset, uint8_t depth) {
    if(depth < NIXIE_DEPTH_MIN) depth = NIXIE_DEPTH_MIN;
    if(depth > NIXIE_DEPTH_MAX) depth = NIXIE_DEPTH_MAX;
    /* Quadratic, 1 at 1 and 64 at 64. The Ulanzi numbers breathe by under a
     * percent per unit, so a linear scale left the top of the wheel still. */
    const int32_t d = (int32_t)depth - 1;
    const int32_t m = 1 + (d * d * 63) / 3969;
    for(int i = 0; i < 3; i++) preset->ripple[i] *= m;
    /* +-80% of the stroke at the top: 7800 * 4096 / 40000 = 799/1024 */
    int32_t field = preset->core_field * m;
    if(field > 7800) field = 7800;
    preset->core_field = field;
    field = preset->glow_field * m;
    if(field > 7800) field = 7800;
    preset->glow_field = field;
    /* Sputter from 16 up: once a minute at 16, thirteen times at 64, and the
     * dip deepens from 20% at 16 down to a fifth of the tube at 64. */
    if(depth >= 16U) {
        preset->sputter_per_min = 1 + ((int32_t)depth - 16) / 4;
        preset->sputter_to = ONE - 205 - (((int32_t)depth - 16) * 595) / 48;
    } else {
        preset->sputter_per_min = 0;
    }
}

static void ring(const uint8_t from[3], int32_t pct, uint8_t out[3]) {
    for(int n = 0; n < 3; n++) out[n] = (uint8_t)(((int32_t)from[n] * pct + 50) / 100);
}

void nixie_preset_set_glow(NixiePreset* preset, NixieGlow glow) {
    uint8_t halo[3];
    memcpy(halo, preset->halo, 3);
    switch(glow) {
    case NixieGlowSoft:
        ring(halo, 55, preset->corner);
        ring(halo, 25, preset->far);
        ring(halo, 12, preset->knight);
        break;
    case NixieGlowWarm:
        ring(preset->core_top, 50, preset->halo);
        ring(preset->core_top, 15, preset->corner);
        memset(preset->far, 0, 3);
        memset(preset->knight, 0, 3);
        break;
    case NixieGlowSoftWarm:
        ring(preset->core_top, 45, preset->halo);
        ring(preset->core_top, 22, preset->corner);
        ring(preset->core_top, 10, preset->far);
        ring(preset->core_top, 5, preset->knight);
        break;
    case NixieGlowTight:
        ring(halo, 70, preset->halo);
        ring(halo, 15, preset->corner);
        memset(preset->far, 0, 3);
        memset(preset->knight, 0, 3);
        break;
    case NixieGlowClassic:
    default:
        break;
    }
}

/* -------------------------------------------------------------- arithmetic */

/* Floored modulo, what Zig's @mod does; C's % truncates toward zero. */
static int64_t mod64(int64_t a, int64_t m) {
    int64_t r = a % m;
    return (r < 0) ? r + m : r;
}

static int32_t sin_at(int64_t phase) {
    return sine[mod64(phase, 256)];
}

/* a cheap integer hash, so which second a tube lets go in is decided without keeping any state */
static uint32_t hash32(uint32_t a) {
    uint32_t x = a;
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

static int64_t phase_of(int64_t t_ms, int64_t mhz) {
    return (mod64(t_ms, WINDOW_MS) * mhz * 256) / 1000000LL;
}

/* Every tube gets one chance a second, takes it sputter_per_min times in sixty,
 * and starts the burst at an offset the same hash decides. */
static bool sputtering(const NixiePreset* pre, int32_t i, int64_t t_ms) {
    if(pre->sputter_per_min <= 0) return false;
    const int64_t slot = mod64(t_ms, WINDOW_MS) / 1000;
    const uint32_t h = hash32((uint32_t)slot * 977U + (uint32_t)i * 61U);
    if(h % 60U >= (uint32_t)pre->sputter_per_min) return false;
    const int64_t start = slot * 1000 + (int64_t)((h >> 8) % (uint32_t)(1000 - SPUTTER_MS));
    /* Compared inside the window. The Zig original compared `start` against
     * the raw epoch time, which is why it never fired on the Ulanzi. */
    const int64_t w = mod64(t_ms, WINDOW_MS);
    return (w >= start) && (w < start + SPUTTER_MS);
}

/* how bright tube i is running, before any strike: the breathing times the sputter */
static int32_t envelope(const NixiePreset* pre, int32_t i, int64_t t_ms) {
    int32_t k = ONE;
    for(int32_t n = 0; n < 3; n++) {
        const int64_t ph = phase_of(t_ms, ripple_mhz[n]) + (int64_t)i * (31 + n * 17);
        k += (pre->ripple[n] * sin_at(ph)) / (4 * 10000);
    }
    if(sputtering(pre, i, t_ms)) k = (k * pre->sputter_to) / ONE;
    /* Deep breathing can swing past zero; a tube dims, it does not go out. */
    if(k < ONE / 16) k = ONE / 16;
    if(k > 2 * ONE) k = 2 * ONE;
    return k;
}

/* the crawling glow, +-4096 over the glyph */
static int32_t field_at(int32_t x, int32_t y, int64_t t_ms, int32_t i) {
    const int64_t off = (int64_t)i * 61;
    const int64_t p1 = 35 * (int64_t)x + 65 * (int64_t)y - phase_of(t_ms, 1100) + off;
    const int64_t p2 = 86 * (int64_t)x - 29 * (int64_t)y + phase_of(t_ms, 1600) + off;
    return (62 * sin_at(p1) + 38 * sin_at(p2)) / 100;
}

/* ----------------------------------------------------------------- drawing */

/* the cathodes: 7x11, one pixel of stroke */
static const char* const digit_art[10][GLYPH_H] = {
    {".#####.", "#.....#", "#.....#", "#.....#", "#.....#", "#.....#", "#.....#", "#.....#", "#.....#", "#.....#", ".#####."},
    {"...#...", "..##...", ".#.#...", "...#...", "...#...", "...#...", "...#...", "...#...", "...#...", "...#...", ".#####."},
    {".#####.", "#.....#", "......#", "......#", "......#", ".....#.", "....#..", "...#...", "..#....", ".#.....", "#######"},
    {".#####.", "#.....#", "......#", "......#", "..####.", "......#", "......#", "......#", "......#", "#.....#", ".#####."},
    {".....#.", "....##.", "...#.#.", "..#..#.", ".#...#.", "#....#.", "#######", ".....#.", ".....#.", ".....#.", ".....#."},
    {"#######", "#......", "#......", "#......", "######.", "......#", "......#", "......#", "......#", "#.....#", ".#####."},
    {"..####.", ".#.....", "#......", "#......", "#......", "######.", "#.....#", "#.....#", "#.....#", "#.....#", ".#####."},
    {"#######", "#.....#", "......#", ".....#.", ".....#.", "....#..", "....#..", "...#...", "...#...", "..#....", "..#...."},
    {".#####.", "#.....#", "#.....#", "#.....#", "#.....#", ".#####.", "#.....#", "#.....#", "#.....#", "#.....#", ".#####."},
    {".#####.", "#.....#", "#.....#", "#.....#", "#.....#", ".######", "......#", "......#", "......#", ".....#.", ".####.."},
};

static void scaled(const uint8_t c[3], int32_t k, uint8_t out[3]) {
    for(int i = 0; i < 3; i++) {
        int32_t s = ((int32_t)c[i] * k) / ONE;
        if(s < 0) s = 0;
        if(s > 255) s = 255;
        out[i] = (uint8_t)s;
    }
}

/* Lift a pixel to at least this colour, channel by channel. Off-panel is a no-op. */
static void lift(uint8_t* rgb, int32_t x, int32_t y, const uint8_t c[3]) {
    if((x < 0) || (y < 0) || (x >= NIXIE_W) || (y >= NIXIE_H)) return;
    uint8_t* p = rgb + (y * NIXIE_W + x) * 3;
    for(int i = 0; i < 3; i++) {
        if(c[i] > p[i]) p[i] = c[i];
    }
}

/* The colour of a cathode at less than full heat: the dominant channel holds
 * most of its value and the others fall away, so orange cools to a deep red,
 * blue to a deep blue. temp is 0..ONE, ONE being the working colour. */
static void ember_colour(const NixiePreset* pre, const uint8_t warm[3], int32_t temp, uint8_t out[3]) {
    int dominant = 0;
    for(int n = 1; n < 3; n++) {
        if(pre->core_top[n] > pre->core_top[dominant]) dominant = n;
    }
    for(int n = 0; n < 3; n++) {
        const int32_t cold = ((int32_t)warm[n] * ((n == dominant) ? 60 : 12)) / 100;
        out[n] = (uint8_t)(cold + (((int32_t)warm[n] - cold) * temp) / ONE);
    }
}

static void draw_glyph(
    uint8_t* rgb,
    const NixiePreset* pre,
    int32_t x0,
    uint8_t d,
    int32_t k,
    int32_t temp,
    int64_t t_ms,
    int32_t i) {
    if((k <= ONE / 100) || (d > 9)) return;
    const int32_t gx = x0 + (TUBE_W - GLYPH_W) / 2;
    for(int32_t r = 0; r < GLYPH_H; r++) {
        for(int32_t c = 0; c < GLYPH_W; c++) {
            if(digit_art[d][r][c] != '#') continue;
            const int32_t x = gx + c;
            const int32_t y = GLYPH_Y + r;
            const int32_t f = field_at(x, y, t_ms, i);
            const int32_t core_k = (k * (ONE + (pre->core_field * f) / (4 * 10000))) / ONE;
            const int32_t glow_k = (k * (ONE + (pre->glow_field * f) / (4 * 10000))) / ONE;
            uint8_t col[3];
            for(int n = 0; n < 3; n++) {
                const int32_t a = pre->core_top[n];
                const int32_t b = pre->core_bot[n];
                col[n] = (uint8_t)(a + ((b - a) * r) / (GLYPH_H - 1));
            }
            uint8_t tmp[3];
            if(temp < ONE) {
                uint8_t cold[3];
                ember_colour(pre, col, temp, cold);
                scaled(cold, core_k, tmp);
            } else {
                scaled(col, core_k, tmp);
            }
            lift(rgb, x, y, tmp);
            scaled(pre->halo, glow_k, tmp);
            lift(rgb, x - 1, y, tmp);
            lift(rgb, x + 1, y, tmp);
            lift(rgb, x, y - 1, tmp);
            lift(rgb, x, y + 1, tmp);
            scaled(pre->corner, glow_k, tmp);
            lift(rgb, x - 1, y - 1, tmp);
            lift(rgb, x + 1, y - 1, tmp);
            lift(rgb, x - 1, y + 1, tmp);
            lift(rgb, x + 1, y + 1, tmp);
            if((pre->far[0] | pre->far[1] | pre->far[2]) > 0) {
                scaled(pre->far, glow_k, tmp);
                lift(rgb, x - 2, y, tmp);
                lift(rgb, x + 2, y, tmp);
                lift(rgb, x, y - 2, tmp);
                lift(rgb, x, y + 2, tmp);
            }
            if((pre->knight[0] | pre->knight[1] | pre->knight[2]) > 0) {
                static const int32_t kdx[4] = {-2, -1, 1, 2};
                static const int32_t kdy[4] = {-1, -2, -2, -1};
                scaled(pre->knight, glow_k, tmp);
                for(int n = 0; n < 4; n++) {
                    lift(rgb, x + kdx[n], y + kdy[n], tmp);
                    lift(rgb, x - kdx[n], y - kdy[n], tmp);
                }
            }
        }
    }
}

/* How far a cathode has heated, t_ms after it began to glow, 0..ONE: from the
 * ember, an ease-in-out that is half way at the boundary and settles into
 * full glow. */
static int32_t heat_k(int64_t t_ms) {
    if(t_ms <= 0) return EMBER;
    if(t_ms >= HEAT_MS) return ONE;
    const int64_t a = (t_ms * ONE) / HEAT_MS;
    int64_t eased;
    if(a < ONE / 2) {
        eased = (2 * a * a) / ONE;
    } else {
        const int64_t rest = ONE - a;
        eased = ONE - (2 * rest * rest) / ONE;
    }
    return (int32_t)(EMBER + ((ONE - EMBER) * eased) / ONE);
}

/* How much heat the old cathode still holds age_ms after it was switched
 * off: a quadratic ease-in, holding on at first and then falling away. */
static int32_t cool_k(int64_t age_ms) {
    if(age_ms >= COOL_MS) return 0;
    const int64_t a = (age_ms * ONE) / COOL_MS;
    const int64_t rest = ONE - a;
    return (int32_t)((rest * rest) / ONE);
}

static void draw_tube(
    uint8_t* rgb,
    const NixiePreset* pre,
    int32_t i,
    int32_t x0,
    uint8_t cur,
    uint8_t was,
    uint8_t next,
    int64_t age_ms,
    int64_t period_ms,
    int64_t wave_ms,
    int64_t t_ms) {
    const int32_t env = envelope(pre, i, t_ms);
    const int32_t cx = x0 + TUBE_W / 2;
    uint8_t tmp[3];

    /* the glass: ((x-cx)/5.4)^2 + ((y-7.5)/8)^2 <= 1, scaled by 432 so it stays in integers */
    scaled(pre->glass, env, tmp);
    for(int32_t y = 0; y < NIXIE_H; y++) {
        for(int32_t x = x0; x < x0 + TUBE_W; x++) {
            const int32_t a = (x - cx) * 80;
            const int32_t b = (2 * y - 15) * 27;
            if(a * a + b * b <= 432 * 432) lift(rgb, x, y, tmp);
        }
    }
    /* the unlit cathodes behind the digit */
    scaled(pre->stack, env, tmp);
    for(int32_t y = GLYPH_Y; y < GLYPH_Y + GLYPH_H; y++) {
        for(int32_t x = cx - 3; x <= cx + 3; x++) lift(rgb, x, y, tmp);
    }

    /* the tube's own clock, held back by its place in the wave */
    const int64_t eff_ms = age_ms - wave_ms;

    if(was != cur) {
        if(eff_ms < 0) {
            /* the wave has not reached this tube: the old digit still burns,
             * the new one only beginning to glow underneath it */
            draw_glyph(rgb, pre, x0, was, env, ONE, t_ms, i);
            if(eff_ms >= -PRE_MS) {
                const int32_t heat = heat_k(eff_ms + PRE_MS);
                draw_glyph(rgb, pre, x0, cur, (heat * env) / ONE, heat, t_ms, i);
            }
        } else {
            /* just after a change: the old one cooling, the new one still heating */
            const int32_t cool = cool_k(eff_ms);
            if(cool > 0) draw_glyph(rgb, pre, x0, was, (cool * env) / ONE, cool, t_ms, i);
            const int32_t heat = heat_k(eff_ms + PRE_MS);
            draw_glyph(rgb, pre, x0, cur, (heat * env) / ONE, heat, t_ms, i);
        }
    } else {
        draw_glyph(rgb, pre, x0, cur, env, ONE, t_ms, i);
    }
    if((next != cur) && (eff_ms >= period_ms - PRE_MS)) {
        /* just before a change: the next one already beginning to glow */
        const int32_t heat = heat_k(eff_ms - (period_ms - PRE_MS));
        draw_glyph(rgb, pre, x0, next, (heat * env) / ONE, heat, t_ms, i);
    }
}

/* ------------------------------------------------------------------ layout */

/* hh:mm is the Ulanzi layout centred: pairs a column apart, a four-column
 * gap with the colon between them. hhmmss packs each pair to a pitch of ten,
 * the glass of the two tubes overlapping by a column as in one socket, and
 * keeps the four-column colon gaps between the pairs: 71 columns in all. */
static const int32_t xs4[4] = {10, 22, 38, 50};
static const int32_t colons4[1] = {35};
static const int32_t xs6[6] = {0, 10, 25, 35, 50, 60};
static const int32_t colons6[2] = {22, 47};

/* how long each digit has been showing its current value, measured inside the
 * day: the tens-of-hours digit turns at 00, 10 and 20 hours, and 86400 is not a
 * multiple of 36000, so modding the raw epoch second would drift a boundary
 * every day */
static const int64_t period_s[6] = {10 * 3600, 3600, 600, 60, 10, 1};

static int64_t day_seconds(int64_t local_s) {
    return mod64(local_s, 86400);
}

static void digits_of(int64_t local_s, uint8_t out[6]) {
    const int64_t day = day_seconds(local_s);
    const int32_t h = (int32_t)(day / 3600);
    const int32_t m = (int32_t)((day % 3600) / 60);
    const int32_t s = (int32_t)(day % 60);
    out[0] = (uint8_t)(h / 10);
    out[1] = (uint8_t)(h % 10);
    out[2] = (uint8_t)(m / 10);
    out[3] = (uint8_t)(m % 10);
    out[4] = (uint8_t)(s / 10);
    out[5] = (uint8_t)(s % 10);
}

void nixie_render(
    uint8_t* rgb,
    int64_t local_s,
    uint32_t ms,
    const NixiePreset* pre,
    bool seconds) {
    memset(rgb, 0, NIXIE_FRAME_BYTES);

    const int32_t count = seconds ? 6 : 4;
    const int32_t* xs = seconds ? xs6 : xs4;
    const int32_t* colons = seconds ? colons6 : colons4;
    const int32_t colon_count = seconds ? 2 : 1;
    const int64_t t_ms = local_s * 1000 + (int64_t)ms;
    const int64_t day_ms = day_seconds(local_s) * 1000 + (int64_t)ms;

    uint8_t cur[6], was[6], next[6];
    digits_of(local_s, cur);
    digits_of(local_s - 1, was);
    digits_of(local_s + 1, next);

    for(int32_t i = 0; i < count; i++) {
        const int64_t period_ms = period_s[i] * 1000;
        const int64_t age_ms = mod64(day_ms, period_ms);
        const int64_t n = count - 1 - i;
        const int64_t wave_ms = WAVE_MS * n * (n + 1) / 2;
        draw_tube(rgb, pre, i, xs[i], cur[i], was[i], next[i], age_ms, period_ms, wave_ms, t_ms);
    }

    /* steady, each colon breathing with the tube to its left */
    for(int32_t n = 0; n < colon_count; n++) {
        uint8_t c[3];
        scaled(pre->colon, envelope(pre, 2 * n + 1, t_ms), c);
        for(int32_t cy = 4; cy <= 9; cy += 5) {
            for(int32_t dy = 0; dy < 2; dy++) {
                for(int32_t dx = 0; dx < 2; dx++) lift(rgb, colons[n] + dx, cy + dy, c);
            }
        }
    }
}
