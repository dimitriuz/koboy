# koboy beyond the Game Boy — running other libretro cores

**Status:** design, pre-implementation
**Date:** 2026-08-26
**Depends on:** v2-core (merged, `d957813`), and the device measurements taken
the same day — see `TESTED.md` and `docs/FOLLOWUPS.md` #23, #25–#27.

## 1. Why this document exists, and why its conclusion is not the obvious one

koboy already `dlopen`s a libretro core through a generic contract. Running a
different core is therefore closer to a configuration problem than a rewrite —
with one hardcoded exception, §3.

The obvious way to choose the next system is "what will the CPU handle". **That
is the wrong question on this hardware, and the 2026-08-26 device session proved
it.** Measured per presented frame on a Kobo Libra 2, Zelda at scale 5:

| stage | mean |
|---|---|
| core (emulation) | 2.3 ms |
| **`video_submit`** (LUT, scale, quantise, 8x8 diff) | **17.0 ms** |
| blit | 2.8 ms |
| refresh submission | 0.4–0.75 ms |

Emulation is 10% of the cost. A render-scale sweep pinned `video_submit` as
pixel-bound:

| scale | output px | submit |
|---|---|---|
| 3 | 207,360 | 8,997 µs |
| 4 | 368,640 | 12,462 µs |
| 5 | 576,000 | 16,639 µs |

Fitting: **`submit ≈ 4.7 ms + 20.7 ns/px`**, which predicts the scale-4 point
within 1%. Cost scales with **output pixels**, i.e. native resolution × scale —
not with how demanding the emulated machine is.

And the binding constraint is not speed at all. It is **how much of the screen
changes per frame**: on a horizontal scroll the panel smears into unreadable
horizontal streaks within seconds, identically on v1 and v2 (`docs/FOLLOWUPS.md`
#25). So the right question for a new core is:

> **How much of the screen changes per frame, and how much colour is being
> thrown away by four grey levels?**

That question ranks the candidates very differently from raw performance, and
puts systems nobody would call impressive at the top.

## 2. The panel budget, measured

On the verified Libra 2 (1264x1680) with shipped defaults, `game_y = 84` and
`chrome_controls_top()` returns 1018, leaving a game rect budget of
**1248 x 934 px**. Largest integer scale and the resulting cost, computed against
that budget with the fit above:

| System | native | max scale | output | est. submit |
|---|---|---|---|---|
| Pokémon Mini | 96x64 | 8 | 768x512 | 12.8 ms |
| Atari 2600 | 160x192 | 4 | 640x768 | 14.9 ms |
| **NES** | 256x240 | 3 | 768x720 | **16.1 ms** |
| C64 / ScummVM (VGA) | 320x200 | 3 | 960x600 | 16.6 ms |
| WASM-4 | 160x160 | 5 | 800x800 | 17.9 ms |
| Genesis | 320x224 | 3 | 960x672 | 18.1 ms |
| Master System / ZX Spectrum | 256x192 | 4 | 1024x768 | 21.0 ms |
| WonderSwan | 224x144 | 5 | 1120x720 | 21.4 ms |
| Game Boy / Game Gear | 160x144 | 6 | 960x864 | 21.9 ms |
| Neo Geo Pocket | 160x152 | 6 | 960x912 | 22.8 ms |

**Nothing here is out of reach**, and the spread is under 2x. Scale is a free
parameter: any system can be made cheaper by rendering smaller. Performance is
not the discriminator, which is the point of §1.

*(Game Boy ships at scale 5, not its maximum 6, for the reasons in the v1 spec
§5 — physical size, not capability.)*

## 3. The one real prerequisite: a resolution-agnostic pipeline

`KOBOY_GB_W`/`KOBOY_GB_H` (160x144) are hardcoded in 11 places across
`src/video.c` and `src/config.c`, and `video_submit` contains:

```c
if (src_w != KOBOY_GB_W || src_h != KOBOY_GB_H) return false;
```

**Any other core renders nothing.** This is the shared prerequisite for every
system below and is worth doing on its own merits.

`src/core.c` already binds `retro_get_system_av_info`, so the geometry can be
*asked for* rather than assumed — **but asking at the obvious moment gives the
wrong answer.**

> **MEASURED, 2026-08-26 (`scripts/probe_core.c`), and it overturned this
> section's original premise.** `retro_get_system_av_info` called right after
> `retro_load_game` returns a **128x128 placeholder for every Game & Watch
> title**. The real canvas is announced only from *inside the first
> `retro_run()`*, via `SET_GEOMETRY` / `SET_SYSTEM_AV_INFO`. A pipeline sized
> from the post-load query — which is what the libretro docs imply, and what
> the first draft of this section specified — renders Mario Bros., a 973x532
> game, into a 128x128 box.
>
> This is not a quirk to route around; it is the contract. Geometry is
> **discovered, not queried**, and any core added later must be assumed to
> behave this way until `probe_core` says otherwise.

The work, as corrected:

- Handle `SET_GEOMETRY` **and** `SET_SYSTEM_AV_INFO` in `core.c`'s environment
  callback and keep geometry live for the ROM's lifetime; treat the
  `video_refresh` callback's per-frame width/height as the reliable source.
- Expose a `core_geometry_changed()` edge so the main loop can re-fit chrome
  and video mid-session rather than only at load.
- Allocate `video`'s intermediate buffer at `max_*`, not at `base_*` and not at
  a constant. (Sizing from `base_*` is a heap overflow, not a cosmetic bug —
  it was mutant-verified as one.)
- Derive scale from the actual resolution in `config_resolve_profile`, keeping
  the existing rule that the rect must clear `chrome_controls_top()` — the
  guard that already stopped a game rect covering a live touch zone once.
- Guard degenerate geometry (a zero dimension is a SIGFPE in the scale search;
  also mutant-verified).

**Risk to respect:** the scale-5-on-all-four-panels property (`tests/test_config.c`)
was broken once already, by a chrome change that reserved a band. Any change to
scale resolution must keep that sweep green and extend it to the new systems.

## 4. Per-system assessment

Ranked by **fit for this panel**, not by capability.

### Tier 1 — the panel's weakness disappears

**Game & Watch** (`gw`, MAME `hh_sm510`). LCD segment games: the background is
a fixed printed artwork, only a handful of segments toggle per frame, and the
output is natively black-on-grey. There is **no scrolling at all**, so #25 —
the defect that makes scrolling platformers unplayable — simply does not
apply. The 8x8 dirty-tile pass, which measured no benefit on Darkwing Duck
(#27), is exactly the workload it was designed for.
**MEASURED, 2026-08-26**, against a real 59-title `.mgw` collection. The format
is bzip2 over a tar-like container of Madrigal BASIC scripts (`main.bs`,
`unit1.bs`), RLE artwork (`im_background*.rle`, `im_number_*`, `btn_*`) and PCM
audio. Artwork dimensions are a big-endian `u16` pair at the head of each RLE
entry, and they are **smaller than feared**:

| Type | Example | Artwork | submit @1x |
|---|---|---|---|
| Wide Screen | Parachute | 658x395 | 10.1 ms |
| Table Top | Snoopy | 443x743 + 320x165 | 12.6 ms |
| Multi Screen | Mario Bros. | 473x532 + 973x532 | 20.6 ms |
| Panorama | Donkey Kong Jr. | 499x771 + 499x456 + 356x190 | 18.8 ms |

Across the whole set the *artwork entries* run width 431–692, height 322–759.
What matters, though, is the **composited canvas the core actually reports**,
which is larger because it stitches a Multi Screen's two LCDs into one
framebuffer: **width 480–1073, height 312–777** (Parachute 658x395, Mario
Bros. 973x532, Donkey Kong 606x748 — each matching its artwork exactly once
composited). Size buffers against the reported canvas, never against the
container's artwork entries.
**All 59 fit the §2 budget at 1:1, costing 8.7–21 ms — at or below the Game
Boy's current 16.6 ms.**

Three consequences worth deciding before implementing:

- **Integer scaling is awkward here.** The artwork is too large to double
  within the game-rect budget, so every title is stuck at 1x and occupies only
  35–77% of the panel width. Doubling is possible for the *portrait* titles only
  if the drawn faceplate is dropped (see below) — but at 32–42 ms of submit,
  double the Game Boy's cost. 1x is the practical answer.
- **The artwork already contains the device's own buttons.** koboy drawing its
  DMG faceplate *around* a picture of a Game & Watch is redundant and slightly
  absurd. The better design is chrome-less for this core, artwork centred, with
  touch zones mapped onto the buttons drawn in the artwork itself. That also
  frees the vertical budget.
- **The artwork is colour** (a scan of the physical unit), so the surround
  becomes greyscale. The gameplay elements — the LCD segments — are
  black-on-grey and unaffected, which is the half that matters.

Also note the PCM entries: these games are substantially about their beeps, and
koboy has no audio. Worth saying out loud rather than discovering.

**ScummVM.** Point-and-click adventures are static screens with occasional
animation, and the native input is a **pointer** — which this device has, and
which the touch d-pad has been fighting to emulate all along. Reading-heavy,
slow-paced, high dwell time per screen: the case e-ink is *for*. This is the
only candidate where the e-reader is a better host than a handheld, not a
worse one.

**WASM-4.** A fantasy console with a **four-colour palette**, mapping to the
four grey levels with no loss whatsoever. Small library; near-perfect technical
fit.

### Tier 2 — good fit, well understood

- **Pokémon Mini** — 96x64, genuinely monochrome, puzzle-heavy, cheapest in §2.
- **ZX Spectrum**, specifically its *flick-screen* classics (Manic Miner, Jet
  Set Willy, Head Over Heels): screen-at-a-time transitions rather than
  scrolling, and high-contrast attribute art. Z80 is nothing for an A9.
- **NES** — the large-library option, comfortable at scale 3. Puzzle and RPG
  titles will be excellent; scrolling platformers will smear exactly as
  Darkwing Duck does.
- **Neo Geo Pocket** and **WonderSwan** — both have genuinely monochrome
  modes, so no colour is discarded.

### Tier 3 — works, but fights the panel

- **C64** — joystick-first library (easier input than Spectrum), but
  colour-dependent art loses more in four greys, and multi-load disk images are
  fiddly.
- **Master System / Game Gear** — cheap and easy, but colour-heavy action.
- **Atari 2600** — trivial CPU, but twitch-and-scroll gameplay is the failure
  case.
- **Genesis** — probably runs; the *worst* fit on this list. Fast scrolling
  action, colour-dependent art.

### Out of scope, unchanged from v1

**GBA** and **SNES**, on the CPU reasoning in the v1 spec §1. Nothing measured
since contradicts it.

## 5. The real blocker is input, not performance

- **Game & Watch** wants two buttons. Already covered.
- **NES / SMS / Genesis / C64** want a d-pad and 2–3 buttons. Covered by the
  touch layout and, since 2026-08-26, by a Bluetooth gamepad.
- **ScummVM** wants a pointer — covered by the touchscreen, which is the
  correct input rather than a compromise — plus a few buttons.
- **ZX Spectrum and C64 keyboard games** want a keyboard. An on-screen keyboard
  on a panel presenting at 7–11 fps is a poor experience and real work. **Scope
  these to joystick-compatible (Kempston) titles initially**, and say so in the
  docs rather than shipping a keyboard nobody enjoys.

## 6. Core build requirements

Every core must clear the same bar gambatte does (`scripts/build-core.sh` is the
working template):

- Cross-compiled for **glibc 2.19**, `-march=armv7-a -mfpu=neon -mfloat-abi=hard`.
- `-static-libstdc++ -static-libgcc` so the device's C++ runtime is irrelevant.
- Must pass `scripts/verify-core.sh`: closure limited to the allowlist
  (`libc`/`libm`/`libdl`/`libpthread`/`libgcc_s`/`ld-linux-armhf`).
- koboy's own closure must remain `libdl.so.2` + `libm.so.6` + `libc.so.6`.

Cores are `dlopen`ed, so multiple can ship side by side; `core_path` already
selects one, and the ROM browser could infer it from the file extension.

## 7. Build order

1. ~~**Resolution-agnostic pipeline** (§3)~~ — **done, 554cc34.** Verified
   against the real core rather than this document's numbers, which is how §3's
   premise was caught. `make test` 1415 checks; the scale-5-on-all-four-panels
   sweep stayed green.
2. **One Tier-1 core end to end** — Game & Watch is the recommendation, because
   it is the only candidate where #25 does not apply, so it proves the device
   can be *good* at something rather than merely adequate.
3. **Core selection by ROM extension** in the browser, once more than one ships.
4. **NES**, as the large-library case.
5. ScummVM, which needs pointer-input work and is its own project.

## 8. Open questions

- ~~Game & Watch artwork resolution against §2's budget~~ — **measured, see
  §4 Tier 1.** All 59 titles fit at 1:1 for 8.7–21 ms. Remaining sub-question:
  whether to drop the drawn faceplate for this core and map touch onto the
  artwork's own buttons.
- ~~Whether `SET_GEOMETRY` mid-run needs honouring, or load-time resolution is
  enough~~ — **answered, and not by choice: load-time resolution is not
  available.** See §3. `SET_GEOMETRY`/`SET_SYSTEM_AV_INFO` handling is
  mandatory for this core, and is implemented (554cc34). Still unverified on a
  *live* re-announcement: a Multi Screen title that toggles screens mid-session
  exercises the path only against the stub, never a real title.
- Whether four grey levels are usable for colour systems in practice. `video.c`
  ships `video_dither_1bit` and a 4-level quantiser; **no colour system has ever
  been rendered on this device**, so the quality is genuinely unknown, and
  `force_dither` has never been evaluated on a real panel.
- Whether a per-core config profile is needed (scale, dither, waveform) or
  whether the existing ini suffices.
