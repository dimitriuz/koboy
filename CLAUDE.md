# koboy

A retro emulator front-end for modern Kobo e-readers, built Game-Boy-first.
No C++ of its own. It `dlopen`s a libretro core chosen from the ROM's
extension — gambatte (.gb/.gbc), gw-libretro (.mgw), fceumm (.nes),
PokeMini (.min) — renders four greys through FBInk to the e-ink panel, and
reads the page-turn buttons and touchscreen straight from evdev.

**v1 is merged and verified on real hardware** (a Kobo Libra 2, playing Tetris
and an action platformer, exiting to a working Nickel without a reboot).
**v2-core has run on the same device, but only partially:** a 2026-08-26
session ran `koboy` directly with `--frames` over ssh (never through
`scripts/koboy.sh`, so Nickel stayed up), which verified the core, the
cartridge-SRAM save path (including its destructive-truncation fix), and the
ROM browser via `--ui-script`, plus real per-stage timing on a real panel.
The takeover, touch d-pad and in-game MENU have since been verified by hand
(2026-08-26). Save *states* writing and re-reading a file have still not run
on hardware. See "Known unfinished".

## Build and test

```sh
make test        # host suite: 25 binaries, 2505 checks. Runs on x86_64.
make host        # host build (SDL platform) + stub core
bash tests/test_dist.sh      # packaging + launcher safety assertions
bash tests/smoke_host.sh     # end-to-end on the host platform
bash scripts/verify-core.sh  # shipped dependency closure

export PATH="$HOME/.cache/koboy-toolchain/arm-linaro-4.9-2014.09/bin:$PATH"
make kobo        # cross-compile koboy-arm + koboy-probe-arm  (PATH needed!)
make dist        # -> dist/koboy-0.1.0.zip, everything under .adds/koboy/
make probe-dist  # just the probe, without the emulator or the core
```

`make kobo` needs the Linaro toolchain on `PATH` and it is **not** there by
default. See `docs/cross-compiling.md`.

## Hard constraints — check these before proposing anything

- **No C++. No dependency beyond libc, libm, libdl.** (Not "C99 only": the
  Makefile has built with `-std=c11` since before v2, and nothing enforces a
  narrower standard. The constraint that actually matters — and the one
  `scripts/verify-core.sh` enforces — is the dependency ceiling, not a
  specific C revision, so that is what this says now.) Two ARM binaries ship,
  with two different closures: the gambatte core
  (`dist/gambatte_libretro.so`) links only `libm.so.6` + `libc.so.6` (+
  `ld-linux-armhf.so.3`, pulled in by `-static-libstdc++`'s TLS-based
  exception globals — see `docs/cross-compiling.md`); `koboy-arm` itself
  additionally needs `libdl.so.2`, because it `dlopen`s that core.
  `scripts/verify-core.sh` checks both against the same allowlist
  (`libc`/`libm`/`libdl`/`libpthread`/`libgcc_s`/`ld-linux-armhf`, matched by
  anchored whole-name comparison since Task 14 closed follow-up #10).
- **glibc 2.19.** The device has no newer symbol. This is why the toolchain is
  pinned to Linaro 4.9-2014.09.
- **Never `#include <linux/input.h>` in portable code.** The project carries its
  own `koboy_ev {type,code,value}` mirror and its own keycode constants so the
  host build compiles anywhere.
- **Tests must pass on the dev host**, not only on-device. `src/platform_if.h`
  is the seam; `platform_sdl.c` and `platform_kobo.c` are the two sides.
- **Never leave the device in a broken state.** See the version-file trap in
  `docs/device-workflow.md` — this is the one mistake that already happened.

## Layout

```
src/platform_if.h     the portability seam: init, shutdown, screen_info,
                      blit_gray8, refresh, poll_input, now_us, should_quit
src/platform_kobo.c   FBInk + evdev. Device quirks live here and nowhere else.
src/platform_sdl.c    host equivalent, so the whole pipeline is testable
src/video.c           RGB565->gray LUT, integer upscale, 4-level quantise,
                      Bayer dither, 8x8-tile dirty rects
src/input.c           protocol-B multitouch decode, axis transpose, d-pad modes
src/chrome.c          the procedural faceplate drawn around the game rect
src/config.c          ini load/save, profile resolution, path resolution
src/core.c            dlopen + retro_* symbol binding
src/probe.c           koboy-probe: --coexist (safe, Nickel up) / --takeover
scripts/koboy.sh      the launcher. Its environment gate is load-bearing.

-- v2 additions: the ROM browser, in-game MENU and save states -----------
src/ui.c              one list widget, edge-triggered, used for BOTH the ROM
                      browser and the in-game MENU (MODE_BROWSE / MODE_MENU)
src/romlist.c         lists ONE directory of rom_dir at a time -- folders
                      sort first, then files, with a ".." row below the root;
                      feeds ui.c's list widget. It does NOT recurse: the
                      flatten it replaced put the same "Game and Watch/"
                      prefix on 59 rows. romlist_is_rom is an ALLOWLIST of
                      extensions (.gb/.gbc/.mgw/.nes/.min) and must stay in
                      step with config_core_for_rom's table -- a real NES
                      collection ships .pal files beside the ROMs
src/uiscript.c        replays a synthetic input script (tap/key/idle) into
                      the ROM BROWSER only -- --ui-script, for bounded
                      unattended runs. MODE_MENU is not scripted; a run whose
                      script selects nothing exits 4.
src/state.c           save-state paths and slot labels, KOBOY_STATE_SLOTS (3)
                      slots per ROM, 1-based
src/safefile.c        temp-file/fsync/rename write + all-or-nothing read,
                      extracted from sram.c so save states share its
                      discipline; used by both now
src/stats.c           per-stage (core/submit/blit/refresh) timing, the
                      koboy.log `stages` line

-- multi-system: koboy is no longer Game-Boy-only ------------------------
Four cores ship. Extension -> core -> layout, all decided in config.c:
  .gb/.gbc  gambatte_libretro.so    DMG faceplate
  .mgw      gw_libretro.so          LCD strip (the unit draws its own buttons)
  .nes      fceumm_libretro.so      DMG faceplate
  .min      pokemini_libretro.so    DMG faceplate + a THIRD face button, C
Adding a system is a build script plus four wiring points: the table in
config_core_for_rom, romlist_is_rom, a non-phony $(CORE_*_SO) rule in the
Makefile, and the generated roms/README.txt.

scripts/build-gw-core.sh      gw-libretro (Game & Watch)
scripts/build-fceumm-core.sh  libretro-fceumm (NES). Three non-default make
                      switches, each justified in the script's header; the
                      headline is WANT_32BPP=0, which gets RGB565 instead of
                      XRGB8888 and halves what video_submit reads per frame.
scripts/build-pokemini-core.sh  libretro/PokeMini (Pokemon Mini). NO BIOS
                      SHIPS AND NONE IS NEEDED -- the core links its own free
                      one, verified against an EMPTY system directory.
All three are pure C: closure is libm+libc only, SMALLER than gambatte's --
no libdl, no libgcc_s, no ld-linux-armhf.
scripts/probe_core.c  standalone: dlopens ANY core with no koboy code in the
                      way and reports geometry BEFORE and AFTER the first
                      retro_run(). This is how the 128x128 placeholder was
                      found; ask it of every new core.
src/text.c            the 5x7 bitmap font, lifted out of main.c because v2
                      has three screens that render arbitrary strings

koboy_layout's c_cx/c_cy/c_r is the Pokemon Mini's third face button, with
c_r == 0 meaning "this system has no C". Three consumers guard on it
(chrome_controls_top, the DMG renderer, input.c's hit test) and each has a
distinct failure if the guard goes -- see the commit and test_chrome.c.
```

Path resolution is against `/proc/self/exe`'s directory — **`dlopen` never
searches the cwd**, which cost a debugging round when the core sat right beside
the binary and still failed to load.

## Reference documents

v2-core (the ROM browser, in-game MENU, save states, multi-rect dirty regions,
the redrawn faceplate) is done as of this task; the Bluetooth companion plan
(`docs/superpowers/plans/2026-08-25-koboy-v2-bluetooth.md`) is not started.

| Document | What it holds |
|---|---|
| `docs/superpowers/specs/2026-08-24-koboy-design.md` | The v1 design, and **four appendices of measured corrections**. The appendices override the body wherever they disagree. |
| `docs/superpowers/specs/2026-08-25-koboy-v2-design.md` | The v2 design: the mode machine, save states, the faceplate, and §13's open measurements. |
| `docs/FOLLOWUPS.md` | 39 deferred findings, ordered by what bites first. Start here for the next session's scope. **#33 is the live one: neither NES nor Pokemon Mini has run on hardware at all.** |
| `docs/device-workflow.md` | Deploying, launching, diagnosing, and the traps. |
| `TESTED.md` | The device matrix. Exactly one device is verified; v2-core's core/SRAM/browser have run on it directly with `--frames`, the takeover/MENU/touch have not. |
| `docs/cross-compiling.md` | Toolchain, including why koxtoolchain was abandoned. |
| `docs/probe-readme.md` | Profiling a device nobody has tried. |

## What the hardware overruled — do not re-derive these

Nearly every design decision that survived v1 is one the device corrected. The
spec's appendices are the record; the short version:

- **A2 is not the fast waveform.** DU4 is ~3.5x faster on Mark 9, against the
  folklore. Then **DU4 lost to AUTO**: forced DU4 cannot *erase*, so it ghosted
  badly in play. `waveform_fast = auto` lets the controller decide per update.
- **The cleanup mitigations DU4 needed cause the problem they were added for.**
  `cleanup_interval` and `full_refresh_permille` at their old values produced
  every flash the user saw while fixing nothing. They ship disabled / at 1000‰.
- **16-level GBC rendering is impossible here.** GL16 measured 321.7 ms and
  GC16 393.3 ms — 3.1 and 2.5 fps. Four levels is not a simplification, it is
  the only option.
- **Refresh cost scales with area**, so dirty rectangles pay for themselves, and
  non-blocking submission beats blocking by ~2.6x. The main loop never waits for
  completion.
- **The design spec's "emulation is cheap; presentation is the entire
  bottleneck" premise is wrong about where the cost is.** Measured on-device
  (Zelda, scale 5, `present_divisor = 3`): core 2.3 ms, blit 2.8 ms, refresh
  0.4-0.75 ms — but `video_submit` (the RGB565->gray LUT, integer scale,
  quantise and 8x8-tile diff, `src/video.c`) is **17.0 ms**, roughly 5x the
  other three stages combined. It is neither emulation nor presentation
  (panel refresh) — it is the pixel pipeline between them, and it is the
  actual bottleneck. Confirmed pixel-bound by a render-scale sweep (submit
  time scales ~4.7 ms + 20.7 ns/px). v2-core's multi-rect work optimised
  `refresh`, already the cheapest stage; `video_submit` is where the next
  optimisation belongs. See `docs/FOLLOWUPS.md` #23.
- **A libretro core's geometry is discovered, not queried.** `gw-libretro`
  answers `retro_get_system_av_info` with a 128x128 placeholder on every one
  of 59 titles until its first `retro_run()`, then announces the real canvas
  via `SET_GEOMETRY`/`SET_SYSTEM_AV_INFO`. Sizing buffers from the post-load
  query -- what the libretro docs imply -- renders a 973x532 game into a
  128x128 box. `main.c` polls `core_geometry_changed()` every frame, which
  also covers a mid-session ROM switch: measured, three titles loaded
  back-to-back into one core instance each re-announced.
- **G&W is CHEAPER than the Game Boy, not more expensive.** `video_submit`
  scales with DESTINATION pixels (~4.7 ms + 20.7 ns/px), and the Game Boy is
  upscaled 160x144 -> 800x720 = 576k px = 16.6 ms. G&W runs at 1x: Parachute
  260k px (10.1 ms), Mario Bros. 518k px (15.4 ms). Comparing G&W's canvas to
  the Game Boy's 160x144 *source* suggests a 20x cost blowup and is wrong.
- Scale 5 with a procedural faceplate, not full-width 7x.
- `viewVertOrigin` is **not** a blit offset.
- "Grab the buttons but not power" is impossible: they share `gpio-keys`.

## Testing culture — this one is not optional

**Three tests in v1 passed whether or not the code they guarded was present.**
One took four review rounds to fix. So: after writing any safety or regression
test, *break the thing it guards and confirm the test fails.* Record the mutant
and its output.

The subtlest instance is worth knowing, because it is a class and not an
accident: a sentinel guard band cannot detect an unclamped `memset` whose length
underflows to near `SIZE_MAX`, because where glibc actually writes for such a
length is an implementation detail that on x86-64 lands *inside* the buffer, not
in the guard. The fix was to stop observing undefined behaviour and assert the
clamped values directly (`chrome_bands`, `src/chrome.c`). If a test can only
fail via UB, it is not a test.

Corollary from the v1 endgame: the first-run deadlock that the final review
caught was invisible to twenty per-task reviews because the scripted-run branch
skips calibration — **every automated test took the one path that could not
reach the bug.** When a code path exists only for scripted runs, ask what it is
hiding.

## Known unfinished

- **The save path (cartridge SRAM) has now run on hardware, and the
  destructive-truncation bug it was carried to test has been proven fixed.**
  2026-08-26 device session: Zelda (cart type `0x03`, `rambanks: 1`, the
  first battery-backed title this project has run — both prior titles were
  `rambanks: 0`) verified write, read round-trip (md5-identical), and the
  truncated-`.srm` destructive path, which left the file at its truncated
  size instead of destroying it further. See `docs/FOLLOWUPS.md` #3
  (closed) for the numbers. **Save *states*** (`state.c`, `safefile.c` — a
  different mechanism from cartridge SRAM, reached through `MODE_MENU`) are
  a separate code path and remain untested on hardware: this session drove
  the ROM browser only, via `--ui-script`, never `MODE_MENU`.
- **One verified device, and v2-core's UI layer has run on it only partially.**
  The 2026-08-26 session ran the `koboy` binary directly with `--frames` over
  ssh — never through `scripts/koboy.sh`, so Nickel was never stopped and the
  takeover, touch d-pad, in-game MENU, and the ROM browser's real touch input
  were **not** exercised. The ROM browser itself did run, driven by
  `--ui-script` against a real directory listing, and device identification
  (panel size, stride, waveform) was correct. Still needed: a NickelMenu
  playtest with real touch input, exercising the takeover and `MODE_MENU`.
  See `TESTED.md`.
- `refresh_fixed_tiles` ships at a starting guess (40), still not a validated
  value: an on-device sweep (20/40/80/split-off) found 20/40/80
  behaviourally identical on real content, which the measurement method
  cannot distinguish further — see `docs/FOLLOWUPS.md` #24 for why the
  in-process timer can't see the panel-side cost the setting is meant to
  amortise. `config/koboy.ini` and `TESTED.md` have the detail.
- ~~`dpad_mode = cross` has no test distinguishing it from RELATIVE~~ — fixed:
  `tests/test_input_touch.c` (search `#1`) now asserts the actual distinction
  (a tap anywhere in the pad steers under CROSS; the same tap reports no
  direction under RELATIVE until the origin is set), not just that both modes
  compile. What is still open, and still the top item in `docs/FOLLOWUPS.md`'s
  v2 section: `--ui-script`, `MODE_MENU`'s coverage gap, and four smaller
  chrome/video findings from this plan.

## Conventions

Comments here record **why**, not what, and especially why a non-obvious choice
is not an oversight ("DISABLED BY DEFAULT, and the history matters because 'off'
looks like an oversight otherwise"). Clamps and guards carry a note saying they
are live so nobody deletes them as dead code. Match that voice.

ROMs are git-ignored (`*.gb`, `*.gbc`, `*.sav`) and must never be committed.
`*.pgm` is marked binary in `.gitattributes` — without it git diffs a golden
image as text and a review package balloons to megabytes.
