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

3. **The save path is unexercised on hardware.** Both titles tested (Tetris,
   Darkwing Duck) report `rambanks: 0` — no battery SRAM. `sram_load` changed
   materially in the final fix round, so this wants a battery-save game.

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
    branch in the emulator loop). `--ui-script` drives `run_list`/`run_menu`
    in `MODE_BROWSE` (see `run_list`'s own comment on why), but the emulator
    loop itself never accepts scripted input, so SAVE/LOAD/RESET/CHOOSE
    ROM/QUIT are exercised by inspection and by hand, never by `make test`.
    Extending `--ui-script` through the emulator loop is the obvious
    follow-up; #17's prerequisite is now cleared. `src/uiscript.h` and
    `run_list`'s comment no longer claim MODE_MENU coverage they do not have.

19. **The d-pad horizontal-arm term in `chrome_controls_top` is provably
    dead.** `src/chrome.c:41-42`. `top = min2(top, dcy - arm/2 - 1)` can never
    win against the line immediately before it (`dcy - dr - 1`): with
    `arm = dr/3`, `dr > arm/2` for every `dr > 0` this layout ever produces,
    so the vertical-arm term is always the smaller of the two. The equality
    check in `tests/test_chrome.c` does not catch this, because the
    function's return value is identical whether the line is there or not --
    only a term-by-term audit finds it.

20. **The speaker grille overdraws its right margin by 1px, on three of the
    four tested panel sizes.** `src/chrome.c:305-315`. Each slash is drawn
    with `hline(..., glx0+s, glx0+s+1, ...)` -- two columns per step -- and
    the length clamp (`if (glx0+len > gx1) len = gx1-glx0`) only fires when
    the *unclamped* last column would exceed `gx1`. When it lands exactly on
    `gx1` instead, the clamp never triggers and the two-wide `hline`'s second
    column paints at `gx1`, one column into the margin `gx1` exists to keep
    clear.

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
