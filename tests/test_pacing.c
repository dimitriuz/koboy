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

    /* presentation is subsampled 1-in-3, but every core frame still ticks.

       THE `0` IS A CLOCK, and every call in this file that passes one is a
       pacer with no settle hold outstanding (hold_until_us == 0), which
       pacer_tick short-circuits before it ever compares against now_us. The
       hold's own assertions are the AREA-AWARE PACING block at the end of the
       file, and they pass a moving clock. A zero here is inert, not a
       shortcut. */
    CHECK(pacer_tick(&p, 0));      /* frame 0 presented */
    CHECK(!pacer_tick(&p, 0));
    CHECK(!pacer_tick(&p, 0));
    CHECK(pacer_tick(&p, 0));      /* frame 3 presented */

    /* after 4 ticks the fourth frame is due at start + 4 * 16742 */
    uint64_t due = 1000 + 4ull * KOBOY_FRAME_US;
    CHECK_EQ_INT(pacer_delay_us(&p, due), 0);
    CHECK_EQ_INT(pacer_delay_us(&p, due - 500), 500);

    /* running late never asks us to sleep, and never sleeps negatively */
    CHECK_EQ_INT(pacer_delay_us(&p, due + 100000), 0);

    /* divisor 1 presents every frame */
    koboy_pacer q;
    pacer_init(&q, 0, 1, 0);
    CHECK(pacer_tick(&q, 0)); CHECK(pacer_tick(&q, 0)); CHECK(pacer_tick(&q, 0));

    /* A nonsense divisor is clamped rather than dividing by zero.

       THE FIELD IS ASSERTED FIRST, and that ordering is the point. Until this
       line existed, the only thing here was the pacer_tick below, which with
       an unclamped 0 computes `frames % 0` -- undefined behaviour that on
       x86-64 raises SIGFPE and kills the process before the harness can print
       a single FAIL. Mutation-checked while adding pacer_set_divisor: the
       binary exited 136 having said nothing at all, so a reader of the output
       could not tell a broken clamp from a broken test. Same class as the
       chrome_bands guard band in CLAUDE.md's testing-culture note: stop
       observing the UB, assert the clamped value. */
    koboy_pacer r;
    pacer_init(&r, 0, 0, 0);
    CHECK_EQ_INT(r.divisor, 1);
    CHECK(pacer_tick(&r, 0));

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
        for (int i = 0; i < 40; i++) pacer_tick(&m, 0);
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
        pacer_tick(&m, 0);
        CHECK_EQ_INT(m.frames, 41);
        CHECK_EQ_INT(pacer_delay_us(&m, t1), KOBOY_FRAME_US);

        /* The presentation phase is continuous across the rebase too: frame
           41 is not a multiple of 3, so it is not a presentation frame. */
        koboy_pacer n;
        pacer_init(&n, t0, 3, 0);
        for (int i = 0; i < 41; i++) pacer_tick(&n, 0);
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
        for (int i = 0; i < 4; i++) pacer_tick(&tap, 0);
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
        pacer_tick(&tap, 0);
        CHECK_EQ_INT(pacer_delay_us(&tap, 900000), tapper_us);
        CHECK_EQ_INT(tap.frames, 5);
    }

    /* Changing the rate MID-RUN, which is what SET_SYSTEM_AV_INFO can do. */
    {
        koboy_pacer s;
        pacer_init(&s, 1000, 3, 0);
        for (int i = 0; i < 100; i++) pacer_tick(&s, 0);

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
        pacer_tick(&s, 0);
        CHECK_EQ_INT(pacer_delay_us(&s, 5000000), 13888);

        /* 0 means the Game Boy's rate here too, not "unpaced". */
        pacer_set_frame_us(&s, 5000000, 0);
        CHECK_EQ_INT(s.frame_us, KOBOY_FRAME_US);
    }

    /* Changing the DIVISOR mid-run, which is what the in-game FRAMES entry
       does. Distinct from the block above in what it must NOT touch: the
       divisor is not in the pacer's wall-clock model at all, so a
       pacer_set_divisor that rebased (or that reset the frame counter, the
       bug pacer_rebase exists to avoid) would break a --frames budget and the
       core's timing for a setting that has nothing to do with either. */
    {
        koboy_pacer d;
        pacer_init(&d, 1000, 3, 0);
        for (int i = 0; i < 12; i++) pacer_tick(&d, 0);      /* 4 presented of 12 */
        uint64_t start_before = d.start_us;

        pacer_set_divisor(&d, 6);
        CHECK_EQ_INT(d.divisor, 6);
        CHECK_EQ_INT(d.frames, 12);                       /* budget survives */
        CHECK_EQ_INT(d.start_us, start_before);           /* clock untouched */
        CHECK_EQ_INT(d.frame_us, KOBOY_FRAME_US);         /* rate untouched */

        /* And it takes effect on the NEXT tick, which is the whole claim the
           menu entry makes. Counting presented frames rather than reading
           d.divisor back is what makes this able to tell 3 from 6: frame 12
           presents under both (12 % 3 == 12 % 6 == 0), and only the twelve
           frames after it separate them -- 4 presented at 3, 2 at 6. */
        int presented = 0;
        for (int i = 0; i < 12; i++) presented += pacer_tick(&d, 0) ? 1 : 0;
        CHECK_EQ_INT(presented, 2);

        pacer_set_divisor(&d, 3);
        presented = 0;
        for (int i = 0; i < 12; i++) presented += pacer_tick(&d, 0) ? 1 : 0;
        CHECK_EQ_INT(presented, 4);

        /* THE DIVIDE-BY-ZERO GUARD, live. pacer_tick computes
           frames % divisor, so a 0 reaching the pacer is not a wrong picture,
           it is a SIGFPE. config_present_divisor_ok rejects such a value
           before it ever gets here; this is the second of the two guards, and
           neither should be deleted on the grounds that the other exists. */
        pacer_set_divisor(&d, 0);
        CHECK_EQ_INT(d.divisor, 1);
        pacer_set_divisor(&d, -7);
        CHECK_EQ_INT(d.divisor, 1);
        presented = 0;
        for (int i = 0; i < 5; i++) presented += pacer_tick(&d, 0) ? 1 : 0;
        CHECK_EQ_INT(presented, 5);                       /* 1 means every frame */
    }

    /* ==================================================== AREA-AWARE PACING
       present_divisor paces by FRAME COUNT alone, so a two-tile sprite move
       and a whole-screen scroll were asked of the panel at exactly the same
       rate although they cost it an order of magnitude apart. On a scroll
       that meant starting a new full-area update several times faster than
       the panel could finish one, and the owner's video shows the result:
       the game area swinging between mean brightness 90 and 166 frame after
       frame, a transition photographed mid-flight. These are the assertions
       that the fix throttles the case that needs it and ONLY that case. */
    {
        /* ---- the model itself, which is affine in area ---- */
        /* Both halves zero is how the whole feature is turned off, and it has
           to be exactly zero rather than nearly so: pacer_presented reads a 0
           as "clear the hold", and a 1 would leave a hold armed forever. */
        CHECK_EQ_INT(pacer_settle_us(0, 0, 576000, 576000), 0);
        /* base alone, at any area at all */
        CHECK_EQ_INT(pacer_settle_us(20000, 0, 576000, 576000), 20000);
        CHECK_EQ_INT(pacer_settle_us(20000, 0, 1, 576000), 20000);
        /* full rect charges base + full; half the rect charges base + full/2.
           The half-area case is the one that can tell an affine model from a
           flat one -- a fixed per-update charge returns 120000 for both. */
        CHECK_EQ_INT(pacer_settle_us(20000, 100000, 576000, 576000), 120000);
        CHECK_EQ_INT(pacer_settle_us(20000, 100000, 288000, 576000), 70000);
        CHECK_EQ_INT(pacer_settle_us(20000, 100000,  57600, 576000), 30000);
        /* A small update rounds DOWN toward base, and does not round UP to
           it: multiplying before dividing is what keeps a 1/576000 update
           from quantising to a full rect's charge (or to nothing). */
        CHECK_EQ_INT(pacer_settle_us(20000, 100000, 5760, 576000), 21000);

        /* THE OVERLAP CLAMP, live. video_split_dirty may emit rects that
           overlap -- main.c says so at the call site -- so the summed dirty
           area CAN exceed the game rect. Without the clamp a doubly-counted
           full screen charges twice a full screen's settle, which is a hold
           the panel never asked for and a visible stall on exactly the
           content that splits most. */
        CHECK_EQ_INT(pacer_settle_us(20000, 100000, 1152000, 576000), 120000);

        /* Degenerate areas: an unknown or empty rect is charged base alone
           rather than dividing by zero. */
        CHECK_EQ_INT(pacer_settle_us(20000, 100000, 576000, 0), 20000);
        CHECK_EQ_INT(pacer_settle_us(20000, 100000, 576000, -1), 20000);
        CHECK_EQ_INT(pacer_settle_us(20000, 100000, 0, 576000), 20000);
        CHECK_EQ_INT(pacer_settle_us(20000, 100000, -5, 576000), 20000);

        /* ---- the hold, against a synthetic clock ---- */
        /* One presented frame at a full rect, then the divisor-eligible
           frames that follow it are vetoed until the settle has elapsed.
           divisor 4 at 16742 us/frame is 66968 us between eligible frames, so
           a 120000 us hold takes out exactly one of them and the run presents
           at an effective divisor of 8 -- which is precisely the setting the
           owner reached by hand, arrived at here without their touching it. */
        koboy_pacer a;
        pacer_init(&a, 0, 4, KOBOY_FRAME_US);
        CHECK_EQ_INT(a.hold_until_us, 0);
        CHECK_EQ_INT(a.held, 0);

        int presented = 0;
        for (int i = 0; i < 32; i++) {
            uint64_t now = (uint64_t)i * KOBOY_FRAME_US;
            if (pacer_tick(&a, now)) {
                presented++;
                pacer_presented(&a, now,
                                pacer_settle_us(20000, 100000, 576000, 576000));
            }
        }
        /* 32 frames, divisor 4, a 120000 us hold against 66968 us of
           divisor: every present is followed by a hold that outlasts the
           divisor's own gap, so the delivered cadence is the HOLD's, 133936 us
           (frames 0, 8, 16, 24) -- an effective divisor of 8, which is exactly
           the setting the owner reached by hand, arrived at here without their
           touching it and only while the content demands it. */
        CHECK_EQ_INT(presented, 4);
        CHECK_EQ_INT(a.held, 16);
        /* And the frame counter advanced on EVERY frame including the held
           ones -- main.c's bounded-run test is `pace.frames >= frame_limit`,
           so a held frame that did not count would make --frames N run
           forever on exactly the content that holds most. */
        CHECK_EQ_INT(a.frames, 32);

        /* SMALL UPDATES ARE LEFT ALONE. Same run, same constants, but a
           sprite-sized dirty area (2% of the rect, ~30000 us) that never
           reaches the 66968 us between eligible frames. Nothing is held and
           the count is bit-identical to a build with no hold at all -- which
           is the regression guard for the 1-bit sprite fix the owner has
           already confirmed on the panel. */
        koboy_pacer b;
        pacer_init(&b, 0, 4, KOBOY_FRAME_US);
        presented = 0;
        for (int i = 0; i < 32; i++) {
            uint64_t now = (uint64_t)i * KOBOY_FRAME_US;
            if (pacer_tick(&b, now)) {
                presented++;
                pacer_presented(&b, now,
                                pacer_settle_us(20000, 100000, 11520, 576000));
            }
        }
        CHECK_EQ_INT(presented, 8);         /* 32 / 4, untouched */
        CHECK_EQ_INT(b.held, 0);

        /* PRESENT_DIVISOR STAYS A CEILING. With the model disabled the hold
           can never make the rate higher, and with it enabled it can never
           make the rate higher either: 8 is what divisor 4 gives over 32
           frames and nothing above produced more. This is the assertion that
           would fail if the hold were ever turned into a "present as soon as
           settled" timer, which would ignore the user's setting entirely. */
        CHECK(presented <= 8);

        /* THE VETO IS ONE FRAME DEEP, not a queue, and it costs NO PHASE: once
           the hold expires the very next frame presents. The pacer does not owe
           the panel the frames it skipped and must not catch up (catching up is
           how a scroll turns into a burst), and it does not wait for a lattice
           point either.

           THIS DISTINGUISHES THE GAP GATE FROM THE MODULO IT REPLACED. A
           100000 us hold against divisor 4 expires during frame 6 (100452 us).
           Under `frames % divisor` the next eligible frame is 8, at 133936 us
           -- a third of the delivered rate given up to phase alone, on exactly
           the content with least to spare. Under a minimum GAP frame 6
           presents: six frames since the last one, and six is more than
           four. */
        koboy_pacer c;
        pacer_init(&c, 0, 4, KOBOY_FRAME_US);
        CHECK(pacer_tick(&c, 0));                       /* frame 0 presents */
        pacer_presented(&c, 0, 100000);                 /* held until 100000 */
        /* frames 1-3 are inside the divisor's gap: refused by the DIVISOR,
           and held must not count them -- conflating the two would make the
           run summary's settle-held number meaningless. */
        for (int i = 1; i < 4; i++) CHECK(!pacer_tick(&c, (uint64_t)i * KOBOY_FRAME_US));
        CHECK_EQ_INT(c.held, 0);
        CHECK(!pacer_tick(&c, 4ull * KOBOY_FRAME_US));  /* past the gap at 66968: held */
        CHECK(!pacer_tick(&c, 5ull * KOBOY_FRAME_US));  /* 83710: still held */
        CHECK_EQ_INT(c.held, 2);
        CHECK(pacer_tick(&c, 6ull * KOBOY_FRAME_US));   /* 100452 >= 100000: through */
        CHECK_EQ_INT(c.held, 2);                        /* and nothing extra held */

        /* THE GATE ADVANCES ON THE TICK, NOT ON THE PRESENTATION, and this is
           the case that proves it: main.c does NOT call pacer_presented for a
           frame the core marked unchanged -- zero dirty rects, an early exit,
           nothing sent to the panel. If the gate waited for pacer_presented it
           would stay open, and video_submit_rects (the measured 17 ms
           bottleneck) would run on every core frame instead of every fourth
           for as long as the screen was static. Frame 6 above presented and
           deliberately gets NO pacer_presented call here. */
        for (int i = 7; i < 10; i++) CHECK(!pacer_tick(&c, (uint64_t)i * KOBOY_FRAME_US));
        CHECK(pacer_tick(&c, 10ull * KOBOY_FRAME_US));  /* four frames after 6 */

        /* A settle of 0 CLEARS an armed hold rather than leaving it. This is
           the path a build with the model disabled takes every single frame,
           and if it did not clear, one non-zero charge before the setting was
           read would stall the pacer for the rest of the session. */
        pacer_presented(&c, 10ull * KOBOY_FRAME_US, 0);
        CHECK_EQ_INT(c.hold_until_us, 0);
        /* Frames 11, 12 and 13 are inside the divisor's gap whatever the hold
           says -- asserted, so that "the hold was cleared" cannot be mistaken
           for "the divisor stopped applying". */
        for (int i = 11; i < 14; i++) CHECK(!pacer_tick(&c, (uint64_t)i * KOBOY_FRAME_US));
        CHECK(pacer_tick(&c, 14ull * KOBOY_FRAME_US));
        CHECK_EQ_INT(c.held, 2);

        /* pacer_init clears the hold, so a mid-session ROM switch cannot
           inherit the previous game's outstanding panel work. */
        pacer_presented(&c, 1000000, 500000);
        CHECK(c.hold_until_us != 0);
        pacer_init(&c, 1000000, 3, KOBOY_FRAME_US);
        CHECK_EQ_INT(c.hold_until_us, 0);
        CHECK_EQ_INT(c.held, 0);
        CHECK_EQ_INT(c.last_settle_us, 0);
        CHECK_EQ_INT(c.next_frame, 0);       /* frame 0 of a session presents */

        /* THE SHIPPED DEFAULTS, checked against the two properties the whole
           design rests on, so that retuning the constants cannot silently
           break either. Both are stated against the SHIPPED divisor's
           interval (KOBOY_PRESENT_DIVISOR_DEFAULT frames of 16742 us):

           1. a sprite-sized update must not be held at all, or the fix the
              owner already confirmed on the panel regresses; and
           2. a full-rect update MUST be held, or this whole task is a no-op
              on the content that produced the complaint. */
        {
            uint32_t base = (uint32_t)KOBOY_SETTLE_BASE_MS_DEFAULT * 1000u;
            uint32_t full = (uint32_t)KOBOY_SETTLE_FULL_MS_DEFAULT * 1000u;
            uint64_t interval = (uint64_t)KOBOY_PRESENT_DIVISOR_DEFAULT * KOBOY_FRAME_US;
            /* 2% of the rect: a Game Boy sprite crossing an 8x8 grid dirties
               about this much, and koboy's own on-device diff measured Dig
               Dug, Donkey Kong and Ms. Pac-Man at 1.5 to 2.6%. */
            CHECK(pacer_settle_us(base, full, 11520, 576000) <= interval);
            CHECK(pacer_settle_us(base, full, 576000, 576000) > interval);
        }
    }
})
