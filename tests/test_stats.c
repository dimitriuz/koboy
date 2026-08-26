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

    /* Must not overrun a short buffer -- and this has to be PROVED, not
       merely implied by a strlen that a runaway sprintf would satisfy anyway.

       The old version passed a bare char tiny[8] and only checked that the
       result fit. Mutating stats.c's snprintf to sprintf -- a 66-byte overrun
       of an 8-byte buffer -- left this file at 13 checks and 0 failures: the
       overrun ran off into whatever followed on the stack, and the assertion
       could only ever have failed by the process happening to crash. A test
       that can only fail via undefined behaviour is not a test.

       So the short buffer is a WINDOW into a larger, fully owned object,
       exactly as tests/test_text.c's GW/GH/PAD block does it. Overflow is
       positive only (stats_format writes forward from `out`), the padding
       belongs to the same array, and reading it back is defined on every
       libc -- so an overrun is observed as changed padding rather than as a
       segfault that may or may not happen. */
    {
        enum { WIN = 8, PAD = 128 };
        static char win[WIN + PAD];
        memset(win, 0x5A, sizeof win);
        stats_format(&s, win, WIN);
        CHECK_EQ_INT(strlen(win) < (size_t)WIN, 1);
        int past = 0;
        for (int i = WIN; i < WIN + PAD; i++) if (win[i] != 0x5A) past++;
        CHECK_EQ_INT(past, 0);

        /* NOT asserted here, deliberately: "stats_format(&s, out, 0) writes
           nothing". It is true, and the guard in stats.c that makes it true is
           worth keeping -- but deleting that guard leaves snprintf(out, 0, ..)
           behind, which by C99 7.19.6.5 also writes nothing. The assertion
           would pass either way, which makes it decoration rather than a test.
           Verified by removing the guard: 15 checks, 0 failures. */
    }
})
