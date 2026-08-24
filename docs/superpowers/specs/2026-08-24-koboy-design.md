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

`scale = min(panel_w / 160, game_area_h / 144)`:

| Panel | Devices | Scale | Game area |
|---|---|---|---|
| 1072 x 1448 | Clara family (6") | 6x | 960 x 864 |
| 1264 x 1680 | Libra family (7") | 7x | 1120 x 1008 |
| 1404 x 1872 | Elipsa family (10.3") | 8x | 1280 x 1152 |
| 1440 x 1920 | Sage (8") | 9x | 1440 x 1296 |

### Two SoC families

2021-23 devices are NXP i.MX; Clara BW/Colour and Libra Colour (2024) are
MediaTek MT8113. They differ in CPU performance and, more importantly, in
e-ink controller behaviour — A2 does not perform identically across them.
This is why `refresh_mode` is an abstraction rather than a waveform
passthrough (§5): the fix is a per-family mapping table.

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
    KOBOY_REFRESH_FAST,   /* A2-ish: 1-bit, fastest, most ghosting */
    KOBOY_REFRESH_GRAY,   /* DU/GL16: more shades, slower          */
    KOBOY_REFRESH_FULL,   /* GC16: full flash, periodic cleanup    */
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

### Orientation

Default is portrait with the grip bezel to the right: the right thumb rests on
the physical buttons, the left thumb reaches the lower-left of the screen. Game
area top-centred, control area below. Landscape (one scale step larger on some
devices) is available via config but puts the buttons on an edge.

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
  luminance, quantised to 16.

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
`KOBOY_REFRESH_FULL` flashes the panel clean.

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
  points as a hard floor: one thumb on the d-pad, one on A/B.
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

### EVIOCGRAB

Grab the touchscreen and key nodes; **never** the power button. Release on exit
and in signal handlers. Stated cost: a hard crash while grabbed can leave touch
unresponsive until reboot. Recoverable, judged worth it, and configurable off.

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

`koboy-probe` deliberately **coexists with Nickel**: no takeover, no fb depth
change, no restore path, so it cannot leave the device in a bad state. It
times refreshes across region sizes and waveform modes, classifies input
devices, dumps keycodes and touch slot counts, and writes
`koboy-probe-<device>.txt` to the root of the FAT partition for retrieval over
USB.

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

These are unknown hardware facts, not undecided design. Each is a probe output
with a bounded blast radius.

| Unknown | Resolved by | Impact if unfavourable |
|---|---|---|
| Achievable fps vs refresh region size, per waveform mode | probe | Retunes the waveform mapping table and default render scale; no architectural change |
| Whether refresh ioctl can be issued non-blocking | probe | Reintroduces the worker thread deferred in §6 |
| Page-turn button keycodes | probe (`evtest`-equivalent) | Populates a keycode map |
| Touch protocol variant and max slots | probe | Selects a protocol handler in `input.c` |
| Whether `EVIOCGRAB` is needed once Nickel is stopped | probe | Config default flip |

## 13. Risks

- **The fps ceiling is unmeasured.** If fast-mode refresh of a full game rect
  lands near single-digit fps, the project is a curiosity rather than a
  handheld. Mitigated by dirty rects, the 8bpp path, and a configurable render
  scale — but not eliminated. This is the one risk that could invalidate the
  premise, and it is measured first.
- **Untestable device coverage.** Only the Libra 2 can be verified. Mitigated
  by capability detection, config-overridable profiles, and a probe that
  generates a profile for any device, making community contribution the growth
  path for `TESTED.md`.
- **Nickel restore path.** A bug here is alarming even though it is reboot
  recoverable. Mitigated by unconditional trap-based restore and by the probe
  not needing takeover at all.

## 14. Build order

1. `koboy-probe` — buildable now, answers §12, zero device risk.
2. Platform seam + `platform_sdl.c` — playable-on-desktop skeleton.
3. `core.c` — libretro contract, verified on the host with a desktop-built
   gambatte core.
4. `video.c` — dither/scale/dirty-rect pipeline, golden-image tested.
5. `input.c` — thumb-pad and zone logic, trace tested.
6. `platform_kobo.c` — FBInk + evdev, informed by probe results.
7. `scripts/koboy.sh` + packaging + `TESTED.md`.
