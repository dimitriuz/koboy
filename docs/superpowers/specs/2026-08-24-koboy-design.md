# koboy — Game Boy emulator for modern Kobo e-readers

**Status:** design approved, pre-implementation
**Date:** 2026-08-24
**License:** GPLv3

## 1. Overview

`koboy` is a Game Boy / Game Boy Color emulator frontend for modern Kobo
e-readers. It loads a libretro core (gambatte) at runtime, renders to the
e-ink framebuffer via FBInk, and takes input from the touchscreen plus, where
present, the device's physical page-turn buttons.

It exists because no Kobo Game Boy emulator does. The equivalent work on
Kindle is `crazy-electron/gambatte-k2`, which is a useful reference but not a
portable base — see §2.

### Goals

- DMG and GBC games playable at a usable frame rate on modern Kobo hardware.
- Runs on more than one Kobo model, without a hardcoded device table.
- Installs by unzipping onto the device. No root, no rootfs modification, no
  possibility of bricking.
- Developable and testable on a Linux desktop with no device attached.

### Non-goals for v1

- Audio. The target devices have no speaker; Nickel owns the Bluetooth audio
  stack. Deferred, possibly permanently.
- GBA. A ~1GHz i.MX6-class core will not run mGBA at full speed.
- Save states. SRAM battery saves only.
- In-app ROM browser. ROM path comes from config or argv.
- Colour output on Kaleido panels. See §9 for why this is called out.

## 2. Why not port gambatte-k2

Investigated 2026-08-24. Findings:

- All six `.c` files in that repo are **valac output** — GObject boilerplate,
  `gee.h`, `G_GNUC_CONST`, `*_construct`/`*_get_type` pairs. The `.vala`
  sources are not published, and upstream is not expected to release them.
  Editing 10k lines of generated C that cannot be regenerated is not a
  maintainable base.
- GTK2/X11 is load-bearing there (`main.c` 155 GTK refs, `file_picker.c` 185,
  `gamejoypad.c` 51, `gray_shm.c` is X11 MIT-SHM). Kindle 5.x firmware ships
  an X server and GTK2. Nickel ships neither. The rendering mode that performs
  best on Kindle is unavailable on Kobo by construction.
- Licensed GPLv3.

What is worth taking as **ideas** (not code): its use of FBInk, Bayer ordered
dithering, an RGB565 lookup table, and a data-driven control layout in an
ini file. Its `retrocore.c` structure is a useful reference for the libretro
contract.

## 3. Target devices and support policy

Support is defined by **runtime capability detection, not a model whitelist.**
Model IDs were misidentified twice from memory during design; a hardcoded
table is the wrong instinct. FBInk already maintains device identification and
panel quirks, so `koboy` asks `fbink_get_state()` and derives everything from
the answer.

**Support criterion:** any Kobo that FBInk supports, reporting a panel of at
least 1072x1448. This naturally excludes the older 800x600 generation.

A `TESTED.md` matrix distinguishes *verified on hardware* from *should work,
unverified*. Only the author's Kobo Libra 2 (firmware 4.38.23684) starts in
the verified column.

### Resolved device profile

```c
typedef struct {
    int      scale;             /* computed, never hardcoded */
    int      game_x, game_y;    /* placement of the render rect */
    bool     has_hw_buttons;    /* selects the control layout family */
    uint32_t wfm_fast, wfm_gray, wfm_full;  /* per-platform waveform mapping */
} koboy_profile;
```

Built-in defaults per device class; every field overridable in `koboy.ini`, so
an unseen device is fixable by config edit rather than recompile.

Scale is a **deliberate choice, not the maximum that fits** (see §5). Default
is **5x -> 800 x 720**, which fits every supported panel with room to spare:

| Panel | Devices | Side margin | Space below game rect |
|---|---|---|---|
| 1072 x 1448 | Clara family (6") | 136 px | 728 px |
| 1264 x 1680 | Libra family (7") | 232 px | 960 px |
| 1404 x 1872 | Elipsa family (10.3") | 302 px | 1152 px |
| 1440 x 1920 | Sage (8") | 320 px | 1200 px |

One default therefore covers every target, which is simpler and more
predictable than a per-device computed scale. `scale` is overridable in
`koboy.ini`; if a configured scale does not fit, it falls back to the largest
integer scale that does.

Caveat: a fixed integer scale means *pixel* size is constant while *physical*
size tracks panel DPI. The Libra 2, Clara family and Sage are all 300 DPI, so
5x is ~68 mm wide on each; the Elipsa is 227 DPI, where the same 800 px is
~89 mm. Integer scaling is worth keeping for its exact pixel replication, so
this is accepted rather than corrected.

### Two SoC families

At least three families, not two: 2021-23 devices are NXP i.MX (`FBID`
`mxc_epdc_fb`), Clara BW/Colour and Libra Colour (2024) are MediaTek, and
FBInk additionally exposes an `isSunxi` flag for Allwinner-based models. They
differ in CPU performance and, critically, in which waveform modes exist and
how fast each one is.

This is why `refresh_mode` is an abstraction rather than a waveform
passthrough (§5). Appendix A shows that decision paying for itself: on Mark 9
the conventionally-fastest mode (A2) is **3x slower** than DU4. A passthrough
design would have hardcoded the wrong mode.

## 4. Architecture

```
koboy/
|-- Makefile
|-- src/
|   |-- main.c            emulator loop
|   |-- probe.c           koboy-probe entry point
|   |-- core.c/.h         libretro dlopen + env callbacks
|   |-- video.c/.h        RGB565 -> gray8, scale, dither    (pure)
|   |-- input.c/.h        touch zones + keycodes -> buttons (pure)
|   |-- chrome.c/.h       procedural faceplate, drawn once
|   |-- config.c/.h       koboy.ini parsing
|   |-- platform_if.h     the seam
|   |-- platform_sdl.c    desktop backend
|   `-- platform_kobo.c   FBInk + evdev backend
|-- tests/                host-only, golden-image and event-trace based
|-- third_party/fbink/
|-- scripts/koboy.sh      Nickel stop -> run -> restore
`-- config/koboy.ini
```

### The platform seam

```c
typedef enum {
    KOBOY_REFRESH_FAST,   /* fastest usable mode; DU4 on Mark 9, see App. A */
    KOBOY_REFRESH_GRAY,   /* more levels, slower                            */
    KOBOY_REFRESH_FULL,   /* GC16: full flash, periodic cleanup             */
} koboy_refresh_mode;

typedef struct {
    uint16_t buttons;                            /* RETRO_DEVICE_ID_JOYPAD_* */
    struct { int x, y; bool down; } touch[2];
} koboy_input_state;

typedef struct koboy_platform {
    int      (*init)(void *ctx, const koboy_config *cfg);
    void     (*shutdown)(void *ctx);
    void     (*screen_info)(void *ctx, int *w, int *h);
    int      (*blit_gray8)(void *ctx, const uint8_t *px, int w, int h,
                           int stride, int x, int y);
    int      (*refresh)(void *ctx, int x, int y, int w, int h,
                        koboy_refresh_mode mode);
    int      (*poll_input)(void *ctx, koboy_input_state *out);  /* non-blocking */
    uint64_t (*now_us)(void *ctx);
} koboy_platform;
```

Three properties this buys:

1. **The platform never converts pixels.** `video.c` always hands down gray8
   at final scale, so the entire hot path is platform-independent and
   identical on both backends. No "works on desktop, broken on device" class
   of bug in rendering.
2. **`refresh_mode` is an abstraction.** Waveform constants live only in
   `platform_kobo.c`, keyed by SoC family.
3. **Input normalises at the seam.** Touch-zone hit-testing lives in
   `input.c`, platform-independent, so a mouse on the desktop drives the same
   code path a thumb will.

Cost: one indirect call per frame, irrelevant at ~20fps.

## 5. Video pipeline

### Orientation and why 5x rather than the largest fit

Default is portrait with the grip bezel to the right: the right thumb rests on
the physical buttons, the left thumb reaches the lower-left of the screen. Game
rect top-centred, chrome and controls below.

Render scale defaults to **5x (800 x 720)** rather than the largest scale the
panel allows, for two measured reasons:

1. **Refresh cost scales with area.** Extrapolating the DU4 curve in Appendix A
   (~20 ms fixed + 2.41e-5 ms/px, fit within 5% on the middle measured point):

   | Scale | Game area | Est. fps |
   |---|---|---|
   | 4x | 640 x 576 | ~41 |
   | **5x** | **800 x 720** | **~34** |
   | 6x | 960 x 864 | ~28 |
   | 7x | 1120 x 1008 | ~23 |

   We need ~20 fps to feel smooth, so 5x buys headroom that absorbs the pixel
   pipeline's cost on a single-core A9 and tolerates a worse fixed cost on
   other devices.

2. **The largest fit is bigger than the hardware being emulated.** The original
   DMG screen was roughly 47 x 43 mm. At 300 DPI, 7x is 95 x 85 mm — twice
   original size, a 5-inch Game Boy. 5x is 68 x 61 mm (1.44x original):
   larger and easier on the eyes than the real thing without being absurd.

The freed space is not waste; it is where the chrome lives.

### Frame pacing

The Game Boy runs at 59.73 Hz; the panel cannot. **The core runs at true
wall-clock 60 Hz and only presentation is subsampled.** Emulation is cheap;
presentation is the entire bottleneck. Running the core at display rate would
make games run at a third speed and break timing-sensitive titles.

### Two render paths

DMG has only 4 shades and `DU`/`GL16` offers 16 grey levels, so **DMG renders
exactly, with no dithering at all**, in grey mode. Dithering is required only
for 1-bit fast mode and for GBC colour content.

- **FAST (1-bit):** nearest-scale to output, then Bayer 16x16 -> 1 bit.
  Mid-tones become dot patterns within each scaled block.
- **GRAY (4-bit):** DMG's 4 shades -> 4 exact levels, no dither. GBC ->
  luminance, also quantised to **4** levels, with dither available to convey
  tone.

  Correction after measurement: this section originally specified 16 levels for
  GBC. Appendix A rules that out --- the 16-level waveforms measured 321.7 ms
  (GL16, 3.1 fps) and 393.3 ms (GC16, 2.5 fps) for a full game rect, against
  46.7 ms for the 4-level DU4. A 16-level GBC path would be unplayable, so DU4's
  four levels are the only viable target for GBC as well as DMG. This is why the
  implementation ships `video_quantise4` and `video_dither_1bit` and no 16-level
  quantiser.

### Ordered dithering is a correctness requirement

Dither is applied **after** scaling, indexed by **absolute screen
coordinates**. A given input value at a given screen position therefore always
yields the same output bit, so static regions produce byte-identical output
frame to frame — which is what makes dirty-rect skipping possible.

Error diffusion would break this: its noise pattern shifts globally with any
input change, nothing is ever skippable, and on e-ink the result is shimmer
plus panel-wide ghosting. Ordered dither is the only option that composes with
partial updates.

### Dirty rectangles

Previous and new output are diffed on an 8x8 tile grid; only the bounding box
of changed tiles is refreshed. Presentation is skipped entirely when nothing
changed (including when the core reports a duplicate frame via `GET_CAN_DUPE`).

Expected to be the difference between unplayable and playable for static-ish
games. Full-screen scrollers get no benefit and hit the worst case.

### Inner loop

Output is a nearest-neighbour **integer** scale of a 160x144 source. Each
source pixel becomes a solid `scale`-wide run; each source row becomes `scale`
identical output rows. Run patterns are precomputed per (shade x dither row
phase) and emitted with `memcpy`, then rows replicated with `memcpy`. Per-pixel
work collapses into block copies. NEON is available if measurement demands it,
but is not required to start.

### Ghost cleanup

Every N presented frames (configurable, ~200) and on pause/exit, one
`KOBOY_REFRESH_FULL` flashes clean — **scoped to the game rect only, never the
full panel**, so the static chrome is never disturbed.

### Static chrome

At 5x the game rect is under a third of the panel. The surrounding area is
drawn once as a Game Boy-style faceplate: bezel around the screen, d-pad and
button glyphs, Start/Select labels, and a status line (battery, game title).

- **Zero per-frame cost.** Drawn once at startup and never refreshed, exactly
  like the control layout in §7. Chrome elaborateness is free at runtime, so
  this is an authoring question rather than a performance one.
- **Drawn with `KOBOY_REFRESH_FULL` (GC16, 16 levels).** Static chrome gets
  full panel quality while only the game rect runs the fast 4-level DU4 path.
  One ~400 ms refresh at launch buys crisp chrome for the whole session.
- **Procedural, not a bitmap.** Geometry derives from the same control-area
  percentages as §7, so one implementation adapts to every panel size. A fixed
  PNG would need authoring per device class and would undercut §3.

## 6. Core integration

### Symbols loaded

`retro_api_version` (assert == 1), `retro_set_environment` (before
`retro_init`), `retro_set_video_refresh`, `retro_set_audio_sample`,
`retro_set_audio_sample_batch`, `retro_set_input_poll`,
`retro_set_input_state`, `retro_init`, `retro_deinit`,
`retro_get_system_info`, `retro_get_system_av_info`, `retro_load_game`,
`retro_unload_game`, `retro_run`, `retro_get_memory_data`,
`retro_get_memory_size`, `retro_reset`.

Serialisation symbols are deliberately not wired (no save states in v1).

### Environment callbacks

Unknown calls return `false`, which is correct and expected. Five matter:

| Callback | Response | Rationale |
|---|---|---|
| `SET_PIXEL_FORMAT` | accept RGB565 and XRGB8888 | RGB565 is what the 65536-entry grey LUT targets; refusing outright pushes the core to legacy 0RGB1555 |
| `GET_VARIABLE` | serve our own core options | see below |
| `GET_CAN_DUPE` | `true` | lets the core signal "identical frame" with a NULL framebuffer — a free skip feeding dirty-rect logic |
| `GET_AUDIO_VIDEO_ENABLE` | video on, audio off | v1 has no audio; tells gambatte to skip generating it |
| `GET_SAVE_DIRECTORY`, `GET_SYSTEM_DIRECTORY` | our paths | SRAM location, optional GBC boot ROM |

Core options that must be set:

- **`gambatte_mix_frames=disabled`** — it blends consecutive frames, which on
  e-ink means every pixel changes every frame. Destroys dirty-rect tracking
  and layers simulated ghosting on top of real ghosting. Actively harmful.
- **`gambatte_gbc_color_correction=disabled`** — we convert to grayscale;
  colour correction only distorts the luminance mapping.

### Audio still needs a stub

Even with audio reported off, a real `audio_sample_batch` callback must be
installed that accepts and discards; a core calling a NULL pointer crashes.

### Obtaining a core that runs on Kobo

The libretro buildbot's prebuilt `linux-armhf` core will very likely fail to
load, for three independent reasons:

1. **glibc version** — built against a much newer glibc than Kobo's rootfs;
   fails at `dlopen`.
2. **libstdc++** — gambatte is C++, and Kobo's libstdc++ is older than any
   modern toolchain's.
3. **CPU baseline** — i.MX devices are Cortex-A9 (ARMv7+NEON); the 2024
   MediaTek devices are Cortex-A53 running a 32-bit userland. A core tuned for
   A53 will SIGILL on an A9.

Plan:

1. Build **koxtoolchain** -> `arm-kobo-linux-gnueabihf`.
2. Build gambatte-libretro with `-march=armv7-a -mfpu=neon -mfloat-abi=hard`
   (the common denominator across both families).
3. Link `-static-libstdc++ -static-libgcc` so the C++ runtime travels with the
   core and the device's version is irrelevant.
4. Verify the dependency closure is only `libc`, `libm`, `libdl`,
   `libpthread`. `lddtree` and `eu-readelf` are already present on-device in
   NiLuJe's toolbox, so this is verifiable on the real target.

The prebuilt core is worth a five-minute try as a shortcut; the design assumes
we build it.

`dlopen` over static linking keeps the core swappable and matches the
ecosystem. Cost: core-load failure is a runtime error, so it must be reported
on-screen (§8), not silently.

### SRAM durability

Battery saves are the only user data we own, and e-readers get killed
unceremoniously.

- Load into `retro_get_memory_data(RETRO_MEMORY_SAVE_RAM)` after
  `retro_load_game`.
- Flush to `<rom>.srm` on exit, on `SIGTERM`, and on a periodic timer while
  dirty.
- Write **atomically** (temp file + `rename`) so a kill mid-write cannot
  corrupt an existing save.

### Threading

Single-threaded for v1, with one requirement: the Kobo backend issues
refreshes **without waiting for completion**, because a blocking refresh ioctl
would stall emulation for the panel's update duration. Threading is revisited
only if measurement shows non-blocking refresh is unavailable.

## 7. Input model

### Normalisation

`input.c` consumes raw evdev events from up to two nodes — touchscreen (all
devices) and key device (button-equipped models only) — and produces one
libretro joypad bitmask. `main.c` is therefore identical across device
classes.

Device nodes are classified with `fbink_input_scan()` rather than a hardcoded
`event*` number, which varies by device and firmware.

### Touch protocol and coordinates

Kobo panels are inconsistent: some use single-touch (`ABS_X`/`ABS_Y` +
`BTN_TOUCH`), newer ones multi-touch protocol B (`ABS_MT_SLOT` +
`ABS_MT_TRACKING_ID`). Coordinate transforms — axis swap, X/Y mirror, panel
rotation — vary per model and are a known source of breakage. The transform
comes from the FBInk-resolved profile; we do not hand-roll a quirks table.

The probe includes a touch verification mode (crosshairs plus raw and
transformed coordinates) so an unseen device is validated in seconds.

### D-pad: relative thumb-pad

A touch d-pad with no tactile feedback, on a panel that confirms input ~50ms
late, is the main threat to playability. Default is a **relative thumb-pad**:
the first touch in the pad region sets an origin, direction derives from
displacement past a dead zone. It is self-centering — the thumb never has to
find a fixed spot. A fixed 8-zone cross is available in config.

**Dead zone and hysteresis are mandatory** in both modes: a direction must be
exited by a margin before releasing, or boundary flicker is constant and the
player has no timely visual cue to correct by.

### Simultaneous touches

- **Touch-only devices** (Clara family, Elipsa) require at least 2 tracked
  points as a hard floor: one thumb on the d-pad, one on A/B. Measured
  headroom is far larger than assumed — the Libra 2 panel reports 10 slots
  (Appendix A) — so a full touch-only layout is comfortably feasible.
- **Button-equipped devices** (Libra 2, Sage, Libra Colour) require only
  **one**, because A/B are hardware. A concrete ergonomic advantage, not a
  cosmetic one.

Maximum slots per panel is a probe output.

### Polling rate

Input is drained **every core iteration (60 Hz)**, not every presented frame
(~20 Hz). Polling only on presentation would drop short presses and add up to
50ms of latency on top of the panel's own. Mechanically: libretro invokes
`input_poll`, we drain evdev non-blocking and latch a bitmask, then answer
`input_state` from the latch.

### EVIOCGRAB — measured, and it forces the takeover

**Nickel holds `EVIOCGRAB` on `event0`, `event1` and `event2`** (measured
2026-08-24, Appendix A). A grabbed evdev node delivers events exclusively to
the grabbing process, so while Nickel runs no other process can read input at
all — not the buttons, not touch, not even the accelerometer.

Two consequences:

1. **Stopping Nickel is mandatory, not merely preferable.** Approach A in §8 is
   the only workable model for anything that reads input. Coexistence is
   impossible by kernel design, not by contention.
2. Once Nickel is stopped nothing competes for input, so our own grab is
   belt-and-braces rather than essential. Grab the touchscreen and key nodes;
   **never** the power button. Release on exit and in signal handlers. A hard
   crash while grabbed can leave touch unresponsive until reboot — recoverable,
   and configurable off.

### First-run button calibration

Rather than shipping a per-device keycode table, `koboy` **learns the mapping
on first run**: it draws "press the button you want as A", reads the first key
event, repeats for B, and writes the result to `koboy.ini`.

This is §3's capability-detection philosophy applied to input. A device nobody
has ever tested gets working buttons with no code change and no keycode
research, and it removes the need to ever determine which physical button emits
which code. Built-in defaults still ship as a starting guess; calibration
overrides them and can be re-run from config.

### Power button

With Nickel stopped nothing manages sleep, which conveniently removes
auto-suspend mid-game. For v1 the power button quits cleanly and the launch
script restores Nickel. Real suspend/resume across a stopped Nickel is a
rabbit hole with no v1 payoff.

### Layout as data

Control layouts live in `koboy.ini` as **percentages of the control area**,
in two families keyed off `has_hw_buttons`. Percentages mean a layout authored
on a Libra 2 lands correctly on a 1072x1448 Clara with no edits.

The layout is drawn **once** at startup with a full refresh and never touched
again, so controls cost nothing per frame.

Deliberate omission: **no visual press feedback.** It would cost an extra
refresh region per input, and at e-ink latency the confirmation arrives too
late to be useful — paying fps to make the experience worse.

## 8. On-device story

### Nickel takeover, and an 8bpp win

Stopping Nickel means terminating its process group and restoring on the way
out. KOReader performs this dance on the same hardware, so the sequence is
settled fact; our script is written from studying its behaviour rather than
copying its file, since KOReader is AGPL and this project is GPLv3.

`fbdepth` (present on-device in NiLuJe's toolbox and shipped with KOReader)
switches the framebuffer to **8bpp** and restores it on exit. Our pipeline
already outputs gray8, so:

> 8bpp framebuffer + gray8 output = zero format conversion and a quarter of
> the memory bandwidth of a 32bpp fb for the same refresh region.

A direct fps win costing one line in the launch script, available only because
we take Nickel over.

### Crash recovery

A crash with Nickel stopped looks identical to a brick. Recovery is designed
in:

- **Restore is unconditional.** The wrapper traps `EXIT INT TERM`; fb depth
  restore and Nickel restart live in the trap, never the happy path.
- **Fatal errors render to the screen** via FBInk, wait for a tap, then
  restore. With no terminal, an unreported error is a mystery.
- **Signal handlers do the minimum**: flush SRAM, release `EVIOCGRAB`, restore
  fb depth. Deliberately minimal — real work in a `SIGSEGV` handler is its own
  hazard.
- **A log** at `.adds/koboy/koboy.log`, readable over USB, recording the
  resolved device profile at every startup so bug reports carry hardware
  context.

### Packaging: no rootfs modification

`koboy` needs no `KoboRoot.tgz` and no root. Everything lives under
`.adds/koboy/` on the user-visible partition: binary, core `.so`, config,
ROMs, saves, log. Install is unzip; uninstall is delete-a-folder. No step can
leave the device unbootable.

To verify rather than assume: the executable bit on a FAT partition. KOReader
runs its binary from the same location so the mount evidently permits it, but
the launcher invokes the wrapper as `sh .../koboy.sh` so nothing depends on
`+x` for the script.

### Launch integration

NickelMenu is primary, matching the config style already on the target device:

```
menu_item : main : koboy : cmd_spawn : quiet : exec /mnt/onboard/.adds/koboy/koboy.sh
```

A KFMon config ships as an optional extra for one-tap launch from the library.

### The probe ships first, and ships safe

The probe has **two modes**, because Nickel's input grab (§7) splits its job in
half:

- **Coexisting mode** — refresh timing across regions and waveform modes, plus
  device identification and input *capability* dumps read from
  `/proc/bus/input/devices`. Needs no takeover, no fb depth change and no
  restore path, so it cannot leave the device in a bad state. This is how
  Appendix A was produced.
- **Takeover mode** — anything that requires actually *reading* input events.
  Impossible alongside Nickel, since a grabbed node delivers events only to the
  grabber. This mode stops Nickel and carries the full restore path of §8.

Either way it writes `koboy-probe-<device>.txt` to the root of the FAT
partition for retrieval over USB.

The original plan had the probe answering the keycode question in coexisting
mode. That turned out to be impossible, which is why §7 moves button mapping to
first-run calibration — a better answer anyway, since it also covers devices
nobody has tested.

Honest caveat: measuring while Nickel is alive makes fps numbers slightly
**pessimistic**, since Nickel may repaint underneath. Acceptable for the
fps-vs-region-size curve, which is what the design needs. A `--takeover` flag
gets clean numbers once the restore path is trusted.

## 9. Deliberate future breaking change

Clara Colour and Libra Colour have Kaleido 3 filter arrays. Grayscale
rendering is unaffected at full resolution, but the CFA cuts contrast and those
panels will likely need their own waveform tuning.

Rendering GBC in actual colour on those devices is attractive and explicitly
out of scope. It would require the pipeline to emit something other than
gray8, i.e. a change to `blit_gray8`. This is recorded as an **accepted future
breaking change to the blit seam**, not designed around now.

## 10. Build targets

| Target | Produces |
|---|---|
| `make host` | SDL binary + unit tests, runnable on a desktop |
| `make kobo` | cross-compiled `koboy` + `koboy-probe` |
| `make dist` | `koboy-<ver>.zip`, laid out for drag-and-drop install |
| `make probe-dist` | probe-only zip, deployable before anything else exists |

## 11. Testing strategy

- `video.c` and `input.c` are pure functions, unit-tested on the host.
  Rendering is verified against golden images; input against synthetic event
  streams.
- **Real hardware traces without hardware:** `evemu-record` and `evemu-play`
  are already on the target device. Once connected, real thumb-on-d-pad and
  button traces are captured and replayed as host-side fixtures, so tests run
  against genuine hardware behaviour while the device is absent.
- `platform_sdl.c` makes the whole emulator runnable and playable on the
  desktop, so integration regressions surface without the device.

## 12. Open measurements

Resolved on hardware 2026-08-24; see Appendix A for data.

| Unknown | Status |
|---|---|
| Achievable fps vs region size, per waveform mode | **Resolved.** DU4 gives 21 fps at the full 7x rect, 44 fps at a small dirty rect. A2 is unusable on this platform (6.8 fps) |
| Whether refresh can be issued non-blocking | **Resolved.** Yes — 28ms submission vs 47ms blocking. No worker thread needed for v1 |
| Touch protocol variant and max slots | **Resolved.** Protocol B, 10 slots, axes transposed relative to the panel |
| Which `/dev/input/event*` node is which | **Resolved.** event0 keys, event1 touch, event2 accelerometer |
| Page-turn button keycodes | **Resolved by design, not measurement.** Nickel's grab makes capture impossible without stopping it, so the mapping is learned by first-run calibration (§7). gpio-keys advertises KEY_F1(59), KEY_POWER(116), KEY_F23(193), KEY_F24(194) as the candidate set |
| Whether `EVIOCGRAB` is needed once Nickel is stopped | **Resolved.** Nickel holds the grab on all three nodes, so stopping it is mandatory; our own grab is then optional hardening |

## 13. Risks

- **The fps ceiling is measured, and the premise holds.** ~21 fps at the full
  7x game rect using DU4, under deliberately pessimistic conditions (Nickel
  running, worst-case full black/white transitions, 32bpp, per-refresh process
  spawn). Dirty rects, non-blocking submission and the 8bpp path all push
  further up from there. This risk is **retired**.
- **A2-only devices may not be viable.** A2 shows a nearly flat cost curve
  (~120-148ms regardless of area), so on any device lacking DU4 neither dirty
  rects nor a smaller render scale will help much. Such devices may cap around
  7 fps. Unknown until one is tested; does not affect Mark 9 targets.
- **Untestable device coverage.** Only the Libra 2 can be verified. Mitigated
  by capability detection, config-overridable profiles, and a probe that
  generates a profile for any device, making community contribution the growth
  path for `TESTED.md`.
- **Nickel restore path.** A bug here is alarming even though it is reboot
  recoverable. Mitigated by unconditional trap-based restore and by the probe
  not needing takeover at all.

## 14. Build order

1. `koboy-probe` — captures button keycodes (the one §12 item still open) and
   generates device profiles for hardware the author does not own. Zero device
   risk; remains the growth path for `TESTED.md`.
2. Platform seam + `platform_sdl.c` — playable-on-desktop skeleton.
3. `core.c` — libretro contract, verified on the host with a desktop-built
   gambatte core.
4. `video.c` — dither/scale/dirty-rect pipeline, golden-image tested.
5. `input.c` — thumb-pad and zone logic, trace tested.
5b. `chrome.c` — procedural faceplate; pure geometry, golden-image tested.
6. `platform_kobo.c` — FBInk + evdev, informed by probe results.
7. `scripts/koboy.sh` + packaging + `TESTED.md`.

---

## Appendix A — Measured hardware facts

Kobo Libra 2, firmware 4.38.23684, measured 2026-08-24 over SSH.

### Platform

| Fact | Value |
|---|---|
| FBInk identity | `Libra 2`, id 388, codename `Io`, platform **Mark 9** |
| FBInk build on device | v1.24.0-78, at `/usr/bin/fbink` |
| Kernel | 4.1.15, `armv7l` |
| CPU | ARM **Cortex-A9** (part 0xc09), **single core**, NEON + VFPv3 |
| RAM | 507600 kB total; 236824 kB free with Nickel running |
| fb driver | `mxc_epdc_fb` (NXP EPDC), `isSunxi=0` |
| Panel | 1264 x 1680, 300 DPI, **BPP=32**, `lineLength=5120` |
| Stride padding | 5120 / 4 = **1280 px stride vs 1264 visible** — blits must honour it |
| Vertical origin | `viewVertOrigin = viewVertOffset = 8` --- **NOT a blit offset, see correction below** |
| Rotation | `currentRota=1` (CW 90), canonical 0, `ntxRotaQuirk=5`, `canRotate=1` |
| Extras | `canHWInvert=1`, `hasEclipseWfm=1`, `isKoboNonMT=0` |

Single-core A9 confirms the `-march=armv7-a -mfpu=neon` baseline in §6, and
independently confirms GBA was correctly ruled out.

### Waveform performance, full 7x game rect (1120x1008), blocking

| Mode | ms/refresh | fps |
|---|---|---|
| **DU4** | **46.7** | **21.4** |
| A2 | 146.7 | 6.8 |
| DU | 215.0 | 4.7 |
| GL16 | 321.7 | 3.1 |
| GC16 | 393.3 | 2.5 |

**DU4 is the fast path on Mark 9, not A2.** DU4 is also a *4-level* mode,
which is exactly DMG's palette — so the no-dither grey path of §5 becomes the
primary path rather than a fallback, and Bayer dithering is needed only for
GBC content or on devices without DU4.

### Region-size scaling

| Region | A2 | DU4 |
|---|---|---|
| 1120x1008 | 148.3 ms | 47.5 ms |
| 1120x504 | 127.5 ms | — |
| 560x504 | 117.5 ms | 25.8 ms |
| 320x288 | 120.0 ms | 22.5 ms |
| 160x144 | 145.8 ms | — |

DU4 scales with area (~19ms fixed + area term), so **dirty rectangles pay
off**. A2 is nearly flat, i.e. dominated by fixed per-update cost — dirty
rects cannot rescue an A2-only device.

### Blocking vs non-blocking submission

| Mode | submit only | blocking |
|---|---|---|
| A2 1120x1008 | 26.7 ms | 146.7 ms |
| DU4 1120x1008 | 28.3 ms | 47.5 ms |

Non-blocking submission works, confirming §6's single-threaded design.

### Input devices

| Node | Device | Capabilities |
|---|---|---|
| `event0` | `gpio-keys` | EV_KEY + EV_SW; codes KEY_F1(59), KEY_POWER(116), KEY_F23(193), KEY_F24(194) |
| `event1` | `Elan Touchscreen` | protocol B, `PROP_DIRECT`, 10 slots (`ABS_MT_SLOT` max 9) |
| `event2` | `kx122-accel` | accelerometer (ABS_X/Y/Z) — orientation sensing, unused |

Touchscreen axis ranges: **`ABS_X`/`ABS_MT_POSITION_X` max 1680**,
**`ABS_Y`/`ABS_MT_POSITION_Y` max 1264**. The panel is 1264 wide x 1680 tall,
so **touch axes are transposed relative to the panel** — precisely the quirk
class §7 anticipated, now measured rather than guessed. Also present:
`ABS_MT_TRACKING_ID`, `ABS_MT_PRESSURE`, `ABS_PRESSURE` (max 4095),
`ABS_MT_TOUCH_MAJOR`, `BTN_TOUCH`.

### Method and caveats

Each refresh was a solid-fill of the region alternating black/white via
`fbink -k <region> -W <mode>`, i.e. **maximum pixel transition** — real Game
Boy frames change far less. Measurements include ~5 ms/iteration of process
spawn and FBInk init (measured separately via `fbink -e`), which the real
in-process application will not pay. Nickel was left running throughout, which
per §8 makes these numbers **pessimistic**. Timing resolution is 10 ms
(`/proc/uptime`, USER_HZ=100) over 12 iterations per cell.

### Input exclusivity (measured)

Nickel holds open descriptors on `/dev/input/event0`, `event1` and `event2`
simultaneously, and holds `EVIOCGRAB` on them. Verified three ways:

- A 35-second `dd if=/dev/input/event0` returned **0 bytes** while page-turn
  buttons were being pressed.
- `evtest --grab` could not acquire the grab, reporting the device busy.
- `event2` (accelerometer, which emits on movement regardless of what is on
  screen) also returned 0 bytes, ruling out any "wrong app in foreground"
  explanation.

Companion processes observed alongside Nickel, which the §8 restore path needs:

| PID | Command |
|---|---|
| 227 | `/usr/local/Kobo/hindenburg` |
| 228 | `/usr/local/Kobo/nickel -platform kobo -skipFontLoad` |
| 1405 | `/bin/sh /usr/local/Kobo/sickel-launcher.sh` |
| 1406 | `/usr/local/Kobo/sickel -platform kobo:noscreen` |

Observing these directly removes any need to derive the restart sequence from a
third-party script.

---

## Appendix B — Second measurement session (2026-08-25)

Taken after the default render scale changed to 5x, and after two failed
attempts to build a cross-toolchain. Same device, same firmware.

### Toolchain ground truth (this supersedes assumptions in section 6)

| Fact | Value |
|---|---|
| Device glibc | **2.19** (`GNU C Library (crosstool-NG 1.24.0.103_75d7525) ... version 2.19`) |
| Misleading symlink | `/lib/libc.so.6 -> libc-2.11.1.so` --- the filename was kept across an upgrade; the library itself reports 2.19 |
| Shell | busybox `ash`; no `ldd` on device |
| Dependency closure of a **working** Kobo ARM binary (`/usr/bin/fbink`) | **`libm.so.6` and `libc.so.6` only** |
| Same for KOReader's `fbink` | identical --- `libm`, `libc` |
| Build attributes | `Advanced_SIMD_arch: NEONv1` |

Consequences:

- **The cross-toolchain target must produce binaries against glibc <= 2.19.** A
  distro `arm-linux-gnueabihf` toolchain targets a far newer glibc and its
  dynamically-linked output will not run here, so that shortcut is closed.
- **koxtoolchain's glibc 2.15 target is older than necessary.** 2.15 is safe
  (older links run on newer) but the actual floor is 2.19, and glibc 2.15's
  configure scripts loop indefinitely on a 2026 host --- see the failure note
  below. A prebuilt toolchain targeting any glibc <= 2.19 would sidestep the
  build entirely.
- **`verify-core.sh`'s acceptance criterion is confirmed correct by ground
  truth**: real Kobo binaries carry only `libm` + `libc`. The prebuilt libretro
  ARM core failed exactly this check (dynamic `libstdc++`, no NEON attributes).

### Refresh at the shipped 5x rect (800 x 720), which was never measured before

| Mode | ms/refresh | fps |
|---|---|---|
| **DU4, non-blocking** | **15.0** | **66.7** |
| DU4, blocking | 39.2 | 25.5 |
| A2, blocking | 135.8 | 7.4 |
| GL16, blocking | 310.8 | 3.2 |

Non-blocking DU4 is the figure that governs the real emulator, since the main
loop issues refreshes without waiting for completion. 66.7 fps against a ~20 fps
requirement is comfortable headroom.

A2 remains ~3.5x slower than DU4, so the Appendix A waveform decision holds
robustly across sessions.

### Measurement variance --- read Appendix A's absolute numbers with caution

Re-measuring the **same** 7x rect (1120 x 1008) with the **same** DU4 waveform
today gave **67.5 ms / 14.8 fps**, against Appendix A's **46.7 ms / 21.4 fps**.
That is 45% run-to-run variance on identical parameters, far beyond the 10 ms
clock resolution. E-ink refresh timing is temperature- and controller-state
dependent, so:

- Treat all absolute ms/fps figures as **order-of-magnitude with ~50% spread**,
  not precise constants.
- The *relative* findings are what survive: DU4 beats A2 by ~3.5x, cost scales
  with area, and non-blocking beats blocking by ~2.6x. Every design decision
  rested on those ratios, and all of them reproduced.
- The section 5 extrapolation predicted ~34 fps for 5x from the 7x fit; the
  measured blocking figure is 25.5 fps. The model was optimistic in absolute
  terms while correct in ordering.

### Toolchain build failure (recorded so it is not retried blindly)

Two attempts at koxtoolchain both hung in **glibc 2.15's `./configure`**, right
after `running configure fragment for ports/sysdeps/arm/elf`, emitting one
repeated line indefinitely (2.37M of 2.63M log lines). Binutils and the core
compiler built successfully; the final gcc never ran. This is a 14-year gap
between a 2012 libc's autoconf idioms and a 2026 host, not a misconfiguration.
Viable routes: a prebuilt toolchain targeting glibc <= 2.19, or building
koxtoolchain inside a container with a period-appropriate host.

---

## Appendix C — Correction: `viewVertOrigin` is not a framebuffer offset

Appendix A recorded `viewVertOrigin = 8` and section 8 stated that blits must
honour it. **That was wrong**, and using 8 as a pixel origin would have rendered
every frame 8 px too low on the panel.

FBInk folds its own text row-balancing `viewVertOffset` into the reported
`viewVertOrigin`. The Libra 2's quirk table sets no `koboVertOffset`, so the
real viewport origin is `origin - offset` = **0**. The Task 17 implementer
derived this by reading FBInk's source; it was then confirmed by measurement on
the device:

```
viewHeight=1680   screenHeight=1680
viewVertOrigin=8  viewVertOffset=8
```

`viewHeight == screenHeight == 1680` is the decisive evidence. A genuine 8 px
framebuffer origin would leave only 1672 viewable rows; the two being equal
proves there is no framebuffer offset at all. The 8 is FBInk's own text-row
shifting, which is why its startup log says "Vertical fit isn't perfect,
shifting rows down by 8 pixels" --- a statement about text placement, not about
the framebuffer.

The **stride** fact from Appendix A stands unchanged and does still bind blits:
`lineLength = 5120` at 32bpp is 1280 px of stride against 1264 visible.

### Two further corrections from the same task

**`hasEclipseWfm` gates DU4.** FBInk silently downgrades `WFM_DU4` to GC4 unless
that quirk is set, so a backend claiming DU4 without checking it would be making
a false claim. Measured on this device: `hasEclipseWfm=1`, `devicePlatform='Mark
9'` --- so DU4 genuinely engages here, but the check is required for correctness
on other models.

**The power button cannot be excluded from an input grab.** Section 7 required
grabbing the key node while never grabbing the power button. On this hardware
those are the same device --- page-turn keys and power both live on `gpio-keys`
(`event0`) --- so the instruction was impossible as written. The resolution is to
read that node without grabbing it at all.

---

## Appendix D — Corrections from the device phase (Tasks 17 and 19)

Everything below was learned by running koboy on a real Kobo Libra 2. None of it
was reachable from the desktop backend, and all of it overrides earlier sections.
Recorded here because the task reports it came from do not survive the run.

### 1. The fast waveform defaults to AUTO, not DU4 — this supersedes section 5

Section 5 and Appendix A concluded that `KOBOY_REFRESH_FAST` should map to DU4,
on the strength of DU4 being ~3.5x faster than A2. **That conclusion was drawn
from a benchmark that could not see the failure mode.** The measurement painted
solid rectangles, so it never tested *erasing*, and DU4 — a fast non-flashing
waveform — cannot cleanly erase. In play this produced severe ghosting: a
falling piece drew at each new position while its previous positions were never
cleared, so the game looked frozen when it was in fact advancing.

The fix came from studying how the Kindle equivalent handles refreshing, and the
finding was an absence: **it never selects a waveform at all.** It refreshes
without specifying one and lets the EPDC driver choose. The controller already
inspects the actual pixel transitions per update region and picks a waveform
accordingly — which is exactly the "does this region need erasing?" question we
were about to answer in software.

Measured on the device, same content, same session, three A/B pairs:

| Policy | mean | worst case |
|---|---|---|
| **AUTO** | **155-164 us** | **629 us** |
| forced DU4 | 206-385 us | 8551 us |

AUTO is both cheaper and far more predictable. `waveform_fast = auto` is the
default; `du4` remains selectable. Confirmed in play: no ghosting, and the user
completed a full game.

DU4 is also **gated on `hasEclipseWfm`** — FBInk silently downgrades DU4 to GC4
without that quirk, so claiming DU4 without checking it would be a false claim.

### 2. The shipped display defaults are the ones validated on hardware

Two mitigations written for forced DU4 became redundant under AUTO and were the
sole cause of observed flashing. A device trace settled it: 35 AUTO refreshes on
small rects, **none** flashing; 21 GC16 flashes, **all** attributable to the
area threshold; and **zero** scheduled cleanups ever firing.

| Key | was | ships as |
|---|---|---|
| `full_refresh_permille` | 450 | 1000 |
| `cleanup_interval` | 60 | 0 |
| `cleanup_max_ms` | 3000 | 0 |
| `dpad_mode` | relative | cross |

`full_refresh_permille = 1000` is **not** unreachable, and an earlier version of
this note wrongly said so. The comparison is `dirty * 1000 >= whole * permille`,
so at 1000 it fires when the dirty rect covers the game rect corner to corner —
reachable on a full-screen wipe, and correct behaviour when it happens. A value
of **1001 or above** is a provable never, since the dirty rect is a bounding box
contained within the game rect.

### 3. The chrome and the input model must agree — `dpad_mode = cross`

Section 7 chose a relative thumb-pad because the device has no tactile
landmarks. But the chrome draws an absolute **cross**, and users press its arms.
A relative pad produces no direction from a tap — only from a drag — so the
d-pad was effectively unusable while A, B and Start (genuine absolute zones)
worked fine. Cross mode ships as the default. The lesson generalises: the drawn
UI is the part people trust, so the input model has to match the drawing.

### 4. `dlopen` never searches the current directory

`config_defaults` originally set a bare `core_path = "gambatte_libretro.so"`.
A name containing no slash sends `dlopen` to the system library paths and
**never** the current directory, so the core failed to load on the device while
sitting beside the binary. Bare names now resolve against the executable's own
directory via `/proc/self/exe`; a path containing a slash is honoured verbatim.
No host test could have caught this — on the desktop the core is always passed
as an absolute path.

### 5. Restarting Nickel requires Nickel's own environment

Section 8's restore path was a bare re-exec. **That corrupts the device.**
Nickel started outside its normal init environment rewrites
`/mnt/onboard/.kobo/version` with a placeholder serial and an empty device code,
after which FBInk reports `Unknown!` / `Mark ?` / `hasEclipseWfm=0` — breaking
per-device quirks for every FBInk tool on the device, KOReader included. Only a
reboot repairs it.

KOReader avoids this by being launched *by* Nickel and inheriting its
environment. So `koboy.sh` gates on that environment being present
(`PLATFORM`, `PRODUCT`, `NICKEL_HOME`) and refuses to restart Nickel without it,
rendering the reason on the panel and exiting non-zero. A menu launch exits
cleanly with no reboot; an SSH launch cannot corrupt anything. The gate is
spoofable by exporting those names — nothing in userspace can prove a process's
parent — but it stops the accident, which is what it exists for.

Three further corrections to section 8's restore sequence:

- **`fbdepth -r` is not a restore.** `-r` is `--rota`, requires an argument, and
  exits 255. Read depth and rotation with `-g`/`-o` and restore by name.
- **Do not unmount `/mnt/onboard`.** KOReader unmounts the *external* `/mnt/sd`;
  unmounting onboard pulls the launcher's own partition out from under it.
- **WiFi must come down before Nickel restarts.** Leaving it up made the
  restarted Nickel fail to insert its own driver (`File exists`) and the device
  rebooted itself ~3.5 minutes later.

### 6. A missing touchscreen is a warning, not a fatal error

Section 8 listed "no touchscreen found" among the fatal paths. It is instead a
warning: a device with working hardware buttons is usable, and refusing to start
would be worse than running degraded. `dlopen` failure and an unreadable ROM do
remain fatal, and render on the panel.
