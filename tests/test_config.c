/* mkdtemp() and system(), used by the blank-value/no-trailing-newline test
   below, are not declared under -std=c11 without this -- matches src/sram.c,
   src/config.c, tests/test_romlist.c and tests/test_uiscript.c. */
#define _DEFAULT_SOURCE
#include "test.h"
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
    CHECK(config_resolve_profile(&p, &c, 1264, 1680));
    CHECK_EQ_INT(p.scale, 5);
    CHECK_EQ_INT(p.game_x, (1264 - 800) / 2);
    CHECK(config_resolve_profile(&p, &c, 1072, 1448));
    CHECK_EQ_INT(p.scale, 5);
    CHECK_EQ_INT(p.game_x, (1072 - 800) / 2);

    /* An impossible configured scale falls back to the largest that fits -- and
       "fits" now means fits above the controls, not merely inside the panel.
       6x would clear the bezel margin on this panel with 176 px to spare and
       still bury the A button and the d-pad, so the answer is 5. */
    c.scale = 99;
    CHECK(config_resolve_profile(&p, &c, 1072, 1448));
    CHECK_EQ_INT(p.scale, 5);

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
    CHECK(config_resolve_profile(&p, &c, 1440, 1920));
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
    CHECK(config_resolve_profile(&p, &c, 1440, 1920));
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

        FILE *f = fopen(path, "w");
        CHECK(f != NULL);
        fputs("grab_input = \nforce_dither =   \n", f);
        fclose(f);

        koboy_config c; config_defaults(&c);
        bool grab_default = c.grab_input, dither_default = c.force_dither;
        CHECK(config_load(&c, path));
        CHECK_EQ_INT(c.grab_input, grab_default);
        CHECK_EQ_INT(c.force_dither, dither_default);

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
})
