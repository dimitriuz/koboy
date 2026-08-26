# koboy

A Game Boy emulator for modern Kobo e-readers. No C++. It `dlopen`s
gambatte-libretro, renders the DMG's four greys through FBInk to the e-ink
panel, and reads the page-turn buttons and touchscreen straight from evdev.

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
make test        # host suite: 22 binaries, 913 checks. Runs on x86_64.
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
src/romlist.c         scans rom_dir for .gb/.gbc, feeds ui.c's list widget
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
src/text.c            the 5x7 bitmap font, lifted out of main.c because v2
                      has three screens that render arbitrary strings
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
| `docs/FOLLOWUPS.md` | 24 deferred findings (16 from v1, 8 from v2-core), ordered by what bites first. Start here for the next session's scope. |
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
