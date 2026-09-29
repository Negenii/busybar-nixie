/**
 * @file nixie_render.h
 * @brief The nixie tubes, as a pure function of time and look.
 *
 * Integer only, no dependencies beyond <stdint.h> and <stdbool.h>, so the same
 * file compiles on the host for tests and in the FAP for the panel. A port of
 * the Ulanzi TC002 face (nixie.zig) from 52 to 72 columns.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NIXIE_W (72)
#define NIXIE_H (16)
#define NIXIE_FRAME_BYTES (NIXIE_W * NIXIE_H * 3)

typedef enum {
    NixieColourOrange = 0,
    NixieColourBlue = 1,
    NixieColourGreen = 2,
    NixieColourRed = 3,
    NixieColourCount,
} NixieColour;

/* A whole look. Colours are the bytes that reach the panel; fractions are in
 * 1/10000 of one (1024); sputter_to is in 1/1024. */
typedef struct {
    uint8_t core_top[3];
    uint8_t core_bot[3];
    uint8_t halo[3];
    uint8_t corner[3];
    uint8_t far[3];
    uint8_t knight[3];
    uint8_t glass[3];
    uint8_t stack[3];
    uint8_t colon[3];
    int32_t ripple[3];
    int32_t core_field;
    int32_t glow_field;
    int32_t sputter_to;
    int32_t sputter_per_min;
} NixiePreset;

/** A colour with the calm gas: the Ulanzi `calm` tuning, breathing 3.7%. */
void nixie_preset_make(NixiePreset* out, NixieColour colour);

#define NIXIE_DEPTH_MIN (1U)
#define NIXIE_DEPTH_MAX (64U)

/**
 * How alive the tube is, 1..64. 1 is the calm Ulanzi tuning. The breathing and
 * the crawl grow with the square of the position, 64-fold at the top; from 16
 * up the discharge also starts to sputter, more often and deeper with every
 * step, until at 64 it lets go a dozen times a minute down to a fifth. The
 * envelope is floored inside the renderer, so no depth turns a tube off.
 */
void nixie_preset_set_depth(NixiePreset* preset, uint8_t depth);

typedef enum {
    NixieGlowClassic = 0, /* the rings as first tuned, behind the TC002's diffuser */
    NixieGlowSoft,       /* the same red halo, spread over two rings */
    NixieGlowWarm,       /* the halo in the core's own colour */
    NixieGlowSoftWarm,   /* both */
    NixieGlowTight,      /* narrower and dimmer than the TC002 tuning */
    NixieGlowCount,
} NixieGlow;

/** Reshape the glow around the stroke. Apply after nixie_preset_make(). */
void nixie_preset_set_glow(NixiePreset* preset, NixieGlow glow);

/**
 * Draw one frame into @p rgb (NIXIE_FRAME_BYTES, row-major, 3 bytes a pixel).
 * The buffer is cleared first.
 *
 * @param local_s  local time, seconds since the epoch (only the time of day is used)
 * @param ms       millisecond within that second, 0..999
 * @param seconds  six tubes (hhmmss) when true, four with a colon (hh:mm) when false
 */
void nixie_render(
    uint8_t* rgb,
    int64_t local_s,
    uint32_t ms,
    const NixiePreset* preset,
    bool seconds);

#ifdef __cplusplus
}
#endif
