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

    /* Coming back from a UI mode REBASES the clock and KEEPS the count.

       main.c called pacer_init on menu exit, which zeroes p->frames -- and the
       bounded-run test in the emulator loop is `pace.frames >= frame_limit`.
       So an unattended --frames N run restarted its entire budget every time
       the menu closed, and could never terminate while the menu kept being
       opened. Rebasing the wall clock is right; zeroing the counter is not,
       and the two must not be the same call. */
    {
        koboy_pacer m;
        uint64_t t0 = 5ull * 1000000ull;
        pacer_init(&m, t0, 3);
        for (int i = 0; i < 40; i++) pacer_tick(&m);
        CHECK_EQ_INT(m.frames, 40);

        /* Thirty seconds pass with a menu on the panel. */
        uint64_t t1 = t0 + 30ull * 1000000ull;
        pacer_rebase(&m, t1);

        /* The budget survives. This is the assertion that fails if rebase is
           quietly pacer_init in disguise. */
        CHECK_EQ_INT(m.frames, 40);
        CHECK_EQ_INT(m.divisor, 3);

        /* The clock is re-anchored: the next core frame is due NOW, not
           thirty seconds ago (which would make the core sprint through ~1800
           frames catching up) and not one whole run-length in the future. */
        CHECK_EQ_INT(pacer_delay_us(&m, t1), 0);
        CHECK_EQ_INT(pacer_delay_us(&m, t1 + KOBOY_FRAME_US), 0);
        pacer_tick(&m);
        CHECK_EQ_INT(m.frames, 41);
        CHECK_EQ_INT(pacer_delay_us(&m, t1), KOBOY_FRAME_US);

        /* The presentation phase is continuous across the rebase too: frame
           41 is not a multiple of 3, so it is not a presentation frame. */
        koboy_pacer n;
        pacer_init(&n, t0, 3);
        for (int i = 0; i < 41; i++) pacer_tick(&n);
        CHECK_EQ_INT(n.frames, m.frames);
    }
})
