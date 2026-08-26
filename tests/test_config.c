/* mkdtemp() and system(), used by the blank-value/no-trailing-newline test
   below, are not declared under -std=c11 without this -- matches src/sram.c,
   src/config.c, tests/test_romlist.c and tests/test_uiscript.c. */
#define _DEFAULT_SOURCE
#include "test.h"
#include "chrome.h"          /* chrome_controls_top, for the new-geometry sweep below */
#include "config.h"
#include <stdio.h>
#include <stdlib.h>

TEST_MAIN({
    koboy_config c;
    config_defaults(&c);
    CHECK_EQ_INT(c.scale, 5);
    CHECK_EQ_INT(c.present_divisor, 3);
    /* The ghosting mitigations default OFF and the flash-promotion threshold
       defaults to firing only on a whole-rect change, which normal gameplay
       essentially never produces (and which is asserted below, not assumed). Both
       were written for forced DU4, which cannot erase; AUTO can, and measuring
       showed the threshold was the sole source of the flashing while the
       cleanup never fired. Asserted here so re-enabling them is a deliberate
       decision with a test to change, not a silent regression. */
    CHECK_EQ_INT(c.cleanup_interval, 0);
    CHECK_EQ_INT(c.full_refresh_permille, 1000);
    CHECK_EQ_INT(c.cleanup_max_ms, 0);
    CHECK_EQ_INT(c.wfm_fast_policy, KOBOY_WFM_AUTO);
    CHECK(c.grab_input);
    /* CROSS: the chrome draws an absolute cross, so the input model has to be
       the absolute one. */
    CHECK_EQ_INT(c.dpad_mode, KOBOY_DPAD_CROSS);
    /* The shipped button mapping, NOT the 0 "uncalibrated" sentinel. Asserted
       by value because the sentinel is what deadlocked first run on a Kobo with
       no page-turn buttons: see the note in config_defaults and test_calib.c.
       193/194 are KEY_F23/KEY_F24, measured on the Libra 2's gpio-keys node;
       116 is KEY_POWER and must never appear here. */
    CHECK_EQ_INT(c.key_a, KOBOY_KEY_PAGE_F23);
    CHECK_EQ_INT(c.key_b, KOBOY_KEY_PAGE_F24);
    CHECK_EQ_INT(c.key_a, 193);
    CHECK_EQ_INT(c.key_b, 194);
    CHECK(c.key_a != KOBOY_KEY_POWER && c.key_b != KOBOY_KEY_POWER);

    /* The default core, and -- separately -- the fact that NOBODY ASKED for
       it. main.c overrides an unasked-for core from the ROM's extension, so
       these two are different facts and both have to hold: the string alone
       cannot distinguish "the user configured gambatte" from "config_defaults
       always writes gambatte". */
    CHECK(strcmp(c.core_path, "gambatte_libretro.so") == 0);
    CHECK(!c.core_explicit);

    /* key_start/key_select: a GUESS (BTN_TL/BTN_TR, the Xbox pad's measured
       shoulder buttons), unlike key_a/key_b's measured page-turn default --
       asserted by value, same reasoning as key_a/key_b above, and never
       KOBOY_KEY_POWER for the same quit-key reason. */
    CHECK_EQ_INT(c.key_start, KOBOY_KEY_BTN_TL);
    CHECK_EQ_INT(c.key_select, KOBOY_KEY_BTN_TR);
    CHECK_EQ_INT(c.key_start, 310);
    CHECK_EQ_INT(c.key_select, 311);
    CHECK(c.key_start != KOBOY_KEY_POWER && c.key_select != KOBOY_KEY_POWER);

    /* The promotion DECISION, not just the stored threshold. Checking only that
       the default is 1000 would let a >= silently become a >, or the bounding
       box shrink by a pixel, and flashing would come back with every test still
       green. The game rect used here is the shipped 5x one, 800x720.

       At the default the promotion fires only on a dirty rect covering the whole
       game rect -- which IS reachable, since the dirty rect is a single merged
       bounding box and a full-screen wipe produces exactly it. That case is
       deliberately inside the promotion: a frame in which everything changed is
       when a flashing refresh is wanted. */
    const long whole = 800L * 720L;
    CHECK(config_promote_full(&c, whole, whole));            /* corner to corner */
    CHECK(!config_promote_full(&c, whole - 1, whole));        /* one pixel short */
    CHECK(!config_promote_full(&c, whole / 2, whole));        /* half the rect */
    CHECK(!config_promote_full(&c, 160L * 144L, whole));      /* a 1x-sized patch */

    /* A lowered threshold promotes proportionally: 450 permille means 45% of
       the rect, and the boundary is again inclusive. */
    c.full_refresh_permille = 450;
    CHECK(config_promote_full(&c, whole * 45 / 100, whole));
    CHECK(config_promote_full(&c, whole, whole));
    CHECK(!config_promote_full(&c, whole * 44 / 100, whole));

    /* <= 0 means disabled, and must not degenerate into "always": a plain
       comparison against 0 permille is true for every non-empty rect. */
    c.full_refresh_permille = 0;
    CHECK(!config_promote_full(&c, whole, whole));
    CHECK(!config_promote_full(&c, 1, whole));
    c.full_refresh_permille = -1;
    CHECK(!config_promote_full(&c, whole, whole));

    /* Degenerate geometry cannot promote, and must not divide or overflow. */
    c.full_refresh_permille = 1000;
    CHECK(!config_promote_full(&c, 0, whole));
    CHECK(!config_promote_full(&c, whole, 0));
    CHECK(!config_promote_full(NULL, whole, whole));

    config_defaults(&c);

    /* 5x fits every supported panel */
    koboy_profile p;
    CHECK(config_resolve_profile(&p, &c, 1264, 1680, KOBOY_GB_W, KOBOY_GB_H, KOBOY_GB_W, KOBOY_GB_H));
    CHECK_EQ_INT(p.scale, 5);
    CHECK_EQ_INT(p.game_x, (1264 - 800) / 2);
    CHECK(config_resolve_profile(&p, &c, 1072, 1448, KOBOY_GB_W, KOBOY_GB_H, KOBOY_GB_W, KOBOY_GB_H));
    CHECK_EQ_INT(p.scale, 5);
    CHECK_EQ_INT(p.game_x, (1072 - 800) / 2);

    /* An impossible configured scale falls back to the largest that fits -- and
       "fits" now means fits above the controls, not merely inside the panel.
       6x would clear the bezel margin on this panel with 176 px to spare and
       still bury the A button and the d-pad, so the answer is 5. */
    c.scale = 99;
    CHECK(config_resolve_profile(&p, &c, 1072, 1448, KOBOY_GB_W, KOBOY_GB_H, KOBOY_GB_W, KOBOY_GB_H));
    CHECK_EQ_INT(p.scale, 5);

    /* PINS the design spec's property that 5x fits every supported panel with
       room to spare, across all four target panel sizes (spec Section 3), with
       the shipped defaults untouched. Nothing else in this file swept every
       panel -- the two CHECK_EQ_INT above only ever covered Libra 2 and Clara
       -- and that gap is exactly how a MENU zone placed at 540 permille
       (mid-panel, not on the Start/Select row already reserved at 920) shipped
       once already: it silently knocked Clara from scale 5 to scale 4 and nothing
       caught it until config_resolve_profile was measured by hand across every
       panel. See the .menu_cx/.menu_cy comment in config_defaults for the
       measured chrome_controls_top values that made the difference. */
    {
        koboy_config dc; config_defaults(&dc);
        static const struct { int w, h; const char *name; } panels[] = {
            { 1072, 1448, "Clara"  },
            { 1264, 1680, "Libra2" },
            { 1404, 1872, "Elipsa" },
            { 1440, 1920, "Sage"   },
        };
        for (size_t i = 0; i < sizeof panels / sizeof panels[0]; i++) {
            koboy_profile pp;
            CHECK(config_resolve_profile(&pp, &dc, panels[i].w, panels[i].h, KOBOY_GB_W, KOBOY_GB_H, KOBOY_GB_W, KOBOY_GB_H));
            if (pp.scale != 5)
                fprintf(stderr, "  %s %dx%d: scale %d, expected 5\n",
                        panels[i].name, panels[i].w, panels[i].h, pp.scale);
            CHECK_EQ_INT(pp.scale, 5);
        }
    }

    /* Extends the sweep above past the one resolution the Game Boy core can
       ever report. A sweep that only ever exercised 160x144 would leave the
       entire point of this task -- deriving scale/the game rect from the
       CORE's geometry instead of KOBOY_GB_W/KOBOY_GB_H -- unguarded: every
       assertion above would pass identically whether or not
       config_resolve_profile's fitting loop actually reads base_w/max_w/h
       or silently still used the compiled-in Game Boy constants.

       Three of the four cases are REAL geometry, measured by running the
       actual gw-libretro core (build/gw_libretro_host.so) against titles
       from a real 59-title Game & Watch collection -- Parachute, Mario
       Bros., and Donkey Kong -- not values transcribed from the design doc.
       The fourth is a synthetic base != max case (a plausible "folds to a
       smaller view" shape for a multi-screen title, not a reproduction of
       any specific title's behaviour observed here) to exercise the one
       field pairing item 3 of the task exists to distinguish: game_w/game_h
       come from max_w/max_h, not base_w/base_h, which base == max in the
       first three cases cannot tell apart.

       want_scale[] is MEASURED the same way the Game Boy's "5" above is --
       by running config_resolve_profile itself against the shipped
       defaults -- so a fitting-loop change that quietly picks a different
       (even if still valid) scale still fails this test, not just an
       out-of-range one. */
    {
        koboy_config dc; config_defaults(&dc);
        static const struct { int w, h; const char *name; } panels[] = {
            { 1072, 1448, "Clara"  },
            { 1264, 1680, "Libra2" },
            { 1404, 1872, "Elipsa" },
            { 1440, 1920, "Sage"   },
        };
        static const struct {
            int base_w, base_h, max_w, max_h;
            int want_scale[4];       /* Clara, Libra2, Elipsa, Sage */
            const char *name;
        } geoms[] = {
            { 658, 395, 658, 395, { 1, 1, 2, 2 }, "Parachute (measured)"   },
            { 973, 532, 973, 532, { 1, 1, 1, 1 }, "Mario Bros. (measured)" },
            { 606, 748, 606, 748, { 1, 1, 1, 1 }, "Donkey Kong (measured)" },
            { 431, 322, 692, 759, { 1, 1, 1, 1 }, "synthetic base!=max"    },
        };

        for (size_t g = 0; g < sizeof geoms / sizeof geoms[0]; g++) {
            for (size_t i = 0; i < sizeof panels / sizeof panels[0]; i++) {
                koboy_profile pp;
                CHECK(config_resolve_profile(&pp, &dc, panels[i].w, panels[i].h,
                                             geoms[g].base_w, geoms[g].base_h,
                                             geoms[g].max_w, geoms[g].max_h));
                if (pp.scale != geoms[g].want_scale[i])
                    fprintf(stderr, "  %s %s %dx%d: scale %d, expected %d\n",
                            geoms[g].name, panels[i].name, panels[i].w, panels[i].h,
                            pp.scale, geoms[g].want_scale[i]);
                CHECK_EQ_INT(pp.scale, geoms[g].want_scale[i]);

                /* game_w/game_h come from max_w/max_h, not base_w/base_h --
                   asserted directly, not just implied by the scale matching,
                   because a config_resolve_profile that silently sized the
                   rect off base while still reporting the max-derived scale
                   would pass a scale-only check. The first three geometries
                   (base == max) cannot distinguish this at all; only the
                   fourth actually exercises it. */
                CHECK_EQ_INT(pp.game_w, geoms[g].max_w * pp.scale);
                CHECK_EQ_INT(pp.game_h, geoms[g].max_h * pp.scale);
                CHECK_EQ_INT(pp.base_w, geoms[g].base_w);
                CHECK_EQ_INT(pp.base_h, geoms[g].base_h);
                CHECK_EQ_INT(pp.max_w,  geoms[g].max_w);
                CHECK_EQ_INT(pp.max_h,  geoms[g].max_h);

                /* The rect must clear the control band -- the exact
                   invariant config_resolve_profile's own comment says this
                   loop exists to keep, now checked against a rect shaped
                   nothing like the Game Boy's 800x720 (5x 160x144), so this
                   is not merely re-running the GB sweep's arithmetic under a
                   new label. */
                int ctrl_top = chrome_controls_top(&dc.layout, panels[i].w, panels[i].h);
                CHECK(pp.game_y + pp.game_h <= ctrl_top);

                /* And the bezel margin, the same four sides as the Sage
                   regression below, swept here across every geometry rather
                   than only the compiled-in default. */
                int left   = pp.game_x;
                int right  = panels[i].w - pp.game_x - pp.game_w;
                int top    = pp.game_y;
                int bottom = panels[i].h - pp.game_y - pp.game_h;
                CHECK(left   >= KOBOY_CHROME_MARGIN);
                CHECK(right  >= KOBOY_CHROME_MARGIN);
                CHECK(top    >= KOBOY_CHROME_MARGIN);
                CHECK(bottom >= KOBOY_CHROME_MARGIN);
            }
        }

        /* A degenerate geometry (a caller error, or a core that answered
           retro_get_system_av_info with nothing before this ever runs) must
           be refused, not fed to a division. */
        koboy_profile bad;
        CHECK(!config_resolve_profile(&bad, &dc, 1264, 1680, 0, 0, 0, 0));
        CHECK(!config_resolve_profile(&bad, &dc, 1264, 1680, 100, 100, -1, 50));
    }

    /* ini overrides defaults; unknown keys are ignored, not fatal */
    FILE *f = fopen("build/t.ini", "w");
    fprintf(f, "# comment\nscale = 4\npresent_divisor=2\nrom = /x/y.gb\n"
               "grab_input = false\nnonsense_key = 1\n");
    fclose(f);
    config_defaults(&c);
    CHECK(config_load(&c, "build/t.ini"));
    CHECK_EQ_INT(c.scale, 4);
    CHECK_EQ_INT(c.present_divisor, 2);
    CHECK(strcmp(c.rom_path, "/x/y.gb") == 0);
    CHECK(!c.grab_input);

    /* key_start/key_select round-trip through the ini the same way key_a/
       key_b already do -- the distinguishing part is that the file's values
       (304/305, a real gamepad's A/B codes, deliberately DIFFERENT from the
       310/311 shipped default) are what come back, not the default surviving
       unnoticed. */
    f = fopen("build/t2.ini", "w");
    fprintf(f, "key_start = 304\nkey_select = 305\n");
    fclose(f);
    config_defaults(&c);
    CHECK_EQ_INT(c.key_start, 310);   /* default, before load */
    CHECK_EQ_INT(c.key_select, 311);
    CHECK(config_load(&c, "build/t2.ini"));
    CHECK_EQ_INT(c.key_start, 304);
    CHECK_EQ_INT(c.key_select, 305);

    /* a missing file is not an error: defaults stand */
    config_defaults(&c);
    CHECK(config_load(&c, "build/definitely-absent.ini"));
    CHECK_EQ_INT(c.scale, 5);

    /* config_save_keys is idempotent: calling it twice on the same file with
       different values should result in the file containing exactly one key_a
       line and one key_b line with the latest values. */
    CHECK(config_save_keys("build/keys1.ini", 111, 222));
    CHECK(config_save_keys("build/keys1.ini", 333, 444));

    /* Count occurrences of key_a and key_b in the file */
    FILE *count_f = fopen("build/keys1.ini", "r");
    CHECK(count_f);
    int key_a_count = 0, key_b_count = 0;
    char line[1024];
    while (fgets(line, sizeof line, count_f)) {
        if (strstr(line, "key_a =")) key_a_count++;
        if (strstr(line, "key_b =")) key_b_count++;
    }
    fclose(count_f);
    CHECK_EQ_INT(key_a_count, 1);
    CHECK_EQ_INT(key_b_count, 1);

    /* Verify the values are the latest ones */
    config_defaults(&c);
    CHECK(config_load(&c, "build/keys1.ini"));
    CHECK_EQ_INT(c.key_a, 333);
    CHECK_EQ_INT(c.key_b, 444);

    /* Preservation: seed a file with a comment, an unrelated key, and a REAL
       key_a line. Call config_save_keys. Verify the comment and unrelated key
       survive, and that the filter actually removed the old key_a line.
       The previous seed here was literally "old key_a = 999" -- a line the
       exact strcmp in config_save_keys' filter never matches, since the key
       name it extracts is "old key_a", not "key_a". That made the filter
       untested: the line survived whether or not the filter existed, and
       still would if the filter were deleted entirely. */
    FILE *seed_f = fopen("build/keys2.ini", "w");
    fprintf(seed_f, "# important comment\nkey_a = 99\nscale = 4\nkey_b = 98\n");
    fclose(seed_f);
    CHECK(config_save_keys("build/keys2.ini", 555, 666));

    /* Verify comment and scale survived */
    config_defaults(&c);
    CHECK(config_load(&c, "build/keys2.ini"));
    CHECK_EQ_INT(c.scale, 4);
    CHECK_EQ_INT(c.key_a, 555);
    CHECK_EQ_INT(c.key_b, 666);

    /* Verify the comment is actually in the file (not just parsed) */
    FILE *verify_f = fopen("build/keys2.ini", "r");
    bool found_comment = false;
    bool found_old_key_a = false;
    int keys2_key_a_count = 0, keys2_key_b_count = 0;
    while (fgets(line, sizeof line, verify_f)) {
        if (strstr(line, "important comment")) found_comment = true;
        if (strstr(line, "key_a = 99")) found_old_key_a = true;
        if (strstr(line, "key_a =")) keys2_key_a_count++;
        if (strstr(line, "key_b =")) keys2_key_b_count++;
    }
    fclose(verify_f);
    CHECK(found_comment);
    /* The old key_a = 99 line must be GONE -- this is the check that the
       previous "old key_a" seed could never fail, because it never matched
       the filter to begin with. */
    CHECK(!found_old_key_a);
    CHECK_EQ_INT(keys2_key_a_count, 1);
    CHECK_EQ_INT(keys2_key_b_count, 1);

    /* Failure path: attempt to save to a non-existent directory.
       Should return false and leave no temp file. */
    bool result = config_save_keys("/nonexistent/dir/keys.ini", 777, 888);
    CHECK(!result);
    CHECK(!fopen("/nonexistent/dir/keys.ini.tmp", "r"));  /* temp file cleaned up */

    /* Chrome margin regression: On a 1440x1920 panel (Kobo Sage), auto-scale
       and explicit scale=9 must both reserve at least KOBOY_CHROME_MARGIN pixels
       on all sides so the bezel doesn't write outside the buffer. */
    config_defaults(&c);
    c.scale = 0;  /* auto */
    CHECK(config_resolve_profile(&p, &c, 1440, 1920, KOBOY_GB_W, KOBOY_GB_H, KOBOY_GB_W, KOBOY_GB_H));
    int left   = p.game_x;
    int right  = 1440 - p.game_x - p.game_w;
    int top    = p.game_y;
    int bottom = 1920 - p.game_y - p.game_h;
    CHECK(left >= KOBOY_CHROME_MARGIN);
    CHECK(right >= KOBOY_CHROME_MARGIN);
    CHECK(top >= KOBOY_CHROME_MARGIN);
    CHECK(bottom >= KOBOY_CHROME_MARGIN);

    /* Same constraint with explicit scale=9 */
    c.scale = 9;
    CHECK(config_resolve_profile(&p, &c, 1440, 1920, KOBOY_GB_W, KOBOY_GB_H, KOBOY_GB_W, KOBOY_GB_H));
    left   = p.game_x;
    right  = 1440 - p.game_x - p.game_w;
    top    = p.game_y;
    bottom = 1920 - p.game_y - p.game_h;
    CHECK(left >= KOBOY_CHROME_MARGIN);
    CHECK(right >= KOBOY_CHROME_MARGIN);
    CHECK(top >= KOBOY_CHROME_MARGIN);
    CHECK(bottom >= KOBOY_CHROME_MARGIN);

    /* #8: a blanked value must not silently mean true. as_bool treated
       everything except "false" and "0" as true, including "", so
       `grab_input = ` turned the grab ON -- the opposite of what someone
       clearing a line intends, and unrecoverable without reading the source. */
    {
        char dir[] = "/tmp/koboy_cfg_XXXXXX";
        CHECK(mkdtemp(dir) != NULL);
        char path[512];
        snprintf(path, sizeof path, "%s/koboy.ini", dir);

        /* force_dither's built-in default is false, so blanking it and
           comparing against that default already distinguishes fixed from
           broken: the old as_bool("") returns true, which does not equal
           false. */
        FILE *f = fopen(path, "w");
        CHECK(f != NULL);
        fputs("force_dither =   \n", f);
        fclose(f);

        koboy_config c; config_defaults(&c);
        bool dither_default = c.force_dither;
        CHECK(config_load(&c, path));
        CHECK_EQ_INT(c.force_dither, dither_default);

        /* grab_input's built-in default is true, which the broken
           as_bool("") also returned -- so comparing a blanked value against
           the compiled-in default cannot fail either way, whichever as_bool
           is linked in. That was a vacuous assertion, precisely the defect
           class this task exists to remove. Fix it by making the assertion
           DISTINGUISHING: set grab_input false on an earlier line, then
           blank it on a later one. The fixed code must leave the prior
           (false) value alone; the broken code flips it back to true. */
        f = fopen(path, "w");
        CHECK(f != NULL);
        fputs("grab_input = false\n", f);
        fclose(f);
        config_defaults(&c);
        CHECK(config_load(&c, path));
        CHECK_EQ_INT(c.grab_input, 0);

        f = fopen(path, "w");
        CHECK(f != NULL);
        fputs("grab_input = false\ngrab_input = \n", f);
        fclose(f);
        config_defaults(&c);
        CHECK(config_load(&c, path));
        CHECK_EQ_INT(c.grab_input, 0);      /* blank did not resurrect it */

        /* Explicit values still work in both directions. */
        f = fopen(path, "w");
        CHECK(f != NULL);
        fputs("grab_input = false\nforce_dither = true\n", f);
        fclose(f);
        config_defaults(&c);
        CHECK(config_load(&c, path));
        CHECK_EQ_INT(c.grab_input, 0);
        CHECK_EQ_INT(c.force_dither, 1);

        /* #9: an ini with NO trailing newline must not have the calibration
           block concatenated onto its last line. It is harmless today only
           because config_load truncates at the resulting '#', but it silently
           rewrites an unrelated line, against the whole point of preserving
           everything else. */
        f = fopen(path, "w");
        CHECK(f != NULL);
        fputs("scale = 4", f);            /* deliberately no newline */
        fclose(f);
        CHECK(config_save_keys(path, 193, 194));

        f = fopen(path, "r");
        CHECK(f != NULL);
        char first[256] = {0};
        CHECK(fgets(first, sizeof first, f) != NULL);
        fclose(f);
        /* The first line must still be exactly the scale line. */
        CHECK(strncmp(first, "scale = 4", 9) == 0);
        CHECK_EQ_INT((int)strlen(first), 10);   /* "scale = 4" plus '\n' */

        /* And the keys survived the round trip. */
        config_defaults(&c);
        CHECK(config_load(&c, path));
        CHECK_EQ_INT(c.scale, 4);
        CHECK_EQ_INT(c.key_a, 193);
        CHECK_EQ_INT(c.key_b, 194);

        char cmd[1024];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
        if (system(cmd) != 0) fprintf(stderr, "NOTE: cleanup failed\n");
    }

    /* ----------------------------------------------- core by extension */
    {
        /* Game & Watch content goes to gw-libretro; everything else the
           browser lists goes to gambatte. Case-insensitive, matching the
           browser filter, because a collection copied off a PC has both
           cases in it. */
        CHECK(strcmp(config_core_for_rom("BALL.mgw"), "gw_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("BALL.MGW"), "gw_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("BALL.Mgw"), "gw_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("/mnt/onboard/.adds/koboy/roms/OCTOPUS.mgw"),
                     "gw_libretro.so") == 0);

        CHECK(strcmp(config_core_for_rom("ZELDA.gb"), "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("ZELDA.GB"), "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("KIRBY.gbc"), "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("KIRBY.GBC"), "gambatte_libretro.so") == 0);
        /* An extensionless name, and the empty/NULL rom_path a run that
           reaches core_open with nothing chosen would carry. Falling back to
           gambatte keeps that run failing exactly the way it used to, in
           core_open, rather than crashing here. */
        CHECK(strcmp(config_core_for_rom("SOMETHING"), "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom(""), "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom(NULL), "gambatte_libretro.so") == 0);
        /* Superstring and prefix, same trap as the browser filter. */
        CHECK(strcmp(config_core_for_rom("BALL.mgwx"), "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("BALL.mg"), "gambatte_libretro.so") == 0);

        /* The result must stay SLASHLESS. config_join_sibling passes any name
           containing a slash through verbatim, so a "cores/gw_libretro.so"
           returned here would reach dlopen as a cwd-relative path and fail on
           a menu launch that sets no cwd -- the exact bug the sibling-join
           exists to prevent. */
        CHECK(strchr(config_core_for_rom("BALL.mgw"), '/') == NULL);
        CHECK(strchr(config_core_for_rom("ZELDA.gb"), '/') == NULL);

        /* The length guard in ends_with_mgw, made deterministic rather than
           ASan-only -- same construction as tests/test_romlist.c's short-name
           case, and for the same reason: with the guard removed the backward
           read for a 1-character name walks off the front of the string, and
           spelling ".mgw" into the bytes immediately before it turns that
           into an observable WRONG ANSWER (a file called "w" routed to the
           Game & Watch core) instead of undefined behaviour a test cannot
           legitimately observe. */
        {
            char pad[5] = { '.', 'm', 'g', 'w', '\0' };
            CHECK(strcmp(config_core_for_rom(&pad[3]),   /* pad[3..] is "w" */
                         "gambatte_libretro.so") == 0);
        }
    }

    /* THE SHIPPED ini must not name a core. `core =` is what marks the choice
       explicit, and an explicit choice turns OFF the .mgw -> gw-libretro
       routing -- so a config/koboy.ini that merely restated the gambatte
       default would pin every install to gambatte with every other test in
       this file still green. It did exactly that until this check was
       written. Read from the repo-relative path DEFAULT_INI uses, so `make
       test` (run from the repo root) checks the real shipped file rather
       than a copy. */
    {
        config_defaults(&c);
        CHECK(config_load(&c, "config/koboy.ini"));
        CHECK(!c.core_explicit);
    }

    /* ------------------------------------------- explicit core wins ---- */
    {
        char dir[] = "/tmp/koboy_core_ini_XXXXXX";
        CHECK(mkdtemp(dir) != NULL);
        char path[512];

        /* An ini that names a core marks the choice explicit, which is what
           stops main.c overriding it from the ROM's extension. */
        snprintf(path, sizeof path, "%s/explicit.ini", dir);
        FILE *f = fopen(path, "w");
        CHECK(f != NULL);
        fputs("core = my_libretro.so\n", f);
        fclose(f);
        config_defaults(&c);
        CHECK(config_load(&c, path));
        CHECK(strcmp(c.core_path, "my_libretro.so") == 0);
        CHECK(c.core_explicit);

        /* An ini that says nothing about the core leaves the flag alone, so
           the default stays overridable. Asserted separately because a
           config_load that set core_explicit unconditionally -- or that never
           set it -- would pass one of these two and fail the other. */
        snprintf(path, sizeof path, "%s/quiet.ini", dir);
        f = fopen(path, "w");
        CHECK(f != NULL);
        fputs("scale = 4\n", f);
        fclose(f);
        config_defaults(&c);
        CHECK(config_load(&c, path));
        CHECK_EQ_INT(c.scale, 4);
        CHECK(strcmp(c.core_path, "gambatte_libretro.so") == 0);
        CHECK(!c.core_explicit);

        char cmd[1024];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
        if (system(cmd) != 0) fprintf(stderr, "NOTE: cleanup failed\n");
    }
})
