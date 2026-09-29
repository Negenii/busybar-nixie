# Changelog

## 1.0.0 - 2026-09-29

First release.

- Four or six cold-cathode tubes on the 72x16 front panel, a port of the
  nixie face made for the Ulanzi TC002.
- Setup: Seconds on or off. On the wheel while the clock shows: tap for the
  colour (orange, blue, green, red), turn for how alive the gas is (64 steps,
  with a gauge along the bottom row), long press for the glow profile
  (Classic, Soft, Warm, Soft Warm, Tight). All of it is remembered.
- A changed digit heats from an ember while the old one cools, the next one
  starting to glow before the boundary; simultaneous changes roll as a wave
  from the right.
- Needs the time service exported to native apps (API 7.2). Until VeryBUSY
  ships that, the release carries a firmware build that does.
