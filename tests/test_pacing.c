#include "test.h"
#include "pacing.h"
#include <math.h>

TEST_MAIN({
    koboy_pacer p;
    /* frame_us 0 is the "Game Boy's rate" sentinel, so every assertion in
       this first block is the one that was here before the pacer could carry
       a rate at all -- and it now also proves the clamp, since a 0 that
       reached pacer_delay_us unclamped would make every frame due at
       start_us and each CHECK below would read 0. */
    pacer_init(&p, 1000, 3, 0);
    CHECK_EQ_INT(p.frame_us, KOBOY_FRAME_US);

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
    pacer_init(&q, 0, 1, 0);
    CHECK(pacer_tick(&q)); CHECK(pacer_tick(&q)); CHECK(pacer_tick(&q));

    /* a nonsense divisor is clamped rather than dividing by zero */
    koboy_pacer r;
    pacer_init(&r, 0, 0, 0);
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
        pacer_init(&m, t0, 3, 0);
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
        pacer_init(&n, t0, 3, 0);
        for (int i = 0; i < 41; i++) pacer_tick(&n);
        CHECK_EQ_INT(n.frames, m.frames);
    }

    /* ---------------------------------------------------------------------
       PER-CORE PACING. Everything above this line paced at the Game Boy's
       59.7275 Hz, because until this task KOBOY_FRAME_US was the only rate
       koboy had -- for all eleven systems. docs/FOLLOWUPS.md #38 and #57.
       --------------------------------------------------------------------- */

    /* THE assertion this whole change turns on: the Game Boy must not move.
       gambatte reports 59.7275 fps (measured, scripts/probe_core.c), and
       pacing it from that number rather than from the constant has to land on
       the same 16742 us the constant always was. If this ever fails, the
       right answer is to keep the Game Boy on the constant, not to move the
       constant. It already fired once: the first version of
       pacer_frame_us_from_fps rounded and produced 16743 here, which is why
       that function truncates. */
    CHECK_EQ_INT(pacer_frame_us_from_fps(59.7275), KOBOY_FRAME_US);

    /* The rest of the measured population, straight out of the cores. Each is
       a rate koboy was previously getting wrong by pacing it at 59.7275. */
    CHECK_EQ_INT(pacer_frame_us_from_fps(60.0),     16666);  /* gearcoleco, freeintv */
    CHECK_EQ_INT(pacer_frame_us_from_fps(60.0998),  16638);  /* fceumm, NES */
    CHECK_EQ_INT(pacer_frame_us_from_fps(59.9227),  16688);  /* genesis_plus_gx */
    CHECK_EQ_INT(pacer_frame_us_from_fps(72.0),     13888);  /* PokeMini */
    CHECK_EQ_INT(pacer_frame_us_from_fps(75.4717),  13249);  /* WonderSwan */
    CHECK_EQ_INT(pacer_frame_us_from_fps(60.25),    16597);  /* race, NGP */
    CHECK_EQ_INT(pacer_frame_us_from_fps(59.92),    16688);  /* stella2014, NTSC */
    CHECK_EQ_INT(pacer_frame_us_from_fps(49.92),    20032);  /* stella2014, PAL */
    /* Tapper and Popeye. THIS is the pair that made the defect matter: at
       KOBOY_FRAME_US they ran at 59.7275/30 = very nearly double speed. */
    CHECK_EQ_INT(pacer_frame_us_from_fps(30.0),     33333);

    /* The rejection path. A core that reports nothing, or reports nonsense,
       must fall back rather than divide it into a frame time. NaN is in here
       deliberately: it is the one input that a `fps < lo || fps > hi` test
       would wave through, and the cast of the resulting NaN is undefined. */
    CHECK_EQ_INT(pacer_frame_us_from_fps(0.0),        KOBOY_FRAME_US);
    CHECK_EQ_INT(pacer_frame_us_from_fps(-60.0),      KOBOY_FRAME_US);
    CHECK_EQ_INT(pacer_frame_us_from_fps(0.001),      KOBOY_FRAME_US);
    CHECK_EQ_INT(pacer_frame_us_from_fps(1000000.0),  KOBOY_FRAME_US);
    CHECK_EQ_INT(pacer_frame_us_from_fps(NAN),        KOBOY_FRAME_US);
    CHECK_EQ_INT(pacer_frame_us_from_fps(INFINITY),   KOBOY_FRAME_US);
    CHECK_EQ_INT(pacer_frame_us_from_fps(-INFINITY),  KOBOY_FRAME_US);

    /* Both edges of the accepted range, from the inside and the outside, so
       the bound is pinned rather than merely "somewhere around there". */
    CHECK_EQ_INT(pacer_frame_us_from_fps(10.0),   100000);
    CHECK_EQ_INT(pacer_frame_us_from_fps(9.99),   KOBOY_FRAME_US);
    CHECK_EQ_INT(pacer_frame_us_from_fps(300.0),  3333);
    CHECK_EQ_INT(pacer_frame_us_from_fps(300.01), KOBOY_FRAME_US);

    /* A pacer built on a non-Game-Boy rate actually PACES at it. Without
       this, every check above could pass while pacer_delay_us went on using
       the constant -- which is exactly what it did before this task. */
    {
        koboy_pacer tap;
        const uint32_t tapper_us = 33333;
        pacer_init(&tap, 1000, 1, tapper_us);
        CHECK_EQ_INT(tap.frame_us, tapper_us);
        for (int i = 0; i < 4; i++) pacer_tick(&tap);
        CHECK_EQ_INT(pacer_delay_us(&tap, 1000 + 4ull * tapper_us), 0);
        CHECK_EQ_INT(pacer_delay_us(&tap, 1000 + 4ull * tapper_us - 500), 500);
        /* And it is NOT the Game Boy's: at the moment the Game Boy's fourth
           frame would have been due, Tapper's is still 4*(33333-16742) away.
           This is the check that fails if frame_us is stored but ignored. */
        CHECK_EQ_INT(pacer_delay_us(&tap, 1000 + 4ull * KOBOY_FRAME_US),
                     4ull * (tapper_us - KOBOY_FRAME_US));

        /* pacer_rebase must use the pacer's own rate too. Rebasing at t means
           "the next frame is due now"; a rebase that walked the clock back by
           frames * KOBOY_FRAME_US instead would leave the next frame
           4*(33333-16742) us in the past and this check would read 0 either
           way -- so the assertion is on the frame AFTER, whose due time only
           comes out right if both the rebase and the delay used 33333. */
        pacer_rebase(&tap, 900000);
        CHECK_EQ_INT(pacer_delay_us(&tap, 900000), 0);
        pacer_tick(&tap);
        CHECK_EQ_INT(pacer_delay_us(&tap, 900000), tapper_us);
        CHECK_EQ_INT(tap.frames, 5);
    }

    /* Changing the rate MID-RUN, which is what SET_SYSTEM_AV_INFO can do. */
    {
        koboy_pacer s;
        pacer_init(&s, 1000, 3, 0);
        for (int i = 0; i < 100; i++) pacer_tick(&s);

        /* A no-op when the rate has not changed -- main.c calls this from a
           branch that fires for every geometry announcement, so the common
           case must not disturb the clock at all. */
        uint64_t before = s.start_us;
        pacer_set_frame_us(&s, 5000000, KOBOY_FRAME_US);
        CHECK_EQ_INT(s.start_us, before);

        /* A real change rebases: the next frame is due now, the frame counter
           survives (--frames N budgets depend on it, see the rebase block
           above), and the NEW rate governs from here. Without the rebase the
           next frame would be due 100 * (13888 - 16742) us -- 285 ms -- in
           the past, and the core would sprint to catch up. */
        pacer_set_frame_us(&s, 5000000, 13888);
        CHECK_EQ_INT(s.frame_us, 13888);
        CHECK_EQ_INT(s.frames, 100);
        CHECK_EQ_INT(pacer_delay_us(&s, 5000000), 0);
        pacer_tick(&s);
        CHECK_EQ_INT(pacer_delay_us(&s, 5000000), 13888);

        /* 0 means the Game Boy's rate here too, not "unpaced". */
        pacer_set_frame_us(&s, 5000000, 0);
        CHECK_EQ_INT(s.frame_us, KOBOY_FRAME_US);
    }
})
