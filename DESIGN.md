# Nixie clock for the BUSY Bar

A port of the `nixie` clock face written for the Ulanzi TC002 (52x16) to a
native BUSY Bar app (72x16). Same tubes, same gas, same numbers; only the
width changed.

## What it is

Cold-cathode tubes showing the time. A 7x11 digit of one-pixel stroke inside an
11x16 oval of unlit glass, the other nine cathodes dim behind it. The gas
breathes on three sines per tube, the glow crawls along the wire, the discharge
sputters, and a changed digit strikes past its level and settles while the old
one fades. Everything is derived from the clock itself (a digit's age is the
time since the boundary that changed it), so the renderer keeps no state and
a frame is a pure function of the time and the look.

## Layout

| Seconds | Tubes | x of each tube | Colons |
| --- | --- | --- | --- |
| Off | 4 (`hh:mm`) | 10, 22, 38, 50 | 2x2 dots at x=35, rows 4 and 9, steady |
| On | 6 (`hh:mm:ss`) | 0, 10, 25, 35, 50, 60 | at x=22 and x=47, same dots |

The four-tube layout is the Ulanzi one (51 px) centred on 72. The six-tube one
packs each pair to a pitch of ten, the glass of the two tubes overlapping by a
column as in one socket, and keeps four-column colon gaps between the pairs:
71 px. A pitch of twelve had the digits floating apart, a narrow `1` most of
all, and left no room for colons.

## Looks and controls

Setup has one row, **Seconds**: Off (`hh:mm`, four tubes and a colon) or On
(`hh:mm:ss`, six tubes and two colons). Everything else is on the wheel while
the clock shows:

- **Tap** steps the colour: Orange, Blue, Green, Red. The nine colour entries
  of each come from the Ulanzi presets `calm`, `neon`, `green`, `red` unchanged.
- **Turn** sets the depth, 1 to 64: how alive the gas is. 1 is the Ulanzi `calm`
  tuning (breathing under a percent per unit, a gentle crawl, no sputter),
  which on the bar's LEDs under auto-brightness reads as still. The breathing
  and the crawl grow with the square of the position, 64-fold at the top (the
  breathing then swings between a tenth and one and a half, the crawl +-80%
  along the stroke); from 16 up the discharge sputters, once a minute at 16 and
  thirteen times at 64, the dip deepening from 20% to a fifth of the tube. The
  envelope is floored so a tube dims but never goes out. While the wheel turns,
  a one-pixel gauge along the bottom row shows the depth, filled from the left;
  it holds 1.5 s after the last click and fades over 0.4 s.
- **Long press** steps the glow profile, the shape of the light around the
  stroke: Classic (halo 128 on the four neighbours, 38 on the diagonals, nothing
  further, as first tuned behind the TC002's diffuser), Soft (the same red halo spread
  over two rings), Warm (the halo in the core's own colour), Soft Warm (both),
  Tight (narrower and dimmer). The bar has no diffuser and its auto-brightness
  floors the dim rings, so the Classic shape reads as a hard cross there; the
  others are candidates for what the diffuser used to do.
- **Back** returns to the Start screen.

The back panel breadcrumb reads `<PROFILE> x<depth>`.

Seconds, colour, depth and profile persist in `/ext/apps_data/nixie/settings`
and come back on the next launch. Default: seconds off, orange, depth 16,
Classic.

## A digit changing

Not the Ulanzi strike (a 60 ms overshoot and a 180 ms afterglow) but a cathode
heating while the old one cools, and they cross rather than pass through dark.
The next digit is known ahead of time, so it starts to glow 300 ms before the
boundary: from an ember, 15% of full and shifted toward the dominant channel
(orange to deep red, blue to deep blue), with an ease-in-out over 650 ms that
is about half way up at the boundary. The old one holds until the boundary and
then falls away with a quadratic ease-in over 600 ms, reddening as it dims.
Everything fits inside the second, so the seconds tube never carries three
digits.

When several tubes change at once they go as a wave from the right: the n-th
tube from the right waits 50 * n * (n + 1) / 2 ms, so 0, 50, 150, 300, 500 and
750 ms across six tubes. Changes nest (a new hour is also a new minute and a new
second), so a tube's delay depends only on its place, never on which others are
in the wave.

## Structure

- `nixie_render.h/.c`: the renderer. C99, integers only, a 256-entry sine table,
  no furi and no GUI, so it compiles on the host. `nixie_render()` takes local
  seconds, the millisecond within the second, a preset and the seconds flag and
  fills a 72x16 RGB buffer.
- `nixie.c`: the app. Same shape as `fireplace.c`: `app_shell` for Start and
  Setup, a periodic event-loop timer at 33 ms, a `Canvas` on the front panel and
  a `MirrorCard` titled NIXIE on the back.
- `tests/nixie_render_test.c`: host test. Run `tests/run.sh`.

## Time

`time_get_local_time()` from the time service gives the local second; the
millisecond within it comes from `furi_get_tick()`, re-anchored whenever the
second changes so drift cannot build up. The time service is not exported to
FAPs in upstream 1.2.4 or VeryBUSY r7; this app needs the one-line export
(`sdk_headers=["time.h"]` on the time service) that ships alongside it.

## Levels

The Ulanzi presets were tuned with linear levels: glass at 1, diagonal glow at
38, halo at 128. The bar applies its auto-brightness as a multiplier, which can
push the lowest levels to zero. The renderer keeps the Ulanzi numbers; if the
glass vanishes on the panel, the floor values are the only numbers to touch.
