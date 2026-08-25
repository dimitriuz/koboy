#include "test.h"
#include "stats.h"

TEST_MAIN({
    koboy_stats s;
    stats_reset(&s);

    /* An empty stage must report zero rather than divide by zero. */
    CHECK_EQ_INT(stats_mean_us(&s, KOBOY_STAGE_CORE), 0);
    CHECK_EQ_INT(stats_max_us(&s, KOBOY_STAGE_CORE), 0);

    stats_add(&s, KOBOY_STAGE_CORE, 10);
    stats_add(&s, KOBOY_STAGE_CORE, 20);
    stats_add(&s, KOBOY_STAGE_CORE, 60);
    CHECK_EQ_INT(stats_mean_us(&s, KOBOY_STAGE_CORE), 30);
    CHECK_EQ_INT(stats_max_us(&s, KOBOY_STAGE_CORE), 60);

    /* Stages are independent. */
    stats_add(&s, KOBOY_STAGE_REFRESH, 1000);
    CHECK_EQ_INT(stats_mean_us(&s, KOBOY_STAGE_CORE), 30);
    CHECK_EQ_INT(stats_mean_us(&s, KOBOY_STAGE_REFRESH), 1000);

    /* The bounds guard, asserted directly rather than by invoking undefined
       behaviour. See stats_stage_valid's comment in stats.h. */
    CHECK_EQ_INT(stats_stage_valid(-1), 0);
    CHECK_EQ_INT(stats_stage_valid(KOBOY_STAGE_COUNT), 0);
    CHECK_EQ_INT(stats_stage_valid(KOBOY_STAGE_CORE), 1);
    CHECK_EQ_INT(stats_stage_valid(KOBOY_STAGE_COUNT - 1), 1);

    char buf[256];
    stats_format(&s, buf, sizeof buf);
    CHECK(strstr(buf, "core=") != NULL);
    CHECK(strstr(buf, "refresh=") != NULL);

    /* Must not overrun a short buffer. */
    char tiny[8];
    stats_format(&s, tiny, sizeof tiny);
    CHECK_EQ_INT(strlen(tiny) < sizeof tiny, 1);
})
