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

    /* Out-of-range stage indices are ignored, not written through. A bad
       index here would corrupt the adjacent stage's totals, which is the
       kind of bug that shows up as a nonsense number in a bug report. */
    stats_add(&s, -1, 999999);
    stats_add(&s, KOBOY_STAGE_COUNT, 999999);
    CHECK_EQ_INT(stats_mean_us(&s, KOBOY_STAGE_CORE), 30);

    char buf[256];
    stats_format(&s, buf, sizeof buf);
    CHECK(strstr(buf, "core=") != NULL);
    CHECK(strstr(buf, "refresh=") != NULL);

    /* Must not overrun a short buffer. */
    char tiny[8];
    stats_format(&s, tiny, sizeof tiny);
    CHECK_EQ_INT(strlen(tiny) < sizeof tiny, 1);
})
