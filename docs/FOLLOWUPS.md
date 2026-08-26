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

### 28. Nothing about Game & Watch has run on the device

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
