# How koboy works

For someone reading the source. The architecture is the short half; the long
half is the set of decisions that look wrong from outside and are not, because
the hardware said so. Those are the ones a newcomer would "fix" into a bug,
and every one below has a number behind it.

Everything measured was measured on a Kobo Libra 2 — Mark 9, single-core
Cortex-A9 with NEON, 1264×1680 panel at 300 dpi, glibc 2.19 — unless the text
says otherwise. `TESTED.md` has the raw sessions; `docs/FOLLOWUPS.md` has
roughly a hundred numbered findings, and the `#N` references below point into
it.

---

## The shape of it

koboy is a C program that `dlopen`s a libretro emulator core, takes the RGB565
frames the core produces, turns them into four-level greyscale at panel scale,
and pushes the changed rectangles into the framebuffer through FBInk. Input
comes off `/dev/input/event*` directly.

```
                        ┌─────────────────────────────────────┐
   ROM extension ──────▶│ config.c   picks core + layout      │
                        │            + per-system scale cap   │
                        └──────────────┬──────────────────────┘
                                       │
   ┌───────────────────────────────────▼─────────────────────────────────┐
   │ main.c   the loop: core_run → video_submit → blit → refresh         │
   └───┬─────────────┬───────────────────┬──────────────┬────────────────┘
       │             │                   │              │
   ┌───▼────┐  ┌─────▼──────┐   ┌────────▼───────┐  ┌───▼──────────────┐
   │ core.c │  │ video.c    │   │ pacing.c       │  │ platform_if.h    │
   │ dlopen │  │ RGB565→    │   │ when the next  │  │  ┌─────────────┐ │
   │ retro_*│  │ gray8, up- │   │ frame may      │  │  │platform_kobo│ │
   │ symbols│  │ scale,     │   │ reach the      │  │  │ FBInk+evdev │ │
   └────────┘  │ quantise,  │   │ panel          │  │  ├─────────────┤ │
               │ dither,    │   └────────────────┘  │  │platform_sdl │ │
               │ dirty diff │                       │  │ the host    │ │
               └────────────┘                       │  └─────────────┘ │
                                                    └──────────────────┘
```

`chrome.c` draws the faceplate around the game rect once; `input.c` turns
touches into d-pad directions and button bits; `ui.c` is one list widget
serving both the ROM browser and the in-game menu; `state.c` and `sram.c`
persist through `safefile.c`.

### The platform seam

`src/platform_if.h` is a vtable of nine functions: `init`, `shutdown`,
`screen_info`, `blit_gray8`, `refresh`, `poll_input`, `now_us`, `should_quit`,
plus two optional ones (`battery_percent`, `set_wfm_policy`) and
`wfm_fast_name`. `platform_kobo.c` and `platform_sdl.c` are the two sides.

The rule that makes it work is that **everything above the seam is tested on
the dev host**. Pixels arrive at `blit_gray8` as gray8 at final scale; input
arrives already normalised to libretro joypad bits. `main.c` is byte-for-byte
the same file on a desktop and on the device.

Two consequences worth knowing before you touch either side:

- **Never `#include <linux/input.h>` in portable code.** The project carries
  its own three-field `koboy_ev {type, code, value}` mirror and its own keycode
  constants, so the host build compiles on a machine that has never seen a
  Kobo. `platform_kobo.c` is the only file allowed to know what a real
  `input_event` looks like.
- **`wfm_fast_name` reads back off the platform, not off the config.** A line
  reporting what `config.c` parsed proves `config.c`. This one fails if the
  setting never reached the backend — and on the device it reports the *real
  mapped* waveform, which is strictly more than an echo: a DU4 request on a
  panel without the eclipse quirk maps to A2, and this is what says so.

There is a third rule that is not about the seam but about the same class of
mistake. **Paths resolve against `/proc/self/exe`'s directory, never the
cwd**, because `dlopen` does not search the cwd. That cost a debugging round
when the core sat right beside the binary and still failed to load.

---

## `video_submit` is the bottleneck, and it is not where the design expected

The v1 design spec said *"emulation is cheap; presentation is the entire
bottleneck."* Half right, and the wrong half matters. Measured on the device,
Zelda, scale 5, per presented frame:

| Stage | Mean | What it is |
|---|---|---|
| core | 2.3 ms | the emulator |
| **submit** | **17.0 ms** | RGB565→gray LUT, integer upscale, quantise, dither, 8×8-tile diff |
| blit | 2.8 ms | copying the result into the mapped framebuffer |
| refresh | 0.4–0.75 ms | *submitting* the ioctl |

`video_submit` is roughly **five times the other three combined**, and it is
neither "emulation" nor "presentation" — it is the pixel pipeline sitting
between them. It is pixel-bound, confirmed by a scale sweep rather than
assumed:

| scale | destination px | submit |
|---|---|---|
| 3 | 207,360 | 8,997 µs |
| 4 | 368,640 | 12,462 µs |
| 5 | 576,000 | 16,639 µs |

The fit is **`submit ≈ 4.7 ms + 20.7 ns/px`**, and it predicts the scale-4
point within 1%. That model is used all over `TESTED.md` to estimate a
system's cost before it has run on hardware, and it has held.

Two things follow, and both are counterintuitive:

**Emulation is genuinely free here.** Every one of the fifteen systems costs
**2.0 to 4.4 ms** a frame on this CPU — a whole Mega Drive for 4.0 ms, a Game
Boy Advance for less than a NES — against 14 to 23 ms of pixel pipeline. The
v1 spec ruled SNES and GBA out of scope on CPU grounds and was wrong about
both.

**Cost scales with DESTINATION pixels, not source pixels.** This is the trap
people fall into when comparing systems. A Game & Watch canvas is 654×396 —
seventeen times the Game Boy's 160×144 — which suggests a twenty-fold cost
blowup. It is the opposite: G&W renders at 1× (Parachute 260k px, 10.1 ms)
while the Game Boy is upscaled to 800×720 (576k px, 16.6 ms). **G&W is
cheaper than the Game Boy.** (`#30`)

`video_submit` is where the next optimisation belongs. v2's multi-rect work
optimised `refresh`, which this table shows was already the cheapest stage by
a wide margin. (`#23`)

---

## Four grey levels, not sixteen

Not a simplification. The only option.

| Waveform | Measured refresh |
|---|---|
| GL16 (16-level) | 321.7 ms → 3.1 fps |
| GC16 (16-level) | 393.3 ms → 2.5 fps |

Sixteen-level rendering is not slow on this panel, it is unusable. Four levels
is what the fast waveforms can do.

### The greyscale mapping is a judgement, so it is selectable

The obvious reduction — Rec.601 luma — is wrong for this panel, and the
obvious correction is wrong too. Both established by rendering real frames and
looking.

Luma weights blue at 29/256, so saturated blue quantises to the *darkest*
level: Sonic Pocket Adventure's sky is `rgb(0,154,255)` → 119 → level 1;
Castlevania's is `rgb(0,36,140)` → 36 → level 0. A black sky the sprite
disappears into.

But `(R+G+B)/3` crushes **more** pixels to black than luma does — 8.9% against
6.7%, over 38 gameplay frames from 19 titles — because what it gives back to
blue it takes from green. What actually fixes the crushing is a **shadow
lift**, equivalent to lowering the first quantiser threshold: 6.7% → 2.5%.

The shipped `balanced` map needs both: weights `(81,118,57)` *and* the lift.
It is selectable at runtime (`MENU → GREYSCALE`, written back to the ini)
because this is a judgement about a reflective panel that cannot honestly be
made from a host render.

**Two things not to do here.** The Game Boy's palette is already neutral —
gambatte emits exactly `(0,0,0)`, `(82,85,82)`, `(173,170,173)`,
`(255,255,255)`, every mapping is the identity on neutral grey, and the lift
keeps both mid greys inside their existing levels. The DMG golden image is
byte-identical under all five maps, so **no per-system exemption exists and
none is needed**. And do not add one keyed on 160×144: **a Game Gear is also
160×144 and is a colour system.** `tests/test_video_gray.c` pins that, and the
mutant that would have been the bug (a `gray_map` exemption for 160×144 in
`video_create`) makes it fail on Sonic's measured sky. (`#43`, `#46`)

---

## The waveform findings, which reversed twice

This is the part of the project where folklore lost most often.

**A2 is not the fast waveform.** DU4 is ~3.5× faster on Mark 9, against most
of what is written about e-ink emulators.

**Then DU4 lost to AUTO.** Forced DU4 cannot *erase*, so it ghosted badly in
play. `waveform_fast = auto` lets the controller decide per update.

**And the mitigations DU4 needed caused the problem they were added for.**
`cleanup_interval` and `full_refresh_permille` at their old values produced
every flash the user saw while fixing nothing. They ship disabled and at
1000‰. The comment in `config/koboy.ini` says so, because "off" reads as an
oversight otherwise.

### Then the real mechanism turned up: the fast waveforms are TWO-LEVEL

The oldest open defect in the project — moving sprites smearing — was fixed by
the one thing nobody had tried: rendering the game as **genuinely two-valued
content** instead of four grey levels.

A DU-class waveform drives a changed pixel to black or white. Ask one for an
intermediate grey at partial-refresh speed and it lands somewhere between:
stale ghost above the sprite, and overshoot *brighter than the background*
below. Both directions of the transition failing at once, which is exactly
what the owner's photographs showed. Sampled off the live framebuffer during
play, the cause was in the data: Super Mario Bros.'s sky is written at level 2
— **a mid grey** — so every sprite transition had to travel most of the way to
an extreme.

Made 1-bit (`force_dither`, the `MENU → MOTION` row), every pixel is `0x00` or
`0xFF` and a two-level waveform completes every transition exactly. Confirmed
on the panel: *"motion is much better in both 1bit modes, no white flashing
now, even scrolling looks not bad."*

**"DU4 cannot erase" is the same mechanism seen from the other side.** DU4 is
the four-level variant; every pixel whose new value is one of the two *middle*
levels is a pixel the panel declines to touch. The two findings never
contradicted each other.

**The Game Boy is the case with most to lose** and has not been judged: its
four shades already *are* the panel's four levels, so it is the one system
where four-level content asks the panel for nothing it cannot do.

### One bug found on the way, and it would have poisoned the judgement

The ditherer thresholded against the raw Bayer matrix — a permutation of
0..255 — so `255 > 255` was false and **one pixel in every 16×16 tile of pure
white came out black**: 2250 isolated dots over an 800×720 rect. Pure white is
not a corner case; it is the Game Boy's lightest shade and most HUD text on
every other system. Fixed by scaling the thresholds to 0..254 (`g_thresh` in
`src/video.c`). Left unfixed, "1-bit looks dirty" would have been true for a
reason with nothing to do with motion.

---

## The settle-time model, and the measurement behind it

This is the newest and most consequential measurement in the project, and it
overturned a design assumption.

`koboy-probe --coexist`, five region sizes spanning **49× in area**, affine
fit, reproduced to 0.1% on a re-run thirty seconds later:

| Waveform | Fixed term | Per pixel | At the 800×720 game rect |
|---|---|---|---|
| **DU4** | 15.1 ms | 15.0 ns | **24.1 ms** |
| A2 | 96.4 ms | 17.2 ns | 106.3 ms |
| **DU** | 144.4 ms | 15.8 ns | **153.5 ms** |
| GC16 | 357.7 ms | 22.5 ns | 370.6 ms |
| AUTO (on 1-bit content) | — | — | **153.0 ms**, i.e. DU |

Four findings, in order of how much they change:

**1. Refresh duration is ~94% FIXED in area.** DU moves only 145.1 → 162.3 ms
across that 49× span. The per-pixel term is the same for every waveform
(15–22 ns) because it is the controller's pixel processing rather than
anything about the waveform.

This does **not** mean dirty rectangles were a mistake — the area term is
real, and a smaller rect leaves less of the picture in flight. It means **a
small update is not a *fast* update**, which is the opposite of what the design
assumed.

**2. On 1-bit content, AUTO *is* DU** — identical to within 0.5 ms at three
region sizes. So the `MOTION` ladder's `1-BIT / DU` rung selects the waveform
`1-BIT / AUTO` was already getting, which is exactly why the owner found them
indistinguishable. (`#97`, `#98`)

**3. It prices the 1-bit fix.** Four-level content could use DU4 at 24.1 ms.
Two-level content gets clean transitions and pays 153.5 ms — a factor of
**6.4**. The scroll flashing was that bill arriving.

**4. There is no back-pressure below koboy.** The probe submitted a new
full-rect update every 6–13 ms, without ever blocking, against a 153 ms
completion. **The driver accepts work it cannot do and returns success.** So
over-driving the panel is invisible from inside the process, and pacing has to
be koboy's job.

The synthetic number and the hand judgement agree, which is what makes it
trustworthy: the owner's bracket — `present_divisor = 4` (67 ms) visibly
flashes, `= 8` (134 ms) "flashes much less" — puts a full-rect settle above
134 ms. The probe says 153. That is the exact shape of "8 helped a lot and did
not finish the job".

### What pacing.c does with it

**Presentation is paced by AREA, not by frame count.** `present_divisor` alone
paced a two-tile sprite move and a whole-screen scroll identically, which is
obviously wrong once the settle model exists.

`pacing.c` has **two gates, ANDed**:

- `present_divisor` as a minimum **gap** between presents — a gap and not
  `frames % divisor`, so a settle hold that expires off-lattice does not
  forfeit the rest of the stride;
- an area-scaled **hold**: each presented frame is charged
  `settle_base_ms + settle_full_ms × dirty/whole`, and the next present waits
  that long.

Measured on the device (Sonic Chaos, 900 core frames, 879×576 rect):

| `present_divisor` | `settle_full_ms` | presented | held |
|---|---|---|---|
| 3 | 0 | **235** | 0 |
| 3 | **150** | **112** | 387 |
| 8 | 0 | 90 | 0 |
| 8 | 150 | 89 | 7 |

Read the pairs together. At divisor 3 the panel was being asked for **more
than twice what it can finish** — one update every 64 ms against a 153 ms
settle. And at divisor 8 the throttle is **very nearly inert: 90 → 89**. That
second row is the strongest evidence the model is right and nobody arranged
it: divisor 8 is the setting the owner reached *by eye*, and the model
independently agrees there is nothing left to take away there.

Divisor 3 with the throttle beats the manual divisor 8 outright — 112
presented frames against 90 — because it gives back the small-area frames a
flat divisor throws away.

**Two implementation notes that are load-bearing.**

`settle_base_ms` ships at **0** although the measured fixed term is 144 ms,
and that is deliberate: the model decides when the next update does *visible
harm*, not how long the last one takes. Charging 144 ms to everything pins the
device to 6.5 fps on static screens for no benefit anyone has reported seeing.
(`#100`)

The gate advances in `pacer_tick`, **not** in `pacer_presented`. `main.c`
takes an early exit on an unchanged frame, and a gate that waited on the
presentation would run `video_submit` — the 17 ms bottleneck — on every core
frame of a static screen.

---

## Dirty rectangles

`video.c` diffs its output buffer in 8×8 tiles and refreshes only what
changed. Two things worth knowing.

**It cannot see ghosting.** The diff compares koboy's own buffers; residue is
panel-side. That is why `#25` outlived two attempts and why the 1-bit verdict
had to come from a person looking at the screen. **No `--frames` run can check
whether the flashing is gone.**

**`refresh_fixed_tiles` ships at an untuned guess of 40**, and the reason is a
limit of the instrument rather than laziness. An on-device sweep found 20, 40
and 80 **behaviourally identical on real content** — same rect count, same
frame count. Splitting is measurably cheaper to *submit* with the split off
(368 µs against 488–750 µs), which is mechanical: each extra rect is another
ioctl.

But refreshes are non-blocking by design, so the in-process timer measures
*submission*, not the panel's work — which is precisely the cost the setting
exists to amortise. Measuring the real benefit would need blocking refreshes,
and this device reports `unreliable_wait_for=1`, so those figures are suspect
by construction. (`#24`)

---

## Core selection, and why it is the extension and nothing else

`config_core_for_rom` is a table in `src/config.c`: extension → core `.so` →
scale ceiling. It is a pure function of the ROM's name, called at load time,
because the browser hands `main.c` a path long after the config was read.

**The extension is the only signal, and that constrains what can be claimed.**
Three deliberate refusals, each of which will eventually be asked about:

- **`.bin` is not read as Mega Drive.** Counted across the author's own
  collection rather than argued: 723 TI-99/4A files end `.bin`, 234 Odyssey 2,
  119 Atari 5200, 72 Arcadia 2001, 71 Vectrex, 68 Astrocade, 56 VC 4000, 38
  Jaguar — and 36 Mega Drive. **The Mega Drive is the ninth-largest claimant**,
  and two files ahead of it are the Intellivision BIOS koboy asks the user to
  install by hand. `.gen` is unambiguous but is five files, and is left out to
  keep the rule one a reader can hold: one system, one extension.
- **`.sgx` is refused** because beetle-pce-fast implements neither the second
  VDC nor the priority mixer a SuperGrafx needs. It would load one and render
  it *wrongly* rather than refuse — the failure mode this project treats as
  worse than absence.
- **`.zip` is claimed for arcade outright**, rather than routed by a
  subdirectory convention or by looking the name up in FBNeo's database. The
  alternative is not "less ambiguous", it is "ambiguous plus a second
  mechanism to get wrong": nothing else koboy ships can open a `.zip` at all.

`romlist_is_rom` in `src/romlist.c` is a **separate allowlist** that must stay
in step with that table. It exists because a real collection carries files
that are not games: `.pal` palettes beside NES ROMs, `boot.rom` / `boot1.rom`
beside WonderSwan and Neo Geo Pocket ones, `boot0`–`boot3` beside an
Intellivision one — two of which *are* the BIOS.

`config_min_rom_bytes` is a per-system floor, and it exists because of one
core: **snes9x2005 SIGFPEs on a short `.sfc`/`.smc` rather than refusing it**
(`% Memory.CalculatedSize`, where `CalculatedSize` rounds the file down to
whole 8 KB blocks and is therefore 0). The file that found it was a 212-byte
macOS `._*.smc` stub, the kind every FAT32 card grows. The floor is per-system
because an Atari 2600 cartridge is legitimately 2048 bytes.

### Geometry is DISCOVERED, not queried

`gw-libretro` answers `retro_get_system_av_info` with a **128×128 placeholder**
on every one of 59 titles until its first `retro_run()`, then announces the
real canvas via `SET_GEOMETRY`. Sizing buffers from the post-load query — what
the libretro documentation implies — renders a 973×532 game into a 128×128
box.

So `main.c` polls `core_geometry_changed()` **every frame**. That also covers
a mid-session ROM switch (three titles loaded back-to-back into one core
instance each re-announced) and a PC Engine changing horizontal resolution
mid-game.

And `Donkey Kong Country` is the case the fallback exists for: it sends
512-wide frames and **announces nothing** — `widths` says 256 and 512,
`geom_calls` says 0. A frame between base and max, arriving with no warning,
into a rect sized from base. `video_fit_rect` shrinks a frame the rect cannot
hold; **do not delete that branch.**

---

## The two layouts

`KOBOY_LAYOUT_DMG` draws a procedural faceplate — d-pad lower-left, A/B
lower-right, Start/Select/MENU on a row — with the game in an **integer-scaled**
rect above `chrome_controls_top`.

`KOBOY_LAYOUT_LCD` is a full-width **fractional** fit with a control strip. It
exists because a Game & Watch unit *draws its own buttons* into the frame, so
there is nothing for a faceplate to add. SNES and Mega Drive later moved to it
too, because their pads are A B X Y L R and A B C X Y Z against the
faceplate's two spare pockets — and the move was measured to cost the two
heaviest SNES titles nothing (identical picture size, identical presented-frame
counts, differences inside run-to-run spread).

`koboy_layout`'s `extra[]` holds the faceplate's **optional discs** —
position, button bit and label per slot, `r == 0` meaning empty. A Pokémon
Mini fills one (`C`); a WonderSwan two (`L1`, `R1`); a ColecoVision two (`K1`,
`K2`); an Intellivision two (`KEY`, `TOP`); an arcade board two (`3`, `4`).
**Three consumers guard on `r`** and each has a distinct failure if the guard
goes. The button bit is always the *core's* choice, read off its input
descriptors, never picked here.

Two findings about controls that are not obvious:

**Two systems have a twelve-key keypad the faceplate cannot draw, and on both
of them titles refuse to start without it.** Proved by running, not assumed:
Donkey Kong on ColecoVision sat on its option screen for 900 frames of START
and A, and started the moment `JOYPAD_Y` — Gearcoleco's keypad 1 — was pressed.
Intellivision's route is stranger: FreeIntv puts keypad 1–9 only on the right
analog stick, which koboy has no source for, but it also has a hold-to-show
mini keypad on `JOYPAD_L`, and that makes **all twelve keys reachable**.
Check any new system's real control set against what the faceplate offers
*before* assuming DMG is enough; this has now cost a round on four systems.
(`#49`, `#50`)

**Arcade is the first system where "what does the hardware have" has no single
answer** — it is 227 boards — so the two discs were chosen by **counting every
romset's input descriptors** rather than by reading a control panel. FBNeo's
map is flat (B = Button 1, A = Button 2, Y = 3, X = 4, SELECT = Coin), and the
counts decided it: Y on 134 boards, X on 71, against 45–48 for each shoulder
button there is no room for. Do the same for the next multi-board system.

---

## Per-system scale ceilings, and why they are not tidiness

`g_core_by_ext`'s third column caps the auto-fitted scale. **A bigger picture
costs real, measured speed**, because `video_submit` is paid per destination
pixel and it is the bottleneck on every system.

The story starts with a fix. The reserved rect used to be sized from the
core's **max** geometry, which made "any frame in [1, max] fits" true by
construction — and cost a SNES 54% of its picture against a 512×512 mode
snes9x2005 never enters. Sizing from **base** geometry instead:

| System | Was presented | Now | Area change |
|---|---|---|---|
| Game Boy | 800×720 | 800×720 | unchanged |
| Mega Drive | 878×672 | 1170×896 | 1.78× |
| PC Engine | 583×486 | 875×729 | 2.25× |
| SNES | 597×448 | 1195×896 | **4.00×** |

**PC Engine got 2.25× the picture for free and slightly better than free** —
its old rect reserved 568k pixels to show 283k of picture, and the diff, blit
and refresh all run over the *rect*. **SNES is where the bill landed**, and
only on the two titles with no headroom:

| Title | before | after |
|---|---|---|
| Super Mario World | 98% | **98%** |
| Zelda — A Link to the Past | 97% | **98%** |
| Kirby Super Star (SA-1) | 96% | **78%** |
| Star Fox (SuperFX) | 93% | **67%** |

Hence the ceilings. Four systems carry one and every number is a device
measurement against the 15,024 ms that 900 frames of 59.9227 Hz content takes
in real time — so **ideal ÷ measured *is* the speed**:

| Extension | System | Ceiling | What it bought |
|---|---|---|---|
| `.sfc` `.smc` | SNES | 3 | Star Fox 67% → 79%, Kirby 78% → 95% |
| `.sms` | Master System | 3 | Sonic Chaos 1172×768 83% → 879×576 **98%** |
| `.gg` | Game Gear | 5 | 1152×864 79% → 960×720 **98%** |
| `.md` | Mega Drive | 3 | 1264×966 70% → 879×672 **98%** |
| `.gba` | Game Boy Advance | 4 | see below |
| `.min` | Pokémon Mini | 8 | a 96×64 handheld was filling the panel |

**The three Sega rects were the three largest in the project** — all over 900k
pixels, all reported as slow in play — on frames that are *smaller than the
Game Boy's or the same size*. Turned into pipeline capacity
(`1000 / (submit + blit + refresh)`) against a demand of 29.96 presented
frames a second:

| System | before | after | demand |
|---|---|---|---|
| Master System | 34.1 /s | **58.1 /s** | 29.96 |
| Game Gear | **29.4 /s** | **41.9 /s** | 29.96 |
| Mega Drive | **27.3 /s** | **49.1 /s** | 29.96 |

Two of the three were **literally unable to meet the demand**, which is what
the owner was reporting as slow.

**The Game Gear is the instructive row.** Its frame is 160×144 — byte for byte
the Game Boy's — and `TESTED.md` recorded for months that it "lands on exactly
800×720, the Game Boy's scale-5 picture, by arithmetic, so it needs no
exemption". That was true when written. Then `pixel_aspect` shipped, the
reserved rect became 192 columns wide, the auto-fit went from 5 to 6, and the
picture became 1152×864 — 1.73× the area — **with nothing watching, because
the reasoning lived in a comment rather than in a test.**

**The GBA ceiling is a different kind of thing: it is what makes the system
exist.** A GBA frame is 240×160, the smallest koboy scales, so it auto-fits
furthest — uncapped it takes the LCD strip's full width, 1264×842:

| scale | rect | pipeline | capacity | demand |
|---|---|---|---|---|
| 3 | 720×480 | 14,783 µs | 67.6 /s | 29.96 |
| **4 (shipped)** | **960×640** | **24,852 µs** | **40.2 /s** | 29.96 |
| 6 (uncapped) | 1264×842 | 40,698 µs | **24.6 /s** | 29.96 |

At the device's own `present_divisor = 2`, an uncapped GBA has a per-core-frame
budget that is **negative**: 20.3 ms of presentation charged against a 16.7 ms
frame, before the emulator runs at all.

**Nine systems still auto-fit uncapped and nobody has measured them.** A row in
that table without a number beside it in the comment is a guess and should be
treated as one. (`#73`, `#78`)

---

## Pixel aspect, and the case that justifies it

Most systems have square pixels; four do not. The correction earns itself most
clearly on PC Engine, which changes horizontal resolution **mid-game** —
Military Madness switches five times in 2500 frames.

| | 256-wide mode | 352-wide mode |
|---|---|---|
| pixel aspect | 1.1391 | 0.8284 |
| reserved rect | 1168×486 at x=48 | 850×486 at x=207 |
| **picture presented** | **583×486, centred at x=632** | **583×486, centred at x=632** |

Both modes have the same *display* width, so with the correction on, the
picture is the same size in the same place and only its detail changes. With
`pixel_aspect = false` it jumps between 512×486 and 704×486 at every scene
change. Verified by rendering both sides and looking at them.

**The one system the correction does not save is the Atari 2600, and this is a
real defect.** The 2600's pixels are ~1.6:1, and stella2014 says so the only
way libretro lets it — by declaring `base_width = 160 × 2` while delivering a
160-wide frame. koboy scales squarely, so **every 2600 title renders about
1.75× too tall**: Ms. Pac-Man's maze comes out 480×630 where the correct shape
is about 840×630. Playable, legible, plainly wrong. Found by rendering frames
and looking at them *after every numeric check passed*. Not fixed because the
fix is anisotropic fitting in `video.c`'s hot path — the one presentation
verified on hardware. (`#51`)

---

## Rotation

Three places and nowhere else:

- **`core.c`** records `SET_ROTATION` and **transposes** `core_get_geometry`'s
  answer for an odd rotation, so every consumer sees the picture as
  *presented*;
- **`video.c`** turns the pixels inside the convert pass it was already making;
- **`main.c`** wires the two and re-syncs on every geometry change.

**Answering `true` to `SET_ROTATION` is a promise.** beetle-wswan stops
rotating in software the moment you do.

Arcade is why this exists: FinalBurn Neo renders Galaga into a 288×224
*landscape* buffer and asks the frontend for three quarter turns. Checked
before assuming the core could do it itself — its `fbneo-vertical-mode`
option, whose name promises exactly that, does **not** rotate the framebuffer;
measured across all five settings, the delivered frame stays 288×224 and only
the aspect ratio and the `SET_ROTATION` value move.

The turn costs nothing measurable: rot 3 at 0.449–0.516 ms against rot 0 at
0.444–0.446 ms, same frame, same destination area.

---

## Persistence

Three mechanisms, and they are not interchangeable.

**Cartridge SRAM** (`sram.c`) goes through `RETRO_MEMORY_SAVE_RAM`, written
atomically every ten seconds and on exit. **Save states** (`state.c`) are the
core's own serialised state, three slots per ROM. Both use `safefile.c`:
temp-file → `fsync` → `rename` on the way out, and an **all-or-nothing read**
on the way in.

That read discipline is not theoretical. Loading a truncated `.srm` used to
destroy it further. Now koboy reports it, draws the message on the panel,
leaves the file at its truncated size and disables writeback for the session.
**Proven on the device** on an 8 KB Game Boy save and a 128 KB GBA one.

**A core's answer to "how big is the save" is not stable, and one core makes
that dangerous.** Genesis Plus GX answers `0x10000` before emulation starts
and, once running, "the index of the highest byte that is not `0xFF`, plus
one" — measured across a collection as anything from 285 bytes to 32160, and
**0 for a cartridge nobody has saved on**. So `core_sram` pins the length at
ROM-load time. Without that, one `MENU → LOAD STATE` would have rewritten a
65536-byte `.srm` at 8191 bytes and the next launch would have reported it
corrupt. Checked rather than assumed for the others: snes9x2005's is constant,
and gpSP reports a flat 131072 always.

**A third mechanism exists because some systems have no save RAM at all.** A
Neo Geo Pocket saves into flash and the core writes `<rom>.ngf` itself, into
whatever directory the frontend answers `GET_SAVE_DIRECTORY` with — observed
directly, fourteen titles into an empty directory left twelve files. An arcade
board has no battery at all; FinalBurn Neo's `hiscore.dat` mechanism is what
exists instead, and koboy turns it on.

---

## Things that will look like bugs and are not

A short list, for a reader skimming the source.

- **`cleanup_interval = 0` and `full_refresh_permille = 1000`.** Both
  disabled. They were mitigations for a pipeline that no longer exists and,
  measured, they caused every flash they were added to prevent.
- **`settle_base_ms = 0` against a measured 144 ms fixed term.** Deliberate;
  see the pacing section.
- **`refresh_fixed_tiles = 40`**, an untuned starting guess that the available
  instrument cannot validate.
- **The Atari 2600's A disc is dead.** stella2014 puts fire on `JOYPAD_B`;
  `JOYPAD_A` does something only for a Genesis pad or paddles.
- **`.md` is a ROM extension anchored to `roms/` in `.gitignore`, not
  global.** An unqualified `*.md` would silently stop README.md, TESTED.md and
  all of `docs/` from ever being committed again.
- **`*.pgm` is marked binary in `.gitattributes`.** Without it, git diffs a
  golden image as text and a review balloons to megabytes.
- **The `--ui-script` path skips calibration**, and that is exactly what hid
  the v1 first-run deadlock from twenty reviews. When a code path exists only
  for scripted runs, ask what it is hiding.

---

## Where to look next

`docs/FOLLOWUPS.md`, ordered by what bites first. The largest open items at
the time of writing:

- **`video_submit`** is the bottleneck on every system and nothing has
  optimised it. (`#23`)
- **Nine systems auto-fit with no measured scale ceiling.** (`#73`, `#78`)
- **Every Atari 2600 title is 1.75× too tall.** (`#51`)
- **Save states have never been written and re-read on a device.** (`#76`)
- **Twelve of fifteen systems have never been played by a person.**
