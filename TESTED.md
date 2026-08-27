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

### NES and Pokemon Mini: NOT RUN ON THE DEVICE, 2026-08-26

Both systems were added and wired on the same day and **neither has been on
a Kobo**. Everything below was measured on the x86_64 dev host with
`scripts/probe_core.c` against ROMs from a real collection; the ARM cores
were cross-built and passed `scripts/verify-core.sh` (libm + libc only, for
both), and that is the whole of the device-side evidence.

| | NES (fceumm) | Pokemon Mini (PokeMini) |
|---|---|---|
| Geometry before the first `retro_run` | 256x240 | 96x64 at 1x (384x256 at the core's own 4x default) |
| ...and after | unchanged | unchanged |
| `SET_GEOMETRY` / `SET_SYSTEM_AV_INFO` at runtime | never | never |
| Pixel format | RGB565 (with `WANT_32BPP=0`; XRGB8888 otherwise) | RGB565 |
| Asks for a system directory | yes | yes |
| Asks for a save directory | no | yes |
| `valid_extensions` | `fds\|nes\|unf\|unif` | `min` |
| `RETRO_MEMORY_SAVE_RAM` | 8192 for a battery cart, 0 without | 0 -- it writes its own `.eep` |
| BIOS required | not for `.nes` (yes for `.fds`, which koboy does not list) | **no** -- the core links a free one, verified against an empty system dir |
| ARM closure | `libm.so.6`, `libc.so.6` | `libm.so.6`, `libc.so.6` |
| ARM size, stripped | 2.2 MB | 217 KB |

Neither reports a placeholder geometry, unlike gw-libretro -- but that was
established by asking, which is the only reason it is written here.

Resolved presentation, from `config_resolve_profile` at the shipped
`scale = 5`, with `video_submit` estimated from #23's 4.7 ms + 20.7 ns/px:

| System | Panel | Scale | Rect | dst px | est. `submit` |
|---|---|---|---|---|---|
| NES 256x240 | 1264x1680 | 3 | 768x720 | 553k | 16.1 ms |
| NES 256x240 | 1440x1920 | 4 | 1024x960 | 983k | 25.0 ms |
| Pokemon Mini 96x64 | any | 5 | 480x320 | 154k | 7.9 ms |
| (Game Boy, for scale) | 1264x1680 | 5 | 800x720 | 576k | 16.6 ms |

NES lands almost exactly on the Game Boy's presentation cost. Pokemon Mini
is by far the cheapest thing koboy renders and correspondingly small on the
panel -- `scale = 0` auto-fits it to 13x (1248x832, ~26 ms). See
`docs/FOLLOWUPS.md` #34 and #35.

### WonderSwan and Neo Geo Pocket: NOT RUN ON THE DEVICE, 2026-08-26

Both systems were added and wired on the same day and **neither has been on
a Kobo**. Everything below was measured on the x86_64 dev host with
`scripts/probe_core.c` and a throwaway harness driving koboy's own
`config.c`/`video.c`/`chrome.c` against ROMs from a real collection; the ARM
cores were cross-built and passed `scripts/verify-core.sh`, and that is the
whole of the device-side evidence.

| | WonderSwan (beetle-wswan) | Neo Geo Pocket (RACE) |
|---|---|---|
| Geometry before the first `retro_run` | 224x144 base, **224x224 max** | 160x152 base and max |
| ...and after | unchanged | unchanged |
| `SET_GEOMETRY` at runtime | **yes, on rotation only** -- base becomes 144x224, max stays 224x224 | never |
| Pixel format | RGB565 | RGB565 |
| Asks for a system directory | no | no |
| Asks for a save directory | no | **yes -- and this is its save path** |
| `valid_extensions` | `ws\|wsc\|pc2` | `ngp\|ngc\|ngpc\|npc` |
| `RETRO_MEMORY_SAVE_RAM` | 128 / 2048 / 8192 / 32768 / 262144 by cart, 0 for the five titles with no battery | **0, always, on both candidate cores** |
| BIOS required | **no** -- verified against an empty system directory | **no** -- links its own HLE BIOS, same verification |
| ARM closure | `libc.so.6` **alone** | `libc.so.6` **alone** |
| ARM size, stripped | 607 KB | 208 KB |
| Host emulation cost | 0.21 ms/frame | 0.16-0.25 ms/frame |

Neither reports a placeholder geometry, unlike gw-libretro -- established by
asking, which is the only reason it is written here.

**The rotation question, settled by measurement.** A WonderSwan has two grips
and many titles are played with the console on its side. beetle-wswan does not
detect that per title (the header bit that would is `#if 0`'d out upstream)
and does not report rotated geometry at load. What it does is offer
`wswan_rotate_display = manual`, which toggles rotation on SELECT, and
`wswan_rotate_keymap = auto`, which swaps the button map to match. koboy
answers neither option, so both keep those defaults, and that turns out to be
exactly right: koboy returns false for `SET_ROTATION` (it is an unknown
command), so the core rotates the pixels itself and announces `144x224` base
with max unchanged -- which main.c's base-only branch already absorbs without
re-fitting anything. Verified end to end: GunPey renders sideways in a 896x576
box, and after one SELECT press upright in a 576x896 one, with no koboy change
at all.

**What the rotation costs.** `wswan_rotate_keymap = auto` also moves the
console's own A and B onto `JOYPAD_L` / `JOYPAD_R`, which the DMG faceplate
had no buttons for. Measured on a real title: `Kaze no Klonoa - Moonlight
Museum` in portrait responds to exactly two inputs, START and `JOYPAD_L`, so
without those buttons it cannot be started. The faceplate now draws an L1 and
an R1 disc for `.ws`/`.wsc`. Still **not** reachable: the rotated map's
X-cursor up/right, on `JOYPAD_Y` / `JOYPAD_X` -- see `docs/FOLLOWUPS.md` #42.

Resolved presentation, from `config_resolve_profile`, with `video_submit`
estimated from #23's 4.7 ms + 20.7 ns per destination pixel:

| System | Panel | Scale | Rect | drawn | est. `submit` |
|---|---|---|---|---|---|
| WonderSwan 224x144 | 1072x1448 | 3 | 672x672 | 672x432 | 10.7 ms |
| WonderSwan 224x144 | 1264x1680 | 4 | 896x896 | 896x576 | 15.4 ms |
| WonderSwan 144x224 (rotated) | 1264x1680 | 4 | 896x896 | 576x896 | 15.4 ms |
| Neo Geo Pocket 160x152 | 1072x1448 | 5 | 800x760 | 800x760 | 17.3 ms |
| Neo Geo Pocket 160x152 | 1264x1680 | 6 | 960x912 | 960x912 | 22.8 ms |
| Neo Geo Pocket 160x152 | 1440x1920 | 7 | 1120x1064 | 1120x1064 | 29.4 ms |
| (Game Boy, for scale) | 1264x1680 | 5 | 800x720 | 800x720 | 16.6 ms |

The WonderSwan's rect is SQUARE because its max is: the core reports 224x224
so that both orientations fit one rect and rotation never re-fits. A landscape
title therefore leaves 36% of that rect as margin for the whole session. That
margin used to quantise to solid black; it is now cleared to the lightest of
the four levels and excluded from the per-frame quantise pass, which is where
the "drawn" column above comes from. Neo Geo Pocket is the most expensive
thing koboy renders -- see `docs/FOLLOWUPS.md` #41.

**How four greys actually look.** Rendered through koboy's real pipeline and
examined, not assumed:

- *Mono WonderSwan* is the best case this project has had. The hardware drives
  eight shades and a frame arrives with seven distinct luma values in it;
  quantised to four, GunPey's dithered skies and Makaimura's stonework both
  read cleanly, because the source is already grey and the reduction is a
  regular 8->4 fold rather than a projection.
- *Mono Neo Geo Pocket* is the same story at 160x152: Samurai Shodown's
  portrait screen and Baseball Stars' logo come through as crisp dark-on-white
  line art.
- *Colour is a different matter, and it is the majority of the owner's
  library* (250 `.ngc` against 27 `.ngp`; 175 `.wsc` against 163 `.ws`). A
  WonderSwan Color or NGPC frame carries 20-90 distinct luma values and loses
  exactly what NES loses. The specific failure is saturated mid-luma colour,
  and blue above all: Rec.601 weights blue at 29/256, so a solid blue sky
  lands at luma ~29 and quantises to the DARKEST level. Sonic Pocket
  Adventure's first zone renders with a **black** sky that Sonic himself
  nearly disappears into; Golden Axe's title does the same. Legible, playable,
  but plainly a lossy translation rather than the free one the mono halves
  get.
- **No core option helps.** Looked for, and there is none: `wswan_mono_palette`
  applies only to MONO titles (its default, "Grayscale", is already the right
  answer there), `wswan_gfx_colors` is a bit-depth switch, and RACE's only
  relevant option is `race_dark_filter_level`, which darkens -- the wrong
  direction on reflective paper, and left at its default 0. Improving colour
  would mean a contrast or gamma stage in koboy's own pipeline before
  `video_quantise4`; see `docs/FOLLOWUPS.md` #43.

**Saves diverge between the two systems, and that matters.** A WonderSwan
cartridge exposes `RETRO_MEMORY_SAVE_RAM` and `sram.c` is the right path for
it. A Neo Geo Pocket one does not: `retro_get_memory_size(SAVE_RAM)` is 0 for
every title on both candidate cores, because an NGP saves into flash, and the
core writes that file itself -- `<rom>.ngf` for RACE, `<rom>.flash` for
beetle-ngp -- into whatever directory the frontend answers
`GET_SAVE_DIRECTORY` with. Observed directly: running fourteen titles against
an initially empty directory left twelve `.ngf` files in it. koboy already
answers that query with `cfg.save_dir`, so this works, but it works through
one environment callback that nothing pinned until now
(`tests/test_core.c`).

**Not established:** everything device-side. The ARM cores have not been
`dlopen`ed on a Kobo, nothing has rendered on the panel, no playable-speed
figure exists, the L1/R1 discs have never been touched by a real finger, and
no WonderSwan `.srm` or Neo Geo Pocket `.ngf` has survived a real session.

### Atari 2600, ColecoVision, Intellivision, Master System + Game Gear: NOT RUN ON THE DEVICE, 2026-08-27

Four systems added on one day and **none has been on a Kobo**. Everything
below was measured on the x86_64 dev host with `scripts/probe_core.c` and a
throwaway harness driving koboy's own `config.c`/`video.c`/`chrome.c` against
ROMs from a real collection; the ARM cores were cross-built and passed
`scripts/verify-core.sh`, and that is the whole of the device-side evidence.

| | Atari 2600 (stella2014) | ColecoVision (Gearcoleco) | Intellivision (FreeIntv) | SMS + Game Gear (Genesis Plus GX) |
|---|---|---|---|---|
| Geometry before the first `retro_run` | 320xH base (a LIE, see below), **320x256 max** | 256x192 base, **512x288 max** | 352x224 base and max | 256x192 (SMS) / 160x144 (GG) base, **284x240 max** |
| ...and after | unchanged | unchanged | unchanged | unchanged |
| Frame the core actually delivers | **160**x210 (NTSC) or 160x250/256 (PAL) | 256x192 | 352x224 | 256x192 / 160x144 |
| `SET_GEOMETRY` at runtime | never | never | never | yes, but never above 284x240 |
| Pixel format | RGB565 | RGB565 | **XRGB8888** -- the only one | RGB565 |
| Asks for a system directory | no | **yes, and needs it** | **yes, and needs it** | yes, but runs without one |
| `valid_extensions` | `a26\|bin\|mvc` | `col\|cv\|bin\|rom` | `int\|bin\|rom` | `m3u\|...\|sms\|gg\|sg\|...` |
| `RETRO_MEMORY_SAVE_RAM` | **0, all 82 titles** | **0, all 28 titles** | **0, all 26 titles** | **0x10000 at load** -- see below |
| BIOS required | no | **YES: `colecovision.rom`** | **YES: `exec.bin` + `grom.bin`** | no |
| ARM closure | libm, libc, ld-linux-armhf | libm, libc, ld-linux-armhf | libm, libc, ld-linux-armhf | **libm, libc** |
| ARM size, stripped | 1.7 MB | 682 KB | 526 KB | 5.9 MB |
| Host emulation cost | 0.118-0.305 ms/frame (82 titles) | 0.201-0.232 (28) | 0.044-0.085 (26) | 0.094-0.247 (121) |
| Titles that load and run | 82 / 82 | 28 / 28 | 26 / 26 | 68 / 68 `.sms`, 53 / 53 Game Gear |

**The BIOS question, and it is the first time this project has answered yes.**
Settled by running with an EMPTY system directory and looking at the frame,
because on one of these cores "it loaded" and "it works" are different
questions:

- *Gearcoleco* loads the cartridge, reports 256x192, runs 300 frames without
  an error -- and every one of those frames is a static bitmap that reads
  `NO BIOS`. `src/no_bios.h` is chosen over the real framebuffer whenever the
  BIOS is missing. With `colecovision.rom` (8192 bytes,
  sha1 `45bedc4c...`) in place, BurgerTime runs to its option screen. The
  core tries `colecovision.rom` then falls back to `coleco.rom`.
- *FreeIntv* logs `HALT!` every frame without `exec.bin` and `grom.bin`, and
  runs everything with them.
- *stella2014* never asks for a system directory at all. *Genesis Plus GX*
  asks, but only for the optional Master System boot ROM behind its own
  `genesis_plus_gx_bios` option, which defaults to disabled; all 121 Sega
  titles run against an empty directory.

Neither BIOS ships. Both go in `.adds/koboy/` -- the directory koboy already
answers `GET_SYSTEM_DIRECTORY` with (`src/core.c`, from `koboy.ini`'s
`save_dir`). `tests/test_dist.sh` asserts no `.rom` or `.bin` reaches the
package AND that the generated `roms/README.txt` names all three files.

**Which MiSTer boot ROM is which, settled by content.** The author's
Intellivision folder carries `boot0.rom` through `boot3.rom` in MiSTer's
naming, and `boot1.rom` and `boot2.rom` are BOTH 2048 bytes, so size cannot
tell them apart and the wrong pick gives a machine that runs and draws
garbage. `cmp` against a batocera BIOS tree that names its files properly:

| MiSTer file | Size | Is | sha1 |
|---|---|---|---|
| `boot0.rom` | 8192 | `exec.bin` | `5a65b922b562cb1f57dab51b73151283f0e20c7a` |
| `boot1.rom` | 2048 | `grom.bin` | `f9608bb4ad1cfe3640d02844c7ad8e0bcd974917` |
| `boot2.rom` | 2048 | Intellivoice speech ROM -- **not needed** | `618563e5...` |
| `boot3.rom` | 24576 | ECS expansion ROM -- **not needed** | `b7ccb38b...` |

Confirmed by running: Atlantis draws `(C) IMAGIC 1982` in legible text, and
that text comes out of GROM.

**THE KEYPADS, which is the real work here.** Two of these four systems have
a twelve-key telephone keypad on the controller and on both of them titles
cannot be STARTED without it. Rendered and read, not assumed.

- *ColecoVision*: every cartridge boots into the console BIOS's `TO SELECT
  GAME OPTION, PRESS BUTTON ON KEYPAD` screen. Proved by running Donkey Kong
  for 900 frames pressing only START and A -- still on the option screen --
  and again pressing `JOYPAD_Y`, which is Gearcoleco's keypad 1: the game
  starts. So the faceplate draws **K1** and **K2** discs for `.col`. What
  stays unreachable: keypad 3-9 and 0 (Gearcoleco spreads them across
  shoulders, sticks and an analog axis), and note that koboy's START and
  SELECT are keypad `*` and `#` on this system whatever their moulded labels
  say. See `docs/FOLLOWUPS.md` #49.
- *Intellivision*: BurgerTime and Bump 'n' Jump stop at "Select 1 or 2
  Players", Diner says "then press enter". FreeIntv puts keypad 1-9 ONLY on
  the right analog stick, which koboy has no source for -- nine dead keys.
  Except that the core has a second route its descriptors call "Show
  Keypad": hold `JOYPAD_L` and a 4x3 keypad is drawn into the frame's corner,
  the d-pad moves a cursor over it, and any face button presses the selected
  key. MEASURED: holding L1 and tapping A on BurgerTime's prompt put a `1` on
  the screen. So the faceplate draws a **KEY** disc (that modifier) and a
  **TOP** disc (the hand controller's third action button), and **all twelve
  keypad keys are reachable**. What stays unreachable: the disc's twelve
  DIAGONAL positions -- koboy's touch pad gives eight of sixteen, and
  FreeIntv only offers the rest on the left analog stick. See #50.
- *Atari 2600*: four directions, one fire button, Reset and Select, all on
  the faceplate already. But **the A disc is DEAD** -- stella2014 puts fire on
  `JOYPAD_B`, and `JOYPAD_A` does something only for a Genesis pad or
  paddles. The two difficulty switches and the colour/B&W switch are
  unreachable console switches; no title in the 82 needs one to start.
  Paddle titles play on the d-pad.
- *Master System / Game Gear*: two buttons on `JOYPAD_B`/`JOYPAD_A` and
  Pause/Start on `JOYPAD_START`. Nothing left over, no extra disc.

FreeIntv's OTHER keypad -- `freeintv_multiscreen_overlay`, which widens the
frame to 1074x600 and paints a photographic 12-key pad beside the game for a
`RETRO_DEVICE_POINTER` to tap -- was built, run and rejected. koboy's LCD
layout already forwards touches as a pointer, so it would have worked, but it
costs **1.335 ms/frame against 0.106** without it (12x, on the host, before
the Cortex-A9 multiplier) and reaches the same twelve keys the mini keypad
above already reaches for nothing.

Resolved presentation, from `config_resolve_profile`, with `video_submit`
estimated from #23's 4.7 ms + 20.7 ns per destination pixel:

| System | Panel | Scale | Rect | drawn | est. `submit` |
|---|---|---|---|---|---|
| Atari 2600 160x210 (NTSC) | 1072x1448 | 3 | 960x768 | 480x630 | 11.0 ms |
| Atari 2600 160x210 (NTSC) | 1264x1680 | 3 | 960x768 | 480x630 | 11.0 ms |
| Atari 2600 160x210 (NTSC) | 1440x1920 | 4 | 1280x1024 | 640x840 | 15.8 ms |
| Atari 2600 160x250 (PAL) | 1264x1680 | 3 | 960x768 | 480x750 | 12.2 ms |
| ColecoVision 256x192 | 1072x1448 | 2 | 1024x576 | 768x576 | 13.9 ms |
| ColecoVision 256x192 | 1264x1680 | 2 | 1024x576 | 768x576 | 13.9 ms |
| ColecoVision 256x192 | 1440x1920 | 2 | 1024x576 | 768x576 | 13.9 ms |
| Intellivision 352x224 | 1264x1680 | 3 | 1056x672 | 1056x672 | 19.4 ms |
| Intellivision 352x224 | 1440x1920 | 4 | 1408x896 | 1408x896 | 30.8 ms |
| Master System 256x192 | 1264x1680 | 3 | 852x720 | 768x576 | 13.9 ms |
| Master System 256x192 | 1440x1920 | 4 | 1136x960 | 1024x768 | 21.0 ms |
| Game Gear 160x144 | 1264x1680 | 3 | 852x720 | **800x720** | 16.6 ms |
| Game Gear 160x144 | 1440x1920 | 4 | 1136x960 | 960x864 | 21.9 ms |
| (Game Boy, for scale) | 1264x1680 | 5 | 800x720 | 800x720 | 16.6 ms |

**The Game Gear / Game Boy collision, handled deliberately --- AND THEN THE
ARITHMETIC IT RELIED ON CHANGED; see the ceiling section below.** A Game Gear
frame is 160x144 -- byte for byte the Game Boy's geometry -- and
`config_resolve_profile` really does key its scale default on that geometry.
It is keyed on MAX, and Genesis Plus GX reports max 284x240, so a Game Gear
is not caught by it. At the time this was written it did not need to be:
auto-fitting landed its picture at exactly **800x720**, the Game Boy's
scale-5 presentation, by arithmetic. *That stopped being true when
`pixel_aspect` shipped* --- the widened 192x144 rect auto-fits to 6 instead,
and the picture jumped to 1152x864, 1.73x the area, with nothing to notice
because the reasoning lived in a comment. A `ceiling` now holds it.
Nothing else keys on that geometry, and in particular the greyscale mapping
does not -- a Game Gear is a COLOUR system and gets the colour treatment.
`tests/test_video_gray.c` pins both halves and the mutant that would have
been the bug (a `gray_map` exemption for 160x144 in `video_create`) makes it
fail on Sonic's measured sky.

**A REAL PRESENTATION DEFECT, found by rendering frames and looking at them:
every Atari 2600 title is drawn about 1.75x too tall.** The 2600's pixels are
not square -- 160 across a 4:3 frame makes each one ~1.6:1 -- and stella2014
says so the only way its API lets it, by declaring `base_width = 160 * 2`
while delivering a 160-wide frame. koboy scales squarely, so Ms. Pac-Man's
maze and River Raid's river come out as narrow vertical strips: 480x630 where
the correct shape is about 840x630. Playable and legible, plainly wrong. It
is also the ONLY system here with the problem -- ColecoVision, Intellivision,
Master System and Game Gear are all square-pixel by arithmetic. Deliberately
not fixed in this batch: anisotropic fitting is a change to `video.c`'s hot
path and to the one presentation that has been verified on hardware. See
`docs/FOLLOWUPS.md` #51.

**How four greys actually look.** Rendered through koboy's real pipeline and
examined:

- *Master System and Game Gear are the best colour result this project has
  had.* Wonder Boy III's password screen, Phantasy Star's dialogue, Sonic's
  first zone and Shining Force Gaiden's portrait art all read cleanly under
  the shipped `balanced` map -- these are 16-colour-per-frame machines with
  designer-chosen palettes, and the reduction has room to work.
- *ColecoVision and Intellivision* have small fixed palettes (16 and 16) and
  come through as crisp poster art: Donkey Kong's girders, Atlantis's
  skyline, Dig Dug's tunnels.
- *Atari 2600* is greyscale-friendly almost by accident -- most titles use
  few colours at widely separated luma -- so the four levels cost it little.
  Its problem is shape, not colour.
- The Intellivision's frame carries a THICK BLACK BORDER (352x224 with the
  320x192 playfield inside it), which on reflective paper is the worst case
  for both readability and the panel's waveforms -- the same reasoning that
  picked the Pokemon Mini's inverted palette. FreeIntv has no option to trim
  it. See `docs/FOLLOWUPS.md` #52.

**Saves: three of these four systems have none, and the fourth found a bug.**
`retro_get_memory_size(SAVE_RAM)` is 0 for every Atari 2600, ColecoVision and
Intellivision title in the author's collections -- none of those consoles put
a battery in a cartridge, and none of the cores writes a save file of its own
either. Genesis Plus GX does have battery SRAM, on 16 of the 121 Sega titles,
and it goes through `sram.c` -- **but it answers the size question with two
different numbers**: `0x10000` before emulation starts, and once running,
"the index of the highest byte that is not 0xFF, plus one", which measured
across the collection is anything from 285 bytes to 32160, and 0 for a
cartridge nobody has saved on. `core_sram` now pins the length at ROM-load
time; without that, one MENU -> LOAD STATE would have rewritten a 65536-byte
`.srm` at 8191 bytes and the next launch would have reported it corrupt.

Verified end to end on the host with the real ARM-equivalent core and a real
cartridge (Phantasy Star): koboy wrote a 65536-byte `PS.srm`, read it back
md5-identical on the next launch, and when the file was truncated to 100
bytes left it at 100 bytes and disabled writeback for the session rather than
destroying it.

**Not established:** everything device-side. The four ARM cores have not been
`dlopen`ed on a Kobo, nothing has rendered on the panel, no playable-speed
figure exists, the K1/K2 and KEY/TOP discs have never been touched by a real
finger, the two BIOS files have never been read off a FAT32 partition, and no
Master System `.srm` has survived a real session.

### Mega Drive, SNES and PC Engine: NOT RUN ON THE DEVICE, 2026-08-27

Systems twelve, thirteen and fourteen, and **none of them has been on a
Kobo**. The test device was off the LAN for the whole session --- not merely
port 22 closed, it did not answer ICMP either, from the first probe to the
last --- so every device figure in this section is EXTRAPOLATED and is marked
as such. The ARM cores cross-build, strip, and pass `scripts/verify-core.sh`.

The SNES half of this is the part worth reading first, because **the v1
design spec ruled that system out of scope on CPU grounds** and this section
exists to answer whether that judgement still holds. It does not.

| | Mega Drive (Genesis Plus GX) | SNES (snes9x2005) | PC Engine (beetle-pce-fast) |
|---|---|---|---|
| Extensions claimed | `.md` ONLY | `.sfc`, `.smc` | `.pce` ONLY |
| Geometry before the first `retro_run` | 256x192 base, **348x240 max** | 256x224 base, **512x512 max** | 256x243 base, **512x243 max** |
| ...and after | **320x224** --- changes on frame 2 | unchanged | **352x243** on many titles |
| `SET_GEOMETRY` at runtime | yes, once per title | only for a 512-wide hi-res mode | **yes, repeatedly --- see below** |
| Display aspect reported | 1.5238 | 1.3333 | 1.2000 |
| Pixel format | RGB565 | RGB565 | RGB565 |
| Reported fps | 59.9227, **49.7015 for PAL titles** | 59.9227 | 59.8200 |
| Asks for a system directory | yes, runs without one | no | no |
| `RETRO_MEMORY_SAVE_RAM` | `0x10000` at load, **shrinks** | constant, does not move | constant |
| BIOS required | no | no | no (`.chd` would need one; not claimed) |
| ARM closure | libm, libc | **libm, libc** | **libm, libc** |
| ARM size, stripped | 5.9 MB (already shipped) | **445 KB** | 2.4 MB |
| Host emulation cost, mean | 0.27--0.66 ms/frame | 0.15--0.84 | **0.15--0.26** |
| Titles that load and run | **3317 / 3317** `.md` | **3762 / 3763** | **661 / 661** |

**The one SNES file that does not load is not a game, and what it does is
worse than not loading.** `._desire_d-zero_....smc` is a 212-byte macOS
AppleDouble stub. snes9x2005 does not refuse it --- it divides by zero in
`LoROMMap` (`% Memory.CalculatedSize`, where `CalculatedSize` rounds the file
down to whole 8 KB blocks and is therefore 0) and raises SIGFPE inside
`retro_load_game`, killing the process. Measured: every size from 0 to 1024
exits 136, 8192 does not. koboy now refuses anything under 8192 bytes for
`.sfc`/`.smc` before the core sees it (`config_min_rom_bytes`), and the floor
is SNES-only because an Atari 2600 cartridge is legitimately 2048 bytes.

#### snes9x2005 has SuperFX and SA-1, contrary to the folklore

The received wisdom --- and the brief this core was added under --- is that
snes9x2005 drops the special-chip titles. At this revision it does not, and
the check was a RENDER rather than a successful `retro_load_game`:

| Title | Chip | What frame 899--1600 actually shows |
|---|---|---|
| Star Fox | SuperFX (GSU-1) | Corneria terrain and a polygonal Arwing. Those polygons ARE the GSU's output; a stubbed SuperFX draws black. |
| Yoshi's Island | SuperFX2 | Gameplay, Yoshi and Baby Mario, pixel-comparable to the same frame under the full `snes9x`. |
| Kirby Super Star | SA-1 | Its "Is this your first time playing?" prompt, which is past the SA-1 boot. |

So the compatibility cost of picking the FASTEST SNES core is much smaller
than expected. It is a property of THIS core at THIS revision: re-render
those three titles before swapping it.

#### PC Engine switches horizontal resolution mid-game, and the picture does not move

The system-specific worry, exercised deliberately. Titles alternate between
256 and 352 pixel widths (not 336, which is what the folklore says).
**Military Madness switches five times in 2500 frames** --- 256 for its title
screen and transitions, 352 for the map --- and each switch drives main.c's
`core_geometry_changed()` poll into a full re-fit.

The result is the good one, and it is the pixel-aspect work (`3444be3`)
paying off on a system it was not written for:

| | 256-wide mode | 352-wide mode |
|---|---|---|
| pixel aspect | 1.1391 | 0.8284 |
| reserved rect | 1168x486 at x=48 | 850x486 at x=207 |
| **picture presented** | **583x486, centred at x=632** | **583x486, centred at x=632** |

Both modes have the same DISPLAY width, so after aspect correction the
picture is the same size and in the same place; only its internal detail
changes. Verified by rendering both (the 256-mode title screen and the
352-mode map) and looking at them. With `pixel_aspect = false` the escape
hatch works and the artifact returns: the picture becomes 512x486 in one mode
and 704x486 in the other, so it JUMPS SIZE every time the game changes scene.
That is the clearest argument for the correction anywhere in this file.

What it costs: each re-fit destroys and rebuilds the video pipeline, which
throws away the dirty-rect history, so every switch is a full-rect redraw.
Five in ~42 seconds of Military Madness. Not measured on a panel.

#### ~~The presentation surprise: SNES and PC Engine are presented SMALLER than the Game Boy~~ --- FIXED 2026-08-27

Left here because the numbers are the before half of the comparison. The rect
is now sized from the core's BASE geometry in `KOBOY_LAYOUT_DMG`, and the
sizes below are what it used to be.

| System | core frame | was presented | now | area change |
|---|---|---|---|---|
| Game Boy | 160x144 | 800x720 | 800x720 | unchanged |
| Mega Drive | 320x224 | 878x672 | **1170x896** | 1.78x |
| PC Engine | 352x243 | 583x486 | **875x729** | 2.25x |
| SNES | 256x224 | 597x448 | **1195x896** | **4.00x** |

The cause was that the reserved rect was sized from the core's MAX geometry,
and both of these cores report a max far larger than any frame they deliver:
snes9x2005 says 512x512 (for an interlaced hi-res mode almost nothing uses)
against a 256x224 frame, and beetle-pce-fast says 512x243. A 512-tall
reservation cannot exceed scale 1 under `chrome_controls_top`.

**What it costs is MEASURED, not modelled --- see "The rect-sizing trade,
measured on the device" below.** Short version: nothing at all for PC Engine
and for SNES titles with CPU headroom, and real speed for the two SNES titles
that had none.

There is a perverse second-order effect worth naming: because SNES is
presented small, `video_submit` costs it LESS, so its per-frame CPU budget is
LARGER than the Mega Drive's. Fixing the rect size would shrink that budget.

#### The rect-sizing trade, MEASURED on the device --- 2026-08-27

The device was awake for this one. Every figure in this subsection is a
measurement on the verified Libra 2, not a model.

**Method.** `koboy-arm` built at `0b71348` (max-sized rect) and at `ae03e76`
(base-sized rect), run alternately over ssh with Nickel up, `./koboy --frames
900`, wall-clock timed. 900 frames is 15,024 ms of real time at 59.9227 Hz and
15,045 ms at the PC Engine's 59.82, so the ratio of ideal to measured IS the
speed. **Ten seconds of idle between runs, and that is not a formality:** a
first pass with the runs back to back produced figures up to 2.4x higher that
kept climbing through the batch (Super Mario World 15,595 ms then 25,378 ms,
Kirby 24,067 then 37,142), and an isolated re-run of the same binary and the
same ROM came back at 15,471 ms. Whatever accumulates --- Nickel reacting to
the `.srm` each run writes is the likeliest --- a saturated Cortex-A9 does not
give repeatable numbers, and a benchmark loop without gaps measures the
benchmark loop.

`presented` is identical between the two binaries for every title (183, 181,
246, 241, 160, 113, 98), which is what makes the pairs comparable: the same
content produced the same number of panel updates, only their cost changed.

| Title | rect BEFORE | rect AFTER | before | after | speed before | speed after |
|---|---|---|---|---|---|---|
| **SNES** | 598x512 | 1196x896 | | | | |
| Super Mario World | | | 15,400 ms | 15,471 ms | 98% | **98%** |
| Zelda --- A Link to the Past | | | 15,433 ms | 15,361 ms | 97% | **98%** |
| Kirby Super Star (SA-1) | | | 15,580 ms | 19,230 ms | 96% | **78%** |
| Star Fox (SuperFX) | | | 16,212 ms | 22,477 ms | 93% | **67%** |
| **MEGA DRIVE** | 957x720 | 1172x896 | | | | |
| Sonic The Hedgehog | | | 15,836 ms | 16,925 ms | 95% | **89%** |
| Virtua Racing (SVP) | | | 17,978 ms | 19,187 ms | 84% | **78%** |
| **PC ENGINE** | 1168x486 / 850x486 | 876x729 | | | | |
| Bonk's Adventure | | | 15,507 ms | 15,359 ms | 97% | **98%** |
| Ninja Spirit | | | 15,398 ms | 15,368 ms | 98% | **98%** |

**PC Engine got a 2.25x picture for free, and slightly better than free.** Its
old rect reserved 568k pixels to show 283k of picture; the new one is 638k
pixels of which every one is picture. The diff, the blit and the refresh all
run over the RECT, so paying for margin was pure loss.

**SNES is where the bill lands, and only on the two titles that had no
headroom.** Super Mario World and Zelda do not move. Kirby Super Star and Star
Fox do, and the lever is measured too --- the same binary with `scale` pinned:

| Title | scale 4 (1196x896, auto) | scale 3 (897x672) | scale 2 (598x448) |
|---|---|---|---|
| Kirby Super Star | 19,230 ms (78%) | **15,770 ms (95%)** | 15,400 ms (98%) |
| Star Fox | 22,477 ms (67%) | 18,959 ms (79%) | 16,177 ms (93%) |
| Sonic The Hedgehog | 16,925 ms (89%) | **15,387 ms (98%)** | 15,361 ms (98%) |

`scale = 2` is the old picture size to the pixel (598x448 against the old
597x448), and it reproduces the old speed to within noise --- which is the
control that says these numbers are measuring rect area and nothing else.
**`scale = 3` is the interesting row**: 2.25x the old picture area, and Kirby
and Sonic are both back at full speed. Mega Drive at scale 3 is FASTER than
the old build was at the same picture size (15,387 vs 15,836), for the PC
Engine reason --- the old rect was 957x720 around an 878x672 picture.

`scale` is a global ini key, so pinning it costs every other system. The
per-system version of it is `docs/FOLLOWUPS.md` #73.

#### Per-frame core cost, MEASURED on the device --- 2026-08-27

`build/corebench-arm`, 900 frames after a 120-frame warmup, on the device.
This is what `docs/FOLLOWUPS.md` #68 asked for and it closes it: the
two-point fit below is superseded by measurement, and **the fit was
roughly right but not reliable per title** --- it predicted Star Fox 12,341
against a measured 12,819 (4% out) and Kirby 7,815 against 9,321 (16% out).

| Title | mean us | p50 | p95 | max | widths seen | geom calls |
|---|---|---|---|---|---|---|
| **SNES (snes9x2005)** | | | | | | |
| Super Metroid | 3,036 | 2,796 | 5,291 | 12,664 | 256 | 0 |
| Zelda --- A Link to the Past | 3,638 | 3,623 | 5,158 | 22,984 | 256 | 0 |
| Yoshi's Island (SuperFX2) | 3,856 | 4,190 | 5,084 | 97,315 | 256 | 0 |
| Donkey Kong Country | 3,909 | 4,248 | 5,679 | 11,529 | **256 and 512** | **0** |
| Super Mario World | 3,982 | 4,369 | 5,445 | 10,382 | 256 | 0 |
| Chrono Trigger | 5,254 | 5,084 | 6,376 | 10,409 | 256 | 0 |
| F-Zero | 5,669 | 5,603 | 6,744 | 13,652 | 256 | 0 |
| Kirby Super Star (SA-1) | 9,321 | 9,450 | 10,761 | 22,490 | 256 | 0 |
| Star Fox (SuperFX) | 12,819 | 11,063 | 22,538 | 28,452 | 256 | 0 |
| **MEGA DRIVE (Genesis Plus GX)** | | | | | | |
| Streets of Rage 2 | 5,672 | 5,928 | 6,816 | 8,451 | 256 and 320 | 1 |
| Gunstar Heroes | 5,770 | 5,410 | 7,883 | 9,955 | 256 and 320 | 1 |
| Sonic The Hedgehog 2 | 6,132 | 5,973 | 7,806 | 11,025 | 256 and 320 | 1 |
| Phantasy Star IV | 6,255 | 6,231 | 7,418 | 14,487 | 256 and 320 | 1 |
| Golden Axe | 6,261 | 6,388 | 7,325 | 14,007 | 256 and 320 | 2 |
| Sonic The Hedgehog | 6,658 | 6,392 | 8,192 | 28,626 | 256 and 320 | 1 |
| Virtua Racing (SVP) | 12,940 | 11,318 | 22,478 | 29,162 | 256 and 320 | 2 |
| **PC ENGINE (beetle-pce-fast)** | | | | | | |
| Ninja Spirit | 2,762 | 2,170 | 3,839 | 5,284 | **256 and 352** | 1 |
| Dragon's Curse | 3,176 | 3,117 | 3,583 | 5,711 | 256 | 1 |
| Bonk's Adventure | 3,310 | 3,279 | 3,686 | 8,242 | 256 | 1 |
| Bomberman '94 | 3,311 | 3,259 | 3,834 | 5,813 | 256 | 1 |
| Blazing Lazers | 3,854 | 3,776 | 4,629 | 30,821 | 256 | 1 |

**Donkey Kong Country sends 512-wide frames and announces NOTHING.** `widths`
says 256 and 512, `geom_calls` says 0. That is the case the fitting path's
containment fallback exists for --- a frame between base and max, arriving
with no warning, into a rect sized from base. It fits here (512x224 at 4:3
lands at 1195x896, exactly filling the rect), and the sweep in
`tests/test_video_pipeline.c` is what says it fits everywhere else.

#### The benchmark, and it is EXTRAPOLATED

Read `## Reading the fps column with the caution it deserves` first. Then
read this, which needs more caution than that section asks for.

The host figures below are measurements (`scripts/corebench.c`, 900 frames
after a 120-frame warmup, x86_64). The device figures are a MODEL, and the
model is a two-point fit, and the two points disagree with each other under
the simple hypothesis. Re-measuring the two cores that DO have on-device
numbers, with this same instrument:

| core | host, corebench | device `core`, TESTED.md | implied ratio |
|---|---|---|---|
| gambatte, Zelda | 98.2 us | 2.3 ms | **23.4x** |
| fceumm, SMB / Kirby | 258.0 us | 4.3--4.6 ms | **17.2x** |

A single multiplier cannot be right for both. A linear fit can, and it has a
physical reading: koboy's `core` stage includes per-frame front-end work
(the evdev input poll, the callbacks) that `corebench` does not, and that
cost is roughly constant per frame rather than proportional to the core.

    device_core_us  ~=  13.45 * host_us  +  979

**This is a two-point fit and it is doing real work in every number below.**
The additive term alone is ~1 ms; for the cheapest PC Engine titles it is a
third of the predicted cost. Treat the device columns as an ORDER OF
MAGNITUDE with a defensible slope, not as a measurement. The earlier arcade
section used a flat 7x against koboy's own `core` instrument; that figure and
this one are not comparable, because they measure different things.

The budget each system has to fit into, per core frame, is
`1e6/fps - (submit + blit + refresh) / present_divisor` at the shipped
divisor of 3, with `submit` from this file's own `4.7 ms + 20.7 ns/px` model
against the presented rects in the table above:

| System | presented px | submit (modelled) | budget per core frame |
|---|---|---|---|
| Mega Drive | 590,016 | 16.9 ms | **9,884 us** |
| PC Engine | 283,338 | 10.6 ms | **12,000 us** |
| SNES | 267,456 | 10.2 ms | **12,109 us** |

| Title | fps | host us | device mean | device p95 | device worst | % speed (mean) | % (worst frame) |
|---|---|---|---|---|---|---|---|
| **MEGA DRIVE** | | | | | | | |
| Sonic The Hedgehog | 59.9 | 317.9 | 5,256 | 6,226 | 6,387 | 100% | 100% |
| Sonic The Hedgehog 2 | 59.9 | 302.4 | 5,047 | 6,065 | 7,316 | 100% | 100% |
| Streets of Rage 2 | 59.9 | 271.7 | 4,634 | 5,029 | 8,056 | 100% | 100% |
| Gunstar Heroes | 59.9 | 291.5 | 4,901 | 5,890 | 7,410 | 100% | 100% |
| Phantasy Star IV | 59.9 | 290.4 | 4,886 | 5,257 | 5,849 | 100% | 100% |
| Thunder Force IV (PAL) | **49.7** | 331.2 | 5,435 | 6,562 | 8,392 | 100% | 100% |
| Golden Axe | 59.9 | 309.7 | 5,146 | 5,701 | 6,239 | 100% | 100% |
| Virtua Racing (SVP) | 59.9 | 657.8 | 9,829 | 16,653 | 17,797 | 100% | **56%** |
| **SNES** | | | | | | | |
| Super Mario World | 59.9 | 261.4 | 4,496 | 5,459 | 6,361 | 100% | 100% |
| Zelda - A Link to the Past | 59.9 | 244.1 | 4,263 | 5,607 | 6,226 | 100% | 100% |
| Super Metroid | 59.9 | 154.9 | 3,063 | 5,473 | 5,742 | 100% | 100% |
| Chrono Trigger | 59.9 | 309.6 | 5,144 | 6,172 | 6,414 | 100% | 100% |
| F-Zero | 59.9 | 361.2 | 5,838 | 6,065 | 6,414 | 100% | 100% |
| Donkey Kong Country | 59.9 | 212.3 | 3,835 | 4,975 | 5,715 | 100% | 100% |
| Star Fox (SuperFX) | 59.9 | 844.5 | 12,341 | 23,434 | 27,901 | **98%** | **43%** |
| Kirby Super Star (SA-1) | 59.9 | 508.1 | 7,815 | 9,401 | 10,518 | 100% | 100% |
| Yoshi's Island (SuperFX2) | 59.9 | 241.0 | 4,221 | 5,163 | 115,717 | 100% | see note |
| **PC ENGINE** | | | | | | | |
| Bonk's Adventure | 59.8 | 201.2 | 3,686 | 3,845 | 4,638 | 100% | 100% |
| R-Type | 59.8 | 228.2 | 4,049 | 4,154 | 7,800 | 100% | 100% |
| Bomberman '94 | 59.8 | 205.7 | 3,746 | 4,033 | 4,289 | 100% | 100% |
| Blazing Lazers | 59.8 | 222.2 | 3,968 | 4,235 | 4,746 | 100% | 100% |
| Ninja Spirit | 59.8 | 153.6 | 3,045 | 4,181 | 4,517 | 100% | 100% |
| Military Madness | 59.8 | 256.1 | 4,424 | 4,652 | 5,244 | 100% | 100% |
| Dragon's Curse | 59.8 | 178.3 | 3,378 | 3,589 | 4,342 | 100% | 100% |
| Devil's Crush | 59.8 | 214.0 | 3,858 | 3,966 | 4,410 | 100% | 100% |

**Yoshi's Island's 115,717 us worst frame is the instrument, not the core.**
Its p95 is 5,163 --- one frame in 900 took 8.5 ms on the host where the
median took 0.26. That is what the p95 column exists to distinguish, and it
is the one row in this table where the max should be ignored.

**The verdict, per system.**

- **PC Engine is comfortable and is the clear winner of the three.** Every
  title sits at a quarter to a third of budget, and the WORST frame measured
  across eight titles is still inside it. It is the cheapest system koboy has
  added since the Game Boy.
- **Mega Drive is comfortable for the ordinary library** --- seven of eight
  titles use half the budget or less, including the ones people actually
  name. **Virtua Racing is the exception and the reason to keep the SVP
  caveat**: its mean fits, but its p95 is 1.7x budget, so it will hitch
  through the 3D sections rather than run slowly and evenly.
- **SNES is playable, and the v1 spec's judgement no longer holds.** Six of
  nine titles sit at a third of budget or less. The two special-chip titles
  are the honest exceptions: Kirby Super Star (SA-1) fits with room, and
  **Star Fox does not** --- 98% at the mean, 43% on its worst frame, which is
  a game that runs but stutters through exactly the moments it should not.

**`present_divisor`, the lever the brief asked about.** Nothing here needs it
raised. At the shipped 3, every system's mean fits. Raising it to 4 buys
~1.6 ms on Mega Drive and ~1.1 ms on the other two, which does NOT rescue
Star Fox or Virtua Racing (both need several ms) and costs a presented frame
in every title that was already fine. Lowering it to 2 costs ~3.3 ms on Mega
Drive, which would put Virtua Racing's mean over budget and leave the rest
still comfortable. **The recommendation is to leave it at 3.**

**What would actually change these numbers** is not the divisor and not the
cores: it is `video_submit`, still the bottleneck (`docs/FOLLOWUPS.md` #23),
and for these two systems specifically the oversized reserved rect described
above --- which is the rare case where fixing the picture and fixing the
speed pull in opposite directions.

**Not established:** everything device-side. Three ARM cores have not been
`dlopen`ed on a Kobo, nothing has rendered on the panel, no real
playable-speed figure exists, the Mega Drive's "A" disc and the SNES's Y/X
discs have never been touched by a real finger, and no Mega Drive or SNES
`.srm` has survived a real session. The `.srm` gap matters more here than for
any previous batch: these are the first two systems koboy has added where
battery saves are the NORM rather than the exception.

### Arcade (FinalBurn Neo): NOT RUN ON THE DEVICE, 2026-08-27

The eleventh system, and **it has not been on a Kobo**. The test device was
off the LAN for the whole session and this host has no `qemu-arm`, so every
number below is x86_64. The ARM core cross-builds, strips to 41 MB and passes
`scripts/verify-core.sh`; that is the whole of the device-side evidence.
Everything else was measured with `scripts/probe_core.c`, a throwaway probe
that drives the core directly, and a harness running koboy's own
`config.c`/`video.c` against the author's 227-romset collection.

| | Arcade (FinalBurn Neo v1.0.0.03, rev ae41c16e of 2025-07-24) |
|---|---|
| Romsets that load | 227 / 227 -- but see the next row, "loads" is not "works" |
| Romsets that are a playable board | **213 / 227.** The other 14 are device and BIOS dumps a complete set carries (`neogeo`, `midssio`, `namcoc69/70/75`, `nmk004`, `ym2608`, `cchip`, `pgm`, `skns`, `isgsm`, `bubsys`, `decocass`) plus `wbmlb2`, whose parent is absent |
| Geometry | per board: base 224x224 to 640x480. **max is always SQUARE**, side = max(w,h), so both orientations fit one buffer |
| `SET_GEOMETRY` at runtime | never, on any of the 227 |
| `SET_ROTATION` | **yes -- 43 boards ask for one quarter turn, 24 for three, 160 for none** |
| Pixel format | RGB565 |
| Asks for a system directory | yes -- `<system_dir>/fbneo/` for `hiscore.dat` |
| Asks for a save directory | yes -- `<save_dir>/fbneo/` for `<board>.hi`, and it `mkdir`s that itself on every load |
| `valid_extensions` | `zip\|7z\|cue\|ccd`; koboy claims `.zip` only |
| `RETRO_MEMORY_SAVE_RAM` | **0, on all 227.** An arcade PCB has no battery |
| `retro_serialize_size` | non-zero on all 213 playable boards: 6 KB (Pooyan) to **145 MB** (DoDonPachi DaiFukkatsu) |
| BIOS required | **no**, for the pre-1990 boards. Later hardware wants a BIOS ZIP beside the games in `roms/`, not in the koboy directory |
| ARM closure | `libpthread`, `libm`, `libc`, `ld-linux-armhf` |
| ARM size, stripped | **41 MB** -- ten times the rest of koboy, which is why it ships separately |
| `timing.fps` | 60 on 150 boards, 59.x on 38, 58.x on 12, 55.x on 14, 54.x on 5, 50 on 2, **30 on 2** (Tapper, Popeye) |

**THE MONITOR WAS ON ITS SIDE, and koboy had to learn to turn the picture.**
FinalBurn Neo renders Galaga into a 288x224 LANDSCAPE buffer and asks the
frontend, through `RETRO_ENVIRONMENT_SET_ROTATION`, for three quarter turns.
Checked before assuming the core could do it itself: its `fbneo-vertical-mode`
option, whose name promises exactly that, does NOT rotate the framebuffer --
measured across all five settings, the delivered frame stays 288x224 and only
the aspect ratio and the SET_ROTATION value move. So the turn is koboy's, and
it is now in `video.c` for every core (see `core_rotation`,
`video_set_rotation`).

Measured cost, same frame and same destination area, three runs: rot 3 at
0.449-0.516 ms against rot 0 at 0.444-0.446. Within noise on this host. The
turning branches read strided and write sequentially, and a pre-1990 board's
source column is ~14 KB against a Cortex-A9's 32 KB L1, so it should hold
there too -- unverified.

**What the panel gets, on a Libra 2 (1264x1680), DMG faceplate.** The rect is
sized from the SQUARE max, so a portrait board leaves a paper margin left and
right; the fitted picture is the second column:

| Board | core frame | presented | reserved rect | fitted picture |
|---|---|---|---|---|
| Galaga, Dig Dug, Xevious, Ms. Pac-Man | 288x224 | 224x288 | 864x864 at (200,84) | **672x864** |
| Donkey Kong, Frogger, Galaxian, Bomb Jack | 256x224 | 224x256 | 768x768 at (248,84) | 672x768 |
| Green Beret, Jail Break | 240x224 | 224x240 | 720x720 at (272,84) | 672x720 |
| Defender, Joust (landscape) | 292x240 | 292x240 | 876x876 at (194,84) | 876x720 |
| Tapper, Popeye (landscape) | 512x480/448 | same | 512x512 at (376,84) | 512x480 |

672x864 is 580,608 pixels against the Game Boy's 800x720 = 576,000, so a
vertical arcade board gets very slightly MORE picture than the Game Boy and
in the panel's own aspect. It is not the 1264x1626 a fractional 5.6x fit
would give: the DMG faceplate reserves everything below `chrome_controls_top`
(1018 on this panel) for the touch controls, and the scale search is integer.
A fractional full-panel fit is what `KOBOY_LAYOUT_LCD` does and it exists
because a Game & Watch draws its own buttons; an arcade board does not.

**Per-frame cost, and it is EXTRAPOLATED.** The left column is koboy's own
`core` stage on this host (the same instrument the device rows above use, so
it includes koboy's input poll); the right is that times 7, the ratio measured
on the two cores koboy HAS run on hardware -- gambatte 0.316 ms here against
2.3 ms there (7.3x), fceumm 0.72 against 4.3-4.6 (6.0-6.4x).

| Board | host `core` | device, estimated |
|---|---|---|
| Joust | 0.40 ms | ~2.8 ms |
| Jail Break | 0.45 | ~3.1 |
| Defender | 0.45 | ~3.2 |
| Ms. Pac-Man | 0.53 | ~3.7 |
| Donkey Kong | 0.55 | ~3.8 |
| **Galaga** | **0.68** | **~4.7** |
| Green Beret | 0.70 | ~4.9 |
| **Dig Dug** | **0.98** | **~6.9** |
| Xevious | 1.03 | ~7.2 |
| Q*bert | 1.04 | ~7.3 |
| Gyruss | 1.23 | ~8.6 |
| Pitfall II | 1.28 | ~8.9 |
| Tapper | 1.82 | ~12.7 |

Emulation is cheap here as it has been on every other system -- but Tapper's
estimate is close enough to a 16.7 ms frame that the sign of the error
matters, and `video_submit` is still the real bottleneck (14-20 ms on the
device for a rect this size).

**Rendered and looked at**, which is how the last three device-visible defects
in this project were found. Galaga, Ms. Pac-Man, Donkey Kong, Dig Dug and
Frogger were run to real gameplay -- coin on SELECT, then START -- pushed
through koboy's actual pipeline and written out as PGM. All five are upright,
legible and playable-looking. Two things that only a frame could say:

- *An arcade board boots slowly and ignores you while it does.* Galaga spends
  roughly 800 frames (13 s) drawing its character-ROM test pattern and then a
  crosshatch grid before it will look at the coin slot. A coin inserted during
  that is simply lost, which looks exactly like a dead SELECT button.
- *This is the darkest content koboy has rendered.* Mean luma after
  quantising: Galaga 0.013 (97.8% pure black), Donkey Kong 0.093, Frogger
  0.130, Ms. Pac-Man 0.163, Dig Dug 0.387. The Pokemon Mini got an inverted
  palette in `src/core.c` for being 83% black at 0.174; arcade is darker and
  cannot take that fix, because light-on-dark is the art. See
  `docs/FOLLOWUPS.md` #59.

**The smearing caveat, measured rather than predicted.** Fraction of the game
rect changing per presented frame during gameplay, from koboy's own dirty-rect
diff: Joust 0.2%, Dig Dug 1.5%, Donkey Kong 1.9%, Ms. Pac-Man 2.6% -- and
Frogger 52.5%, Xevious 59.5%, **Galaga 67.1%**, Galaxian 85.5%. The scrolling
shooters were expected to smear. Galaga and Galaxian were not: they are
single-screen games with a full-screen scrolling STARFIELD, so the premise
that "single-screen boards cannot smear" is wrong for the two most famous
ones. See `docs/FOLLOWUPS.md` #58.

**Controls, counted rather than read off a control panel.** This is the first
system where "what does the hardware have" has 227 answers, so every romset's
`retro_input_descriptors` were read. FBNeo's mapping is flat: JOYPAD_B is
always the board's Button 1, A is Button 2, Y is Button 3, X is Button 4, and
Coin 1 / Start 1 are SELECT / START.

| retropad | boards binding it (of 227) |
|---|---|
| SELECT (Coin) | 214 |
| LEFT / RIGHT | 212 |
| START | 209 |
| B (Button 1) | 208 |
| UP / DOWN | 198 |
| A (Button 2) | 185 |
| **Y (Button 3)** | **134** |
| **X (Button 4)** | **71** |
| R / L | 48 / 45 |
| R2 / L2 | 46 / 45 |
| L3 / R3 | 26 / 14 |

Y and X get the two extra discs, labelled `3` and `4`. Unreachable: the
shoulder and stick-click buttons, i.e. the six-button layouts, all of which
are outside this batch's scope except **Defender's "Reverse"** (JOYPAD_R).

**Saving, and it is not `.srm`.** `retro_get_memory_size(SAVE_RAM)` is 0 on
all 227. What exists instead is FinalBurn Neo's `hiscore.dat` mechanism, which
koboy now turns on by answering `fbneo-hiscores` (the core's stated default is
"enabled", but the option is left OFF when a frontend REFUSES the query, which
is what koboy does with every key it has no opinion about). Verified as a
round trip on Ms. Pac-Man, whose default high score is blank rather than a ROM
constant: a 220-point game writes `fbneo/mspacman.hi` (11 bytes) on unload,
and the next launch's attract screen reads `HIGH SCORE 220` where a fresh
directory shows nothing. Both frames rendered and compared. The owner supplies
`hiscore.dat` in `.adds/koboy/fbneo/`; without it the feature is inert.

**"It loaded" is not "it works" here either.** `retro_load_game` returns TRUE
for a missing or version-mismatched romset and the core draws a 640x480
mostly-white error page naming the problem. Discriminator, found by scanning
all 227: an error page is base 640x480 AND `retro_serialize_size() == 0`. That
is what produces the 213/227 figure above. `astdelux` reports 640x480
legitimately (it is a vector game) and has a real serialize size, which is why
both halves of the test are needed. Unlike SMS Plus GX, FBNeo is null-safe
about a refused `GET_LOG_INTERFACE` (`log_dummy`).

**The romset was verified against the dat before any of this.** All 227 zips
were CRC-checked member by member against
`FinalBurn Neo (ClrMame Pro XML, Arcade only).dat` v1.0.0.03. Every pre-1990
board matches exactly, including the device zips a set needs beside a game
(`tapper` wants `midssio.zip`, present). The only shortfalls are alternate
Neo Geo BIOS revisions, of which FBNeo needs one, and `wbmlb2`'s missing
parent.

### Game & Watch: VERIFIED on the device, 2026-08-26

Confirmed working by the device owner on the Kobo Libra 2: a Game & Watch
title launched from the ROM browser, rendered at full panel width in the LCD
layout, and was playable using the control strip.

What that establishes, and no more:

| | |
|---|---|
| The core loads and runs on-device | yes |
| Geometry resolves (late, from inside the first `retro_run`) | yes |
| The LCD layout renders at full width | yes, `1264x765` for Mickey Mouse (654x396 source) |
| The strip's controls drive the game | yes |
| Every one of the 59 titles | **not established** -- one title was played |
| A full round, scoring, game-over | **not established** |

Measured on-device, LCD layout, per presented frame:

| Title | Source | Rendered | `submit` | `blit` | `refresh` |
|---|---|---|---|---|---|
| Mickey Mouse (Wide Screen) | 654x396 | 1264x765 | 23.9 ms | 10.5 ms | 13.1 ms |
| Donkey Kong (Multi Screen) | 606x748 | 1020x1260 | 32.9 ms | 15.0 ms | 4.3 ms |

`submit` scales with the RENDERED size, so filling the panel roughly doubled
it against the 1x this layout replaced (Mickey was ~12 ms at 654x396). That
is the price of the size, not a regression --- see `docs/FOLLOWUPS.md` #30 for
why the Game Boy comparison people reach for is the wrong one.

**How these titles are driven**, because it is not guessable and cost a
session to establish: `SELECT` moves a cursor over the unit's own buttons
(GAME A / GAME B / TIME), and `START` presses the highlighted one. Game
controls are per-title retropad bindings --- Mickey Mouse is
`up`=NORTHWEST, `down`=SOUTHWEST, `x`=NORTHEAST, `b`=SOUTHEAST; Donkey Kong
is the full d-pad plus a JUMP button. `START` with no cursor active opens the
core's own overlay, which names that title's bindings.

### NES and Pokemon Mini: run on the device, 2026-08-26

Both cross-built cores `dlopen`, run and render on the Kobo Libra 2. Measured
with `--frames` over ssh (Nickel up, never through `koboy.sh`), so this
covers loading, geometry and speed --- **not** a playtest.

| | NES (fceumm) | Pokemon Mini |
|---|---|---|
| Reported geometry | 256x240 | 96x64 |
| Resolved rect | 768x720 at (248,84), scale 3 | **1248x832** at (8,84) |
| `core` | 4.3--4.6 ms | 2.0 ms |
| `submit` | 14.1--14.9 ms | 20.5 ms |
| `blit` | 1.4--2.5 ms | 2.2 ms |
| `refresh` | 108--132 us | 800 us |

Two things worth reading off that table.

**Emulation is genuinely cheap on this CPU.** fceumm costs 4.6 ms a frame ---
a whole NES for a quarter of what `video_submit` spends. The v1 spec's premise
that emulation would be the constraint is wrong for this system too; the
pixel pipeline is still the bottleneck, exactly as `docs/FOLLOWUPS.md` #23
found for the Game Boy.

**Pokemon Mini fills the panel.** 1248x832 from a 96x64 source is the
per-system scale default working: under the old rule it inherited the Game
Boy's `scale = 5` and would have rendered 480x320, a postage stamp. NES sits
at scale 3 and costs *less* than the Game Boy's 16.6 ms, because 768x720 is
fewer destination pixels than 800x720.

**NES battery saves work on hardware.** The Zelda cart wrote
`Legend of Zelda, The (USA) (Rev 1).srm` and a later run logged
`koboy: loaded ...srm`, so the round trip is real, through the same
`sram.c`/`safefile.c` path the Game Boy cartridge save was verified against.

Game & Watch titles also write `.srm` (777 bytes --- high scores), which was
not something this project set out to support and is a pleasant accident of
the core exposing save RAM.

**Not established:** no playtest of either system. Nobody has driven NES with
the d-pad on the panel, and the smearing that makes scrolling platformers
unpleasant (#25) is expected to apply to NES side-scrollers exactly as it
does to the Game Boy's.

### All fourteen systems run on the device, 2026-08-27

Every shipped core `dlopen`s, resolves geometry, paces itself and renders on
the Kobo Libra 2. Measured with `--frames` over ssh (Nickel up, never through
`koboy.sh`). This covers **loading, geometry, pacing and speed --- not a
playtest.** Nobody has played any of these.

| System | rect | `core` mean | `submit` mean | paced at |
|---|---|---|---|---|
| SNES (Zelda) | 897x672 | 2.8 ms | 16.2 ms | 59.92 fps |
| Mega Drive (Sonic) | 1172x896 | 4.0 ms | 22.0 ms | 59.92 fps |
| PC Engine (R-Type) | 876x729 | 3.8 ms | 16.1 ms | 59.82 fps |
| Neo Geo Pocket (Metal Slug) | 960x912 | 3.5 ms | 18.0 ms | 60.25 fps |
| ColecoVision (DK Jr) | 1024x768 | 3.7 ms | 18.3 ms | 60.00 fps |
| Intellivision (Astrosmash) | 1056x672 | 2.4 ms | 23.1 ms | 60.00 fps |
| Atari 2600 (Breakout) | 1002x750 | 3.2 ms | 18.5 ms | **49.92 fps** |
| Master System (Sonic Chaos) | 1172x768 | 2.0 ms | 21.1 ms | 59.92 fps |
| Game Gear (Sonic Chaos) | 1152x864 | 2.0 ms | 20.7 ms | 59.92 fps |
| WonderSwan (beatmania) | 1120x720 | 2.9 ms | 16.8 ms | **75.47 fps** |
| Arcade (Galaga) | 648x864 | 4.4 ms | 14.5 ms | 60.00 fps |

**Emulation is cheap on this hardware and `video_submit` is the whole
bottleneck.** Every core costs 2.0-4.4 ms a frame --- a whole Mega Drive for
4.0 ms --- against 14-23 ms of pixel pipeline. The v1 spec's premise that
emulation would be the constraint is wrong for all fourteen systems, not just
the Game Boy (`docs/FOLLOWUPS.md` #23).

**Per-core pacing is visibly working:** the Atari's PAL board paces at 49.92
fps and the WonderSwan at 75.47, where every system used to run at the Game
Boy's 59.7275.

**Mega Drive re-resolves geometry mid-run** (256x192 -> 320x224 on Sonic),
which is the per-frame `core_geometry_changed()` path doing its job on real
content.

### ColecoVision's BIOS is verified, and this is why it needed a picture

`colecovision.rom` (md5 `2c66f5911e5b42b8ebe113403548eee7`) renders the real
boot screen. WITHOUT it, gearcoleco **loads, runs, reports no error and paces
normally** --- and draws a black screen reading `NO BIOS`. Rendered both, side
by side, because nothing in a log distinguishes them. A system that looks like
it works and does not is the failure this project keeps returning to.

Intellivision's `exec.bin` and `grom.bin` are byte-identical to the MiSTer
`boot0.rom` and `boot1.rom` (md5 `62e76103...` and `0cd5946c...`).

### Not established

No system here has been PLAYED. The pixel-aspect correction changed eight
systems' presentation and has still never been judged on the panel by a
person; `pixel_aspect = false` is the way back. `refresh_fixed_tiles` is
still 40, against rects two to four times larger than it was tuned for.

### 1-bit output fixes the motion smearing, 2026-08-27 --- confirmed by the owner

**The oldest open defect in this project (`docs/FOLLOWUPS.md` #25) is
substantially fixed**, by the one mechanism nobody had tried: rendering the
game as GENUINELY TWO-VALUED content instead of four grey levels.

Owner's verdict, playing NES Super Mario Bros. on the panel: *"motion is much
better in both 1bit modes, no white flashing now, even scrolling looks not
bad, there are some ghosting of background, but still it is better than
before."* Scrolling was the worst case and the reason this defect has stood
since v1.

**Why it works, and why every earlier attempt failed.** A DU-class waveform
is TWO-LEVEL: it drives a changed pixel to black or white. Four-level content
therefore asks a fast waveform for states it cannot reach, and at partial
refresh speed it lands somewhere between --- which is what the owner's first
video shows, a vertical column containing a stale ghost above, the sprite in
the middle and bands of overshoot BRIGHTER THAN THE SKY below. Both directions
of the transition were failing at once.

Measured from the live framebuffer during play, the cause was visible in the
data: SMB's sky is written at level 2 (170) --- a MID GREY. Every sprite
transition therefore had to travel most of the way to an extreme. Made 1-bit,
every pixel is 0x00 or 0xFF and a two-level waveform completes every
transition exactly.

This does NOT contradict the appendix finding that forced DU4 "cannot erase".
DU4 is the FOUR-level variant; that failure is this mechanism seen from the
other side.

| | |
|---|---|
| Both `1-BIT / AUTO` and `1-BIT / DU` | better than 4-level |
| White flashing | **gone** |
| Scrolling | "not bad" --- previously unplayable |
| Residual | some background ghosting remains |

**RESOLVED 2026-08-27, on the panel:** the two 1-bit rungs are
**visually indistinguishable** to the owner. The win is the CONTENT being
two-valued, not the forced waveform --- AUTO already selects something that
completes a two-level transition. So `MENU -> MOTION`'s DU rung buys nothing
observable, and the waveform does not belong in any model built on this.

**Also measured by hand:** `present_divisor = 8` "flashes much less on
scrolling" than 4. Presenting LESS often during a full-area change largely
removes the wash-out --- which brackets the panel's settle time for a
full-rect update between roughly 67 ms (divisor 4, visibly too fast) and
133 ms (divisor 8, mostly settling in time). That bracket comes from the panel
doing the real job, so it outranks any synthetic probe number that disagrees
with it.

The cost of that manual fix is 7.5 fps EVERYWHERE --- menus, static screens,
and the Game & Watch titles that never scroll at all. That is what
area-aware pacing exists to avoid.

~~**Still unknown:** DU has never been TIMED on any panel~~ --- **MEASURED
2026-08-27, and it explains the "indistinguishable" verdict rather than merely
corroborating it: on 1-bit content AUTO *is* DU.** See the section below.

**The Game Boy is the case with most to lose** and has not been judged: its
four shades already ARE the panel's four levels, so it is the one system where
4-level content asks the panel for nothing it cannot do.

### The panel's refresh cost, measured at last --- 2026-08-27

`koboy-probe --coexist` on the Libra 2, Nickel up, run twice thirty seconds
apart. This is the first time this project has had a settle number for
anything other than DU4, and it is what area-aware present pacing is built on.
Full method, caveats and the negative result in Appendix E of
`docs/superpowers/specs/2026-08-24-koboy-design.md`.

Affine fit over five region sizes from 160x144 to 1120x1008 --- a 49x span in
area:

| waveform | fixed | per pixel | at the shipped 800x720 game rect |
|---|---|---|---|
| **DU4** | 15.1 ms | 15.0 ns | **24.1 ms** |
| A2 | 96.4 ms | 17.2 ns | 106.3 ms |
| **DU** | 144.4 ms | 15.8 ns | **153.5 ms** |
| GC16 | 357.7 ms | 22.5 ns | 370.6 ms |
| AUTO (on 1-bit content) | --- | --- | **153.0 ms**, i.e. DU |

Four findings, in order of how much they change:

1. **Refresh duration is ~94% FIXED in area.** DU moves only 145.1 -> 162.3 ms
   across a 49x area span. The per-pixel term is the same for every waveform
   (15-22 ns), because it is the controller's pixel processing rather than
   anything about the waveform. Dirty rectangles still pay --- the area term is
   real and a smaller rect leaves less of the picture in flight --- but a small
   update is not a *fast* update, which is the opposite of what the design
   assumed.
2. **AUTO is DU on the content koboy now sends.** Identical to within 0.5 ms at
   all three region sizes where AUTO gave a clean reading. So the MOTION
   ladder's `1-BIT / DU` rung selects the waveform `1-BIT / AUTO` was already
   getting --- exactly why the owner found the two indistinguishable, and
   `docs/FOLLOWUPS.md` #97 closed.
3. **It prices the 1-bit fix.** Four-level content could use DU4 at 24.1 ms.
   Two-level content gets clean transitions and pays 153.5 ms, a factor of
   **6.4**. The scroll flashing was that bill arriving.
4. **There is no back-pressure below koboy.** The probe submitted a new
   full-rect update every 6-13 ms without ever blocking, against a 153 ms
   completion. The driver takes work it cannot do and says nothing, so
   over-driving the panel is invisible from inside the process.

**The synthetic number agrees with the hand judgement.** The owner's bracket
--- divisor 4 (67 ms) visibly flashes, divisor 8 (134 ms) "flashes much less"
--- puts a full-rect settle above 134 ms. The probe says 153 ms, which is the
exact shape of "8 helped a lot and did not finish the job".

Reproducibility, same parameters thirty seconds apart: 145.05 -> 145.15,
148.51 -> 148.52, 153.47 -> 153.52 ms. Appendix B's 2.2x spread warning stands
BETWEEN instruments; within this one the figures are stable to 0.1%. Six of
fifty cells returned literal 5-second `MXCFB_WAIT_FOR_UPDATE_COMPLETE`
timeouts and are discarded --- `unreliable_wait_for=1` on this device, exactly
as Appendix B warned.

### Area-aware present pacing --- SHIPPED, NOT YET JUDGED ON THE PANEL

`settle_base_ms = 0`, `settle_full_ms = 150`: every presented frame is charged
`base + full * dirty/whole` and the next present is held until it elapses.
Everything about it is verified on the host --- the model, the hold, the
clamps, and an end-to-end pair of runs spending the same 100 ms as a flat
charge (205 frames held) and as an area charge (6 frames held).

**ON THE DEVICE, 2026-08-27.** `koboy --frames 900` run directly over ssh with
Nickel up (never through `scripts/koboy.sh`), Sonic Chaos on the Master
System, the owner's own settings (`gray_map = luma`, `force_dither = true`,
`waveform_fast = du`), an 879x576 game rect. Four runs, ten seconds apart, the
only variable being the divisor and whether the throttle is on:

| present_divisor | settle_full_ms | presented | settle-held |
|---|---|---|---|
| 3 | 0 | **235** | 0 |
| 3 | **150** | **112** | 387 |
| 8 | 0 | 90 | 0 |
| 8 | 150 | 89 | 7 |

Read the four rows together, because each pair says something the other
cannot:

- **At divisor 3 the panel was being asked for more than twice what it can
  finish.** 235 presents in 900 core frames is one every 64 ms against a
  ~153 ms settle. The throttle cuts it to 112 --- one every 134 ms --- which
  is what the panel can actually complete.
- **At divisor 8 the throttle is very nearly inert: 90 -> 89, seven frames
  held in nine hundred.** That is the strongest evidence in this table that
  the model is right, and nobody arranged it: divisor 8 is the setting the
  owner arrived at BY EYE, and the throttle independently agrees there is
  almost nothing left to take away there.
- **Divisor 3 with the throttle beats the owner's manual divisor 8 outright:
  112 presented frames against 90, 24% MORE**, while never over-driving the
  panel on the large updates. The extra frames are the small-area ones a flat
  divisor throws away for nothing.

So the recommendation to the owner is concrete: **put `present_divisor` back
to 3.** The setting they raised to 8 by hand is now done automatically, only
while the content needs it.

Stage timings from the same runs, which reconfirm `docs/FOLLOWUPS.md` #23 on a
larger rect than it was measured on: core 5.6 ms, **submit 20.3 ms**, blit
2.2 ms, refresh 0.7 ms (mean per presented frame, 879x576 = 506k px).

Device integrity after the session: `/mnt/onboard/.kobo/version` still carries
the real serial, `fbink -e` still reports `deviceName='Libra 2'`,
`Mark 9`, `hasEclipseWfm=1`, Nickel up throughout, `koboy.ini` untouched, the
previous binary kept as `koboy.prev`.

**What no `--frames` run can check is whether the flashing is GONE.** That is
the owner's, playing a scroller and looking at it.

**What no host can check is whether the flashing is gone.** That is the
owner's, playing a scroller. Expect scrolling to present at about 6.6 fps
instead of 7.4 and to look choppier and cleaner; expect everything that is not
a large-area change --- menus, static screens, Game & Watch --- to be exactly
as responsive as before, which is what separates this from `present_divisor
= 8`.

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
before and after (the real serial intact), FBInk still reporting
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

## `present_divisor` above 3, and the first scripted run of `MODE_MENU`

**2026-08-27, Libra 2, `koboy` run directly over ssh with `--frames` (Nickel
never stopped, so this says nothing about the takeover).** The divisor plan,
Task 1.

### The range nobody had tried

Darkwing Duck, 600 core frames per run, one run per value, 15 s idle between
runs. The three values `docs/FOLLOWUPS.md` #26 measured on 2026-08-26 were
re-run first and came back **identical to the frame**, which is what makes the
four new rows comparable rather than merely new:

| `present_divisor` | presented | fps | wall (ms) | submit mean |
|---|---|---|---|---|
| 1 | 115 | 11.2 | 10263 | 11.7 ms |
| 2 | 102 | 9.9 | 10255 | 14.5 ms |
| 3 (shipped) | 76 | 7.4 | 10243 | 15.0 ms |
| 4 | 67 | 6.5 | 10243 | 17.7 ms |
| 6 | 49 | 4.8 | 10261 | 18.4 ms |
| 8 | 39 | 3.8 | 10243 | 25.6 ms |
| 12 | 31 | 3.0 | 10268 | 24.8 ms |

**Wall clock is flat across a 12x range.** 10243-10268 ms, a spread of 0.24%.
Whatever this setting costs, it is not emulation speed, at any value.

**The delivered rate is not 1/divisor, and that is what set the menu ladder's
top at 8.** Requested presents fall as 1/d; delivered ones fall much more
slowly, because koboy suppresses an unchanged frame and a wider gap means
fewer of the frames it does present are duplicates. 8 -> 12 halves what is
requested and removes 8 presented frames in ten seconds. Past 8 the setting
stops buying what it exists to buy.

`submit`'s mean rises with the divisor for the same reason: each presented
frame carries more change, so more tiles are dirty. It is still the dominant
stage at every value (`docs/FOLLOWUPS.md` #23), and `blit` (0.14-0.38 ms) and
`refresh` (0.18-0.60 ms) are still noise beside it.

**What this does NOT establish, and it is the important half:** nobody has yet
looked at 4, 6 or 8 in motion on the panel. The numbers say what each value
costs in frames; they say nothing about whether the reduced smearing is worth
the choppiness. That judgement is the owner's, it can only be made while
looking at the game, and MENU -> FRAMES exists so that it can be.

### `MODE_MENU` driven by a script, on the device

The first automated run of an in-game MENU handler on real hardware. A
`koboy-arm` build carrying the `--ui-script` `menu` verb, run against
`roms/darkwing-duck.gb` with `menu` + `tap 200 360` (row 4, FRAMES):

```
koboy: present_divisor 3          <- read back off the live pacer at startup
koboy: present_divisor = 4        <- the row cycled it
```

and the ini it was pointed at went from `present_divisor = 3` to
`present_divisor = 4` with its other keys intact. So on a real panel, with a
real framebuffer and real geometry, the menu opened, the row was hit, the
value cycled and it persisted.

**Still not established on hardware:** the takeover (`scripts/koboy.sh` was
not run and Nickel was never stopped), real TOUCH input into the menu (the
script bypasses the touch transform and feeds panel coordinates directly),
and save states. `MENU_SAVE`/`MENU_LOAD` are now driven end to end on the HOST
-- `tests/smoke_host.sh` writes a state to slot 1 from a script and reads it
back in a second run, the first automated save state this project has produced
-- and the same two runs would work on the device, with Nickel up, exactly as
the FRAMES run above did. Nobody has done it. See `docs/FOLLOWUPS.md` #76.

The temporary binary and ini files were removed afterwards; the device still
runs the pre-change `koboy` and its `koboy.ini` is untouched.

## SNES and Mega Drive on the LCD control strip, and four measured ceilings

Kobo Libra 2, 2026-08-27, `--frames 900` over ssh with Nickel up. Never
through `scripts/koboy.sh`; the takeover was not exercised. Every number is
wall-clock against the 15,024 ms that 900 frames of 59.9227 Hz content takes
in real time, so **ideal / measured IS the speed**. Ten to fifteen seconds of
idle between runs, for the reason the rect-sizing section above gives: a
saturated Cortex-A9 does not give repeatable numbers.

### The layout move cost the two SNES titles nothing

`.sfc`, `.smc` and `.md` moved from the DMG faceplate to the LCD control
strip, because a SNES pad is A B X Y L R and a six-button Mega Drive is
A B C X Y Z, against the faceplate's two spare pockets. The strip built for
Game & Watch already carries a d-pad, four face buttons, two shoulders,
SELECT and START.

**The trap was that the LCD fit is fractional and full-width, so the SNES
scale ceiling had to survive the move.** It does, and the picture is
identical to the pixel: 897x672 in both layouts on this panel. Measured by
alternating the two binaries on the same ROM, twice each:

| Title | rect before | rect after | before | after | speed before | speed after |
|---|---|---|---|---|---|---|
| Star Fox (SuperFX) | 897x672 | 897x672 | 19,021 / 18,989 ms | 19,101 / 19,090 ms | 79.1% | **78.7%** |
| Kirby Super Star (SA-1) | 897x672 | 897x672 | 16,048 / 16,056 ms | 15,968 / 15,843 ms | 93.6% | **94.5%** |

`presented` is identical across every one of those eight runs (241 for Star
Fox, 246 for Kirby), which is what makes the pairs comparable. Star Fox is
0.4 points slower and Kirby 0.9 points faster --- both inside the run-to-run
spread, on the same picture size. **The layout move is free.**

Mega Drive did move, because its rect is fitted differently: 1172x896 on the
faceplate, 1264x966 fractional on the strip, 1.16x the area. Sonic went
17,017 / 17,117 ms to 17,459 / 17,458 ms --- 88.0% to 86.1%, repeatable to
within a millisecond. That regression is what the ceiling below removes.

### The three Sega rects were the largest in the project

They are the only ones over 900k pixels, and the owner reported all three as
slow in play. `video_submit` is paid per pixel, so this is one number each.
Sonic Chaos on both handhelds, Sonic on the Mega Drive, `scale` pinned in a
copy of the device's own ini, `presented` identical down each column:

| System | scale | rect | px | wall | speed | submit | blit | refresh |
|---|---|---|---|---|---|---|---|---|
| **Master System** | 4 (auto) | 1172x768 | 900,096 | 18,127 ms | 82.9% | 24.7 ms | 3.6 | 1.0 |
| | **3** | 879x576 | 506,304 | **15,373 ms** | **97.7%** | 15.3 | 1.9 | 0.5 |
| | 2 | 586x384 | 225,024 | 15,364 ms | 97.8% | 7.5 | 0.9 | 0.8 |
| **Game Gear** | 6 (auto) | 1152x864 | 995,328 | 19,097 ms | 78.7% | 25.5 | 6.6 | 1.9 |
| | 4 | 768x576 | 442,368 | 15,377 ms | 97.7% | 11.9 | 2.5 | 0.8 |
| | 3 | 576x432 | 248,832 | 15,385 ms | 97.7% | 7.4 | 1.4 | 0.5 |
| **Mega Drive** | uncapped | 1264x966 | 1,221,024 | 21,358 ms | 70.3% | 29.8 | 6.2 | 0.6 |
| | 4 | 1172x896 | 1,050,112 | 20,140 ms | 74.6% | 27.7 | 5.0 | 0.7 |
| | **3** | 879x672 | 590,688 | **16,628 ms** | 90.4% | 17.3 | 2.8 | 0.3 |

The Mega Drive column is later in the session than the other two and its
absolute numbers are inflated by whatever accumulates on this device --- the
same effect the rect-sizing section documents. Its ORDERING is what the
choice rests on, and the ordering is unambiguous.

**Master System takes 3.** Scale 2 buys nothing further (15,364 against
15,373 is noise), and 4 is the 83%.

**Mega Drive takes 3**, landing on 879x672 --- within eighteen columns of the
SNES's own capped picture. The two 16-bit systems end up presented alike,
which is a coincidence of the arithmetic and a welcome one.

**Game Gear takes 5, and 5 is not a new judgement.** Its frame is 160x144,
byte for byte the Game Boy's, and this file recorded for months that it
"lands on exactly 800x720, the Game Boy's scale-5 picture, by arithmetic, so
it needs no exemption". That was true when written. Then `pixel_aspect` made
the reserved rect 192 columns wide, the auto-fit went from 5 to 6, and the
picture became 1152x864 --- 1.73x the Game Boy's area on the same frame ---
with nothing watching, because the reasoning was in a comment rather than in
a test. The ceiling of 5 puts it back where it was believed to be: 960x720,
the Game Boy's rows with the pixel aspect the correction added. 4 would be
768x576, SMALLER than the Game Boy, for speed that 5 already has.

The sub-scale steps do not exist on the DMG faceplate --- the fit is an
integer --- so "the smallest reduction that works" is one of 4, 3 or 2 and
nothing between.

### The shipped ceilings, confirmed on the device

The sweep above pins `scale` in a copy of the device's own ini. This run does
not: it is the SHIPPED default path, no `--config`, no override, three
minutes of idle before the batch, so what is measured is the `ceiling` table
itself.

| System | rect before | rect after | wall before | wall after | speed before | speed after |
|---|---|---|---|---|---|---|
| Master System | 1172x768 | **879x576** | 18,127 ms | **15,365 ms** | 82.9% | **97.8%** |
| Game Gear | 1152x864 | **960x720** | 19,097 ms | **15,396 ms** | 78.7% | **97.6%** |
| Mega Drive | 1264x966 | **879x672** | 21,358 ms | **15,383 ms** | 70.3% | **97.7%** |
| Mega Drive (vs its pre-task DMG rect) | 1172x896 | 879x672 | 17,459 ms | 15,383 ms | 86.1% | **97.7%** |

All three now finish within 400 ms of the 15,024 ms the content itself takes,
which is the wall a paced emulator cannot go faster than. The last row is the
comparison that matters most for the Mega Drive and it is exactly like for
like: `presented` is 160 on both sides.

**What the cap bought, as pipeline capacity.** `1000 / (submit + blit +
refresh)` is the most presented frames a second this pipeline can deliver.
The device's `koboy.ini` asks for one every other core frame, so the DEMAND
is 29.96 a second:

| System | before | after | demand |
|---|---|---|---|
| Master System | 34.1 /s | **58.1 /s** | 29.96 |
| Game Gear | **29.4 /s** | **41.9 /s** | 29.96 |
| Mega Drive (1264x966) | **27.3 /s** | **49.1 /s** | 29.96 |
| Mega Drive (1172x896) | 29.9 /s | 49.1 /s | 29.96 |

**Two of the three were literally unable to meet the demand** --- the Game
Gear at 29.4 and the Mega Drive at 27.3, against 29.96 --- which is what the
owner was reporting as slow. All three now clear it by 1.4x or better.

A caveat on the `presented` counts: they are content-bound, and Sonic Chaos
does not reach the same scene every run (235 here against 348 in the sweep).
That does not weaken the speed claims --- a 15,3xx ms run is at the pacing
wall regardless of how many frames it chose to present --- but it does mean
presented-frames-per-second is not a like-for-like number across the two
sessions, and the capacity table above is used instead.

### The device is running `present_divisor = 2`, not 3

Worth stating because it changes what "slow" means. The device's own
`koboy.ini` says 2 (the owner cycled it there through the in-game menu), so
the loop asks for a presented frame every 16.7 ms --- 30 a second, not 20.
At the Master System's uncapped 29.3 ms of pipeline per presented frame there
is no headroom at all; at 17.7 ms there is.

### What is still not established

- **Nothing here is a playtest.** Nickel stayed up for every run, so the
  takeover, the touch d-pad, the six-button grid under a real thumb and the
  in-game MENU by touch are all still unexercised on hardware for these
  systems.
- **The six-button Mega Drive pad is auto-detected from the cartridge
  header** (`rominfo.peripherals & 2`, the `6` in the ROM's I/O-support
  field). Read out of Genesis Plus GX, not observed: no six-button title has
  been played on the device to see X, Y and Z respond.
- **Only one title per Sega system was measured.** Virtua Racing, the
  heaviest Mega Drive title this project has met (84% at 957x720 before any
  of this), is not on the device.

## Game Boy Advance, measured on the device, 2026-08-27

The fifteenth system, and the second one the v1 design spec ruled out on CPU
grounds. SNES was the first, and re-testing that judgement rather than
inheriting it is why this one was re-tested too. **The spec was wrong about
this one as well.**

The device slept for most of the session and woke for the last of it, so this
section has both kinds of number in it and says which is which on every
table. The core benchmark and the dynarec check are MEASURED on the Libra 2.

### THE MEASUREMENT: gpSP on the Libra 2

`corebench-arm`, 600 frames after a 2,400-frame warmup, `--mash` on (so the
measured window is gameplay and not a title screen --- see
`scripts/corebench.c`), Nickel up. The percentages are computed against the
**4,316 us** budget measured in the rect sweep below --- the per-core-frame
allowance at the device's own `present_divisor = 2` with the GBA rect at its
shipped ceiling of 4. (The run itself was given `--budget-us 6142`, a figure
modelled before the sweep existed; the microseconds are the measurement and
the percentages here are recomputed from them.)

| Title | class | mean us | p95 us | worst us | % of budget (mean) | % (p95) | % (worst frame) |
|---|---|---|---|---|---|---|---|
| Advance Wars 2 | turn-based | 2,868.2 | 3,524 | 7,069 | 66% | 82% | 164% |
| Fire Emblem | turn-based | 2,477.1 | 3,289 | 13,040 | 57% | 76% | 302% |
| Final Fantasy Tactics Advance | turn-based | 2,662.1 | 3,059 | 4,146 | 62% | 71% | 96% |
| Golden Sun | turn-based | 2,611.1 | 3,547 | 7,180 | 60% | 82% | 166% |
| Metroid Fusion | action | 1,804.6 | 2,707 | 4,795 | 42% | 63% | 111% |
| Castlevania: Aria of Sorrow | action | 2,561.5 | 3,252 | 6,590 | 59% | 75% | 153% |
| Super Mario Advance 2 | action | 2,476.6 | 2,995 | 4,010 | 57% | 69% | 93% |
| Astro Boy: Omega Factor | action | 3,165.5 | 4,108 | 6,326 | 73% | 95% | 147% |

**Every title runs at full speed, the tightest mean at 73% of budget.** For
scale, the two systems measured on this device before it: gambatte 2.1--2.5
ms, fceumm 4.3--4.6 ms. **A Game Boy Advance costs less per frame here than a
NES**, which is what an ARM recompiler buys against two interpreters.

The `worst` column crosses budget on six titles and Fire Emblem's crosses it
three times over --- one frame in 600 at 13.0 ms. That is what `p95` is
printed for: every p95 is inside 95% of budget, so those are single frames (a
scene transition, a battle animation spawning), not a sustained state. The
same shape as the SNES rows earlier in this file, and less severe than Star
Fox's.

**Turn-based titles are not the cheap ones**, which is worth noting because
the system was added for them: Advance Wars 2 and Golden Sun cost MORE than
Metroid Fusion and Super Mario Advance 2 in this run. A strategy map is a
screen full of sprites and a scrolling background; a corridor is not. The
case for the turn-based library on this panel is about e-ink dwell time and
legibility, not about emulation cost.

### The same eight with the screen SCROLLING, which is a different answer

`--mash` presses START and A and no direction, so nothing in the table above
scrolls --- and scrolling is where a GBA's four background layers cost what
they cost. `--walk` holds RIGHT as well (see `corebench.c`), and it is run
here at a 8,400-frame warmup so the measured window is deeper into each
title. Same device, same budget:

| Title | `--mash` mean | `--walk` mean | `--walk` p95 | `--walk` worst | % of budget (walk mean) |
|---|---|---|---|---|---|
| Advance Wars 2 | 2,868.2 | 2,022.7 | 2,152 | 3,199 | 47% |
| Fire Emblem | 2,477.1 | 2,339.1 | 2,835 | 5,260 | 54% |
| Final Fantasy Tactics Advance | 2,662.1 | 2,706.4 | 3,035 | 4,270 | 63% |
| Golden Sun | 2,611.1 | 3,524.6 | 4,209 | 17,126 | 82% |
| **Metroid Fusion** | 1,804.6 | **4,467.3** | 5,236 | 6,884 | **104%** |
| Castlevania: Aria of Sorrow | 2,561.5 | 1,318.1 | 1,943 | 3,308 | 31% |
| Super Mario Advance 2 | 2,476.6 | 2,637.9 | 3,972 | 5,324 | 61% |
| Astro Boy: Omega Factor | 3,165.5 | 3,153.3 | 4,098 | 6,138 | 73% |

**Metroid Fusion is 2.5x more expensive once it is actually moving** ---
1,804 us of cutscene against 4,467 us of Samus running through a corridor.
That is the single most useful number in this section, and `--mash` alone
would have reported the cheaper one. It is the one title that ends up ON the
budget rather than inside it: 4,467 against 4,316 is **99.1% of full speed**,
94.8% at p95. See the ceiling section below for why the remedy is the divisor
and not a smaller picture.

The two runs are NOT the same trajectory --- RIGHT is held through the menus
too, so a different option gets selected and a different scene is reached
(Aria of Sorrow ends up somewhere cheaper, Advance Wars 2 likewise). The
honest reading is the **worse of the two per title**, which is what the
"playable?" judgement below uses. Golden Sun's 17,126 us worst frame is one
frame in 600 against a 3,503 us median; that is what p95 is printed for.

### The core was chosen from three, on the host

The device slept for the part of the session when the choice had to be made,
so the shootout is a host measurement. Host, x86_64, 600 frames after a
2400-frame warmup, `--mash`, mean us per `retro_run`:

| title | gpSP | mGBA | vba-next |
|---|---|---|---|
| Advance Wars 2 | 377.4 | 310.2 | 791.1 |
| Fire Emblem | 319.3 | 339.1 | 580.2 |
| Final Fantasy Tactics Advance | 493.9 | 632.7 | 934.2 |
| Golden Sun | 250.0 | 336.4 | 480.3 |
| Metroid Fusion | 220.1 | 310.6 | 463.0 |
| Castlevania: Aria of Sorrow | 328.4 | 453.5 | 659.4 |
| Super Mario Advance 2 | 335.9 | 410.9 | 951.5 |
| Astro Boy: Omega Factor | 458.5 | 570.9 | 748.7 |
| **sum** | **2783.5** | 3364.3 | 5608.4 |

gpSP wins by 1.21x over mGBA and 2.01x over vba-next **as an interpreter** ---
its dynarec is ARM-only, so this column is gpSP's worst case and the other
two cores' only case. **vba-next being the slowest is not what the folklore
says**, and it is not close: it loses to mGBA by 1.67x on every one of the
eight. That reputation dates from when mGBA was new.

### THE DYNAREC RUNS ON THIS KERNEL --- checked directly, not inferred

The one thing that had to be established before any device figure could be
believed. gpSP's ARM recompiler writes ARM instructions into a buffer and
jumps into them, so it asks `mmap` for memory that is writable AND
executable; a hardened kernel refuses, gpSP falls back to its interpreter,
the run completes, and the numbers look like numbers. Silent, and it would
have corrupted every row below.

Checked by looking at the process's own memory map while it ran, which is the
only check that cannot be fooled by the timings:

```
# ./corebench-arm --frames 3000 --warmup 200 --mash ./gpsp_libretro_arm.so ./aw2.gba &
# grep rwxp /proc/6203/maps
760fe000-76b7e000 rwxp 00000000 00:00 0
```

One 11,206,656-byte read-write-execute mapping. That is the JIT cache, the
kernel granted it, and every core figure in this section is a figure about
gpSP's ARM RECOMPILER on a Cortex-A9.

`scripts/build-gba-core.sh kobo` produces a 681 KB stripped ARM core needing
`libm.so.6` and `libc.so.6` and nothing else --- smaller than every other core
koboy ships except the Game & Watch one.

### The ceiling of 4, MEASURED --- and uncapped is not merely slow, it is impossible

A GBA frame is 240x160 with square pixels: the smallest frame koboy scales,
so it auto-fits furthest. Uncapped it takes the LCD strip's full width.
`koboy-arm --frames 900` on the device, Advance Wars 2, `scale` pinned in a
copy of the device's own ini so only the rect differs, three minutes of idle
before the batch:

| scale | rect | px | submit | blit | refresh | pipeline | capacity | demand at divisor 2 |
|---|---|---|---|---|---|---|---|---|
| 3 | 720x480 | 345,600 | 11,330 us | 2,388 | 1,065 | 14,783 us | 67.6 /s | 29.96 /s |
| **4 (shipped)** | **960x640** | **614,400** | **18,148** | **4,405** | **2,299** | **24,852 us** | **40.2 /s** | 29.96 /s |
| 6 (uncapped fit) | 1264x842 | 1,064,288 | 30,273 | 7,693 | 2,732 | 40,698 us | **24.6 /s** | 29.96 /s |

The fourth row of that run is the SHIPPED path with no `scale` override at
all --- 960x640, `submit` 18,014, within 0.7% of the pinned scale-4 row, so
the `ceiling` table really is what produces it.

**The uncapped rect cannot meet the demand.** 24.6 presented frames a second
against a demand of 29.96 is the Game Gear's situation before its ceiling,
and worse. Turned into the per-core-frame budget the benchmark above is
scored against, `16742 - pipeline/divisor`:

| scale | budget at divisor 2 (the device's own) | budget at divisor 3 (shipped) |
|---|---|---|
| 3 | 9,350 us | 11,815 us |
| **4** | **4,316 us** | 8,458 us |
| 6 | **negative** --- presentation alone exceeds the frame | 3,176 us |

At the owner's `present_divisor = 2` an uncapped GBA has **no budget at all**:
40.7 ms of pipeline every other frame is 20.3 ms charged against a 16.7 ms
frame before the emulator runs. That is not a ceiling that buys headroom, it
is a ceiling that makes the system exist.

**What 4 costs, stated rather than left to be found.** At divisor 2 the
budget is 4,316 us, and the worst mean measured on this device is Metroid
Fusion's 4,467 us with the screen scrolling --- 3.5% over, i.e. **99.1% of
full speed**. Its p95 puts it at 94.8%. Every other title in both runs is
inside. So at the device's current setting, the heaviest scrolling action
title has no headroom left and everything else has plenty.

**The remedy is the divisor, not a smaller picture for everybody.** At the
shipped `present_divisor = 3` the same rect gives 8,458 us and Fusion sits at
53% of budget; the divisor is a menu entry the owner already cycles. Dropping
the ceiling to 3 would buy the same headroom by throwing away 44% of the
picture area for all fifteen hundred titles, to rescue one.

### Compatibility: 1692 of 1693 files load and run

Whole-collection sweep, host, two frames each. **One file crashes**, and it
crashes the process:

```
SIGSEGV in execute_arm(), called from retro_run()
  4 Homebrew/Battlenetwork Rockman Crystal (PD).gba   4,194,304 bytes
```

It is not a truncated file --- 4 MB, a valid Nintendo logo, a header naming
"RockMan" --- so `config_min_rom_bytes` cannot help, and the fault is in
`retro_run` rather than `retro_load_game`, so a floor at the load site could
not catch it either. **mGBA runs the same file happily; vba-next crashes on
it too.** That is the compatibility cost of choosing the fastest core, and it
is one homebrew file in 1693.

The same sweep against mGBA, because the comparison is not one-sided:
`TOTAL=1693 OK=1690 CRASH=0 REFUSED=3`. mGBA *rejects* three homebrew files
(`Gapman (PD)`, `Pacoman`, `Dwarrendelf`) that gpSP loads and runs. gpSP has
the better LOAD compatibility of the two --- 1693 against 1690 --- and one
fatal where mGBA has none. The folklore that gpSP trades a lot of
compatibility for its speed is, at this revision, worth about one file in
either direction.

### The four greys: this system reduces BETTER than expected

Rendered through koboy's own pipeline (`video.c` at the shipped
`gray_map = balanced`, scale 4) at frame 3000 of a mashed run, so what was
looked at is gameplay and not a logo. Eight titles: Advance Wars 2's map,
Fire Emblem's battle map, Metroid Fusion's opening text, Super Mario Advance
2's overworld, FFTA's dialogue, Golden Sun's village, Pokemon Emerald's
character screen, Aria of Sorrow's Soul Set menu.

**Text is the surprise.** 240x160 at 4x is 960x640 on a 300 dpi panel, and a
GBA's 8-pixel font lands at 32 px --- Aria of Sorrow's stat table, FFTA's
name-entry keyboard and Pokemon Mystery Dungeon's are all crisply legible.
The turn-based library this system was added for is the library that reads
best.

`gray_map` needs no per-system exception: `luma`, `balanced`, `bright` and
`equal` are near-indistinguishable on GBA art, and only `value`
(`max(R,G,B)`) is wrong for it --- Golden Sun's village blows out to white.
The shipped default stands.

### Saves: the mechanism is proven, one class is not

gpSP reports `RETRO_MEMORY_SAVE_RAM` as a flat **131072 bytes at load and
131072 running**, on every title tried, because it hands back its whole
`gamepak_backup` array ("assume 128KiB, biggest possible save") regardless of
which backup chip the cartridge has. So:

- **`core_sram`'s pin-at-load is harmless here and not load-bearing**, unlike
  Genesis Plus GX, whose size drops to 0 mid-run. Established rather than
  assumed, the same way the SNES core's was.
- Every `.gba` gets a 128 KB `.srm` whatever its real save size is.
- **Detection is gpSP's job and it does it three ways**: a per-cartridge
  override table (`gba_over.h`, which has explicit entries for Pokemon Ruby,
  Sapphire and Emerald and printed `gamepak code match for : BPEE` / `AXVE`
  on load here), a scan of the ROM for `EEPROM_V` / `SRAM_V` / `FLASH1M_V` /
  `FLASH_V` signatures, and a Pokemon-family fallback that forces 128 KB
  flash. The classic wrong-save-type failure is what that table exists for.

**Round trip, proven ON THE DEVICE, on Fire Emblem.** `koboy-arm --frames
600` against the Libra 2's own FAT32, four runs:

| step | result |
|---|---|
| run 1, no save present | wrote `fe.srm`, **131,072 bytes**, md5 `c839acb750926e32a7149f8cefd6e2ce` |
| run 2, same conditions | **same md5** --- the instrument is deterministic on the device too |
| `.srm` cut to 65,536 bytes, then run | file left at **65,536** --- not overwritten short, not destroyed |
| run 3, loading the full `c839acb7…` | wrote **`8dbce47990f4e47f3c7d78ddbe0faef3`** --- a different save |

The last row is the round trip: the game read the file koboy wrote and its
state diverged from a fresh boot. The third is `sram_writeback` staying false
for the session when a save file exists but cannot be read whole --- the
destructive-truncation guard that FOLLOWUPS #3 was opened for, now shown
working on a 128 KB GBA save as well as on Zelda's 8 KB one.

The same divergence was established on the host beforehand, with a tighter
instrument: two identical runs produced byte-identical FRAMES (md5
`1d85cea…` twice), and a run loading koboy's `.srm` produced a different one.

**Pokemon: the mechanism reaches the game, a real save was never made.** An
automated masher cannot get Emerald past Birch's intro (START restarts it),
so no in-game save was reached in 90,000 frames. What was established instead
is that the file koboy writes is READ: booting Emerald with a 128 KB
random-bytes `.srm` produced a different frame from booting it with none, on
the same deterministic instrument. Combined with the override table matching
`BPEE`, that says the flash koboy persists is the flash the game sees ---
but **no Pokemon save has been created, written and reloaded**, and that is
the one gap in this section a player could actually lose progress to.

Six of ten titles wrote to backup memory within 4000 mashed frames (Fire
Emblem 20,223 bytes; Aria of Sorrow 32,768; Advance Wars 2 1,548 at offset
61,440; Pokemon Mystery Dungeon 470; Metroid Fusion 192; Super Mario Advance
2 1,504). Golden Sun, FFTA and both Pokemon titles wrote nothing, because
none of them had reached a save point.

## A ROM that will not load, on the device, 2026-08-27

Reported from the device twice: **selecting a game from RECENT exits koboy
back to Nickel.** Verified fixed on the Libra 2, with `--ui-script` runs over
ssh (Nickel up, never `scripts/koboy.sh`), all against isolated
`--save-dir`/`--rom-dir` trees under `/tmp` so the owner's own saves and play
history were never written to.

The failure is real rather than simulated: a 212-byte `.sfc`, which is the
one refusal that can be produced from outside the process without a core that
lies (`config_min_rom_bytes`; snes9x2005 SIGFPEs on it).

| Path | What ran | Result |
|---|---|---|
| RECENT | 8,192-byte `BAD.sfc` recorded, truncated to 212, then selected | panel drew `COULD NOT LOAD / BAD.sfc / too short…`, returned to MAIN MENU, second pick (`ZGOOD.gb`) loaded and played, **exit 0** |
| ALL GAMES | same file selected from the browser | same, and the core moved `snes9x2005 -> gambatte` and the faceplate `LCD -> DMG` on the retry |
| MENU -> CHOOSE ROM | `menu`, CHOOSE ROM, ALL GAMES, the bad file, ALL GAMES, a good one | `switched to /tmp/kt3/roms/ZGOOD.gb`, **exit 0** |

Everything derived from the extension --- core, faceplate, buttons, ceiling
--- is re-derived per attempt, and the LCD -> DMG move in row two is the
device's own confirmation of it.

**The error message holds the panel for 20 seconds** when nothing taps: the
run above measured exactly 20 s between the failed ROM and the MAIN MENU.
That is `platform_kobo_fatal`'s acknowledgement wait, unchanged, now reached
by a non-terminal message too. See `docs/FOLLOWUPS.md` #90.

### The owner's corrupted RECENT row, repaired against their own file

`.adds/koboy/recent.dat` (md5 `94ad597d…`, 10 entries) carried the row
`4652789` was reported for: path `…/GBA/Advance Wars 2 - Black Hole Rising
(USA).gba`, display `Pokemon - Emerald Version (USA, Europe).gba`. It also
carried a second, harmless divergence nobody had noticed: `…/roms/gbc/Hamtaro
- Ham-Hams Unite! (USA).gbc` displayed as `gbc/Hamtaro - Ham-Hams Unite!
(USA).gbc`, a relative name left over from the flattened browser.

A COPY of that file was run through the fixed binary: selecting the Advance
Wars row loaded Advance Wars (`gpsp_libretro.so`, `gamepak code match for :
AW2E`), and the rewritten copy names every row after its own path, both
divergences included.

**The device's own `recent.dat` was not modified** --- md5 `94ad597d…` before
and after, with backups at `/tmp/recent.dat.bak` on the device and in the
session scratchpad. It repairs itself the next time RECENT is opened by a
build carrying this fix, because the derivation runs in `recent_load`.

The fixed binary is on the device as `.adds/koboy/koboy-test`, beside the
shipped `koboy`, which was left untouched: nothing launches it, so the device
still runs exactly what it ran before.

## Switching systems mid-session, on the device, 2026-08-27

The owner's second report: playing a GBA game, MENU -> CHOOSE ROM, picked a
Mega Drive game, koboy died to Nickel. Reproduced and fixed on the Libra 2,
`--ui-script` over ssh with Nickel up, isolated `--save-dir` throughout.

**The reproduction, on the binary the device was running** (`4febe63`, the
one deployed at 14:17): `--rom` a GBA title, `menu`, CHOOSE ROM, ALL GAMES,
into a Sega folder, pick a game.

    koboy: core /mnt/onboard/.adds/koboy/gpsp_libretro.so
    koboy: switched to .../MasterSystem/Sonic Chaos (Europe, Brazil).sms
    gamepak code match for : AW2E
    bad jump 8000000 (8000000)
    Segmentation fault
    EXIT=139

One core line for two games. The GBA core was handed Sega data and executed
it as ARM code. `.sms` here, `.md` in the owner's report -- the extension
does not matter, only that it is not the one the open core was chosen for.

**The same script on the fixed binary**, exit 0:

| | first session | second session |
|---|---|---|
| core | `gpsp_libretro.so` | `genesis_plus_gx_libretro.so` |
| faceplate | LCD | DMG |
| geometry | 240x160 (max 240x160), game 960x640 | 256x192 (max 284x240), game 879x576 |
| pacing | 59.7275 fps, 16742 us | 59.9227 fps, 16688 us |
| save | `Advance Wars 2 ….srm` | `Sonic Chaos ….srm` |

The frame rate is worth a line of its own: the old path kept the FIRST core's
pacer, so even a switch that had not crashed would have run the second game
at the first system's rate. Two `.srm` files, each named for its own ROM, is
the other half -- a stale binding wrote the incoming game's memory into the
outgoing game's save file, which is a silent corruption rather than a crash.

The owner's exact case (GBA -> `MegaDrive/Sonic The Hedgehog (USA,
Europe).md`) was run separately and also exits 0.

### Four sessions in one process, with the real cores

The session loop's whole job is being re-entered, and `core_close` on gpSP is
a `dlclose` of a C++ core with a dynamic recompiler -- something core.h used
to say was "avoidable, so it is avoided". It is now routine, so it was
measured rather than assumed: two ROMs copied into a tmpfs directory, three
switches in one run.

    gpsp -> genesis_plus_gx -> gpsp -> genesis_plus_gx      exit 0
    20 presented frames, 24 rects, both .srm files written

gpSP was closed and reopened twice on the device without incident.

### Three rc=139 crashes in the log, and only one of them is this bug

`koboy.log` on the device holds three: 13:19:00, 13:20:06 and 14:20:34. Only
the last is the switch, and only the last is on the deployed `4febe63`.

The other two are on the 13:09 binary, involve no ROM switch at all, and both
sit next to a `present_divisor` cycled up to 8 -- the first right after
`present_divisor = 4 -> 6 -> 8` during play, the second on the very next
launch, which started at 8 (the menu writes it back to the ini) and faulted
before presenting a frame. **Not reproduced**: 60 frames of Pokemon Emerald
at `present_divisor = 8` exits 0 on both the deployed binary and the fixed
one. The device's live `koboy.ini` still says 8. See `docs/FOLLOWUPS.md` #92.

## The motion pair: 1-bit output and a two-level waveform (2026-08-27)

**NOTHING IN THIS SECTION HAS BEEN SEEN ON A PANEL.** It is here so that the
next session knows exactly what was and was not established, because the thing
this change is for -- e-ink residue behind a moving sprite -- is invisible to
every instrument koboy has. Residue is panel-side; koboy's dirty diff compares
koboy's own output buffers, so a `--frames` run cannot see ghosting at all.
That is why `docs/FOLLOWUPS.md` #25 has outlived two attempts.

### What the photographs established

The owner filmed Super Mario Bros. under the takeover. A jump leaves a
vertical column one dirty-rect wide holding a faint grey ghost above the
sprite, the solid sprite, and **bright white bands below, brighter than the
sky**. The white is the finding: those pixels were black (sprite) and are
being driven toward a mid-grey sky, and they overshoot past it. Both
directions of the transition land wrong. The residue never leaves the dirty
rect's column, so the dirty-rect logic is exonerated -- this is waveform and
levels.

Sampled off the live framebuffer during play, before the panel does anything:
**sky 170 (level 2), brick 85 (level 1)**. A mid-grey background is what makes
every sprite transition a hard one for a fast waveform.

### What was verified, on the host only

| claim | how |
|---|---|
| 1-bit output really is two-valued end to end | `tests/test_video_pipeline.c`: exactly 2 distinct byte values over the whole 800x720 game rect, and they are 0x00 and 0xFF |
| the flag flips on a LIVE pipeline | same file: 4 levels -> 2 -> 4 across `video_set_dither`, so the menu row does not need a relaunch |
| pure white stays pure white under dither | `tests/test_video_quant.c`: 0 dark pixels over four whole 16x16 tiles of 255 (this was NOT true before -- see below) |
| both halves reach the live objects | `tests/smoke_host.sh`: `koboy: motion 1-bit / DU` printed off the live `koboy_video` and the live backend, not off `koboy_config` |
| the ladder steps the PAIR | three scripted MENU runs, one per rung, each from the rung below |
| both ini keys persist, in one write | same runs; `force_dither` and `waveform_fast` each appear exactly once after five cycles |

### The white-speckle bug this found

The ditherer thresholded against the raw Bayer matrix, a permutation of
0..255, so `255 > 255` was false and **one pixel in every 16x16 tile of pure
white came out black** -- 2250 isolated black dots over an 800x720 game rect
at 16px spacing. Pure white is not a corner case: it is the Game Boy's own
lightest shade and most HUD text on every other system. Fixed by scaling the
thresholds to 0..254 (`g_thresh` in `src/video.c`). This would have poisoned
the very judgement the owner is being asked to make -- "1-bit looks dirty" for
a reason that has nothing to do with motion.

### Per-stage cost, host, dither on versus off

Measured through `koboy --frames`, same ROM, same geometry, so the numbers are
comparable to each other and NOT to any device figure.

| force_dither | `submit` mean (3 runs) | `presented` | rects |
|---|---|---|---|
| false | 1110 / 994 / 1075 us | 300 | 1 per frame |
| true  | 1286 / 1284 / 1374 us | 300 | 1 per frame |

**Dithering costs about 24% more `video_submit`**, which is the stage CLAUDE.md
identifies as the pipeline's bottleneck. On the device that stage is ~17 ms for
a Game Boy at scale 5, so expect roughly +4 ms per presented frame. At the
owner's `present_divisor = 8` there is ample budget; at 1 or 2 it will cost
presented frames. The rect count does not move -- the Bayer phase is indexed by
absolute screen position, so a static region dithers identically every frame
and the dirty diff still suppresses it.

`blit` and `refresh` here are SDL numbers and mean nothing about a panel;
`core` is a stub. Only the `submit` column is a real comparison, and only
against itself.

### What is still needed

A play session. Cycle MENU -> MOTION through its three rungs on the same jump
in SMB and say which column looks least wrong. The middle rung (1-BIT / AUTO)
is the control: if it is already as good as 1-BIT / DU, the waveform is not
doing the work. Expect the 1-bit rungs to make a flat sky a fine black-and-
white pattern rather than a flat grey -- 170 is about two thirds white -- and
expect the Game Boy to LOSE from this, since its four shades already are the
panel's four levels.

Also worth doing in the same session, because it is one probe run with Nickel
up: `koboy-probe --coexist` now times DU alongside AUTO/DU4/A2/GC16. Nobody
has a number for what DU costs on this panel, and if it is as slow as FBInk's
header guesses (~260 ms) that changes the verdict independently of how it
looks. `docs/FOLLOWUPS.md` #97.

