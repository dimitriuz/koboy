# koboy

A retro emulator front-end for modern Kobo e-readers, built Game-Boy-first.
No C++ of its own. It `dlopen`s a libretro core chosen from the ROM's
extension — gambatte (.gb/.gbc), gw-libretro (.mgw), fceumm (.nes),
PokeMini (.min), beetle-wswan (.ws/.wsc), RACE (.ngp/.ngc), stella2014
(.a26), Gearcoleco (.col), FreeIntv (.int), Genesis Plus GX (.sms/.gg/.md),
snes9x2005 (.sfc/.smc), beetle-pce-fast (.pce), FinalBurn Neo (.zip, arcade) — renders four greys through FBInk to the e-ink
panel, and reads the page-turn buttons and touchscreen straight from evdev.

**Arcade ships as its own archive.** `dist/fbneo_libretro.so` is 41 MB, ten
times the whole rest of the project, so `make dist` deliberately does NOT
carry it and `make fbneo-dist` builds `dist/koboy-fbneo-0.1.0.zip` separately.
`tests/test_dist.sh` asserts the main package stays clean of it. Arcade is
also the ONLY system whose core and content are version-locked: the romset
must match FBNeo v1.0.0.03 (revision ae41c16e, 2025-07-24), which
`scripts/build-fbneo-core.sh` pins by SHA and explains at length.

**Two of the fourteen systems need a BIOS, and it is the owner's to supply.**
ColecoVision wants `colecovision.rom`; Intellivision wants `exec.bin` and
`grom.bin`. Both go in `.adds/koboy/` (what koboy answers
`GET_SYSTEM_DIRECTORY` with). Nothing ships them and `tests/test_dist.sh`
asserts nothing ever will.

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
make test        # host suite: 25 binaries, 3803 checks. Runs on x86_64.
make host        # host build (SDL platform) + stub core
bash tests/test_dist.sh      # packaging + launcher safety assertions
bash tests/smoke_host.sh     # end-to-end on the host platform
bash scripts/verify-core.sh  # shipped dependency closure

export PATH="$HOME/.cache/koboy-toolchain/arm-linaro-4.9-2014.09/bin:$PATH"
make kobo        # cross-compile koboy-arm + koboy-probe-arm  (PATH needed!)
make dist        # -> dist/koboy-0.1.0.zip, everything under .adds/koboy/
make fbneo-dist  # -> dist/koboy-fbneo-0.1.0.zip, the arcade core ALONE (41 MB)
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
src/video.c           RGB565->gray LUT (five selectable mappings, koboy_gray_map),
                      integer upscale, 4-level quantise, Bayer dither,
                      8x8-tile dirty rects
src/input.c           protocol-B multitouch decode, axis transpose, d-pad modes
src/chrome.c          the procedural faceplate drawn around the game rect
src/config.c          ini load/save, profile resolution, path resolution
src/core.c            dlopen + retro_* symbol binding
src/probe.c           koboy-probe: --coexist (safe, Nickel up) / --takeover
scripts/koboy.sh      the launcher. Its environment gate is load-bearing.
                      ROTATION lives in three places and nowhere else:
                      core.c records SET_ROTATION and TRANSPOSES
                      core_get_geometry's answer for an odd one (so every
                      consumer sees the picture as PRESENTED); video.c turns
                      the pixels inside the convert pass it was already
                      making; main.c wires the two and re-syncs on every
                      geometry change. Answering true is a PROMISE --
                      beetle-wswan stops rotating in software the moment you
                      do.

-- v2 additions: the ROM browser, in-game MENU and save states -----------
src/ui.c              one list widget, edge-triggered, used for BOTH the ROM
                      browser and the in-game MENU (MODE_BROWSE / MODE_MENU)
src/romlist.c         lists ONE directory of rom_dir at a time -- folders
                      sort first, then files, with a ".." row below the root;
                      feeds ui.c's list widget. It does NOT recurse: the
                      flatten it replaced put the same "Game and Watch/"
                      prefix on 59 rows. romlist_is_rom is an ALLOWLIST of
                      extensions (.gb/.gbc/.mgw/.nes/.min/.ws/.wsc/.ngp/
                      .ngc/.a26/.col/.int/.sms/.gg) and must stay in step
                      with config_core_for_rom's table -- a real NES
                      collection ships .pal files beside the ROMs,
                      WonderSwan and Neo Geo Pocket ones ship boot.rom /
                      boot1.rom, and an Intellivision one ships boot0-boot3,
                      two of which ARE the BIOS. `.bin` is deliberately not
                      claimed even though three cores accept it: exec.bin
                      and grom.bin are .bin
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
Thirteen cores ship for FOURTEEN systems -- Genesis Plus GX answers for three
of them. Extension -> core -> layout, all decided in config.c:
  .gb/.gbc  gambatte_libretro.so         DMG faceplate
  .mgw      gw_libretro.so               LCD strip (the unit draws its own buttons)
  .nes      fceumm_libretro.so           DMG faceplate
  .min      pokemini_libretro.so         DMG faceplate + a C disc
  .ws/.wsc  mednafen_wswan_libretro.so   DMG faceplate + L1 and R1 discs
  .ngp/.ngc race_libretro.so             DMG faceplate
  .a26      stella2014_libretro.so       DMG faceplate (its A disc is DEAD --
                                         stella2014 puts fire on JOYPAD_B)
  .col      gearcoleco_libretro.so       DMG faceplate + K1 and K2 discs, and
                                         NEEDS colecovision.rom
  .int      freeintv_libretro.so         DMG faceplate + KEY and TOP discs,
                                         and NEEDS exec.bin + grom.bin
  .sms/.gg  genesis_plus_gx_libretro.so  DMG faceplate
  .md       genesis_plus_gx_libretro.so  DMG faceplate + A and Y discs --
                                         SAME .so as .sms/.gg. The Mega Drive
                                         cost NO build at all.
  .sfc/.smc snes9x2005_libretro.so       DMG faceplate + Y and X discs
  .pce      mednafen_pce_fast_libretro.so DMG faceplate, NO extra discs
Adding a system is a build script plus four wiring points: the table in
config_core_for_rom, romlist_is_rom, a non-phony $(CORE_*_SO) rule in the
Makefile, and the generated roms/README.txt. Mega Drive needed only the
wiring points -- check whether a core you already ship covers the system
before writing a fifth build script.

scripts/build-gw-core.sh      gw-libretro (Game & Watch)
scripts/build-fceumm-core.sh  libretro-fceumm (NES). Three non-default make
                      switches, each justified in the script's header; the
                      headline is WANT_32BPP=0, which gets RGB565 instead of
                      XRGB8888 and halves what video_submit reads per frame.
scripts/build-pokemini-core.sh  libretro/PokeMini (Pokemon Mini). NO BIOS
                      SHIPS AND NONE IS NEEDED -- the core links its own free
                      one, verified against an EMPTY system directory.
scripts/build-wswan-core.sh   beetle-wswan (WonderSwan + Color). One core,
                      two extensions. No non-default switch: its unix block
                      already picks RGB565.
scripts/build-race-core.sh    RACE (Neo Geo Pocket + Color). Chosen over
                      beetle-ngp by MEASUREMENT, both cross-built: RACE is
                      pure C and ~3x faster; beetle-ngp needs
                      -static-libstdc++ and then libm + ld-linux-armhf too.
                      The script's header carries the numbers.
scripts/build-stella-core.sh  libretro/stella2014-libretro (Atari 2600).
                      NOT libretro/stella2014 (404s) and NOT libretro/stella,
                      which forces -std=c++17 -- a flag Linaro 4.9.2 rejects
                      outright. That is the whole choice.
scripts/build-gearcoleco-core.sh  drhelius/Gearcoleco (ColecoVision;
                      libretro/gearcoleco 404s too). The libretro port is a
                      SUBDIRECTORY of the emulator repo. NEEDS A BIOS: with
                      none it draws a static NO BIOS bitmap forever, which
                      only a rendered frame reveals.
scripts/build-freeintv-core.sh  libretro/FreeIntv (Intellivision). The only
                      core for the system, and the only one koboy ships that
                      asks for XRGB8888. NEEDS exec.bin + grom.bin.
scripts/build-gpgx-core.sh    libretro/Genesis-Plus-GX (Master System AND
                      Game Gear). Chosen over Gearsystem by MEASUREMENT
                      (pure C, ~2.5x faster over 121 titles); SMS Plus GX is
                      DISQUALIFIED -- it segfaults in retro_load_game calling
                      a null log pointer kept from a refused
                      GET_LOG_INTERFACE, which koboy also refuses.
scripts/build-snes-core.sh    libretro/snes9x2005 (SNES). Chosen over
                      snes9x2010 and snes9x by MEASUREMENT, all three
                      cross-built: fastest by 1.36x/1.58x and pure C. AND THE
                      COMPATIBILITY FOLKLORE IS FALSE -- this revision HAS
                      SuperFX and SA-1; Star Fox, Yoshi's Island and Kirby
                      Super Star all run, RENDERED and looked at rather than
                      inferred from a successful load.
scripts/build-pce-core.sh     libretro/beetle-pce-fast-LIBRETRO (PC Engine).
                      The bare name 404s -- fourth variant of that trap. Its
                      Makefile links with $(CXX) and -lrt whatever it
                      compiled, so counting .cpp files predicts the wrong
                      flags; -Wl,--as-needed is what actually gives the
                      libm+libc closure. Cartridge .pce ONLY.
scripts/build-fbneo-core.sh   libretro/FBNeo (arcade). NOT
                      finalburnneo/FBNeo -- a NEW variant of the 404 trap,
                      because BOTH names exist and both are real FBNeo
                      repositories; only the libretro fork has
                      src/burner/libretro at all. THE REVISION IS PINNED BY
                      SHA to the day the owner's dat was published, because
                      the version number 1.0.0.03 has covered five years of
                      master and a romset that does not match the build fails
                      exactly like a broken core. 7-Zip support is compiled
                      OUT for the device: lib7z does not build against glibc
                      2.19's headers.
Nine of the fourteen are pure C: closure is libm+libc or less. The two
WonderSwan/Neo Geo cores need libc ALONE. FBNeo is the only one that pulls in
libpthread.
scripts/probe_core.c  standalone: dlopens ANY core with no koboy code in the
                      way and reports geometry BEFORE and AFTER the first
                      retro_run(). This is how the 128x128 placeholder was
                      found; ask it of every new core.
scripts/corebench.c   its sibling for SPEED: microseconds per retro_run, with
                      mean/p50/p95/max, the save-RAM size at load AND after a
                      warmup, and every distinct frame width seen. Ask it of
                      every new core too -- "does a frame fit in the budget"
                      is the question that decides whether a system ships.
                      Needs -std=c11 to CROSS-build; Linaro 4.9.2 is gnu89.
src/text.c            the 5x7 bitmap font, lifted out of main.c because v2
                      has three screens that render arbitrary strings

koboy_layout's extra[] holds the DMG faceplate's OPTIONAL discs -- position,
KOBOY_BTN_* bit and label per slot, r == 0 meaning "empty". A Pokemon Mini
fills one ("C"); a WonderSwan fills two ("L1", "R1"); a ColecoVision fills
two ("K1", "K2" -- keypad 1 and 2, without which no cartridge can be
started); an Intellivision fills two ("KEY", "TOP"), where KEY is not a
button at all but FreeIntv's hold-to-show-the-mini-keypad modifier, and is
what makes all twelve of that console's keypad keys reachable; an arcade
board fills two ("3", "4" -- buttons 3 and 4, because on this system the
faceplate's own B and A really ARE buttons 1 and 2). Three
consumers guard on r (chrome_controls_top, the DMG renderer, input.c's hit
test) and each has a distinct failure if the guard goes -- see the commit and
test_chrome.c. The bit is always the CORE's choice, read off its input
descriptors, never picked here.

TWO of the fourteen systems have a 12-KEY KEYPAD the faceplate cannot draw, and
on both of them titles refuse to start without it. Check any new system's real
control set against what the faceplate offers BEFORE assuming DMG is enough;
this has now cost a round on Game & Watch, Pokemon Mini, WonderSwan, and both
of these.

ARCADE is the first system where "what does the hardware have" has no single
answer -- it is 227 different boards -- so the two discs were chosen by
COUNTING every romset's input descriptors rather than by reading a control
panel. FBNeo's map is flat (B=Button 1, A=Button 2, Y=Button 3, X=Button 4,
SELECT=Coin, START=Start) and the counts decided it: Y 134 boards, X 71,
against 45-48 for each shoulder button there is no room for. Do the same for
the next multi-board system.
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
| `docs/FOLLOWUPS.md` | 72 deferred findings, ordered by what bites first. Start here for the next session's scope. **#40 and #55 are the live ones: TEN of the fourteen systems have never run on hardware at all**; #46 is its twin for the greyscale default, #51 is a device-visible defect found by looking at a rendered frame (every Atari 2600 title is ~1.75x too tall), and #57 is frame pacing, which arcade turns from a rounding error into 77 boards running at the wrong speed. From the newest batch, **#67 is the biggest presentation win in the project** (SNES and PC Engine present at under half the Game Boy's area because the rect is sized from a max their cores never draw) and #68 is why every speed figure for those systems is a model rather than a measurement. |
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
- **Rec.601 luma is the wrong RGB→grey reduction for this panel, and equal
  weights are not the fix either.** Luma weights blue 29/256, so a bright blue
  sky quantises to the *darkest* level (Sonic Pocket Adventure rgb(0,154,255)
  → 119 → level 1; Castlevania rgb(0,36,140) → 36 → level 0). But `(R+G+B)/3`
  crushes *more* pixels to black than luma does — 8.9% vs 6.7% over 38
  gameplay frames from 19 titles — because what it returns to blue it takes
  from green. What removes the crushing is a **shadow lift**, equivalent to
  lowering `video_quantise4`'s first threshold: 6.7% → 2.5%. The shipped
  default (`gray_map = balanced`) needs both — weights (81,118,57) *and* the
  lift. Selectable, because this is a judgement about a reflective panel and
  cannot honestly be made from a host render: `MENU → GREYSCALE` cycles it
  in-game and writes the ini key back.
- **The Game Boy's palette is neutral, so none of that touches it.** gambatte
  emits exactly rgb(0,0,0)/(82,85,82)/(173,170,173)/(255,255,255); every
  mapping is the identity on neutral grey and the lift keeps both mid greys
  inside their existing levels, so the DMG golden is byte-identical and **no
  per-system exemption exists**. Do not add one keyed on 160x144 — Game Gear
  is also 160x144 and is a colour system.
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
- **The reserved rect comes from the core's BASE geometry, not its max** (DMG
  layout only; LCD still uses max). Max made "any frame in [1, max] fits"
  true by construction, and it cost a SNES 54% of its picture area against a
  512x512 mode snes9x2005 never enters. `video_fit_rect` now shrinks a frame
  the rect cannot hold, which is what replaces that guarantee — do not delete
  that branch. Measured on the device: free for PC Engine and for SNES titles
  with headroom, and 96%→78% for Kirby Super Star. `docs/FOLLOWUPS.md` #73.
- **Benchmarking this device back to back gives numbers that climb.** A first
  pass with no gaps read up to 2.4x high and kept rising through the batch;
  an isolated re-run of the same binary on the same ROM came back at the
  first row's figure. Ten seconds of idle between runs, every time.
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
- **The arcade "portrait panel, portrait game" win is real but smaller than it
  sounds, and two of its headline titles smear anyway.** A vertical board
  presents at 672x864 inside a square 864x864 reserved rect — 580,608 pixels
  against the Game Boy's 576,000, in the panel's own aspect. It is NOT the
  1264x1626 a fractional 5.6x fit would give, because the DMG faceplate
  reserves everything below `chrome_controls_top` for the touch controls and
  the scale search is integer. And the "single-screen boards cannot smear"
  premise is wrong for the two most famous ones: measured with koboy's own
  dirty diff, Galaga changes 67% of the game rect per frame and Galaxian 86%,
  because the STARFIELD scrolls. Dig Dug, Donkey Kong and Ms. Pac-Man are 1.5
  to 2.6%.
- **A core's MAX geometry, not its real frame, sizes the reserved rect -- and
  two of the fourteen systems report a max nothing like what they draw.**
  snes9x2005 says 512x512 against a 256x224 frame and beetle-pce-fast says
  512x243 against 352x243, so a 512-tall reservation cannot exceed scale 1
  under chrome_controls_top. Measured on the verified panel: SNES presents at
  597x448 and PC Engine at 583x486, against the Game Boy's 800x720 -- **less
  than half its area, on higher-resolution systems**. Sizing from the real
  frame would give SNES roughly 896x672, about 2.2x. Not fixed (it is the
  fitting path in video.c, the one presentation verified on hardware) and it
  is the largest presentation win available. Note the perverse coupling
  before "fixing" it: a smaller rect costs less video_submit, so these two
  systems' CPU budgets SHRINK when their pictures grow.
- **The pixel-aspect correction earns itself on PC Engine, which changes
  horizontal resolution mid-game.** Titles alternate 256 and 352 wide (not
  336); Military Madness does it five times in 2500 frames. Both modes have
  the same DISPLAY width, so with the correction on, the picture is 583x486
  and centred at x=632 in BOTH -- same size, same place, only the detail
  changes. With `pixel_aspect = false` it jumps between 512x486 and 704x486
  every scene change. Verified by rendering both sides and looking.
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

- **TEN OF THE FOURTEEN SYSTEMS HAVE NEVER RUN ON A KOBO.** Game Boy and Game
  & Watch are verified; NES and Pokemon Mini have run on the device. NOTHING
  device-side exists for WonderSwan, Neo Geo Pocket, Atari 2600,
  ColecoVision, Intellivision, Master System / Game Gear, ARCADE, MEGA DRIVE,
  SNES or PC ENGINE — every figure in `TESTED.md` for those ten is a host
  measurement through koboy's own `config.c`/`video.c`/`chrome.c` plus a
  cross-build that passes `verify-core.sh`. In particular the two BIOS files
  have never been read off a FAT32 partition, the K1/K2 and KEY/TOP discs
  have never been touched by a real finger, and **the picture has never been
  ROTATED on the panel** — which is the difference between Galaga and
  Galaga sideways.
- **Every device speed figure for the three newest systems is EXTRAPOLATED
  from a TWO-POINT FIT, and the two points disagree under a simple ratio.**
  Re-measured with `scripts/corebench.c`, gambatte implies 23.4x
  host-to-device and fceumm implies 17.2x; a linear fit
  (`device ~= 13.45 * host + 979 us`) reconciles them, and its additive ~1 ms
  is plausibly koboy's own per-frame front-end work, which `corebench` does
  not measure and `TESTED.md`'s `core` stage does. It is still a two-point
  fit doing real work in every number. Do NOT compare those figures with the
  arcade section's flat 7x: that one scaled koboy's own instrument, this one
  scales a different one.
- **Mega Drive and SNES are the first systems koboy has added where a battery
  save is the NORM rather than the exception, and no `.srm` from either has
  survived a real session.** GPGX's shrinking save length is already handled
  (`core_sram`, `1fb3802`); snes9x2005's is constant, measured, so the pin is
  harmless there rather than load-bearing. Neither has been round-tripped on
  hardware.
- **snes9x2005 CRASHES on a short `.sfc`/`.smc` rather than refusing it** —
  SIGFPE in `LoROMMap`, under 8192 bytes. koboy guards it at the load site
  (`config_min_rom_bytes` + `load_rom_into`), and the guard is per-system
  because a 2600 cartridge is legitimately 2048 bytes. The file that found it
  was a 212-byte macOS `._*.smc` AppleDouble stub, the kind every FAT32 card
  grows. If another core is ever measured to do worse than refuse bad
  content, that is where the row goes.
- **Every Atari 2600 title renders about 1.75x too tall**, found by rendering
  frames and looking at them after every numeric check passed. The 2600 is
  the only non-square-pixel system koboy runs. Not fixed here because the fix
  is anisotropic fitting in `video.c`'s hot path, which is the one
  presentation verified on hardware. `docs/FOLLOWUPS.md` #51 has the
  mechanism.
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

ROMs are git-ignored (`*.gb`, `*.gbc`, `*.sav`, and the extensions the other
thirteen systems use) and must never be committed. `.md` is the ONE entry in
that list that is also an everyday non-ROM extension, so it is anchored to
the roms directories rather than global — an unqualified `*.md` would
silently stop this file, README.md and all of docs/ from ever being
committed again. Neither may a BIOS: two
systems need one now, and `tests/test_dist.sh` asserts no `.rom` or `.bin`
reaches the package.
`*.pgm` is marked binary in `.gitattributes` — without it git diffs a golden
image as text and a review package balloons to megabytes.
