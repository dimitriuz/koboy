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
| Kobo Libra 2 (`Io`, FBInk id 388) | Mark 9, `mx6sll-ntx`, Cortex-A9 single core | 4.38.23684 | 1264x1680 @ 300dpi, 32bpp, 1280px stride | 2 page-turn keys, KEY_F23(193)/KEY_F24(194) --- the shipped default pair, though this device's own calibration assigned them the other way round (`key_a = 194`) | `AUTO` (DU4 available: `hasEclipseWfm=1`) | 25.5 fps blocking / 66.7 fps non-blocking for the 800x720 rect | **verified** --- full game played, exits to a working Nickel, no reboot |

That row was verified with the shipped defaults, which is the point of shipping
them: `waveform_fast = auto`, `full_refresh_permille = 1000` (never force a
flash), `cleanup_interval = 0` and `cleanup_max_ms = 0` (no periodic flash --- 
AUTO already erases where erasing is needed), and `dpad_mode = cross`. A full
game of Tetris on those settings: controls responsive, **no flashing at all**,
and slight ghosting the player was happy to live with. The earlier defaults
(`450` / `60` / `3000`) were mitigations for a forced-DU4 pipeline and, once
measured, turned out to cause every flash between them while fixing nothing.

### Second title, an action game (Darkwing Duck, MBC1)

Tetris is a generous first test: small dirty rectangles, no scrolling. An action
platformer is the harder case, so `Darkwing Duck (USA)` was run on the same
device and the same shipped defaults. Measured from the launcher log:

| | |
|---|---|
| Wall clock | 173 s (15:30:50 to 15:33:43), launched from NickelMenu |
| Presented frames | 2261, i.e. **13.1 presented fps** at `present_divisor = 3` |
| Game-rect cleanups | 0 --- the periodic flash never fired, as intended |
| Large-area full refreshes | 16, about one per 11 s, all from real full-screen changes rather than a threshold |
| Layout | scale 5, an 800x720 game rect at (232,84) on the 1264x1680 panel |
| Exit | `rc=0`, framebuffer restored to 32bpp, Nickel restarted **without a reboot** |

13.1 presented fps against Tetris's headroom is the honest cost of a game that
dirties most of the screen most of the time: the pipeline pushes changed
rectangles, and in a scrolling platformer the changed rectangle is nearly the
whole picture.

### v2-core: the UI layer verified by hand, 2026-08-26

The player launched v2-core from NickelMenu and **exercised every in-game MENU
action by hand, reporting all of them working**: the ROM browser, the MENU
button, and the menu's actions. Two sessions exited `rc=0` with `restore: done`,
Nickel restarted without a reboot, the framebuffer returned to 32bpp, and
`/mnt/onboard/.kobo/version` stayed byte-identical with the real serial intact.

That closes the gap this file has carried since v2 began: the takeover, the
touch d-pad, the drawn MENU zone and the list widget's real touch input had
never met a finger. They have now.

**One thing to be precise about:** no `.stN` file exists in `saves/` afterwards,
so while the menu's save-state *screens* were navigated, a state was not
demonstrably written to and re-read from disk. `state.c` and `safefile.c` remain
covered by host tests only. The cartridge-SRAM path is separately verified (see
above) and does not depend on them.

**Correction, 2026-08-26.** This section previously ended "It was judged good to
play at that rate." That was too generous, and the player has since said so
directly: on a horizontal scroll the picture degrades into heavy horizontal
smearing within a few seconds, and v1 and v2-core look **the same** doing it. So
this is not a v2 regression -- it is a standing limitation of the whole
approach, and it was under-reported here.

The mechanism is the one v1 Appendix D already names for a different game: a
fast non-erasing waveform draws the new content without clearing the old, and
during a scroll every frame's background is offset horizontally from the last,
so successive frames superimpose into horizontal streaks. `waveform_fast = auto`
fixed this for Tetris, where the changed region is small and the controller
picks an erasing waveform for it. It does not fix a scroller, where nearly the
whole rect changes every frame.

`full_refresh_permille` is the lever -- it promotes a frame to a flashing,
erasing refresh once the changed area crosses a threshold -- and its shipped
value of 1000 was tuned on Tetris, where it essentially never fires. Tuning it
for scrollers is open; see `docs/FOLLOWUPS.md`. What is settled is that the
honest characterisation of this device is: **excellent for games with small
dirty regions, poor for full-screen scrollers**, which is what the v1 design
spec predicted before any of it was built.

The clean exit matters as much as the frame rate. Getting back to a working
Nickel without a reboot is what the launcher's environment gate and `restore()`
exist for, and this run is the evidence they work: the device identity file was
byte-identical afterwards, with its real serial intact.

**What this run did NOT test:** Darkwing Duck is cartridge type `0x01` (MBC1)
with `rambanks: 0` --- no battery-backed SRAM, exactly like Tetris. So the save
path is still unexercised on hardware by either title, and `sram_load` changed
materially in the final fix round. A battery-save game (a Zelda, Pokemon, or
Kirby's Dream Land 2) is the missing test.

## How to add a row

You do not need to build or deploy the emulator to add a row. Run
`koboy-probe --coexist` on the device -- it is safe over ssh and safe with
Nickel running, because it never reads an input event, never grabs a node,
and never changes the framebuffer's bit depth:

```sh
cd /mnt/onboard/.adds/koboy && ./koboy-probe --coexist
```

That writes `/mnt/onboard/koboy-probe-<device>.txt` with `device=`,
`platform=`, `panel=`, `stride=`, `bpp=`, `origin=`, `wfm_du4_capable=`,
`touch_slots=`, `touch_transpose=`, and a refresh-timing sweep ending in
`wfm_fast_name=`/`wfm_fast_ms=`. If the device also has page-turn buttons,
stop Nickel (`killall -TERM nickel hindenburg sickel`) and run
`./koboy-probe --takeover` afterwards -- it appends the key codes and touch
samples it captured to that same file, and bring Nickel back (or reboot)
once it is done. `docs/probe-readme.md` has the full walkthrough, including
why `--takeover` refuses to run while Nickel is still up.

Fill in the row from that file, say what worked and what did not, and be
specific about the failure if it failed --- a row saying "d-pad unusable,
touch axes came out transposed" is worth more than no row.

`koboy-probe --coexist` has none of `koboy`'s own menu-launch restriction --
it never touches Nickel or the input grabs, so ssh is fine for it. `koboy`
itself (worth running too, if you get that far -- actually playing something
is the real test) still refuses to run over ssh; launch that from the
device's own menu, as the README explains.

## Reading the fps column with the caution it deserves

The Libra 2 figures above come from direct waveform measurements on that device
(design spec, Appendices A and B), not from a guess --- but e-ink refresh
timing depends on panel temperature and controller state, and re-measuring the
*same* rect with the *same* waveform on the *same* device has now given 31.2,
46.7 and 67.5 ms across three sessions --- a factor of 2.2, and that is a floor
on the spread rather than a bound. Treat every absolute number here as
order-of-magnitude.

Blocking figures deserve extra suspicion: this device reports
`unreliable_wait_for=1`, and that flag applies to the very ioctl a blocking
measurement waits on, so those numbers are questionable by construction.
`koboy-probe` prints a caveat beside them when the flag is set. The main loop
never waits for completion, so what it depends on is the non-blocking path.

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

## v2-core: first device session, 2026-08-26

Same device as the row above (Kobo Libra 2, `Io`, Mark 9, firmware
4.38.23684), over ssh. One finding for the workflow doc first: **this device
does not answer ping** -- it has to be found by probing port 22, not with a
ping sweep. Cost time this session; recorded in `docs/device-workflow.md`.

### Method, and its limits

The v2 dist was deployed and the `koboy` binary was run **directly with
`--frames`**, never through `scripts/koboy.sh`. That means **Nickel was never
stopped**, so the takeover, the touch d-pad, the in-game MENU, and the ROM
browser's real touch input were **not** exercised. The browser was driven
only by `--ui-script`. A NickelMenu playtest with real touch input has still
not happened.

Device integrity held throughout: `/mnt/onboard/.kobo/version` byte-identical
before and after (real serial `N4181B1025136` intact), FBInk still reporting
`deviceName='Libra 2'`, `devicePlatform='Mark 9'`, `hasEclipseWfm=1`, Nickel
alive, no reboot needed. The device's own calibration is still reversed
(`key_a = 194`, `key_b = 193`) and was carried forward from the v1 ini during
deployment, per the workflow doc's redeploy caveat.

### The save path ran on hardware for the first time (FOLLOWUPS #3, closed)

ROM: `Legend of Zelda, The - Link's Awakening (USA, Europe) (Rev 2).gb`,
cartridge type `0x03` (MBC1+RAM+BATTERY), RAM size `0x02`. Gambatte reported
**`rambanks: 1`** -- the first cartridge with battery SRAM this project has
ever run; both Tetris and Darkwing Duck (above) are `rambanks: 0`.

Three directions verified:

- **Write:** `sram_save` produced `saves/zelda.srm` at exactly 8192 bytes.
- **Read:** a marked save round-tripped intact -- md5
  `daa6696c5da463305bdec570cdad2a82` identical before and after a run, with
  `koboy: loaded .../zelda.srm` in the log.
- **The destructive path:** truncated to 100 bytes, koboy reported "could not
  be read whole; SRAM left as the core initialised it and saving is disabled
  this session", drew the message on the panel, and **left the file at 100
  bytes**. That is the exact bug `src/main.c`'s comment records -- loading a
  truncated save used to destroy it further -- now proven fixed on real
  hardware.

Save **states** (`state.c`/`safefile.c`, a different mechanism reached
through `MODE_MENU`) are not covered by this: this session never exercised
`MODE_MENU`, so save states remain untested on hardware.

### A measurement that overturns a spec premise: `video_submit`, not "presentation," is the bottleneck

The v1 design spec §5 says *"Emulation is cheap; presentation is the entire
bottleneck."* Measured per presented frame at scale 5, Zelda,
`present_divisor = 3`:

| stage | mean | max |
|---|---|---|
| core (emulation) | 2.3 ms | 12.8 ms |
| **submit (pixel pipeline)** | **17.0 ms** | 44.8 ms |
| blit | 2.8 ms | 13.4 ms |
| refresh (submission) | 0.4-0.75 ms | 29.2 ms |

`video_submit` -- the RGB565->gray LUT, integer scale, quantise and 8x8 tile
diff -- dominates everything else combined by roughly 5x, and it is
**neither** "emulation" **nor** "presentation" in the spec's dichotomy: it is
the pixel pipeline sitting between the two. Confirmed pixel-bound by a
render-scale sweep (submit time only):

| scale | output px | submit |
|---|---|---|
| 3 | 207,360 | 8,997 µs |
| 4 | 368,640 | 12,462 µs |
| 5 | 576,000 | 16,639 µs |

Linear fit `submit ≈ 4.7 ms + 20.7 ns/px` predicts the scale-4 point within
1%. v2-core's multi-rect dirty-region work optimised `refresh` -- this
measurement shows `refresh` was already the cheapest of the four stages by a
wide margin. `video_submit` is where the next optimisation belongs; see
`docs/FOLLOWUPS.md` #23.

### `refresh_fixed_tiles` tuning: inconclusive by construction

Same ROM, same frame count, `--frames 900`:

| `refresh_fixed_tiles` | rects / frames | refresh mean |
|---|---|---|
| 20 | 339 / 292 | 604 µs |
| 40 (shipped) | 339 / 292 | 750 µs |
| 80 | 339 / 292 | 488 µs |
| 100000 (splitting off) | 292 / 292 | 368 µs |

20/40/80 are **behaviourally identical on real content** -- exactly what a
host reviewer predicted from the code before any device was available.
Splitting is measurably more expensive to *submit* (368 µs with it off vs
488-750 µs on), which is mechanical: each extra rect is another ioctl.

**But the in-process metric cannot see what the cost model actually
optimises.** Refreshes are non-blocking by design (see the ratios below), so
`refresh` times the *submission*, not the panel's work, which happens
asynchronously afterwards. Measuring the real benefit would need blocking
refreshes -- and this device reports `unreliable_wait_for=1`, so those
figures are suspect by construction. This is a **limit of the measurement**,
not a verdict on the split feature. `refresh_fixed_tiles` stays shipped at 40
(the untuned starting guess), unvalidated against actual panel time; see
`docs/FOLLOWUPS.md` #24.

### First on-device run of the v2 UI layer

The ROM browser works on the real panel: `koboy: chose
/mnt/onboard/.adds/koboy/roms/zelda.gb` from a real directory listing, driven
by `--ui-script`. Device identification was correct throughout: `Libra 2 (id
388, Io, Mark 9), 1264x1680 @ 32bpp, stride 5120 bytes (1280 px), origin
(0,0), fast=AUTO`, resolving to `scale 5, game 800x720 at (232,84)`.

The next device session should prioritise, in order: a NickelMenu playtest
with real touch input (the takeover, the touch d-pad, `MODE_MENU`'s
interactive branches, save states), then anything further on
`video_submit`'s cost now that it is the known bottleneck.
