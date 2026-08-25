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
