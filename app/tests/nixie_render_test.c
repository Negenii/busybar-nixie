/* Host test for the renderer: cc -std=c99 -Wall -Wextra -Werror -I.. nixie_render_test.c ../nixie_render.c */
#include "../nixie_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, ...)                                          \
    do {                                                          \
        if(!(cond)) {                                             \
            failures++;                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);           \
            printf(__VA_ARGS__);                                  \
            printf("\n");                                         \
        }                                                         \
    } while(0)

static uint8_t frame[NIXIE_FRAME_BYTES];
static uint8_t other[NIXIE_FRAME_BYTES];

static int column_lit(const uint8_t* rgb, int x) {
    for(int y = 0; y < NIXIE_H; y++) {
        const uint8_t* p = rgb + (y * NIXIE_W + x) * 3;
        if(p[0] || p[1] || p[2]) return 1;
    }
    return 0;
}

static int lit_count(const uint8_t* rgb) {
    int n = 0;
    for(int i = 0; i < NIXIE_W * NIXIE_H; i++) {
        if(rgb[i * 3] || rgb[i * 3 + 1] || rgb[i * 3 + 2]) n++;
    }
    return n;
}

static int brightness_at(const uint8_t* rgb, int x, int y) {
    const uint8_t* p = rgb + (y * NIXIE_W + x) * 3;
    return p[0] + p[1] + p[2];
}

static void write_ppm(const char* path, const uint8_t* rgb) {
    FILE* f = fopen(path, "wb");
    if(!f) return;
    fprintf(f, "P6\n%d %d\n255\n", NIXIE_W, NIXIE_H);
    fwrite(rgb, 1, NIXIE_FRAME_BYTES, f);
    fclose(f);
}

int main(void) {
    NixiePreset pre;
    nixie_preset_make(&pre, NixieColourOrange);
    const int64_t t = 13 * 3600 + 45 * 60 + 27; /* 13:45:27 */

    /* four tubes sit centred: columns 0..9 and 61..71 stay dark, the colon is lit */
    nixie_render(frame, t, 500, &pre, false);
    for(int x = 0; x < 10; x++) CHECK(!column_lit(frame, x), "hh:mm lit column %d", x);
    for(int x = 61; x < NIXIE_W; x++) CHECK(!column_lit(frame, x), "hh:mm lit column %d", x);
    CHECK(brightness_at(frame, 35, 4) > 0, "colon dot missing at 35,4");
    CHECK(brightness_at(frame, 36, 10) > 0, "colon dot missing at 36,10");
    CHECK(lit_count(frame) > 200, "hh:mm frame too dark: %d pixels", lit_count(frame));
    write_ppm("hhmm.ppm", frame);

    /* six tubes fill 71 columns: pairs packed, colon gaps between them */
    nixie_render(frame, t, 500, &pre, true);
    CHECK(!column_lit(frame, 71), "hhmmss lit column 71");
    CHECK(column_lit(frame, 0) && column_lit(frame, 70), "hhmmss should reach both edges");
    CHECK(!column_lit(frame, 21) && !column_lit(frame, 24), "first colon gap not clear");
    CHECK(!column_lit(frame, 46) && !column_lit(frame, 49), "second colon gap not clear");
    CHECK(brightness_at(frame, 22, 4) > 0 && brightness_at(frame, 48, 10) > 0, "hhmmss colons missing");
    write_ppm("hhmmss.ppm", frame);

    /* the same input draws the same frame */
    nixie_render(other, t, 500, &pre, true);
    CHECK(memcmp(frame, other, NIXIE_FRAME_BYTES) == 0, "render is not deterministic");

    /* the digit changes on the second, and a fresh digit strikes from half */
    nixie_render(frame, t, 999, &pre, true);
    nixie_render(other, t + 1, 0, &pre, true);
    CHECK(memcmp(frame, other, NIXIE_FRAME_BYTES) != 0, "seconds digit did not change");
    /* tube 5 core pixel of a '8' (t+1 = :28): centre column cx=65, row 7 is the middle bar */
    nixie_render(frame, t + 1, 0, &pre, true);
    nixie_render(other, t + 1, 500, &pre, true);
    CHECK(
        brightness_at(frame, 65, 7) < brightness_at(other, 65, 7),
        "strike should start dim: %d vs %d",
        brightness_at(frame, 65, 7),
        brightness_at(other, 65, 7));

    /* minutes tube changes at the minute boundary, and not before */
    nixie_render(frame, 13 * 3600 + 45 * 60 + 59, 900, &pre, false);
    nixie_render(other, 13 * 3600 + 46 * 60, 900, &pre, false);
    CHECK(memcmp(frame, other, NIXIE_FRAME_BYTES) != 0, "minute did not change");

    /* every colour at two depths draws something different */
    uint8_t* looks[2 * NixieColourCount];
    int n = 0;
    for(int i = 0; i < 2; i++) {
        for(int c = 0; c < NixieColourCount; c++) {
            NixiePreset p;
            nixie_preset_make(&p, (NixieColour)c);
            nixie_preset_set_depth(&p, (uint8_t)(i ? 48 : 1));
            looks[n] = malloc(NIXIE_FRAME_BYTES);
            nixie_render(looks[n], t, 500, &p, false);
            CHECK(lit_count(looks[n]) > 200, "look %d/%d too dark", i, c);
            for(int m = 0; m < n; m++) {
                CHECK(memcmp(looks[n], looks[m], NIXIE_FRAME_BYTES) != 0, "look %d/%d equals an earlier one", i, c);
            }
            n++;
        }
    }
    for(int i = 0; i < n; i++) free(looks[i]);

    /* a deep tube sputters, a calm one never does */
    NixiePreset live;
    nixie_preset_make(&live, NixieColourOrange);
    nixie_preset_set_depth(&live, NIXIE_DEPTH_MAX);
    /* The whole 1000 s flicker window: the hash is deterministic, and a two
     * minute slice of it happened to hold no burst for tube 0. */
    int dips = 0;
    for(int s = 0; s < 1000; s++) {
        for(int m = 0; m < 1000; m += 30) {
            nixie_render(frame, t + s, (uint32_t)m, &live, false);
            /* core of tube 0 at its '1' stem: gx=12, col 3 -> x=15, row 7 */
            if(brightness_at(frame, 15, 7) < 200) dips++;
        }
    }
    CHECK(dips > 20, "live sputtered only %d times in the window", dips);

    /* the deepest breathing still leaves every tube lit and every byte in range */
    {
        NixiePreset deep;
        nixie_preset_make(&deep, NixieColourOrange);
        nixie_preset_set_depth(&deep, NIXIE_DEPTH_MAX);
        int min_core = 9999, max_core = 0;
        for(int m = 0; m < 1000; m += 33) {
            nixie_render(frame, t, (uint32_t)m, &deep, false);
            const int b = brightness_at(frame, 15, 7);
            if(b < min_core) min_core = b;
            if(b > max_core) max_core = b;
            CHECK(lit_count(frame) > 200, "depth 16 frame went dark at %d ms", m);
        }
        CHECK(max_core - min_core > 60, "depth 16 barely moves: %d..%d", min_core, max_core);
        nixie_preset_set_depth(&deep, 0);
        nixie_preset_set_depth(&deep, 200);
    }

    /* nothing in the frame relies on time before the epoch behaving */
    nixie_render(frame, -5, 0, &pre, true);

    if(failures == 0) printf("nixie_render: all checks passed\n");
    return failures ? 1 : 0;
}
