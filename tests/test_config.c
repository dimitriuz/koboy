#include "test.h"
#include "config.h"

TEST_MAIN({
    koboy_config c;
    config_defaults(&c);
    CHECK_EQ_INT(c.scale, 5);
    CHECK_EQ_INT(c.present_divisor, 3);
    CHECK_EQ_INT(c.cleanup_interval, 60);
    CHECK_EQ_INT(c.full_refresh_permille, 450);
    CHECK_EQ_INT(c.cleanup_max_ms, 3000);
    CHECK_EQ_INT(c.wfm_fast_policy, KOBOY_WFM_AUTO);
    CHECK(c.grab_input);
    CHECK_EQ_INT(c.dpad_mode, KOBOY_DPAD_RELATIVE);
    CHECK_EQ_INT(c.key_a, 0);

    /* 5x fits every supported panel */
    koboy_profile p;
    CHECK(config_resolve_profile(&p, &c, 1264, 1680));
    CHECK_EQ_INT(p.scale, 5);
    CHECK_EQ_INT(p.game_x, (1264 - 800) / 2);
    CHECK(config_resolve_profile(&p, &c, 1072, 1448));
    CHECK_EQ_INT(p.scale, 5);
    CHECK_EQ_INT(p.game_x, (1072 - 800) / 2);

    /* an impossible configured scale falls back to the largest that fits */
    c.scale = 99;
    CHECK(config_resolve_profile(&p, &c, 1072, 1448));
    CHECK_EQ_INT(p.scale, 6);

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

    /* Preservation: seed a file with a comment, an unrelated key, and an old
       key_a line. Call config_save_keys. Verify the comment and unrelated key
       survive and still parse correctly. */
    FILE *seed_f = fopen("build/keys2.ini", "w");
    fprintf(seed_f, "# important comment\nscale = 4\nold key_a = 999\n");
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
    while (fgets(line, sizeof line, verify_f)) {
        if (strstr(line, "important comment")) {
            found_comment = true;
            break;
        }
    }
    fclose(verify_f);
    CHECK(found_comment);

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
})
