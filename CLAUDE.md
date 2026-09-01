# koboy

A retro emulator front-end for modern Kobo e-readers, built Game-Boy-first.
No C++ of its own. It `dlopen`s a libretro core chosen from the ROM's
extension — gambatte (.gb/.gbc), gw-libretro (.mgw), fceumm (.nes),
PokeMini (.min), beetle-wswan (.ws/.wsc), RACE (.ngp/.ngc), stella2014
(.a26), Gearcoleco (.col), FreeIntv (.int), Genesis Plus GX (.sms/.gg/.md),
snes9x2005 (.sfc/.smc), beetle-pce-fast (.pce), gpSP (.gba),
FinalBurn Neo (.zip, arcade) — renders four greys through FBInk to the e-ink
panel, and reads the page-turn buttons and touchscreen straight from evdev.

**ONE ARCHIVE, and this reversed.** Arcade shipped separately behind `make
fbneo-dist` on the grounds that 41 MB against the other cores' 18 was a
tenfold blowup. MEASURED COMPRESSED, the core deflates 67% to 13.6 MB and the
whole package is 18.6 MB, so the split cost more than it saved and the target
is GONE (`core-fbneo` still rebuilds just that core). `tests/test_dist.sh`
asserts the core IS in the package, with real bytes, under a 32 MB cap.
Deleting `.adds/koboy/fbneo_libretro.so` is how an owner without a romset
reclaims the space, and both packaged READMEs (`packaging/roms-README.txt`,
`packaging/README-fbneo.txt` -- real files `make dist` COPIES, as is
`packaging/KOBOY-INSTALL.md`, the one entry in the zip that is NOT hidden)
say so by filename. Arcade is still the ONLY system whose core and content are
version-locked: the romset must match FBNeo v1.0.0.03 (ae41c16e, 2025-07-24),
pinned in `scripts/pins.txt` and explained in `scripts/build-fbneo-core.sh`.

**Two of the fifteen systems need a BIOS, and it is the owner's to supply.**
ColecoVision wants `colecovision.rom`; Intellivision wants `exec.bin` and
`grom.bin`. Both go in `.adds/koboy/` (what koboy answers
`GET_SYSTEM_DIRECTORY` with). Nothing ships them and `tests/test_dist.sh`
asserts nothing ever will.

**Verified on real hardware, on exactly one device** (a Kobo Libra 2). v1
plays Tetris and an action platformer and exits to a working Nickel without a
reboot; v2-core's takeover, touch d-pad, ROM browser and in-game MENU were
driven by hand from NickelMenu on 2026-08-26, and cartridge SRAM and save
states have both been round-tripped there. Every one of the fifteen systems
has since `dlopen`ed, resolved geometry, paced itself and rendered on that
panel (`TESTED.md`).

**That is NOT a regression test.** Nothing in `make test` drives what a hand
verified once, and the fifteen-system sweep is `--frames` over ssh with Nickel
up -- loading, geometry, pacing and speed, and nothing a finger does. See
"Known unfinished".

## Build and test

```sh
make test        # host suite: 30 binaries, 6417 checks. Runs on x86_64.
make lint        # clang -Werror -fsyntax-only over src/ AND tests/. A SECOND
                 # front end: `make test` is gcc, and clang carries classes
                 # gcc has no equivalent for. Green on the tree; CI gates on
                 # it. Skips itself if clang is absent.
make coverage    # per-file line coverage of src/, from the whole suite
                 # (scripts/coverage.sh). BUILD-TIME TOOLING ON THE HOST: it
                 # links nothing into koboy-arm, so the dependency ceiling is
                 # untouched -- gcov is a compiler feature, not a library.
make host        # host build (SDL platform) + stub core
bash tests/test_dist.sh      # packaging + launcher safety assertions
bash tests/smoke_host.sh     # end-to-end on the host platform
bash scripts/verify-core.sh  # shipped dependency closure

export PATH="$HOME/.cache/koboy-toolchain/arm-linaro-4.9-2014.09/bin:$PATH"
make kobo        # cross-compile koboy-arm + koboy-probe-arm  (PATH needed!)
make dist        # -> dist/koboy-$(VERSION).zip, everything under .adds/koboy/
make probe-dist  # just the probe, without the emulator or the core
core-fbneo       # rebuild the arcade core alone (it ships in `dist` now)
```

`make kobo` needs the Linaro toolchain on `PATH` and it is **not** there by
default. See `docs/cross-compiling.md`.

## Hard constraints — check these before proposing anything

- **No C++. No dependency beyond libc, libm, libdl.** Not "C99 only" — the
  Makefile has built with `-std=c11` since before v2 and nothing enforces a
  narrower standard; the constraint `scripts/verify-core.sh` enforces is the
  DEPENDENCY CEILING, not a C revision. Two closures ship: the gambatte core
  links only `libm.so.6` + `libc.so.6` (+ `ld-linux-armhf.so.3`, pulled in by
  `-static-libstdc++`'s TLS-based exception globals — see
  `docs/cross-compiling.md`), while `koboy-arm` also needs `libdl.so.2`
  because it `dlopen`s that core. Both are checked against one allowlist
  (`libc`/`libm`/`libdl`/`libpthread`/`libgcc_s`/`ld-linux-armhf`), matched by
  anchored whole-name comparison.
- **glibc 2.19.** The device has no newer symbol. This is why the toolchain is
  pinned to Linaro 4.9-2014.09.
- **Every upstream is PINNED to a commit in `scripts/pins.txt`**, the only
  place a revision is written down. Build scripts fetch through
  `koboy_fetch_pinned` (`scripts/pins.sh`); none may `git clone`, and CI
  asserts both halves. The pins are what TESTED.md's numbers were measured
  against, so **moving one invalidates that file's rows for that system** --
  say so there. A pin that cannot be fetched stops the build; do NOT "fix"
  that by cloning master.
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
                      integer upscale, 4-level quantise, Bayer dither
                      (thresholds are the matrix SCALED to 0..254, or pure
                      white speckles one pixel per 16x16 tile -- g_thresh),
                      8x8-tile dirty rects
src/input.c           evdev touch decode, axis transpose, d-pad modes. FOUR
                      Kobo touch protocols, not one -- see input_feed_from's
                      header. The LIFT is the whole difficulty: protocol B
                      retires a contact with ABS_MT_TRACKING_ID == -1, and the
                      other three families NEVER SEND THAT (Phoenix keeps the
                      id, Snow repeats it, the pre-multitouch panels have no id
                      at all), so BTN_TOUCH == 0 is the other half. Reading
                      only the id was github issue #1: one tap per
                      koboy_input object worked and every tap after it was
                      dead. input_feed takes a SOURCE because ABS_X/ABS_Y are a
                      finger on the touchscreen and an analog stick on a
                      gamepad
src/chrome.c          the procedural faceplate drawn around the game rect
src/config.c          ini load/save, profile resolution, path resolution
src/core.c            dlopen + retro_* symbol binding
src/probe.c           koboy-probe: --coexist (safe, Nickel up) / --takeover
scripts/koboy.sh      the launcher. Its environment gate is load-bearing.

ROTATION lives in three files and nowhere else: core.c records SET_ROTATION
and TRANSPOSES core_get_geometry's answer for an odd quarter-turn (so every
consumer sees the picture as PRESENTED); video.c turns the pixels inside the
convert pass it was already making; main.c wires the two and re-syncs on every
geometry change. Answering true is a PROMISE -- beetle-wswan stops rotating in
software the moment you do.

-- v2 additions: the ROM browser, in-game MENU and save states -----------
src/ui.c              one list widget, edge-triggered, used for BOTH the ROM
                      browser and the in-game MENU (MODE_BROWSE / MODE_MENU)
src/screens.c         the six full-panel screens that DRIVE that widget, plus
                      the MENU_* / MAIN_* / BROWSE_* enums main() switches on.
                      Split out of main.c for ONE reason: main.c is filtered
                      out of $(SRC), so nothing linked it and its 714
                      executable lines were the whole coverage deficit.
                      Nothing here may name platform_kobo_* or platform_sdl_*
                      -- everything goes through the koboy_platform vtable,
                      which is what lets it link into every test binary.
                      koboy_stop is DEFINED here for the same reason.
                      tests/test_screens.c + tests/fakeplat.h
src/romlist.c         lists ONE directory of rom_dir at a time -- folders
                      first, then files, ".." below the root; feeds ui.c's
                      widget. It does NOT recurse: the flatten it replaced put
                      the same "Game and Watch/" prefix on 59 rows.
                      romlist_is_rom is an ALLOWLIST (.gb/.gbc/.mgw/.nes/.min/
                      .ws/.wsc/.ngp/.ngc/.a26/.col/.int/.sms/.gg/.md/.sfc/
                      .smc/.pce/.gba/.zip) that must stay in step with
                      config_core_for_rom's table -- a real NES collection
                      ships .pal files beside the ROMs, WonderSwan and Neo Geo
                      Pocket ones ship boot.rom/boot1.rom, an Intellivision one
                      ships boot0-boot3 of which two ARE the BIOS. `.bin` is
                      deliberately unclaimed though three cores accept it:
                      exec.bin and grom.bin are .bin
src/uiscript.c        replays a synthetic input script (tap/key/idle/menu)
                      into the ROM BROWSER and, via the `menu` verb, the
                      in-game MENU -- --ui-script, for bounded unattended
                      runs. A run whose script selects nothing exits 4.
src/state.c           save-state paths and slot labels, KOBOY_STATE_SLOTS (3)
                      slots per ROM, 1-based
src/safefile.c        temp-file/fsync/rename write + all-or-nothing read,
                      extracted from sram.c so save states share its
                      discipline; used by save states, SRAM and screenshots
src/png.c             an 8-bit greyscale PNG writer using STORED deflate
                      blocks -- no zlib, because the dependency ceiling
                      forbids one and a stored-block stream needs no
                      compressor, only CRC32 and Adler32. Do not "improve" it
                      into a Huffman coder
src/shot.c            MENU -> SCREENSHOT: the filename stem, the counter
                      (SCANNED off shot_dir, never held in memory), and the
                      composite. THE COMPOSITE IS THE POINT: `panel` holds the
                      faceplate, video.c's buffer holds the game, and nothing
                      holds both -- so a capture builds the whole picture
src/stats.c           per-stage (core/submit/blit/refresh) timing, the
                      koboy.log `stages` line
src/recent.c          the most-recently-played list. recent_touch is PURE (no
                      filesystem) so it tests like romlist_is_rom; load/save
                      are thin wrappers over safefile.c
src/calib.c           first-run page-turn key calibration. It MUST be
                      escapable by a touch: the loop advances only on a raw
                      key press, and a touch-only Kobo has no key to press
src/btinput.c         finds a Bluetooth gamepad by PARSING
                      /proc/bus/input/devices -- no libbluetooth, no D-Bus, no
                      new dependency. bluetoothd's `input` plugin has already
                      turned the pad into an ordinary /dev/input/eventN, so
                      "controller support" is finding the right node. The parse
                      is pure, so it tests against recorded /proc text rather
                      than a physically present device. platform_kobo.c rescans
                      once a second (BlueZ reconnects on its own schedule,
                      usually AFTER koboy starts) and deliberately does not
                      EVIOCGRAB the node
src/pacing.c          when the next frame reaches the panel. TWO gates, ANDed:
                      present_divisor as a minimum GAP between presents (a gap,
                      not `frames % divisor`, so a hold expiring off-lattice
                      does not forfeit the stride), and an area-scaled settle
                      HOLD that stops koboy starting a full-area update the
                      panel has not finished. The gate advances in pacer_tick,
                      NOT pacer_presented: main.c exits early on an unchanged
                      frame, and a gate waiting on presentation would run
                      video_submit -- the 17 ms bottleneck -- every core frame
                      on a static screen

-- multi-system: koboy is no longer Game-Boy-only ------------------------
Fourteen cores ship for FIFTEEN systems -- Genesis Plus GX answers for two
of them (.sms/.gg is one, .md the other). The counting that matters is
README.md's table, which has fifteen rows; BUILD.md and this file agree with
it. Extension -> core -> layout, all decided in config.c:
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
  .md       genesis_plus_gx_libretro.so  LCD strip, six-button ROWS6 face
                                         (X Y Z over A B C), SELECT says MODE
                                         -- SAME .so as .sms/.gg. The Mega
                                         Drive cost NO build at all.
  .sfc/.smc snes9x2005_libretro.so       LCD strip, diamond face + L and R
  .pce      mednafen_pce_fast_libretro.so DMG faceplate, NO extra discs
  .gba      gpsp_libretro.so             LCD strip, PAIR2 face (A and B only)
                                         + L and R shoulder pills
  .zip      fbneo_libretro.so            DMG faceplate + 3 and 4 discs
                                         (arcade -- .zip is a CONTAINER, not
                                         a system, and nothing else koboy
                                         ships can open one)
FOUR SYSTEMS ARE ON THE LCD STRIP, the rest on the DMG faceplate. .mgw
because the unit draws its own buttons; .md and .sfc/.smc because their pads
do NOT FIT the faceplate's five controls plus two spare discs; .gba for a
different reason -- it fits, but the two spare pockets are FACE pockets and a
GBA's L and R are a left one and a right one. config_layout_for_rom argues all
four.
ADDING A SYSTEM is a build script plus four wiring points:
config_core_for_rom's table, romlist_is_rom, a non-phony $(CORE_*_SO) rule,
and packaging/roms-README.txt (a real file that is COPIED). A pad that does not
fit the DMG faceplate also needs config_layout_for_rom and
config_lcd_pad_for_rom; one that fits with an extra disc or two needs
config_extra_buttons_for_rom. Mega Drive needed wiring points and NO BUILD --
check whether a shipped core already covers the system first.

scripts/build-gw-core.sh      gw-libretro (Game & Watch)
scripts/build-fceumm-core.sh  libretro-fceumm (NES). Three non-default make
                      switches, justified in the script's header; the headline
                      is WANT_32BPP=0, which gets RGB565 instead of XRGB8888
                      and halves what video_submit reads per frame.
scripts/build-pokemini-core.sh  libretro/PokeMini (Pokemon Mini). NO BIOS
                      SHIPS AND NONE IS NEEDED -- the core links its own free
                      one, verified against an EMPTY system directory.
scripts/build-wswan-core.sh   beetle-wswan (WonderSwan + Color). One core,
                      two extensions. No non-default switch: its unix block
                      already picks RGB565.
scripts/build-race-core.sh    RACE (Neo Geo Pocket + Color). Chosen over
                      beetle-ngp by MEASUREMENT, both cross-built: RACE is pure
                      C and ~3x faster; beetle-ngp needs -static-libstdc++ and
                      then libm + ld-linux-armhf. Numbers in the script.
scripts/build-stella-core.sh  libretro/stella2014-libretro (Atari 2600).
                      NOT libretro/stella2014 (404s) and NOT libretro/stella,
                      which forces -std=c++17 -- a flag Linaro 4.9.2 rejects
                      outright. That is the whole choice.
scripts/build-gearcoleco-core.sh  drhelius/Gearcoleco (ColecoVision;
                      libretro/gearcoleco 404s too). The libretro port is a
                      SUBDIRECTORY of the emulator repo. NEEDS A BIOS: without
                      one it draws a static NO BIOS bitmap forever, which only
                      a rendered frame reveals.
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
                      snes9x2010 and snes9x by MEASUREMENT, all cross-built:
                      fastest by 1.36x/1.58x and pure C. THE COMPATIBILITY
                      FOLKLORE IS FALSE -- this revision HAS SuperFX and SA-1;
                      Star Fox, Yoshi's Island and Kirby Super Star all run,
                      RENDERED and looked at rather than inferred from a load.
scripts/build-pce-core.sh     libretro/beetle-pce-fast-LIBRETRO (PC Engine).
                      The bare name 404s -- fourth variant of that trap. Its
                      Makefile links with $(CXX) and -lrt whatever it
                      compiled, so counting .cpp files predicts the wrong
                      flags; -Wl,--as-needed is what actually gives the
                      libm+libc closure. Cartridge .pce ONLY.
scripts/build-gba-core.sh     libretro/gpsp (Game Boy Advance). Chosen over
                      mGBA and vba-next by MEASUREMENT, all cross-built -- and
                      the host column UNDERSTATES it, because gpSP's dynarec
                      targets ARM and was off on x86_64 while neither rival has
                      one. The received wisdom that vba-next is the fast one is
                      FALSE at these revisions: it is the slowest on all eight
                      titles. Numbers and the dynarec check in the script.
scripts/build-fbneo-core.sh   libretro/FBNeo (arcade). NOT
                      finalburnneo/FBNeo -- a NEW variant of the 404 trap:
                      BOTH names exist and are real FBNeo repositories, and
                      only the libretro fork has src/burner/libretro. THE
                      REVISION IS PINNED BY SHA to the day the owner's dat was
                      published, because the version number 1.0.0.03 has
                      covered five years of master and a mismatched romset
                      fails exactly like a broken core. 7-Zip is compiled OUT:
                      lib7z does not build against glibc 2.19's headers.
Nine of the fourteen CORES are pure C: closure is libm+libc or less. The two
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
KOBOY_BTN_* bit and label per slot, r == 0 meaning "empty". Pokemon Mini fills
one ("C"); WonderSwan two ("L1", "R1"); ColecoVision two ("K1", "K2" -- keypad
1 and 2, without which no cartridge starts); Intellivision two ("KEY", "TOP"),
where KEY is not a button but FreeIntv's hold-to-show-the-mini-keypad
modifier, which is what makes all twelve keypad keys reachable; an arcade
board two ("3", "4", because there the faceplate's own B and A really ARE
buttons 1 and 2). THREE consumers guard on r (chrome_controls_top, the DMG
renderer, input.c's hit test), each with a distinct failure if the guard goes.
The bit is always the CORE's choice, read off its input descriptors.

TWO of the fifteen systems have a 12-KEY KEYPAD the faceplate cannot draw, and
titles on both refuse to start without it. Check any new system's real control
set against what the faceplate offers BEFORE assuming DMG is enough; this has
cost a round on Game & Watch, Pokemon Mini, WonderSwan and both of these.

ARCADE is the first system where "what does the hardware have" has no single
answer -- 227 different boards -- so the two discs were chosen by COUNTING
every romset's input descriptors. FBNeo's map is flat (B=Button 1, A=2, Y=3,
X=4, SELECT=Coin, START=Start) and the counts decided it: Y 134 boards, X 71,
against 45-48 for each shoulder there is no room for. Do the same for the next
multi-board system.
```

Path resolution is against `/proc/self/exe`'s directory — **`dlopen` never
searches the cwd**, which cost a debugging round when the core sat right beside
the binary and still failed to load.

## Reference documents

v2-core (the ROM browser, in-game MENU, save states, multi-rect dirty regions,
the redrawn faceplate) is done. So is the Bluetooth half
(`docs/superpowers/plans/2026-08-25-koboy-v2-bluetooth.md`): `src/btinput.c`
ships and `platform_kobo.c` opens the pad it finds. That plan is the design
record, not a to-do list.

| Document | What it holds |
|---|---|
| `docs/superpowers/specs/2026-08-24-koboy-design.md` | The v1 design, and **four appendices of measured corrections**. The appendices override the body wherever they disagree. |
| `docs/superpowers/specs/2026-08-25-koboy-v2-design.md` | The v2 design: the mode machine, save states, the faceplate, and §13's open measurements. |
| `docs/FOLLOWUPS.md` | 69 findings that are still open, grouped by subsystem, with a six-item "Start here" at the top. Everything CLOSED and everything whose only content was "not run on hardware" was cut (2026-08-28); the 41 retired numbers are indexed in a table at the bottom, so a `#N` in a source comment still resolves. Numbers are never reused. The live ones: **#23** (`video_submit` is the bottleneck on all fifteen systems and nothing has optimised it), **#84 / #72** (one file segfaults the process, and twelve cores have never been swept for the same), **#92 / #95** (two unexplained SIGSEGVs in the owner's log, on a path a remote session cannot exercise), **#78** (nine systems auto-fit with no measured scale ceiling), **#25** (scroller smearing, improved by 1-bit output and not solved). |
| `docs/device-workflow.md` | Deploying, launching, diagnosing, and the traps. |
| `TESTED.md` | The device matrix, and the record every "measured" claim in this file points at. Exactly ONE device is verified (a Kobo Libra 2). All fifteen systems have rendered on it; only the Game Boy has been played. Sections are dated and LATER ones supersede earlier ones — three "NOT RUN ON THE DEVICE" sections dated 2026-08-27 are overturned by "All fourteen systems run on the device" later the same day, so read the file forwards. |
| `docs/cross-compiling.md` | Toolchain, including why koxtoolchain was abandoned. |
| `docs/probe-readme.md` | Profiling a device nobody has tried. |
| `README.md` | The PUBLIC readme, for someone who owns a Kobo and has never built anything. Not reference material -- if you need the core/extension table, this file has it. |
| `BUILD.md` | Building from source for a stranger: host, toolchain, cores, packaging. Folds in `docs/cross-compiling.md` rather than duplicating it. |
| `INTERNALS.md` | The architecture and the measured decisions, for someone reading the source. "What the hardware overruled" below, expanded for an audience that was not in the room. |
| `LICENSES.md` | koboy is GPL-3 (`LICENSE`). Per-core terms, with pinned commits. **Three cores restrict commercial use** -- Genesis Plus GX, FBNeo, and snes9x2005, whose clause is 160 lines below an MIT grant. |
| `scripts/pins.txt` | The upstream commit of every shipped dependency. |
| `VERSION` | The release number, and the ONLY place it is written. Three things read this file -- the Makefile, `tests/test_dist.sh` and `release.yml`'s tag gate -- so a release is one edit. It was a `VERSION :=` line in the middle of the Makefile and the other two each parsed it back out with their own awk. |
| `.github/RELEASE-NOTES.md` | The body of the NEXT GitHub release, passed to `gh release create --notes-file`. Rewrite its top half before tagging; `release.yml` refuses a tag whose version the file does not mention, because stale notes fail silently and mislead the only person reading them. |
| `.github/workflows/` | `ci.yml` on push (host only, no toolchain); `release.yml` on a `v*` tag (cores as a matrix, cached per pinned SHA). |

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
  crushes *more* pixels to black — 8.9% vs 6.7% over 38 gameplay frames from
  19 titles — because what it returns to blue it takes from green. What
  removes the crushing is a **shadow lift**, equivalent to lowering
  `video_quantise4`'s first threshold: 6.7% → 2.5%. The shipped default
  (`gray_map = balanced`) needs BOTH — weights (81,118,57) *and* the lift.
  Selectable, because this is a judgement about a reflective panel that cannot
  honestly be made from a host render: `MENU → GREYSCALE` cycles it in-game
  and writes the ini key back.
- **The Game Boy's palette is neutral, so none of that touches it.** gambatte
  emits exactly rgb(0,0,0)/(82,85,82)/(173,170,173)/(255,255,255); every
  mapping is the identity on neutral grey and the lift keeps both mid greys
  inside their levels, so the DMG golden is byte-identical and **no per-system
  exemption exists**. Do not add one keyed on 160x144 — Game Gear is also
  160x144 and is a colour system.
- **DU4's failure is NOT evidence against DU; they are not the same
  waveform.** DU4 is FOUR-level, DU is two-level. FBInk's header says a
  DU-class waveform "will leave on-screen pixels as-is for new content that is
  *not* B&W", so against four-level output every pixel landing on one of the
  two MIDDLE levels is one the panel declines to touch — exactly what "DU4
  cannot erase" looked like from the inside. `waveform_fast = du` plus
  `force_dither` (the **MENU → MOTION** row cycles the PAIR) was the untried
  combination and the one that worked: 1-bit output ships as the default and
  the owner confirms it fixes the smearing. Not free — clean two-level
  transitions cost 153.5 ms a full-rect refresh where four-level DU4 cost
  24.1 ms, which is what area-aware pacing paces to. `docs/FOLLOWUPS.md` #25,
  #96.
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
  layout only; LCD still uses max). Max made "any frame in [1, max] fits" true
  by construction and cost a SNES 54% of its picture area against a 512x512
  mode snes9x2005 never enters. Under max-sizing, measured on the verified
  panel: SNES presented at 597x448 and PC Engine at 583x486 against the Game
  Boy's 800x720 — less than half the area, on higher-resolution systems.
  `video_fit_rect` now shrinks a frame the rect cannot hold, which replaces
  the fits-by-construction guarantee — DO NOT DELETE THAT BRANCH. The speed
  cost, measured: free for PC Engine and for SNES titles with headroom,
  96%→78% for Kirby Super Star, which is what the per-system scale ceiling
  exists to hold. Note the perverse coupling before changing any of this: a
  smaller rect costs less `video_submit`, so a system's CPU budget SHRINKS
  when its picture grows. `docs/FOLLOWUPS.md` #73.
- **Benchmarking this device back to back gives numbers that climb.** A first
  pass with no gaps read up to 2.4x high and kept rising; an isolated re-run of
  the same binary on the same ROM came back at the first row's figure. Ten
  seconds of idle between runs, every time.
- **A libretro core's geometry is discovered, not queried.** `gw-libretro`
  answers `retro_get_system_av_info` with a 128x128 placeholder on all 59
  titles until its first `retro_run()`, then announces the real canvas via
  `SET_GEOMETRY`/`SET_SYSTEM_AV_INFO`. Sizing buffers from the post-load query
  — what the libretro docs imply — renders a 973x532 game into a 128x128 box.
  `main.c` polls `core_geometry_changed()` every frame, which also covers a
  mid-session ROM switch: measured, three titles loaded back to back into one
  core instance each re-announced.
- **G&W is CHEAPER than the Game Boy, not more expensive.** `video_submit`
  scales with DESTINATION pixels (~4.7 ms + 20.7 ns/px), and the Game Boy is
  upscaled 160x144 -> 800x720 = 576k px = 16.6 ms. G&W runs at 1x: Parachute
  260k px (10.1 ms), Mario Bros. 518k px (15.4 ms). Comparing G&W's canvas to
  the Game Boy's 160x144 *source* suggests a 20x blowup and is wrong.
- **The arcade "portrait panel, portrait game" win is real but smaller than it
  sounds, and two headline titles smear anyway.** A vertical board presents at
  672x864 inside a square 864x864 rect — 580,608 pixels against the Game Boy's
  576,000, in the panel's own aspect. NOT the 1264x1626 a fractional 5.6x fit
  would give, because the DMG faceplate reserves everything below
  `chrome_controls_top` and the scale search is integer. And "single-screen
  boards cannot smear" is wrong for the two most famous: measured with koboy's
  own dirty diff, Galaga changes 67% of the game rect per frame and Galaxian
  86%, because the STARFIELD scrolls. Dig Dug, Donkey Kong and Ms. Pac-Man are
  1.5 to 2.6%.
- **The pixel-aspect correction earns itself on PC Engine, which changes
  horizontal resolution mid-game.** Titles alternate 256 and 352 wide (not
  336); Military Madness does it five times in 2500 frames. Both modes have
  the same DISPLAY width, so with the correction on the picture is 583x486
  centred at x=632 in BOTH -- same size, same place, only the detail changes.
  With `pixel_aspect = false` it jumps between 512x486 and 704x486 every scene
  change. Verified by rendering both sides and looking.
- **The panel's refresh duration is ~94% FIXED, not area-scaled, and the fixed
  term belongs to the WAVEFORM.** Measured 2026-08-27 with `koboy-probe
  --coexist` across five region sizes spanning 49x in area: DU4 15.1 ms +
  15.0 ns/px, A2 96.4 + 17.2, DU 144.4 + 15.8, GC16 357.7 + 22.5 -- so at the
  shipped 800x720 rect, 24.1 / 106.3 / 153.5 / 370.6 ms. The per-pixel term is
  the same for every waveform (the controller's pixel processing) and small.
  This does NOT contradict "refresh cost scales with area": dirty rects still
  pay, because the area term is real and a smaller rect leaves less of the
  picture in flight. It does mean a small update is not a *fast* update.
  Reproduced to 0.1%; Appendix E has the method and why the wait ioctl had to
  be avoided (`unreliable_wait_for=1` returned literal 5-second timeouts in six
  of fifty cells).
- **On 1-bit content, AUTO *is* DU** -- measured identical to within 0.5 ms at
  three region sizes. So the MOTION ladder's `1-BIT / DU` rung selects what
  `1-BIT / AUTO` was already getting, which is why the owner found them
  indistinguishable. It also prices the 1-bit fix: clean two-level transitions
  cost 153.5 ms where four-level DU4 cost 24.1 ms, a factor of 6.4.
  `docs/FOLLOWUPS.md` #98.
- **Nothing below koboy applies back-pressure.** The probe submitted a new
  full-rect update every 6-13 ms without ever blocking, against a 153 ms
  completion. The EPDC accepts work it cannot do and says nothing, so
  over-driving the panel is invisible from inside the process -- which is why
  `present_divisor` alone could ask for fifteen full-area updates a second with
  nothing complaining. Pacing has to be koboy's job.
- **Presentation is paced by AREA, not by frame count.** `present_divisor`
  alone paced a two-tile sprite move and a whole-screen scroll identically.
  `settle_base_ms` / `settle_full_ms` charge each presented frame
  `base + full * dirty/whole` and hold the next present until it elapses; the
  divisor remains a ceiling. Measured (Sonic Chaos, 900 core frames, 879x576
  rect): at divisor 3 the throttle cuts 235 presents to 112 -- the panel was
  being asked for more than twice what it can finish -- and at divisor 8 it is
  nearly inert, 90 -> 89. That second row is the evidence, and nobody arranged
  it: **divisor 8 is what the owner reached by eye, and the model independently
  agrees there is nothing left to take away there.** Divisor 3 with the
  throttle beats their manual 8 outright, 112 presented frames against 90,
  because it gives back the small-area frames a flat divisor throws away.
  `settle_base_ms` ships at **0** although the measured fixed term is 144 ms,
  deliberately: the model decides when the next update does VISIBLE harm, not
  how long the last one takes, and charging 144 ms to everything pins the
  device to 6.5 fps on static screens. `src/koboy.h`, `docs/FOLLOWUPS.md` #100.
- **Four-level content is what smears; 1-bit content does not.** The fast
  waveforms are TWO-LEVEL -- DU drives a changed pixel to black or white, so
  asking for an intermediate grey at partial-refresh speed lands between,
  leaving a stale ghost AND overshoot brighter than the background. Rendering
  genuinely two-valued (`force_dither`, `MENU -> MOTION`) fixes the motion
  smearing open since v1 -- confirmed on the panel by the owner, 2026-08-27.
  The old "forced DU4 cannot erase" finding is the same mechanism from the
  other side.
- Scale 5 with a procedural faceplate, not full-width 7x.
- `viewVertOrigin` is **not** a blit offset.
- "Grab the buttons but not power" is impossible: they share `gpio-keys`.

## Testing culture — this one is not optional

**Three tests in v1 passed whether or not the code they guarded was present.**
One took four review rounds to fix. So: after writing any safety or regression
test, *break the thing it guards and confirm the test fails.* Record the mutant
and its output.

The subtlest instance is a CLASS, not an accident: a sentinel guard band
cannot detect an unclamped `memset` whose length underflows to near
`SIZE_MAX`, because where glibc writes for such a length is an implementation
detail that on x86-64 lands *inside* the buffer, not in the guard. The fix was
to stop observing UB and assert the clamped values directly (`chrome_bands`).
If a test can only fail via UB, it is not a test.

Corollary from the v1 endgame: the first-run deadlock the final review caught
was invisible to twenty per-task reviews because the scripted-run branch skips
calibration — **every automated test took the one path that could not reach the
bug.** When a code path exists only for scripted runs, ask what it is hiding.

**The instrument needs a mutant too, and both of ours did.** `make lint` ran
clean and was WRONG: clang reported an injected shadowed local and `make` still
exited 0, because a warning is not an error — `-Werror` is what makes that
target a gate rather than a printer. Two of its four diagnostic flags are also
not what their names suggest, and only a mutant says so:
`-Wtautological-compare` does not catch `unsigned x >= 0` and
`-Wunreachable-code` does not catch a statement after `return`. `make coverage`
was checked the same way — neutering every `pacer_settle_us` call in
`tests/test_pacing.c` dropped `src/pacing.c` from 48/48 to 40/48, exactly the
eight lines of that function.

**`gcov` cannot see inside a test here.** `tests/test.h`'s `TEST_MAIN(...)`
takes the whole test body as one macro argument, so gcc attributes all of it to
the expansion point: ten instrumented lines for a 663-line file. So the
mechanical hunt for a check that cannot fail does NOT exist yet — `make
coverage` measures `src/` honestly and says nothing about `tests/`. Finding a
vacuous assertion is a human reading code, and the shapes to read for are: **an
assertion gated on the behaviour under test** (`if (a2 == UI_SELECT)
CHECK(...)`), **a counter asserted against its own initialiser** with a
`continue` above it, and **a comparison of two things the test itself wrote
down**. All three were live as of 2026-08-28; the tooling fix is
`docs/FOLLOWUPS.md` #105.

**A fourth shape, which got past this section's author: a check whose FIXTURE
does not put the code in the state the check is named for.**
`tests/test_screens.c` had a case for "a tap on the faceplate's A disc, under a
full-panel list, selects the row and does not page" — the whole reason
`screen_list` calls `input_ui_state` instead of `input_state`. It tapped the A
disc's ROW at the panel's horizontal CENTRE, nowhere near the disc, so no A bit
was ever synthesised and swapping the two functions changed nothing. The check
passed, read correctly, and tested the wrong thing; the fix was to tap the
disc's own centre computed from `koboy_layout`. **Ask of every new test not
only "can this fail" but "does the fixture actually reach the state the name
claims" — and the mutant answers the second question too.**

## Known unfinished

- **ALL FIFTEEN SYSTEMS HAVE NOW RUN ON A KOBO.** Established: every shipped
  core `dlopen`s, resolves geometry, paces itself and renders on the Libra 2;
  both BIOS files have been read off the device's own card (ColecoVision's boot
  screen was RENDERED and looked at, because a log cannot tell a working BIOS
  from a missing one); and the picture HAS been rotated on the panel — Galaga
  presents at 648x864 portrait.
  **What those runs are not is a playtest.** They are `--frames` over ssh with
  Nickel up: loading, geometry, pacing and speed, and nothing a finger does.
  The owner has separately played by hand (next-to-last bullet), but no
  PER-SYSTEM playtest is written down, so the K1/K2 and KEY/TOP discs, the LCD
  strip's six-button Mega Drive face and the GBA's shoulder pills have no
  record of a finger on them, and the pixel-aspect correction that changed
  eight systems' presentation has never been judged by a person on the panel.
  "Has run" is answered; "has been played" only in aggregate.
- **The two-point host-to-device fit is retired for the systems it was built
  for, and still live for the arcade table.** Mega Drive, SNES and PC Engine
  were extrapolated (`device ~= 13.45 * host + 979 us`, reconciling gambatte's
  23.4x against fceumm's 17.2x) and have since been measured directly, so read
  their rows and not the fit. The arcade table's separate flat 7x — a different
  instrument, do not compare them — still carries 226 of 227 boards. One board
  has been measured and the model held to within 7% (Galaga, 4.4 ms against
  ~4.7 predicted), a data point at the CHEAP end; the boards near a 16.7 ms
  frame are where the error's sign decides playability. `docs/FOLLOWUPS.md`
  #56.
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
- **The Atari 2600 rendered about 1.75x too tall, and only rendering frames
  and looking at them found it** — every numeric check passed. Fixed: the
  core's `geometry.aspect_ratio` is carried through the fit. Two corrections
  the original diagnosis needed, both worth keeping because they are the kind
  of thing that gets re-derived wrong: the 2600 was **not** the only affected
  system (eight are, including the NES), and `base_width / delivered width` is
  the WRONG signal — it gives 2.0 where the truth is 1.75.
  `docs/FOLLOWUPS.md` #65 has the two structural choices the fix made and
  nobody has re-examined.
- **The save path (cartridge SRAM) has run on hardware, and the
  destructive-truncation bug it was carried to test is proven fixed.**
  2026-08-26: Zelda (cart type `0x03`, `rambanks: 1`, the first battery-backed
  title this project has run) verified write, read round-trip (md5-identical),
  and the truncated-`.srm` path, which left the file at its truncated size
  rather than destroying it further. `TESTED.md` has the numbers.
  **Save *states*** (`state.c`, `safefile.c` — a different mechanism, reached
  through `MODE_MENU`) are driven end to end on the host by
  `tests/smoke_host.sh` and have been exercised by hand on the device.
- **A screenshot costs one frame about 6 ms, measured, so it stays on the main
  loop.** `MENU -> SCREENSHOT` arms a capture the next presented frame writes
  as a PNG into `shot_dir` (`src/shot.c`, `src/png.c`, plus main.c's arming
  branch and capture site); the PNG decodes at 1264x1680 after transfer, so the
  hand-rolled stored-DEFLATE writer works there too. Still unknown: whether the
  confirmation plaque's FULL-refresh erase leaves residue where the text was --
  a judgement no instrument here can make. `docs/FOLLOWUPS.md` #101-#104.
- **The 1-bit motion pair has been seen on a panel and shipped.**
  `force_dither` is the default, **MENU → MOTION** cycles `4 GREYS / AUTO` →
  `1-BIT / AUTO` → `1-BIT / DU`, and the owner confirms it fixes the smearing.
  Two things stayed open: the third rung selects nothing the second did not
  already get (AUTO measures identical to DU on 1-bit content, #98), and the
  other lever identified — putting a flat background on an extreme level — has
  no shipped mapping that does it without taking the foreground too (#96).
  **No measurement koboy can take can see ghosting**: residue is panel-side and
  the dirty diff only compares koboy's own buffers, which is why this defect
  outlived two attempts and why the verdict had to be the owner's.
- **One verified device, and everything measured is measured on that one.** The
  owner has played on it by hand — cores, controls, the in-game MENU, cartridge
  saves and save states — so the "nobody has run this on hardware" gap is
  closed. That is NOT a regression test: nothing in `make test` drives it, and
  `docs/FOLLOWUPS.md`'s "Coverage gaps" is where those live. Nearly every
  automated device run in `TESTED.md` is `--frames` over ssh with Nickel up,
  which is why #95 matters: the takeover and input path is a surface a remote
  session cannot exercise at all.
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
  compile. `MODE_MENU`'s coverage gap is closed for GREYSCALE, FRAMES, SAVE
  STATE and LOAD STATE, which `tests/smoke_host.sh` now drives through
  `--ui-script`'s `menu` verb, and **still open for CHOOSE ROM and QUIT** —
  the same hook reaches both and nothing uses it (`docs/FOLLOWUPS.md` #18).

## Conventions

Comments here record **why**, not what, and especially why a non-obvious choice
is not an oversight ("DISABLED BY DEFAULT, and the history matters because 'off'
looks like an oversight otherwise"). Clamps and guards carry a note saying they
are live so nobody deletes them as dead code. Match that voice.

ROMs are git-ignored (`*.gb`, `*.gbc`, `*.sav`, and every other system's
extensions) and must never be committed. `.md` is the ONE entry that is also an
everyday non-ROM extension, so it is anchored to the roms directories rather
than global — an unqualified `*.md` would silently stop this file, README.md
and all of docs/ from ever being committed again. Neither may a BIOS: two
systems need one, and `tests/test_dist.sh` asserts no `.rom` or `.bin` reaches
the package. `*.pgm` is marked binary in `.gitattributes` — without it git
diffs a golden image as text and a review package balloons to megabytes.
