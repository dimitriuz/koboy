#include "test.h"
#include "pacing.h"

TEST_MAIN({
    koboy_pacer p;
    pacer_init(&p, 1000, 3);

    /* the core must run at true 60Hz, so the first frame is due immediately */
    CHECK_EQ_INT(pacer_delay_us(&p, 1000), 0);

    /* presentation is subsampled 1-in-3, but every core frame still ticks */
    CHECK(pacer_tick(&p));      /* frame 0 presented */
    CHECK(!pacer_tick(&p));
    CHECK(!pacer_tick(&p));
    CHECK(pacer_tick(&p));      /* frame 3 presented */

    /* after 4 ticks the fourth frame is due at start + 4 * 16742 */
    uint64_t due = 1000 + 4ull * KOBOY_FRAME_US;
    CHECK_EQ_INT(pacer_delay_us(&p, due), 0);
    CHECK_EQ_INT(pacer_delay_us(&p, due - 500), 500);

    /* running late never asks us to sleep, and never sleeps negatively */
    CHECK_EQ_INT(pacer_delay_us(&p, due + 100000), 0);

    /* divisor 1 presents every frame */
    koboy_pacer q;
    pacer_init(&q, 0, 1);
    CHECK(pacer_tick(&q)); CHECK(pacer_tick(&q)); CHECK(pacer_tick(&q));

    /* a nonsense divisor is clamped rather than dividing by zero */
    koboy_pacer r;
    pacer_init(&r, 0, 0);
    CHECK(pacer_tick(&r));
})
