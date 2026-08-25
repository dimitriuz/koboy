#include "test.h"
#include "calib.h"
#include "config.h"

TEST_MAIN({
    koboy_config c; config_defaults(&c);

    /* FIRST RUN MUST NOT DEADLOCK. A freshly-defaulted config is already
       playable: it ships the two measured page-turn codes, so calibration is
       offered as a choice, never as a gate. Before this, config_defaults left
       both keys at the 0 sentinel, calib_needed() was therefore true on every
       first run, and the calibration loop advances only on a hardware key --
       so a touch-only Kobo (Clara family, Nia, Elipsa) sat on "press the
       button you want as A" with no button to press. */
    CHECK(!calib_needed(&c));
    CHECK_EQ_INT(c.key_a, 193);
    CHECK_EQ_INT(c.key_b, 194);
    /* Never the power button: it shares gpio-keys with the page-turn buttons
       and it is the quit key. */
    CHECK(c.key_a != KOBOY_KEY_POWER);
    CHECK(c.key_b != KOBOY_KEY_POWER);
    CHECK(c.key_a != c.key_b);

    /* the sentinel still means "calibrate", for a user who asks for it */
    c.key_a = 0;
    CHECK(calib_needed(&c));
    c.key_a = 193; c.key_b = 0;
    CHECK(calib_needed(&c));
    c.key_a = 1; c.key_b = 2;
    CHECK(!calib_needed(&c));

    /* The ESCAPE. A user who zeroes a key by hand still reaches the loop, so
       the loop must be escapable and the escape must leave a usable mapping --
       zeros would make input_feed_key ignore every key for the session. */
    config_defaults(&c);
    c.key_a = 0; c.key_b = 0;
    calib_escape(&c);
    CHECK(!calib_needed(&c));                   /* usable, not zeros */
    CHECK(c.key_a != 0 && c.key_b != 0);
    CHECK(c.key_a != c.key_b);
    CHECK(c.key_a != KOBOY_KEY_POWER && c.key_b != KOBOY_KEY_POWER);
    CHECK_EQ_INT(c.key_a, 193);
    CHECK_EQ_INT(c.key_b, 194);

    /* a half-calibrated config keeps the key it has and fills in the other */
    c.key_a = 59; c.key_b = 0;
    calib_escape(&c);
    CHECK_EQ_INT(c.key_a, 59);
    CHECK_EQ_INT(c.key_b, 194);
    CHECK(!calib_needed(&c));

    /* and the fill-in never collides with, or duplicates, what is there */
    c.key_a = 194; c.key_b = 0;
    calib_escape(&c);
    CHECK_EQ_INT(c.key_a, 194);
    CHECK_EQ_INT(c.key_b, 193);
    CHECK(c.key_a != c.key_b);

    /* the power button can never survive the escape as a game button */
    c.key_a = KOBOY_KEY_POWER; c.key_b = KOBOY_KEY_POWER;
    calib_escape(&c);
    CHECK(c.key_a != KOBOY_KEY_POWER && c.key_b != KOBOY_KEY_POWER);
    CHECK(c.key_a != c.key_b);

    /* The escape prompt has to tell the user how to get out, and has to be
       renderable by main.c's 5x7 font, which has letters and space only. */
    const char *esc = calib_escape_prompt();
    CHECK(strstr(esc, "tap") != NULL || strstr(esc, "TAP") != NULL);
    bool renderable = true;
    for (const char *q = esc; *q; q++)
        if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || *q == ' '))
            renderable = false;
    CHECK(renderable);

    config_defaults(&c);
    koboy_calib k;
    calib_begin(&k, &c);
    CHECK(strstr(calib_prompt(&k), "A") != NULL);

    /* the power button must never be accepted as a game button */
    CHECK(!calib_feed_key(&k, 116));
    CHECK(strstr(calib_prompt(&k), "A") != NULL);

    CHECK(!calib_feed_key(&k, 193));            /* A captured, B still pending */
    CHECK(strstr(calib_prompt(&k), "B") != NULL);

    /* the same code cannot serve both buttons */
    CHECK(!calib_feed_key(&k, 193));
    CHECK(strstr(calib_prompt(&k), "B") != NULL);

    CHECK(calib_feed_key(&k, 194));             /* done */

    CHECK(calib_commit(&k, &c, "build/calib.ini"));
    CHECK_EQ_INT(c.key_a, 193);
    CHECK_EQ_INT(c.key_b, 194);

    /* committed values survive a reload */
    koboy_config c2; config_defaults(&c2);
    CHECK(config_load(&c2, "build/calib.ini"));
    CHECK_EQ_INT(c2.key_a, 193);
    CHECK_EQ_INT(c2.key_b, 194);
    CHECK(!calib_needed(&c2));
})
