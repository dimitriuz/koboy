# Tested devices

koboy derives everything it can from the hardware at runtime --- panel size,
stride, framebuffer depth, the touch layer's raw range and rotation, whether the
fast DU4 waveform is available --- so it has a fair chance of working on a Kobo
nobody has tried. "A fair chance" is not the same as tested, and this file is
the difference.

**Exactly one device is verified.** Every other Kobo is *unverified*: not known
broken, just unmeasured. If you run koboy on one, please add a row.

| Device | Platform | Firmware | Panel | Buttons | `wfm_fast` | fps at 5x | Status |
|---|---|---|---|---|---|---|---|
| Kobo Libra 2 (`Io`, FBInk id 388) | Mark 9, `mx6sll-ntx`, Cortex-A9 single core | 4.38.23684 | 1264x1680 @ 300dpi, 32bpp, 1280px stride | 2 page-turn keys, both usable after calibration | `AUTO` (DU4 available: `hasEclipseWfm=1`) | 25.5 fps blocking / 66.7 fps non-blocking for the 800x720 rect | **verified** --- full game played, exits to a working Nickel, no reboot |

That row was verified with the shipped defaults, which is the point of shipping
them: `waveform_fast = auto`, `full_refresh_permille = 1000` (never force a
flash), `cleanup_interval = 0` and `cleanup_max_ms = 0` (no periodic flash --- 
AUTO already erases where erasing is needed), and `dpad_mode = cross`. A full
game of Tetris on those settings: controls responsive, **no flashing at all**,
and slight ghosting the player was happy to live with. The earlier defaults
(`450` / `60` / `3000`) were mitigations for a forced-DU4 pipeline and, once
measured, turned out to cause every flash between them while fixing nothing.

## How to add a row

Run koboy once on the device and read the facts off its own self-test, which
prints exactly what the backend decided:

```sh
cd /mnt/onboard/.adds/koboy && ./koboy --frames 300 --selftest
```

That reports `panel=`, `stride=`, `bpp=`, `origin=`, `wfm_du4_capable=`,
`touch_transpose=`, `input_keys=` and, after the run, a `refresh_fast=` line
with the measured mean and maximum refresh cost. Fill in the row from those,
say what worked and what did not, and be specific about the failure if it
failed --- a row saying "d-pad unusable, touch axes came out transposed" is
worth more than no row.

Please launch it from the device's own menu, as the README explains, and not
over ssh. koboy refuses the second case on purpose.

## Reading the fps column with the caution it deserves

The Libra 2 figures above come from direct waveform measurements on that device
(design spec, Appendices A and B), not from a guess --- but e-ink refresh
timing depends on panel temperature and controller state, and re-measuring the
*same* rect with the *same* waveform on the *same* device gave figures 45%
apart across two sessions. Treat every absolute number here as
order-of-magnitude.

What did reproduce across sessions, and is what the design actually rests on:

- DU4 is roughly 3.5x faster than A2 on Mark 9. A2 is not the fast path here,
  contrary to most of the folklore about e-ink emulators.
- Refresh cost scales with area, so dirty rectangles pay for themselves.
- Non-blocking refresh beats blocking by ~2.6x, which is why the main loop never
  waits for a refresh to complete.

## Unverified, and honestly so

- **Devices without `hasEclipseWfm`** (anything older than Mark 9, and some
  Mark 9s) get no DU4. koboy falls back automatically, but the fallback is
  measurably slower --- A2 was 7.4 fps for the 5x rect on the Libra 2 --- and
  nobody has played a game that way to say whether it is tolerable.
- **The 2024 MediaTek Kobos** (Clara Colour, Libra Colour) have a different SoC
  and a colour filter array over the panel. The core is built for
  `armv7-a`+NEON, which those run, but nothing else about them is tested.
- **Kobos with no page-turn buttons** (Clara, Nia, Touch): calibration has
  nothing to record and the touch d-pad is the only input. The button
  calibration path has never been run on such a device.
- **Firmware other than 4.38.x.** The launcher's Nickel restart follows the
  mechanism KOReader has used across many firmware versions, but it has been
  exercised on exactly one.
