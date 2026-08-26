# koboy — known follow-ups after v1

Everything here was found by review during v1, judged real, and deliberately
deferred rather than forgotten. None is a known live bug: each is either a
coverage gap on code that was hand-verified, or a cheap hardening/doc fix. They
are recorded because the review workspace they came from is scratch and gets
deleted, and a finding nobody wrote down is a finding you pay for twice.

Ordered by what would actually bite first.

## Test coverage on code real users depend on

1. **`dpad_mode = cross` is the shipped default and its distinguishing
   behaviour is untested.** `src/config.c:63`, `src/input.c:116`. Every existing
   touch test happens to touch down exactly at the pad's centre, where CROSS and
   RELATIVE are indistinguishable — so the one thing that makes CROSS different
   (a fixed origin regardless of where the finger lands) has no automated
   coverage. The logic is one conditional and is not observed broken, but this
   is the control scheme every user gets by default. **Highest value item here.**

2. **`flip_x` / `flip_y` touch mirroring is never exercised.** `src/input.c:59-60`;
   no caller of `input_set_touch_transform` passes `true`. `platform_kobo.c:715-717`
   wires real per-device probe data into it, so any Kobo needing a mirrored touch
   axis depends on an untested path. Irrelevant on the verified Libra 2,
   load-bearing on hardware nobody has tried.

3. ~~**The save path is unexercised on hardware.**~~ CLOSED, 2026-08-26 device
   session. `Legend of Zelda, The - Link's Awakening (USA, Europe) (Rev 2).gb`
   (cart type `0x03`, MBC1+RAM+BATTERY) is the first title this project has
   run with `rambanks: 1` — both Tetris and Darkwing Duck report `rambanks:
   0`. All three directions verified on the Libra 2, binary run directly
   with `--frames` over ssh (Nickel not stopped — see the caveat on this in
   `TESTED.md`, but `sram_save`/`sram_load` and the panel draw do not depend
   on the input grab):
   - **Write:** `sram_save` produced `saves/zelda.srm` at exactly 8192 bytes.
   - **Read:** a marked save round-tripped intact — md5
     `daa6696c5da463305bdec570cdad2a82` identical before and after a run,
     `koboy: loaded .../zelda.srm` in the log.
   - **The destructive path:** the `.srm` truncated to 100 bytes reproduced
     the exact failure `src/main.c:650`'s comment describes — "could not be
     read whole; SRAM left as the core initialised it and saving is disabled
     this session" — drawn on the panel, and the file was **left at 100
     bytes** rather than being overwritten with a fresh/short save. This is
     the truncated-save-destroys-itself bug the comment records, now proven
     fixed on real hardware rather than by inspection.
   Save **states** (`state.c`/`safefile.c` — a different mechanism from
   cartridge SRAM) are not covered by this and remain untested on hardware —
   this session drove the ROM browser via `--ui-script` only (item 18 covers
   `MODE_MENU`'s wider coverage gap, which save/load-state goes through).

4. **`force_dither = true` never runs end-to-end.** `tests/test_video_pipeline.c:19`,
   `src/video.c:191-195`. The dither component is well tested directly; the
   `if (v->dither)` wiring a GBC user would hit is not.

5. **ROM failure paths are inspection-verified only.** `src/core.c:180-215`. No
   test deletes the ROM, denies read, or truncates it — all user-reachable.

6. **The ini-preservation test doesn't exercise its filter.** `tests/test_config.c:121-134`
   seeds a key literally named `old key_a`, which the exact-strcmp filter never
   matches, so it is preserved trivially. Rename the seed key to close it.

7. **Stub-core observation flags are dlsym-able**, so the gdb-only check in
   `tests/test_core.c` could become a real assertion.

## Cheap hardening

8. **`as_bool` treats everything except `false`/`0` as true**, including the
   empty string. `src/config.c:187`. A blanked `grab_input = ` silently becomes
   `true`.

9. **A missing trailing newline in the ini concatenates the calibration block
   onto the last line.** `src/config.c:296-324`. Harmless today because
   `config_load` truncates at the resulting `#`, but it silently rewrites an
   unrelated line, against the "preserve everything else" intent.

10. **`verify-core.sh`'s allowlist is unanchored.** `scripts/verify-core.sh:31`:
    a library named `reallibc.so.6` would pass by substring. Build-time script,
    not attacker-facing, but anchors are free.

11. **`xdlsym`'s error omits the `.so` path** where its sibling `dlopen` error
    includes it. `src/core.c:102-113`. On a device where a photo of the panel is
    the only diagnostic, that path is the useful half.

12. **`g_bayer` lazy init assumes single-threaded.** `src/video.c:56-57, 78-81`.
    True today (no threads anywhere in `src/`); wants a comment saying so.

## Documentation and structure

13. **`config/koboy.ini`'s `rom` and `save_dir` comments** don't mention
    install-relative resolution, though `config_resolve_paths` applies the same
    rule to all three paths and `core`'s comment does explain it. Lines 82-83, 92.

14. **`platform_kobo_*` prototypes are duplicated** between `src/main.c:36-40`
    and `src/platform_kobo.c:656-661` with no shared header. Currently
    consistent; nothing would catch future drift. A small `platform_kobo.h`.

15. **`docs/probe-readme.md` never names `make probe-dist`** (`Makefile:121-129`),
    the lightweight target that builds only the probe. A contributor reading the
    docs would run the heavier full `make dist`.

16. **`video_scale_gray` has undocumented preconditions** (`scale >= 1`,
    `dst_stride >= src_w * scale`). `src/video.c:31-50`. Its only caller
    satisfies them by construction.

## v2 follow-ups

Found during the v2-core plan (ROM browser, in-game MENU, save states,
multi-rect dirty regions, the redrawn faceplate). Same rule as above: real,
deliberately deferred, not a known live bug.

17. ~~**A `--ui-script` whose first verb is `tap` selects nothing.**~~ FIXED.
    `src/ui.c:32` initialises a freshly built list's `prev_touch = true` so it
    requires one release before it will accept a tap -- deliberate, to survive
    a still-down finger chained across screens (see the comment there). A
    script that opened with `tap X Y` therefore had its press swallowed as
    "already held" and its release eaten as the priming edge, so the run
    exhausted and returned "no selection" exactly as if the user had quit --
    confirmed on hardware, `printf 'tap 300 300\n'` printed "no rom chosen"
    and exited **0**. `run_list`'s scripted branch now feeds one released
    state before the script's first entry, so a script is robust whatever its
    opening verb, and a scripted run that selected nothing exits 4 instead of
    0. `tests/smoke_host.sh` drives a deliberately tap-first script.

18. **`MODE_MENU`'s interactive branches are verified by construction, not by
    an executed test.** `src/main.c:647` (the `input_take_menu_request`
    branch in the emulator loop). `--ui-script` drives `run_list` only in
    `MODE_BROWSE` (see `run_list`'s own comment on why); `run_menu` is never
    passed a script -- its one call site, `src/main.c:691`, passes `NULL, 0`
    -- so the emulator loop itself never accepts scripted input, and
    SAVE/LOAD/RESET/CHOOSE ROM/QUIT are exercised by inspection and by hand,
    never by `make test`. Extending `--ui-script` through the emulator loop is
    the obvious follow-up; #17's prerequisite is now cleared. `src/uiscript.h`
    and `run_list`'s comment no longer claim MODE_MENU coverage they do not
    have.

19. **The d-pad horizontal-arm term in `chrome_controls_top` is provably
    dead.** `src/chrome.c:41-42`. `top = min2(top, dcy - arm/2 - 1)` can never
    win against the line immediately before it (`dcy - dr - 1`): with
    `arm = dr/3`, `dr > arm/2` for every `dr > 0` this layout ever produces,
    so the vertical-arm term is always the smaller of the two. The equality
    check in `tests/test_chrome.c` does not catch this, because the
    function's return value is identical whether the line is there or not --
    only a term-by-term audit finds it.

20. ~~**The speaker grille overdraws its right margin by 1px, on three of the
    four tested panel sizes.**~~ FIXED, task 15, then the grille itself was
    REMOVED, task 16 -- CLOSED as "removed, not fixed". `src/chrome.c`, the
    grille's slash loop. Each slash was drawn with `hline(..., glx0+s,
    glx0+s+1, ...)` -- two columns per step -- and the length clamp (`if
    (glx0+len > gx1) len = gx1-glx0`) only fired when the *unclamped* last
    column would exceed `gx1`. When it landed exactly on `gx1` instead, the
    clamp never triggered and the two-wide `hline`'s second column painted at
    `gx1`, one column into the margin `gx1` exists to keep clear. The clamp
    was widened to reserve the inclusive second column too (`if
    (glx0+len+1 > gx1) len = gx1-glx0-1`), and `tests/test_chrome.c` asserted
    the margin directly (swept over all four supported panels) with a
    verified mutant -- see the task 15 report. The grille itself also moved,
    lower-right below the A/B cluster, matching the reference photo. Task 16
    then removed the grille from the faceplate entirely at the user's
    request (a purely aesthetic simplification -- it reclaims no vertical
    space, since the grille's placement was independent of
    `chrome_controls_top`), which took this fix's own code and its margin
    test out with it. Left in the record rather than deleted, because the
    inclusive-endpoint clamp bug it documents is a real class of mistake
    (`hline`'s two-columns-per-call convention) that the next feature to use
    the same pattern can still walk into.

21. **`video_split_dirty`'s overflow fallbacks are untested.**
    `src/video.c:245` (tile grid larger than `KOBOY_SPLIT_MAX_TILES`) and
    `src/video.c:304` (more band/column candidates than
    `KOBOY_SPLIT_MAX_CANDIDATES`) both degrade to the single merged rect and
    are safe by construction, but nothing in `tests/test_video_multirect.c`
    (or anywhere else) constructs a dirty pattern large or pathological
    enough to actually reach either branch.

22. **Emitted rects may partially overlap once the candidate list is capped.**
    `src/video.c:313-323`. The merge-to-cap loop only removes a candidate that
    ends up *fully contained* in another (documented in place as deliberate --
    "does not attempt general deoverlap"); two capped rects can still
    partially overlap, and every downstream consumer (`blit_gray8`, `refresh`)
    redoes that overlap's area twice. Coverage is unaffected -- a union only
    grows -- so this is a cost, not a correctness bug.

23. **`video_submit` is the real optimisation target, not "presentation."**
    2026-08-26 device session, Zelda at scale 5, `present_divisor = 3`,
    per-stage means: core 2.3 ms, **submit 17.0 ms**, blit 2.8 ms, refresh
    0.4-0.75 ms (max 29.2 ms, the fixed cost + unreliable-timing spread
    `TESTED.md` already documents). `video_submit` -- the RGB565->gray LUT,
    integer scale, quantise and 8x8-tile diff, `src/video.c` -- dominates the
    other three stages combined by roughly 5x. That contradicts the v1
    design spec §5's stated premise ("Emulation is cheap; presentation is
    the entire bottleneck"): `video_submit` is neither emulation nor
    presentation (panel refresh) in that dichotomy, it is the pixel pipeline
    sitting between them, and it is the bottleneck. Confirmed pixel-bound by
    a render-scale sweep (submit time only): scale 3 / 207,360 px / 8,997
    µs, scale 4 / 368,640 px / 12,462 µs, scale 5 / 576,000 px / 16,639 µs --
    linear fit `submit ~= 4.7 ms + 20.7 ns/px` predicts the scale-4 point
    within 1%. v2-core's multi-rect work (§7 of the v2 design spec) reduces
    `refresh`, which this measurement shows is already the cheapest of the
    four stages -- the optimisation that would actually move the needle is
    in `video_submit`'s per-pixel work, unstarted.

24. **`refresh_fixed_tiles` tuning (20 vs 40 vs 80 vs split-off) is
    inconclusive by construction, not just unmeasured.** 2026-08-26 device
    session, same Zelda run, `--frames 900`: 20/40/80 all produced the same
    339 rects over 292 frames (604 / 750 / 488 µs mean `refresh`) --
    behaviourally identical on real content, exactly as a host reviewer
    predicted from the code before any device was available. Splitting off
    entirely (100000) dropped to 292 rects / 368 µs, which is the expected
    mechanical cost of one ioctl per extra rect, not evidence against
    splitting. The reason none of this settles the tuning question: refresh
    submission is non-blocking by design (see "What the hardware overruled"
    in `CLAUDE.md`), so the in-process `refresh` timer measures submission
    only, never the panel's actual asynchronous work -- which is what the
    fixed-cost-per-rect model in the v2 design spec §7 is actually trying to
    amortise. Measuring the real benefit needs blocking refreshes, and this
    device reports `unreliable_wait_for=1`, which applies to exactly the
    ioctl a blocking measurement waits on -- so those figures would be
    suspect by construction too. `refresh_fixed_tiles` stays shipped at 40
    (the untuned starting guess) with this recorded as a limit of the
    measurement method, not a verdict on the split heuristic.

---

## v2-core, open after the 2026-08-26 play session

### 25. Full-screen scrollers smear, on v1 and v2 alike — the biggest open problem

**Not a regression.** The player confirms v1 looked the same. On a horizontal
scroll the picture degrades into heavy horizontal streaking within a few
seconds: a fast non-erasing waveform draws each frame's background without
clearing the last, and because a scroll offsets that background horizontally
every frame, successive frames superimpose.

`waveform_fast = auto` solved this for Tetris — small changed region, controller
picks an erasing waveform. It does not solve it for a scroller, where nearly the
whole rect changes every frame and the controller still picks a fast one.

The lever is `full_refresh_permille`, which promotes a frame to a flashing
erasing refresh once the changed area crosses a threshold. Its shipped value of
**1000 was tuned on Tetris, where it essentially never fires**, and that value
was carried into v2 unexamined. Untested candidates, in order: lower the
threshold (400 was tried, result unrecorded here); re-enable `cleanup_interval`;
or accept the tradeoff and document scrollers as out of scope.

Whatever the answer, it is a **policy** question about when to spend a flash,
not a bug — and the v1 design spec predicted it: "Full-screen scrollers get no
benefit and hit the worst case."

### 26. `present_divisor` trades frame rate directly against smearing

Measured 2026-08-26 on Darkwing Duck, 600 core frames, wall clock 10.24 s in
every case (the core holds 60 Hz regardless):

| `present_divisor` | presented | fps |
|---|---|---|
| 3 (shipped) | 76 | 7.4 |
| 2 | 102 | 10.0 |
| 1 | 115 | 11.2 |

Lowering it is free in emulation speed but costs *smearing*: more partial
updates per second means residue accumulates proportionally faster. Tried at 2
during the session and the player reported it looked the same or worse. Any
future tuning of this must be judged on the panel, not on the frame counter.

### 27. Multi-rect splitting shows no measurable benefit on real content

Six device runs, splitting on versus off, produced **identical** presented-frame
counts at every `present_divisor` (76/76, 102/102, 115/115), with rect counts
differing by two. Combined with #23 — `refresh` is ~0.4 ms of a ~23 ms frame
while `video_submit` is ~16 ms — v2-core's multi-rect work optimised a stage
that was never the constraint. Consider defaulting `refresh_fixed_tiles` to a
value that disables splitting until a workload is found where it pays.

## Game & Watch (multi-system, 2026-08-26)

### 28. ~~Nothing about Game & Watch has run on the device~~ -- CLOSED 2026-08-26

Verified by the device owner: a title launched from the browser, rendered at
full panel width, and was playable. See `TESTED.md` for what that does and
does not establish (one title, not all 59). Original text follows.


The core cross-builds, ships in `dist/`, passes `verify-core.sh` with a
closure of `libm` + `libc` only, and the browser lists and loads `.mgw`
end-to-end on the host against real titles. **None of that is a device
run.** Needed: one NickelMenu launch that opens a `.mgw`, to confirm the
canvas lands on the panel at the right size and that the printed artwork
survives four-level quantisation in a way that is actually readable.

The rendered host output (Parachute, Mario Bros., Donkey Kong Circus) looks
good in four greys, which is evidence but not proof: the host renders to a
PNG, not to an e-ink panel with its own contrast curve.

### 29. `present_divisor` may want to be per-core

G&W has **no scrolling**, so #25's smearing cannot occur and #26's
divisor/smearing tradeoff has nothing to trade. The shipped `3` was chosen
against a scrolling platformer; for a segment-LCD game where only a handful
of tiles change per frame it is probably too conservative. This is a config
question, not a code one, until someone measures it on the panel.

Do NOT reason about this from a G&W-vs-Game-Boy pixel-count comparison
against 160x144 -- see the correction in #30.

### 30. The G&W cost comparison that looks alarming and is wrong

`video_submit` scales with **destination** pixels (~4.7 ms + 20.7 ns/px, #23).
The Game Boy is upscaled 160x144 -> 800x720 = 576k px = 16.6 ms. G&W renders
at 1x, so:

| Title | Canvas | dst px | est. submit | vs Game Boy |
|---|---|---|---|---|
| Parachute | 658x395 | 260k | 10.1 ms | 0.45x |
| Donkey Kong Circus | 498x771 | 384k | 12.6 ms | 0.67x |
| Mario Bros. | 973x532 | 518k | 15.4 ms | 0.90x |
| upper bound | 1073x777 | 834k | 22.0 ms | 1.45x |

Typical G&W titles are **cheaper** than the Game Boy, not 9-20x more
expensive. Comparing a G&W canvas to the Game Boy's 160x144 *source* is what
produces the alarming number, and the Game Boy never renders at 160x144.

Estimates, not measurements -- the constant comes from a host-era sweep and
every absolute timing this project has taken moved by up to 2.2x between
sessions.

### 31. RECENT can show two identically-named rows

Folder navigation strips the folder prefix from a row's DISPLAY label. Paths
are unchanged and existing `recent.dat` rows are untouched, but two ROMs with
the same filename in different folders now render as the same row in RECENT.
Harmless until it happens; the fix is to show the folder on the row when a
duplicate label exists, not to put the prefix back on every row.

### 32. `present_divisor` may want to be per-system, not per-config

Filling the panel roughly doubled `submit` (see TESTED.md's LCD table), and
Game & Watch has no scrolling, so #26's divisor-versus-smearing tradeoff --
measured on a scrolling platformer -- does not apply to it at all. The
shipped `3` was chosen against Darkwing Duck. A per-layout default would let
Game & Watch present more often without touching the Game Boy's tuning.
Config question, not a code one, until someone reports a title feeling slow.

## NES and Pokemon Mini (added 2026-08-26)

### 33. ~~(closed 2026-08-26: both run on the device, see TESTED.md)~~ -- NEITHER NEW SYSTEM HAS RUN ON HARDWARE

The highest-value item in this file. Everything about fceumm and PokeMini in
this repo was measured on the x86_64 host with `scripts/probe_core.c` and a
throwaway frame dumper; the ARM cores were cross-built and put through
`scripts/verify-core.sh` (both pass with libm+libc alone), and nothing else.
Not established on the device:

| | |
|---|---|
| The ARM cores `dlopen` at all | not established |
| Either system renders on the panel | not established |
| Playable speed | not established -- see #34 |
| The C button is reachable by a real finger | not established |
| A NES battery save survives a real session | not established (the path is verified end to end on the host, and the same path was verified on the device for a Game Boy cartridge) |

A `--frames` run over ssh, as the 2026-08-26 session did for the Game Boy,
would settle the first three cheaply and without stopping Nickel.

### 34. NES may not be fast enough, and the shipped scale is not the tuned one

At the shipped `scale = 5` a 256x240 NES frame does not fit the Libra 2's
1264x1680 panel, so the resolver demotes to 3: a 768x720 rect, 553k
destination pixels, ~16.1 ms of `video_submit` by #23's model -- almost
exactly the Game Boy's 16.6 ms. On the larger Elipsa/Sage panels it holds
scale 4: 1024x960, 983k px, ~25 ms. So the presentation cost is in family,
but fceumm's own emulation cost against gambatte's 2.3 ms is UNMEASURED, and
the NES is a considerably bigger machine. `present_divisor` may want to move
for it (#32 is the same question for Game & Watch).

### 35. Pokemon Mini renders a postage stamp at the shipped scale

96x64 at `scale = 5` is a 480x320 rect -- 38% of the panel width, and only
154k destination pixels (~7.9 ms, by far the cheapest system koboy runs).
`scale = 0` would auto-fit it to 13x, 1248x832, ~26 ms. Neither number is
wrong; the shipped one is simply tuned for the Game Boy. A per-system scale
default is the real answer and it is the same shape of question as #32.
Until then the owner should set `scale = 0` in `koboy.ini` when playing
Pokemon Mini, and the resolver will still demote for every other system.

### 36. `[BIOS] ....min` is listed in the browser as a selectable game

The Pokemon Mini core links its own free BIOS and needs no dumped one
(verified against an empty system directory), so the `[BIOS] Nintendo Pokemon
Mini (World).min` that ships in a normal collection is inert -- but it is a
`.min`, so `romlist_is_rom` lists it. Deliberately not filtered: that
predicate is an allowlist of EXTENSIONS, and a name-prefix rule would also
hide a homebrew named that way and would be a second, invisible rule for a
user to discover when their file vanished. Revisit only if someone actually
selects it and is confused by what happens.

### 37. `.fds` is accepted by fceumm and not listed by koboy

fceumm's `valid_extensions` is `fds|nes|unf|unif`, and a real collection has
a `3 Famicom Disk System/` folder in it. koboy does not list `.fds`, because
the FDS needs `disksys.rom` in a system directory koboy has no concept of --
measured: an `.fds` fails `retro_load_game` outright with an empty system
directory. Supporting it means giving koboy a system directory, which is a
design decision, not an extension entry.

### 38. Frame pacing is still the Game Boy's for every system

`KOBOY_FRAME_US` is 16742 us (59.7275 Hz). fceumm reports 60.0998 fps and
PokeMini reports 72. Nothing reads `retro_system_timing` (core.c says so at
the SET_SYSTEM_AV_INFO handler). With `present_divisor = 3` on an e-ink panel
this is very unlikely to be visible, but it is now wrong for three of the
four cores rather than one.

### 39. `ROMLIST_NAME` is 128 and a real NES collection overflows it

Found by pointing the browser at the owner's actual `NES/` directory: one
file is skipped and reported as "1 entry not shown", because ROM names in a
translated NES set run long --
`Go Go! Nekketu Hockey Club - Multi-Sport Battle (Japan) [T-En by
Disconnected Translations v0.99] [Add by GAFF Translations v1.00] [n].nes`
is 138 characters. The cap behaves exactly as designed (a name that would
truncate is skipped, and the count is shown rather than swallowed), so this
is a size question, not a bug: 128 was chosen when "Tetris (World).gb" was
the shape of a filename. A dirent name is at most 255 bytes, and the row is
elided for display anyway, so 256 would cost 128 bytes per entry -- 2.5 MB
across a 20000-ROM hard cap, which is the real reason to think about it
rather than just raising it.

## WonderSwan and Neo Geo Pocket (added 2026-08-26)

### 40. NEITHER NEW SYSTEM HAS RUN ON HARDWARE

The highest-value item in this file, and the same shape as #33 was for NES
and Pokemon Mini. Everything about beetle-wswan and RACE in this repo was
measured on the x86_64 host -- `scripts/probe_core.c`, plus a throwaway
harness driving koboy's own `config.c`/`video.c`/`chrome.c` and dumping
panel-sized PGMs. The ARM cores were cross-built and put through
`scripts/verify-core.sh` (both pass with `libc.so.6` alone, the smallest
closure of any core this project ships), and nothing else.

| | |
|---|---|
| The ARM cores `dlopen` at all | not established |
| Either system renders on the panel | not established |
| Playable speed | not established -- see #41 |
| The L1/R1 discs are reachable by a real finger | not established |
| A WonderSwan `.srm` survives a real session | not established (the path is the one verified on-device for a Game Boy cartridge) |
| A Neo Geo Pocket `.ngf` is written at all on the device | not established -- and it is a DIFFERENT path from every other system's, see #44 |

A `--frames` run over ssh, as the 2026-08-26 session did for the Game Boy,
settles the first three cheaply and without stopping Nickel.

### 41. Neo Geo Pocket is now the most expensive thing koboy renders

160x152 auto-fits to scale 6 on the Libra 2: a 960x912 rect, 875k destination
pixels, ~22.8 ms of `video_submit` by #23's model -- against the Game Boy's
measured 17.0 ms. On a Sage (1440x1920) it takes scale 7: 1120x1064, 1.19M
px, ~29.4 ms. Both are estimates from a linear model fitted on one device,
not measurements, and `present_divisor = 3` may absorb the difference
entirely. But it is the first system whose auto-fit lands materially ABOVE
the Game Boy's tuned cost, and if anything feels slow this is where to look
first. `scale = 5` in `koboy.ini` brings it back to 800x760 / 17.3 ms.

### 42. Two WonderSwan buttons are still unreachable in portrait

`wswan_rotate_keymap = auto` remaps the retropad when a title is rotated.
The faceplate now covers the d-pad (Y cursor), START, SELECT, the two
X-cursor buttons that land on `JOYPAD_A`/`JOYPAD_B`, and -- via the new L1/R1
discs -- the console's own A and B on `JOYPAD_L`/`JOYPAD_R`. What is left out
is the X cursor's UP and RIGHT, which land on `JOYPAD_Y` and `JOYPAD_X`. The
DMG faceplate has no X or Y disc and `KOBOY_MAX_EXTRA_BTNS` is 2 because that
is what fits between the d-pad and the B button without moving anything.

No title measured so far needs them -- GunPey's portrait mode answers
`JOYPAD_A`, Klonoa's answers `JOYPAD_L` -- so this is filed rather than
fixed. If a report arrives, the options are a third and fourth slot (needing
somewhere to put them) or a MENU toggle that remaps the two drawn face discs.

### 43. ~~Colour on four greys: blue skies go black~~ -- CLOSED 2026-08-26

Fixed in `src/video.c`: the greyscale mapping is now selectable
(`koboy_gray_map`, five entries), the default is no longer Rec.601, and it is
reachable from `koboy.ini` (`gray_map`) and from the in-game MENU
(`MENU -> GREYSCALE`, which cycles and writes the key back).

Two things the diagnosis below got wrong, both found by measuring rather than
reasoning, and both worth keeping:

- **Equal weights `(R+G+B)/3` are not the fix.** Over 38 gameplay frames from
  19 colour titles they crush MORE pixels to black than Rec.601 does (8.9%
  against 6.7%), because what they hand back to blue they take from green. On
  Kirby's Adventure they turn the floor solid black, which Rec.601 did not.
- **What removes the crushing is a shadow LIFT**, which is exactly equivalent
  to lowering `video_quantise4`'s first threshold. With it, "carries visible
  colour yet renders black" falls from 6.7% of pixels to 2.5%.

The shipped default needs both: weights `(81,118,57)` (blue at roughly twice
Rec.601's, which is what raises the skies) plus the lift. Green stays high
enough that Sonic's own blue body remains a level BELOW the sky he is drawn
against -- the failure `equal` gets close to.

The original finding, for the record:


The owner's Neo Geo Pocket library is 250 `.ngc` against 27 `.ngp`, and the
WonderSwan one 175 `.wsc` against 163 `.ws`, so both systems are in practice
COLOUR systems here -- the "natively greyscale, nothing is discarded"
framing is true of the mono hardware only. Rec.601 weights blue at 29/256, so
a saturated blue fill lands at luma ~29 and `video_quantise4`'s first
threshold (43) puts it on the darkest level: Sonic Pocket Adventure's first
zone renders with a black sky, and Sonic is nearly invisible against it.

Checked for a core-side answer and there is none. `wswan_mono_palette`
applies only to mono titles; `wswan_gfx_colors` is a bit-depth switch; RACE's
`race_dark_filter_level` only darkens. The fix, if one is wanted, is a
contrast or per-frame histogram stretch in koboy's own pipeline between the
gray LUT and the quantiser -- which is the same stage #23 already wants
optimised, and would be cheapest folded into that work. It would also affect
NES, which has the identical problem, so it is a video.c question rather than
a per-system one.

### 44. Neo Geo Pocket saves do not go through sram.c, and nothing on the device has proved they go anywhere

`retro_get_memory_size(RETRO_MEMORY_SAVE_RAM)` is 0 for every one of the
owner's ten `.ngp` titles on BOTH candidate cores, because an NGP cartridge
saves into flash. RACE writes that flash itself as `<rom>.ngf` in the
directory the frontend answers `GET_SAVE_DIRECTORY` with, and koboy answers
with `cfg.save_dir`, whose shipped default resolves to the install directory.
So saves should work -- verified on the host, where fourteen titles left
twelve `.ngf` files in an initially empty directory -- but:

- the file lands in `.adds/koboy/` beside the binary, not in a `saves/`
  subdirectory, and nothing in koboy names or manages it;
- `sram_save`'s atomic temp-file/fsync/rename discipline does not apply to
  it, because koboy is not the writer;
- an unwritable `save_dir` fails silently, exactly the way a working save
  looks.

`tests/test_core.c` now pins the callback's answer. What it cannot pin is
that the answer is a directory that exists and is writable on the device.

### 45. `KOBOY_MAX_EXTRA_BTNS` is 2 for a spatial reason, not a design one

`koboy_layout.extra[]` is sized 2 because the DMG faceplate has room for
exactly two more discs -- the pocket below A (the Pokemon Mini's C) and the
column between the d-pad and B (the WonderSwan's L1/R1 pair) -- without
moving a Game Boy control or pushing `chrome_controls_top` up into the game
rect. A seventh system needing three would need somewhere to put the third,
not just a bigger number. Recorded so nobody raises the constant and
discovers that at render time.

## The greyscale mapping (added 2026-08-26)

### 46. The default was chosen on a host monitor, not on the panel it is for

Every frame that decided `gray_map = balanced` was rendered through the real
`video.c` and looked at **on a backlit sRGB display**. An e-ink panel's four
DU4 levels are not 0x00/0x55/0xAA/0xFF as reflectance -- the spacing is the
controller's, the white point is paper, and CLAUDE.md's own history is a list
of decisions the device overruled. The in-game MENU entry exists precisely
because this call cannot honestly be made from a host render, but nobody has
yet cycled it on the Libra 2 and said which one is right.

Cheapest resolution: load a `.nes` or `.ngc`, open MENU, and step through the
five. `koboy.log` names the active mapping on every launch, so whatever the
owner settles on is recoverable from the log.

### 47. The MENU handler for GREYSCALE has no automated coverage

`MENU_GRAY`'s branch in `src/main.c` -- cycle, `video_set_gray_map`,
`config_save_gray_map` -- is not reachable from any test, for the same reason
`MENU_SAVE`, `MENU_LOAD` and `MENU_RESET` are not: `--ui-script` drives the
ROM BROWSER only, and `MODE_MENU` has no script hook. Every PART of the branch
is tested (`video_set_gray_map` through the pipeline, `config_save_gray_map`
through `test_config`, the row's label through `test_ui`, and the ini ->
`video_create` plumbing through `smoke_host`), but the three-line branch that
wires them together is verified by reading only.

This is the existing `MODE_MENU` coverage gap (the v2 section above), now with
one more inhabitant. A `--ui-script` hook into `MODE_MENU` would close it for
all of them at once, which is the argument for doing that rather than
special-casing this entry.

### 48. `value` can make a HUD disappear, and the menu offers it anyway

`gray_map = value` (`max(R,G,B)`) puts Super Mario Bros.' white HUD text on a
white sky: the text is gone, not merely low-contrast. It is shipped as a menu
option regardless, because it is genuinely the best of the five for line art
and text-heavy screens and the owner is entitled to see it -- but if a
"cannot render nothing" rule is ever wanted, this is the entry that breaks it.
`koboy.ini`'s comment says so in as many words. Recorded so the day someone
reports "the score vanished", the answer is one line away.

## Four more systems (added 2026-08-27)

### 49. Ten of the ColecoVision's twelve keypad keys are unreachable

The DMG faceplate has room for two extra discs and a ColecoVision controller
has a twelve-key keypad. `config_extra_buttons_for_rom` spends both slots on
keypad 1 and 2, because those are the two the console's own BIOS option
screen asks for and without keypad 1 a cartridge cannot be started at all.
Gearcoleco puts 3-8 on the shoulders and thumbsticks and 9/0 on an analog
axis; koboy answers `RETRO_DEVICE_JOYPAD` only, so those eight are gone.
`*` and `#` are reachable, but only because the core binds them to
`JOYPAD_START` / `JOYPAD_SELECT`, which means koboy's START and SELECT discs
are LYING about what they do on this system -- the faceplate's labels are
moulded into `chrome.c` and are not per-system.

The titles this actually costs are the ones with in-play menus (Fortune
Builder, the Super Action titles), not the ones with a start screen. Two ways
out if it ever matters: a per-system label for the START/SELECT pillows, or
the modal trick FreeIntv uses -- but Gearcoleco has no such mode, so koboy
would have to draw the keypad itself.

### 50. The Intellivision disc is 16-way and koboy offers 8

FreeIntv maps the four cardinals to the retropad d-pad and synthesises the
four diagonals from pairs, which is what koboy's touch pad already produces.
The remaining EIGHT intermediate positions of the real 16-direction disc are
offered only on the LEFT ANALOG STICK, and koboy has no analog source at all
(`src/core.c` answers `RETRO_DEVICE_JOYPAD` and `RETRO_DEVICE_POINTER`).
Titles that steer finely -- Astrosmash's ship, Auto Racing, Night Stalker --
are coarser here than on the hardware. Nothing is unplayable; nothing is
exact either. A touch d-pad that reported an ANGLE rather than a bitmask
could feed the analog axes, which is a real feature and not a small one.

### 51. Every Atari 2600 title renders about 1.75x too tall

Found by rendering frames and looking at them, which is the only way it could
have been found -- every numeric check passes. The 2600's pixels are ~1.6:1,
not square: 160 TIA pixels span a 4:3 frame. stella2014 says so the only way
the API lets it, by declaring `base_width = 160 * 2 = 320` while delivering a
160-wide frame, and koboy's DMG path scales squarely. Result on a Libra 2:
480x630 where the correct shape is about 840x630. Ms. Pac-Man's maze is a
narrow vertical strip.

It is the only system koboy runs with the problem -- ColecoVision,
Intellivision, Master System and Game Gear are all square-pixel by
arithmetic, and so is every handheld here.

The machinery to fix it already exists and is already used: the LCD layout's
`video_scale_gray_frac` does arbitrary `dw x dh`. What is needed is (a) a
per-frame pixel-aspect input to `video_fit_rect`'s DMG branch and (b) making
`video_submit`'s DMG scaler fall back to the fractional path when `dw` is not
`src_w * (dh / src_h)`. Both touch the hot path of the ONE presentation that
has been verified on hardware, which is why this was not done in the same
batch that added the system. Whoever does it should keep the Game Boy's
block-copy path bit-for-bit and prove it with the existing goldens.

Where the aspect number should come from is the second question. The core's
`aspect_ratio` (1.3333 here) is the honest source and `src/core.c` does not
currently read it; `base_width / delivered width` also works for stella2014
but only because that core is encoding the hint in a field it is supposed to
be reporting a fact in.

### 52. An Intellivision frame is mostly black border, on a reflective panel

FreeIntv's output is a fixed 352x224 with the 320x192 playfield inside it;
the rest is the console's border, which most titles leave black. On a Libra 2
that is a solid black band around a 1056x672 game rect -- both hard to read
on paper and the worst case for the panel's waveforms, which is the same
reasoning that already picked the Pokemon Mini's inverted palette
(`src/core.c`). The core has no option to trim it, so the fix would be
koboy's: either crop the known border on submit, or grow the existing
"unused corner is paper" idea into "a border the core draws every frame is
not content". Neither is obviously right, which is why it is written down
rather than done.

### 53. `stella2014_libretro.so` is the largest ARM core shipped, and GPGX is larger

Genesis Plus GX cross-builds to 5.9 MB stripped, against gambatte's 2.6 MB
and RACE's 208 KB, because it carries a Mega Drive, a Sega CD (libchdr,
zstd, tremor, an MP3 decoder) and an SVP DSP that koboy will never load a
single ROM for -- `config_core_for_rom` routes only `.sms` and `.gg` there.
Nothing is broken and `dist/` is not size-constrained, but if it ever
becomes so, GPGX's Makefile has `HAVE_CDROM`/`USE_LIBCHDR` switches and the
right answer is to turn them off rather than to change core.

### 54. `.bin` is a legitimate ROM extension on three of these systems and koboy claims none of it

stella2014 accepts `a26|bin|mvc`, Gearcoleco `col|cv|bin|rom`, FreeIntv
`int|bin|rom`. koboy's allowlist claims only `.a26`, `.col` and `.int`, and
`tests/test_romlist.c` asserts that `exec.bin`, `grom.bin` and
`colecovision.rom` are NOT listed as games -- which is the whole reason. A
user whose Atari 2600 collection is named `*.bin` (a common TOSEC shape) will
see an empty browser and no explanation. If that is ever reported, the fix is
not to claim `.bin` -- it is to say so in `roms/README.txt`, or to add a
rename hint, because the two BIOS files this project now asks users to
install are literally `.bin`.


## Arcade / FinalBurn Neo (added 2026-08-27)

### 55. NOTHING IN THIS BATCH HAS RUN ON A KOBO

The device was off the LAN for the whole session (`ping 192.168.1.27`, no
reply) and there is no `qemu-arm` on this host, so every number in
`TESTED.md`'s arcade table is an x86_64 measurement. The ARM core
cross-builds, strips to 41 MB and passes `scripts/verify-core.sh`; that is
the entire device-side evidence. Rotation in particular has never been seen
on the panel, and rotation is the difference between Galaga and Galaga
sideways.

### 56. The per-frame cost on the device is EXTRAPOLATED, not measured

`TESTED.md` quotes device figures derived from a host-to-device ratio
measured on two cores koboy has already run on hardware: gambatte's `core`
stage is 0.316 ms on this host against 2.3 ms on the Libra 2 (7.3x), and
fceumm's is 0.72 ms against 4.3-4.6 ms (6.0-6.4x). Arcade numbers are the
host figure times 7. That is a defensible extrapolation and it is not a
measurement; the boards near the top of the range (Tapper at an estimated
12.7 ms) are close enough to a 16.7 ms frame that the sign of the error
matters. Re-measure on the device before believing any of them.

### 57. `fps` is per BOARD, and koboy still paces everything at 60

`retro_get_system_av_info().timing.fps` varies across the 227 romsets: 150
report 60, 38 report 59.x, 12 report 58.x, 14 report 55.x, five 54.x, two 50
and **two report 30** (Tapper and Popeye). koboy's `src/pacing.c` paces every
core at the fixed `KOBOY_FRAME_US` the Game Boy measured, so 77 of 227 boards
run at the wrong speed -- Tapper at double. This is #38 with a much larger
population and the first system where a single core produces both extremes,
so a per-core fix is not enough; the value has to come from the av_info.

### 58. Galaga's starfield is a full-screen animation, and the "single-screen
boards do not smear" premise is wrong for it

Measured with koboy's own dirty-rect pipeline, mean fraction of the game rect
changing per presented frame during real gameplay:

| Board | dirty per frame | why |
|---|---|---|
| Dig Dug | 1.5% | static earth, a few sprites |
| Donkey Kong | 1.9% | static girders |
| Ms. Pac-Man | 2.6% | static maze |
| Joust | 0.2% | static platforms |
| Frogger | 52.5% | every lane of logs and cars moves |
| Galaga | 67.1% | **the starfield scrolls continuously** |
| Galaxian | 85.5% | same, and denser |
| Xevious | 59.5% | scrolling shooter, as expected |

Galaga and Galaxian are single-screen games with a full-screen scrolling
BACKGROUND, so they will smear like a scrolling platformer even though
nothing about the playfield scrolls. Whether that is tolerable is a
panel question nobody has asked the panel yet.

### 59. Arcade is the darkest content koboy has ever rendered

Mean luma of a rendered gameplay frame, after the four-level quantiser:
Galaga 0.013, Donkey Kong 0.093, Frogger 0.130, Ms. Pac-Man 0.163, Dig Dug
0.387. Galaga is **97.8% level 0**. The Pokemon Mini was called out in
`src/core.c` for being 83% black at mean luma 0.174 and got an inverted
palette for it; arcade is darker still and cannot take the same fix, because
light-on-dark IS the art -- inverting Ms. Pac-Man's maze would be wrong, not
better. Dig Dug shows the shape of the good case (a bright earth field), so
this is per-board rather than per-system. No action proposed; it is written
down because "arcade suits this panel" needs the qualifier.

### 60. FBNeo returns SUCCESS for a romset it cannot load, and draws an error
screen instead

`retro_load_game` returns true for a missing or mismatched set, and the core
renders a 640x480 mostly-white page reading "This game is known but one of
your romsets is missing files for THIS VERSION of FBNeo". This is the
Gearcoleco `NO BIOS` situation exactly -- "it loaded" and "it works" are
different questions -- and koboy will show that page rather than a
`core rejected rom` line. It is arguably the better outcome (the page names
the problem) but it means koboy's own error path never fires for arcade, and
nothing in the test suite can tell a working board from a broken one.

The reliable discriminator, found by scanning all 227: an error page reports
base 640x480 with `retro_serialize_size() == 0`. Exactly 14 zips match, and
they are precisely the device/BIOS dumps a complete set carries (`neogeo`,
`midssio`, `namcoc69/70/75`, `nmk004`, `ym2608`, `cchip`, `pgm`, `skns`,
`isgsm`, `bubsys`, `decocass`) plus `wbmlb2`, whose parent `wbml.zip` the
owner does not have. One board, `astdelux`, legitimately reports 640x480 --
it is a vector game -- and has a real serialize size, which is why the
serialize half of the test is load-bearing.

### 61. Save states exist for arcade but some are 145 MB

`retro_serialize_size()` is non-zero on all 213 playable boards, so
`MODE_MENU`'s save states are the working way to keep an arcade game. The
range is 6 KB (Pooyan) to **151,911,260 bytes** (the DoDonPachi DaiFukkatsu
family). `src/state.c` allocates that in one block and writes it through
`safefile.c`; on a 512 MB device with three slots per ROM that is not going
to work, and nothing currently checks. The pre-1990 boards this batch is
scoped to are all under 120 KB, so this bites only if someone plays the
later hardware that happens to run.

### 62. Six-button boards lose their shoulder buttons, and Defender loses Reverse

Counted across all 227 romsets: 45 boards bind JOYPAD_L, 48 bind R, 45/46
bind L2/R2, 26/14 bind L3/R3. The DMG faceplate has room for the two discs
this batch added (Y and X, buttons 3 and 4) and no more -- see
`KOBOY_MAX_EXTRA_BTNS`, #45. Every affected board is outside the pre-1990
scope except **Defender**, whose "Reverse" is on JOYPAD_R and is therefore
unreachable; Hyperspace and Thrust are reachable through the new discs.

### 63. The `.zip` row will claim a zipped ROM for any other system

`config_core_for_rom` routes every `.zip` to FinalBurn Neo, which is correct
because nothing else koboy ships can open a zip at all -- but a user who
drops a zipped `.nes` into `roms/` now gets a browser row that fails to load
where before the file was invisible. The failure is diagnosable (the log
names the core, and FBNeo's own error page appears -- see #60) and the fix is
not a dat parser in a 40 KB front-end. If it is ever reported, the cheap
answer is a line in `roms/README.txt` saying koboy does not read zipped ROMs
for any system but arcade.

### 64. 7-Zip support is compiled out of the DEVICE core only

`scripts/build-fbneo-core.sh` passes `INCLUDE_7Z_SUPPORT=0` for the Kobo
target because `dep/libs/lib7z/CpuArch.c` does not compile against glibc
2.19's headers (`HWCAP_NEON undeclared`). The HOST target keeps it on, so the
two builds differ in a capability. No koboy code path reaches the difference
-- `.7z` is claimed by neither `config_core_for_rom` nor `romlist_is_rom`,
and `tests/test_romlist.c` asserts that -- but if `.7z` is ever wanted, the
device build is where the work is.
