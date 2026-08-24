#include "test.h"
#include "calib.h"
#include "config.h"

TEST_MAIN({
    koboy_config c; config_defaults(&c);
    CHECK(calib_needed(&c));                    /* fresh config needs it */
    c.key_a = 1; c.key_b = 2;
    CHECK(!calib_needed(&c));

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
