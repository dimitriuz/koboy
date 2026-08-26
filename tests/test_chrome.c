#include "test.h"
#include "pgm.h"
#include "chrome.h"
#include "config.h"
#include "input.h"
#include <stdlib.h>

static int render(koboy_config *c, int W, int H, uint8_t *fb, koboy_profile *p)
{
    config_resolve_profile(p, c, W, H);
    memset(fb, 0x7F, (size_t)W * H);
    chrome_render(fb, W, p, &c->layout);
    return 1;
}

/* Press one finger at a panel coordinate, read the resulting buttons, lift it
   again. Goes through input_feed rather than reimplementing the hit tests, so
   what is asserted is the zone geometry the emulator actually uses. The caller
   must have installed an identity touch transform (raw_max == panel - 1), which
   makes scale_axis a no-op and the probe pixel-exact. */
static uint16_t touch_probe(koboy_input *in, int x, int y)
{
    koboy_ev down[5] = {
        { KOBOY_EV_ABS, KOBOY_ABS_MT_SLOT,        0 },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, 1 },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_X,  x },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_Y,  y },
        { KOBOY_EV_SYN, 0, 0 },
    };
    koboy_ev up[2] = {
        { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, -1 },
        { KOBOY_EV_SYN, 0, 0 },
    };
    input_feed(in, down, 5);
    uint16_t b = input_state(in)->buttons;
    input_feed(in, up, 2);
    return b;
}

TEST_MAIN({
    koboy_config c; config_defaults(&c);
    static uint8_t fb[1440 * 1920];
    koboy_profile p;

    /* Libra 2 */
    render(&c, 1264, 1680, fb, &p);
    CHECK(pgm_compare_golden("chrome_1264x1680", fb, 1264, 1680, 1264) == 1);

    /* CRITICAL: chrome must not paint inside the game rect, or the first
       frame would be drawn over by static art. */
    int intruded = 0;
    for (int y = p.game_y; y < p.game_y + p.game_h; y++)
        for (int x = p.game_x; x < p.game_x + p.game_w; x++)
            if (fb[y * 1264 + x] != 0x7F) intruded++;
    CHECK_EQ_INT(intruded, 0);

    /* something was actually drawn outside it */
    int painted = 0;
    for (int y = 0; y < 1680; y++)
        for (int x = 0; x < 1264; x++)
            if (fb[y * 1264 + x] != 0x7F) painted++;
    CHECK(painted > 10000);

    /* the same code adapts to a 6in Clara panel with no changes */
    render(&c, 1072, 1448, fb, &p);
    CHECK(pgm_compare_golden("chrome_1072x1448", fb, 1072, 1448, 1072) == 1);
    intruded = 0;
    for (int y = p.game_y; y < p.game_y + p.game_h; y++)
        for (int x = p.game_x; x < p.game_x + p.game_w; x++)
            if (fb[y * 1072 + x] != 0x7F) intruded++;
    CHECK_EQ_INT(intruded, 0);

    /* Guard-band tests for bounds clamping. Three scenarios:
       1. Symmetric overflow: proves coarse "skip entire line" checks work
       2. Horizontal overflow: exercises hline's x-clamping branches
       3. Vertical overflow: exercises vline's y-clamping branches */

    const int GUARD = 64;
    const uint8_t SENTINEL = 0x42;

    /* Test 1: Symmetric overflow (all four margins tight) */
    int TW = 400, TH = 500;
    size_t buf_size = (size_t)(TW + 2 * GUARD) * (TH + 2 * GUARD);
    uint8_t *guarded = malloc(buf_size);
    memset(guarded, SENTINEL, buf_size);
    uint8_t *panel_start = guarded + (size_t)GUARD * (TW + 2 * GUARD) + GUARD;

    koboy_profile sym;
    memset(&sym, 0, sizeof sym);
    sym.scale = 1;
    sym.panel_w = TW;
    sym.panel_h = TH;
    sym.game_x = 2;
    sym.game_y = 2;
    sym.game_w = 396;   /* TW - game_x - 2 */
    sym.game_h = 496;   /* TH - game_y - 2 */
    /* Bezel from (1,1) size (398,498); frame's loop runs i = 0..5 (t=6), so
       the max offset is 5 and the far edge lands at x/y + w/h - 1 + 5:
       Leftmost: x = 1 - 5 = -4 (OUT); Topmost: y = 1 - 5 = -4 (OUT)
       Rightmost: x = 1 + 398 - 1 + 5 = 403 >= 400 (OUT)
       Bottommost: y = 1 + 498 - 1 + 5 = 503 >= 500 (OUT)
       All four edges overflow; both horizontal and vertical writes need clamping. */

    chrome_render(panel_start, TW + 2 * GUARD, &sym, &c.layout);

    int corrupted = 0;
    uint8_t *p_start = guarded;
    for (size_t i = 0; i < buf_size; i++) {
        int x = (int)(i % (size_t)(TW + 2 * GUARD));
        int y = (int)(i / (size_t)(TW + 2 * GUARD));
        int is_guard = (x < GUARD || x >= TW + GUARD || y < GUARD || y >= TH + GUARD);
        if (is_guard && p_start[i] != SENTINEL) corrupted++;
    }
    CHECK_EQ_INT(corrupted, 0);

    /* Test 2: Horizontal overflow (exercises hline x-clamping) */
    int TW2 = 400, TH2 = 500;
    memset(guarded, SENTINEL, buf_size);
    panel_start = guarded + (size_t)GUARD * (TW2 + 2 * GUARD) + GUARD;

    koboy_profile horiz;
    memset(&horiz, 0, sizeof horiz);
    horiz.scale = 1;
    horiz.panel_w = TW2;
    horiz.panel_h = TH2;
    horiz.game_x = 2;       /* tight left margin */
    horiz.game_y = 50;      /* comfortable top margin, far from edge */
    horiz.game_w = 396;     /* extends past right edge */
    horiz.game_h = 200;     /* reasonable height */
    /* Bezel from (1,49) size (398,202); frame's loop runs i = 0..5 (t=6), so
       the max offset is 5 and the far edge lands at x/y + w/h - 1 + 5:
       y-range: 49 - 5 = 44 to 49 + 202 - 1 + 5 = 255 (both in [0, 500))
       x-range: 1 - 5 = -4 to 1 + 398 - 1 + 5 = 403 (extends beyond [0, 400))
       hline is called with valid y-values but out-of-bounds x-ranges.
       Requires x0 < 0 and x1 >= W clamping to stay in bounds. */

    chrome_render(panel_start, TW2 + 2 * GUARD, &horiz, &c.layout);

    corrupted = 0;
    for (size_t i = 0; i < buf_size; i++) {
        int x = (int)(i % (size_t)(TW2 + 2 * GUARD));
        int y = (int)(i / (size_t)(TW2 + 2 * GUARD));
        int is_guard = (x < GUARD || x >= TW2 + GUARD || y < GUARD || y >= TH2 + GUARD);
        if (is_guard && p_start[i] != SENTINEL) corrupted++;
    }
    CHECK_EQ_INT(corrupted, 0);

    /* Test 3: Vertical overflow (exercises vline y-clamping) */
    int TW3 = 400, TH3 = 500;
    memset(guarded, SENTINEL, buf_size);
    panel_start = guarded + (size_t)GUARD * (TW3 + 2 * GUARD) + GUARD;

    koboy_profile vert;
    memset(&vert, 0, sizeof vert);
    vert.scale = 1;
    vert.panel_w = TW3;
    vert.panel_h = TH3;
    vert.game_x = 50;       /* comfortable left margin, far from edge */
    vert.game_y = 2;        /* tight top margin */
    vert.game_w = 200;      /* reasonable width */
    vert.game_h = 496;      /* extends past bottom edge */
    /* Bezel from (49,1) size (202,498) [h = game_h + 2 = 496 + 2]; frame's
       loop runs i = 0..5 (t=6), so the max offset is 5 and the far edge
       lands at x/y + w/h - 1 + 5:
       x-range: 49 - 5 = 44 to 49 + 202 - 1 + 5 = 255 (both in [0, 400))
       y-range: 1 - 5 = -4 to 1 + 498 - 1 + 5 = 503 (extends beyond [0, 500))
       vline is called with valid x-values but out-of-bounds y-ranges.
       Requires y0 < 0 and y1 >= H clamping to stay in bounds. */

    chrome_render(panel_start, TW3 + 2 * GUARD, &vert, &c.layout);

    corrupted = 0;
    for (size_t i = 0; i < buf_size; i++) {
        int x = (int)(i % (size_t)(TW3 + 2 * GUARD));
        int y = (int)(i / (size_t)(TW3 + 2 * GUARD));
        int is_guard = (x < GUARD || x >= TW3 + GUARD || y < GUARD || y >= TH3 + GUARD);
        if (is_guard && p_start[i] != SENTINEL) corrupted++;
    }
    CHECK_EQ_INT(corrupted, 0);

    /* Test 4: chrome_render's OWN clamps, with the resolver taken out of the
       picture. Until now config_resolve_profile was the only thing keeping the
       two background memsets in chrome_render in range, and it lives in another
       file -- so a resolver change (there is one, right below) could reintroduce
       a heap smash here with every test still green. These two profiles violate
       the invariant directly: a negative game_x, and a game rect running off the
       right edge. Both lengths are plain ints cast to size_t, so without the
       local clamps each memset gets a length near SIZE_MAX. Unclamped, this does
       not "fail" -- it flattens the heap and the test dies. */
    memset(guarded, SENTINEL, buf_size);
    panel_start = guarded + (size_t)GUARD * (TW + 2 * GUARD) + GUARD;

    koboy_profile neg;
    memset(&neg, 0, sizeof neg);
    neg.scale = 1; neg.panel_w = TW; neg.panel_h = TH;
    neg.game_x = -20;          /* left band width is NEGATIVE */
    neg.game_y = 100;
    neg.game_w = 100;
    neg.game_h = 100;
    chrome_render(panel_start, TW + 2 * GUARD, &neg, &c.layout);

    corrupted = 0;
    for (size_t i = 0; i < buf_size; i++) {
        int x = (int)(i % (size_t)(TW + 2 * GUARD));
        int y = (int)(i / (size_t)(TW + 2 * GUARD));
        int is_guard = (x < GUARD || x >= TW + GUARD || y < GUARD || y >= TH + GUARD);
        if (is_guard && p_start[i] != SENTINEL) corrupted++;
    }
    CHECK_EQ_INT(corrupted, 0);

    memset(guarded, SENTINEL, buf_size);
    panel_start = guarded + (size_t)GUARD * (TW + 2 * GUARD) + GUARD;

    koboy_profile over;
    memset(&over, 0, sizeof over);
    over.scale = 1; over.panel_w = TW; over.panel_h = TH;
    over.game_x = 350;
    over.game_y = 100;
    over.game_w = 200;         /* game_x + game_w = 550 > W: W - rx is NEGATIVE */
    over.game_h = 100;
    chrome_render(panel_start, TW + 2 * GUARD, &over, &c.layout);

    corrupted = 0;
    for (size_t i = 0; i < buf_size; i++) {
        int x = (int)(i % (size_t)(TW + 2 * GUARD));
        int y = (int)(i / (size_t)(TW + 2 * GUARD));
        int is_guard = (x < GUARD || x >= TW + GUARD || y < GUARD || y >= TH + GUARD);
        if (is_guard && p_start[i] != SENTINEL) corrupted++;
    }
    CHECK_EQ_INT(corrupted, 0);

    /* And the upper bound, which is the case that misbehaves PREDICTABLY and so
       is the one this file leans on. game_x past the right edge makes the left
       band 500 px wide on a 400 px panel, so without `if (lx > W) lx = W` every
       game row spills exactly 100 bytes into the right guard: 10,000 bytes,
       every run, on every libc.
       The two underflow profiles above hand memset a length near SIZE_MAX
       instead, and what that overwrites is an implementation detail -- glibc
       2.41 on x86-64 writes a short block just *below* the destination, which
       lands in the guard band for the negative game_x (10,800 bytes) and inside
       the panel for the overhanging rect. That unpredictability is the argument
       for the clamp, not against the test: the contract is "the length is
       clamped", not "the crash is reproducible". */
    memset(guarded, SENTINEL, buf_size);
    panel_start = guarded + (size_t)GUARD * (TW + 2 * GUARD) + GUARD;

    koboy_profile wide;
    memset(&wide, 0, sizeof wide);
    wide.scale = 1; wide.panel_w = TW; wide.panel_h = TH;
    wide.game_x = 500;         /* left band is WIDER than the panel */
    wide.game_y = 100;
    wide.game_w = -200;        /* keeps rx = 300 in range, isolating the lx clamp */
    wide.game_h = 100;
    chrome_render(panel_start, TW + 2 * GUARD, &wide, &c.layout);

    corrupted = 0;
    for (size_t i = 0; i < buf_size; i++) {
        int x = (int)(i % (size_t)(TW + 2 * GUARD));
        int y = (int)(i / (size_t)(TW + 2 * GUARD));
        int is_guard = (x < GUARD || x >= TW + GUARD || y < GUARD || y >= TH + GUARD);
        if (is_guard && p_start[i] != SENTINEL) corrupted++;
    }
    CHECK_EQ_INT(corrupted, 0);

    free(guarded);

    /* The clamp itself, asserted directly rather than by watching for an
       overrun. This exists because the sentinel guard band above CANNOT cover
       the right-hand band: with the `rx > W` clamp removed, `W - rx` is a
       length near SIZE_MAX, and what glibc 2.41/x86-64 does with that is write
       a short block near the destination -- which for an overhanging rect lands
       inside the panel, not in the guard. The `over` profile above therefore
       reports zero corruption whether or not that clamp is present, which was
       verified by deleting the clamp and watching all 222 checks still pass.
       A test that cannot fail is not coverage, so the contract gets asserted
       where it is deterministic: chrome_bands' clamped output, which is the
       same arithmetic chrome_render depends on, on every libc, with no
       undefined behaviour needed to observe it. Remove either clamp in
       chrome_bands and the matching CHECK below fails. */
    koboy_profile b;
    int lx = -1, rx = -1;
    memset(&b, 0, sizeof b);
    b.scale = 1; b.panel_w = 400; b.panel_h = 300; b.game_y = 100; b.game_h = 100;

    /* the shipped, invariant-respecting case passes through untouched */
    b.game_x = 100; b.game_w = 200;
    chrome_bands(&b, b.panel_w, &lx, &rx);
    CHECK_EQ_INT(lx, 100);
    CHECK_EQ_INT(rx, 300);

    /* negative game_x: the left band must not become a huge length */
    b.game_x = -20; b.game_w = 100;
    chrome_bands(&b, b.panel_w, &lx, &rx);
    CHECK_EQ_INT(lx, 0);
    CHECK_EQ_INT(rx, 80);

    /* rect overhanging the right edge: rx must stop at W so that W - rx is 0,
       not negative. This is the case the guard band cannot see. */
    b.game_x = 350; b.game_w = 200;
    chrome_bands(&b, b.panel_w, &lx, &rx);
    CHECK_EQ_INT(rx, 400);
    CHECK_EQ_INT(b.panel_w - rx, 0);

    /* game_x past the right edge: the left band must stop at W */
    b.game_x = 500; b.game_w = -200;
    chrome_bands(&b, b.panel_w, &lx, &rx);
    CHECK_EQ_INT(lx, 400);
    CHECK_EQ_INT(rx, 300);

    /* a rect entirely left of the panel: both must floor at 0 */
    b.game_x = -600; b.game_w = 100;
    chrome_bands(&b, b.panel_w, &lx, &rx);
    CHECK_EQ_INT(lx, 0);
    CHECK_EQ_INT(rx, 0);

    /* Test 5: chrome.h's contract at EVERY scale the resolver can choose, on
       every panel spec §3 supports -- not just at the shipped default 5, which
       is all it was ever checked at. scale = 0 is documented in koboy.ini as a
       supported setting ("auto-fit to the largest integer scale that fits"), and
       auto-fit used to reserve only the 8 px bezel margin: on the Libra 2 it
       chose 7 and put 15,677 chrome pixels inside the game rect. The touch zones
       stay live under a rect drawn over them, so the second loop matters as much
       as the first -- tapping the lower playfield was pressing A. */
    static const int panels[][2] = {
        { 1072, 1448 },   /* Clara family, 6"    */
        { 1264, 1680 },   /* Libra family, 7"    */
        { 1404, 1872 },   /* Elipsa family, 10.3" */
        { 1440, 1920 },   /* Sage, 8"            */
    };
    static const int scales[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 99 };

    int chrome_intruded = 0, zone_hits = 0, combos = 0;
    for (size_t pi = 0; pi < sizeof panels / sizeof panels[0]; pi++) {
        const int W = panels[pi][0], H = panels[pi][1];
        for (size_t si = 0; si < sizeof scales / sizeof scales[0]; si++) {
            koboy_config cc; config_defaults(&cc);
            cc.scale = scales[si];
            koboy_profile q;
            CHECK(config_resolve_profile(&q, &cc, W, H));
            combos++;

            memset(fb, 0x7F, (size_t)W * H);
            chrome_render(fb, W, &q, &cc.layout);
            /* The battery lamp is chrome too, and it is drawn from the same
               places chrome_render is -- but this sweep did not call it, which
               is exactly how a BATTERY label that escaped the game-rect guard
               shipped. Both a full lamp (the largest fill) and an unknown
               battery (no fill at all, label only) go into the same buffer, so
               the intrusion count below covers every shape it can take. */
            chrome_render_battery(fb, W, &q, &cc.layout, 100);
            chrome_render_battery(fb, W, &q, &cc.layout, -1);

            int bad = 0;
            for (int y = q.game_y; y < q.game_y + q.game_h; y++)
                for (int x = q.game_x; x < q.game_x + q.game_w; x++)
                    if (fb[(size_t)y * W + x] != 0x7F) bad++;
            if (bad) {
                fprintf(stderr, "  chrome/battery inside game rect: %dx%d scale=%d -> "
                        "resolved %d, %d px\n", W, H, scales[si], q.scale, bad);
                chrome_intruded += bad;
            }

            koboy_input *in = input_create(&cc, &q);
            CHECK(in != NULL);
            input_set_touch_transform(in, W - 1, H - 1, false, false, false);

            /* POSITIVE CONTROL, and it is not optional: without it a probe that
               silently returned 0 for every coordinate -- a broken transform, a
               missing SYN -- would make the whole sweep below vacuous, which is
               the exact failure mode this branch has already shipped three
               times. The A button and the Start pill must still be reachable. */
            CHECK(touch_probe(in, cc.layout.a_cx * W / 1000,
                              cc.layout.a_cy * H / 1000) == KOBOY_BTN_A);
            CHECK(touch_probe(in, cc.layout.start_cx * W / 1000,
                              cc.layout.start_cy * H / 1000) == KOBOY_BTN_START);

            int hits = 0;
            /* Coarse grid over the whole rect. Step 8 cannot skip a zone: the
               smallest one is a start/select pill, ~79 px tall on the narrowest
               supported panel. */
            for (int y = q.game_y; y < q.game_y + q.game_h; y += 8)
                for (int x = q.game_x; x < q.game_x + q.game_w; x += 8)
                    if (touch_probe(in, x, y)) hits++;
            /* Dense along the bottom edge, where an overlap starts, so a zone
               poking in by fewer than 8 rows cannot slip between samples. */
            int y0 = q.game_y + q.game_h - 16;
            if (y0 < q.game_y) y0 = q.game_y;
            for (int y = y0; y < q.game_y + q.game_h; y++)
                for (int x = q.game_x; x < q.game_x + q.game_w; x++)
                    if (touch_probe(in, x, y)) hits++;
            if (hits) {
                fprintf(stderr, "  touch zone inside game rect: %dx%d scale=%d "
                        "-> resolved %d, %d hits\n", W, H, scales[si], q.scale, hits);
                zone_hits += hits;
            }
            input_destroy(in);
        }
    }
    CHECK_EQ_INT(combos, 52);
    CHECK_EQ_INT(chrome_intruded, 0);
    CHECK_EQ_INT(zone_hits, 0);

    /* GENERAL guard, carried forward from Task 8, REDESIGNED after a review
       found the first version too weak. That version compared each control's
       rendered pixels against chrome_controls_top()'s RETURN VALUE, which is
       a min() over seven terms (two for the d-pad's arms, one each for A, B,
       Start, Select, Menu). Deleting a non-binding term leaves the min()
       unchanged, so nothing the pixel probe looked at moved -- the review
       deleted each of the seven terms in turn and only one (`a`, the term
       that happened to be the binding minimum on the one profile tested) was
       caught. The other six were provably unguarded. A second, independent
       bug in that version: the `menu` column probe (750..950 permille) and
       the `a` probe (745..915 permille) overlap, so the menu check was
       silently reading A's pixels rather than the MENU box's.

       The fix: compute the SAME seven-term minimum independently here, from
       koboy_layout and the panel size, and assert EQUALITY with
       chrome_controls_top()'s actual return value -- not an inequality
       against a pixel sample. This deliberately duplicates chrome.c's min2
       chain. That duplication IS the guard: a future edit to the chain
       either updates this expected value to match (which is a deliberate,
       visible part of the same diff) or it does not, and then EVERY one of
       the seven terms makes this fail, not just whichever one happens to be
       binding on whichever panel is tested -- because any single term
       increasing means chrome_controls_top()'s actual minimum can only stay
       the same or drop, while this independently-computed value tracks
       whatever the test author believes each control's real top edge is.
       There is no pixel sampling left to overlap, either.

       Run at all four supported panel sizes, not only the Libra 2 this file
       otherwise defaults to: Clara (1072x1448) is the one where the margin
       between the bezel and chrome_controls_top is tightest (nine pixels
       before this task's fix-round redesign of the bezel/decoration sizing),
       which is exactly where a future strapline or bezel change would erode
       it first without this catching it. */
    {
        static const int panels[][2] = {
            { 1072, 1448 },   /* Clara family, 6"     -- tightest margin  */
            { 1264, 1680 },   /* Libra family, 7"                        */
            { 1404, 1872 },   /* Elipsa family, 10.3"                    */
            { 1440, 1920 },   /* Sage, 8"                                */
        };
        koboy_config c; config_defaults(&c);
        const koboy_layout *l = &c.layout;

        for (size_t pi = 0; pi < sizeof panels / sizeof panels[0]; pi++) {
            int W = panels[pi][0], H = panels[pi][1];

            int dcy = l->dpad_cy * H / 1000, dr = l->dpad_r * W / 1000;
            int arm = dr / 3;
            int expected = dcy - dr - 1;
            if (dcy - arm / 2 - 1 < expected) expected = dcy - arm / 2 - 1;
            int a_top = l->a_cy * H / 1000 - l->a_r * W / 1000;
            if (a_top < expected) expected = a_top;
            int b_top = l->b_cy * H / 1000 - l->b_r * W / 1000;
            if (b_top < expected) expected = b_top;
            int start_top = l->start_cy * H / 1000 - (l->start_h * H / 1000) / 2;
            if (start_top < expected) expected = start_top;
            int select_top = l->select_cy * H / 1000 - (l->select_h * H / 1000) / 2;
            if (select_top < expected) expected = select_top;
            int menu_top = l->menu_cy * H / 1000 - (l->menu_h * H / 1000) / 2;
            if (menu_top < expected) expected = menu_top;
            if (expected < 0) expected = 0;

            int got = chrome_controls_top(l, W, H);
            if (got != expected)
                fprintf(stderr, "  %dx%d: chrome_controls_top=%d, expected %d\n",
                        W, H, got, expected);
            CHECK_EQ_INT(got, expected);
        }
    }

    /* The default-layout, 4-panel check above is necessary but not
       sufficient: on the SHIPPED layout, `a` beats every other term on all
       four supported panels by a comfortable margin (measured -- see the
       task report's table), which means deleting any of the other six terms
       leaves chrome_controls_top()'s return value completely unchanged and
       an equality check against it, however it is computed, cannot observe
       the deletion. That is not a flaw in comparing with equality instead of
       inequality; it is a property of min(): whichever comparison operator
       is used, a term that is never the actual minimum is invisible to any
       test that only inspects the aggregate result on a layout where it
       never wins.

       So each term is isolated in its own synthetic layout instead: every
       OTHER control is parked far down the panel with a negligible radius
       (cy = 990 permille, r/half-extent = 5 permille), and the one control
       under test is brought up to an unmistakably dominant position (cy =
       300 permille, a generous size). With every rival out of contention,
       chrome_controls_top()'s return value can only equal what that term's
       own formula produces (if the term is present) or something larger --
       whatever the next surviving term computes -- if it has been deleted.
       That is what makes each of these individually decisive. */
    {
        const int W = 1264, H = 1680;

        struct { const char *name; int dpad, a, b, start, select, menu; } cases[] = {
            { "dpad",   1, 0, 0, 0, 0, 0 },
            { "a",      0, 1, 0, 0, 0, 0 },
            { "b",      0, 0, 1, 0, 0, 0 },
            { "start",  0, 0, 0, 1, 0, 0 },
            { "select", 0, 0, 0, 0, 1, 0 },
            { "menu",   0, 0, 0, 0, 0, 1 },
        };
        for (size_t ci = 0; ci < sizeof cases / sizeof cases[0]; ci++) {
            koboy_layout l;
            memset(&l, 0, sizeof l);
            /* Parked: negligible footprint, far down the panel. */
            l.dpad_cx = l.a_cx = l.b_cx = l.start_cx = l.select_cx = l.menu_cx = 500;
            l.dpad_cy = l.a_cy = l.b_cy = l.start_cy = l.select_cy = l.menu_cy = 990;
            l.dpad_r = l.a_r = l.b_r = 5;
            l.start_w = l.select_w = l.menu_w = 10;
            l.start_h = l.select_h = l.menu_h = 10;

            /* Bring exactly one control up to a clearly dominant position. */
            if (cases[ci].dpad)   { l.dpad_cy   = 300; l.dpad_r   = 80; }
            if (cases[ci].a)      { l.a_cy      = 300; l.a_r      = 80; }
            if (cases[ci].b)      { l.b_cy      = 300; l.b_r      = 80; }
            if (cases[ci].start)  { l.start_cy  = 300; l.start_h  = 160; }
            if (cases[ci].select) { l.select_cy = 300; l.select_h = 160; }
            if (cases[ci].menu)   { l.menu_cy   = 300; l.menu_h   = 160; }

            int expected;
            if (cases[ci].dpad) {
                int dcy = l.dpad_cy * H / 1000, dr = l.dpad_r * W / 1000;
                int arm = dr / 3;
                int v = dcy - dr - 1, h = dcy - arm / 2 - 1;
                expected = v < h ? v : h;
            } else if (cases[ci].a) {
                expected = l.a_cy * H / 1000 - l.a_r * W / 1000;
            } else if (cases[ci].b) {
                expected = l.b_cy * H / 1000 - l.b_r * W / 1000;
            } else if (cases[ci].start) {
                expected = l.start_cy * H / 1000 - (l.start_h * H / 1000) / 2;
            } else if (cases[ci].select) {
                expected = l.select_cy * H / 1000 - (l.select_h * H / 1000) / 2;
            } else {
                expected = l.menu_cy * H / 1000 - (l.menu_h * H / 1000) / 2;
            }

            int got = chrome_controls_top(&l, W, H);
            if (got != expected)
                fprintf(stderr, "  isolated %s: chrome_controls_top=%d, expected %d\n",
                        cases[ci].name, got, expected);
            CHECK_EQ_INT(got, expected);
        }

        /* The d-pad's HORIZONTAL arm term cannot be isolated the same way,
           and this is a proven mathematical property of the current code,
           not a gap in this test: arm = dr / 3 inside chrome_controls_top,
           and dcy - dr - 1 <= dcy - arm / 2 - 1 for every dr >= 0 (equal
           only at the degenerate dr = 0, otherwise strictly less), because
           the vertical bar of a plus-shaped d-pad is always taller than the
           horizontal bar is thick. So the horizontal-arm term can never be
           chrome_controls_top's binding minimum under any layout, and
           deleting it changes the function's return value for NO input --
           verified exhaustively for dr in [0, 2000). No test that only
           observes chrome_controls_top's aggregate output -- this one
           included -- can distinguish "the term is present" from "the term
           is absent" for that reason alone; the two are behaviourally
           identical. It stays in the chain because it is what the actual
           frame() call for the horizontal arm draws (see the function's own
           doc comment), matching drawing to formula the same way the other
           six terms do, and because a future change to how `arm` relates to
           `dr` could make it stop being dominated -- at which point this
           very comment is what should be revisited. */
    }

    /* The faceplate must be LABELLED. Before this task A, B, Start and Select
       were four indistinguishable grey shapes. Labels are the difference
       between a faceplate and a set of blobs, and text.c exists so they are
       possible at all. */
    {
        koboy_config c; config_defaults(&c);
        koboy_profile p;
        const int W = 1264, H = 1680;
        static uint8_t fb2[1264 * 1680];
        config_resolve_profile(&p, &c, W, H);
        memset(fb2, 0x7F, (size_t)W * H);
        chrome_render(fb2, W, &p, &c.layout);

        /* The chrome is drawn with KOBOY_REFRESH_FULL, i.e. GC16 and sixteen
           levels. The four-level ceiling constrains the GAME RECT only, and
           before this task the faceplate used three values out of sixteen. A
           tonal ramp costs nothing at runtime. */
        int distinct = 0;
        int seen[256] = {0};
        for (size_t i = 0; i < (size_t)W * H; i++)
            if (!seen[fb2[i]]) { seen[fb2[i]] = 1; distinct++; }
        CHECK(distinct >= 5);

        /* Still never inside the game rect -- the contract that predates this
           redraw and survives it. */
        int intruded = 0;
        for (int y = p.game_y; y < p.game_y + p.game_h; y++)
            for (int x = p.game_x; x < p.game_x + p.game_w; x++)
                if (fb2[y * W + x] != 0x7F) intruded++;
        CHECK_EQ_INT(intruded, 0);

        CHECK(pgm_compare_golden("chrome_1264x1680", fb2, W, H, W) == 1);
    }

    /* The battery lamp renders from a percentage, and an unknown battery (-1)
       is a valid input rather than a crash: the SDL backend has no battery and
       an unseen Kobo may not expose one either. */
    {
        koboy_config c; config_defaults(&c);
        koboy_profile p;
        const int W = 1264, H = 1680;
        static uint8_t a[1264 * 1680], b[1264 * 1680];
        config_resolve_profile(&p, &c, W, H);

        memset(a, 0xFF, (size_t)W * H);
        chrome_render(a, W, &p, &c.layout);
        chrome_render_battery(a, W, &p, &c.layout, 100);

        memset(b, 0xFF, (size_t)W * H);
        chrome_render(b, W, &p, &c.layout);
        chrome_render_battery(b, W, &p, &c.layout, 5);

        /* Different levels must look different, or the indicator is a lie. */
        CHECK(memcmp(a, b, (size_t)W * H) != 0);

        /* Unknown must not write inside the game rect either. */
        memset(a, 0x7F, (size_t)W * H);
        chrome_render(a, W, &p, &c.layout);
        chrome_render_battery(a, W, &p, &c.layout, -1);
        int intruded = 0;
        for (int y = p.game_y; y < p.game_y + p.game_h; y++)
            for (int x = p.game_x; x < p.game_x + p.game_w; x++)
                if (a[y * W + x] != 0x7F) intruded++;
        CHECK_EQ_INT(intruded, 0);

        /* THE FILL IS A CHORD OF THE LAMP, not a band across its bounding
           box. Reported from the device as "the battery fill is a rectangle":
           the fill ran hline from cx - r to cx + r, so the level was painted
           the full width of the disc's bounding box and spilled outside the
           circle; the ring() drawn afterwards just outlined a circle over the
           overspill.

           Neither existing assertion could catch that. "100% and 5% render
           differently" is true of a rectangle too, and so is "unknown does not
           intrude". So: diff each level against the 0% render -- which cancels
           the disc, the ring and the label, leaving only the fill -- and
           require every differing pixel to lie within radius r of the lamp
           centre. */
        {
            int cx = p.game_x / 2;
            int cy = p.game_y + p.game_h / 2;
            int r  = W / 60; if (r < 4) r = 4;

            memset(a, 0xFF, (size_t)W * H);
            chrome_render_battery(a, W, &p, &c.layout, 0);

            static const int levels[] = { 5, 25, 50, 75, 100 };
            int outside = 0, inside = 0;
            for (size_t li = 0; li < sizeof levels / sizeof levels[0]; li++) {
                memset(b, 0xFF, (size_t)W * H);
                chrome_render_battery(b, W, &p, &c.layout, levels[li]);
                for (int y = 0; y < H; y++)
                    for (int x = 0; x < W; x++) {
                        if (a[(size_t)y * W + x] == b[(size_t)y * W + x]) continue;
                        long dx = x - cx, dy = y - cy;
                        if (dx * dx + dy * dy > (long)r * r) outside++;
                        else inside++;
                    }
            }
            if (outside)
                fprintf(stderr, "  battery fill outside the lamp: %d px\n", outside);
            CHECK_EQ_INT(outside, 0);
            /* Positive control: the diff is not empty, so "no pixel outside"
               is not passing because nothing was drawn at all. */
            CHECK(inside > 0);
        }
    }

    /* The battery lamp's game-rect guard, swept against the REAL resolver over
       a wide range of panel widths rather than only the four supported ones.

       chrome_render_battery guarded the DISC (r = W/60, five pixels on a small
       panel) and not the ~42 px "BATTERY" label beneath it. All four supported
       panels happen to be clear -- the Clara family by 48 px of margin -- so a
       sweep restricted to them proves nothing about the guard; it proves the
       four panels are lucky. Sweeping the resolver finds thousands of
       width/scale combinations where the label ran into the game rect.

       Only the game-rect columns are examined, and the lamp is drawn into a
       freshly filled buffer, so this measures the battery element alone. */
    {
        int intruding_combos = 0, checked = 0;
        static const int sweep_scales[] = { 0, 1, 2, 3, 4, 5 };
        enum { SWEEP_MAX_W = 1440, SWEEP_MAX_H = SWEEP_MAX_W * 4 / 3 };
        /* One buffer for the whole sweep, sized for the largest panel below.
           Allocated once rather than per width so the sweep contributes one
           check to the suite's count instead of three hundred. */
        uint8_t *sfb = malloc((size_t)SWEEP_MAX_W * SWEEP_MAX_H);
        CHECK(sfb != NULL);
        for (int W = 176; sfb && W <= SWEEP_MAX_W; W += 4) {
            int H = W * 4 / 3;
            for (size_t si = 0; si < sizeof sweep_scales / sizeof sweep_scales[0]; si++) {
                koboy_config cc; config_defaults(&cc);
                cc.scale = sweep_scales[si];
                koboy_profile q;
                if (!config_resolve_profile(&q, &cc, W, H)) continue;
                checked++;

                int bad = 0;
                /* Every shape the lamp can take: unknown (label only), empty,
                   and full. */
                static const int pcts[] = { -1, 0, 100 };
                for (size_t pi = 0; pi < sizeof pcts / sizeof pcts[0]; pi++) {
                    memset(sfb, 0x7F, (size_t)W * H);
                    chrome_render_battery(sfb, W, &q, &cc.layout, pcts[pi]);
                    for (int y = q.game_y; y < q.game_y + q.game_h; y++)
                        for (int x = q.game_x; x < q.game_x + q.game_w; x++)
                            if (sfb[(size_t)y * W + x] != 0x7F) bad++;
                }
                if (bad) {
                    if (intruding_combos < 5)
                        fprintf(stderr, "  battery inside game rect: %dx%d scale=%d"
                                " -> resolved %d, game_x=%d, %d px\n",
                                W, H, sweep_scales[si], q.scale, q.game_x, bad);
                    intruding_combos++;
                }
            }
        }
        free(sfb);
        /* The sweep must actually have swept: a resolver change that made
           every combination unresolvable would otherwise leave this vacuous. */
        CHECK(checked > 1000);
        CHECK_EQ_INT(intruding_combos, 0);
    }

    /* Case tone, task 15's user-chosen ordering -- CORRECTED in round 2
       against the reference photo. Round 1 shipped the controls LIGHTER
       than the case (near-white d-pad/A/B/pills on a grey case), which
       read as holes punched in the case rather than raised controls; the
       photo's actual ordering, darkest to lightest, is d-pad, bezel,
       A/B, Start/Select/MENU, case. "Clearly" is pinned at more than one
       GC16 waveform step (~17 levels of 256) apart, so relationships
       survive being quantised down to the panel's real 16-level driver,
       not just on this exact 8-bit render; the pill-vs-case gap is
       deliberately NOT held to that bar -- the photo's pills sit much
       closer to the case tone than the buttons do ("a little darker",
       not "clearly darker").
       Sampled from coordinates guaranteed to land on exactly one tone
       regardless of layout: (2,2) is above and left of the bezel on every
       supported panel; the A disc's own centre is always BUTTON; the
       Start pill's own centre is always PILL; game_x - 3 at the rect's
       own vertical middle sits inside the LEFT bezel band, solid DARK for
       every side_t >= 3 (side_t floors at 5); and the d-pad's vertical
       arm at dr/2 above centre is always DPAD fill -- above the hub disc
       (radius arm/2 = dr/6) and below where the tip ridges start
       (>= ~3*dr/4), for every dr > 0. */
    {
        koboy_config c; config_defaults(&c);
        koboy_profile p;
        const int W = 1264, H = 1680;
        static uint8_t fb3[1264 * 1680];
        config_resolve_profile(&p, &c, W, H);
        memset(fb3, 0x7F, (size_t)W * H);
        chrome_render(fb3, W, &p, &c.layout);

        int case_v  = fb3[2 * W + 2];
        int a_cx = c.layout.a_cx * W / 1000, a_cy = c.layout.a_cy * H / 1000;
        int button_v = fb3[(size_t)a_cy * W + a_cx];
        int bezel_x = p.game_x - 3, bezel_y = p.game_y + p.game_h / 2;
        int bezel_v = fb3[(size_t)bezel_y * W + bezel_x];
        int scx = c.layout.start_cx * W / 1000, scy = c.layout.start_cy * H / 1000;
        int pill_v = fb3[(size_t)scy * W + scx];
        int dcx = c.layout.dpad_cx * W / 1000, dcy = c.layout.dpad_cy * H / 1000;
        int dr = c.layout.dpad_r * W / 1000;
        int dpad_v = fb3[(size_t)(dcy - dr / 2) * W + dcx];

        if (case_v - bezel_v <= 17 || bezel_v - dpad_v <= 17 ||
            case_v - button_v <= 17 || pill_v <= button_v || case_v <= pill_v ||
            dpad_v >= 64)
            fprintf(stderr, "  case tone: dpad=%d bezel=%d button=%d pill=%d case=%d\n",
                    dpad_v, bezel_v, button_v, pill_v, case_v);
        CHECK(case_v - bezel_v > 17);   /* bezel clearly darker than the case        */
        CHECK(bezel_v - dpad_v > 17);   /* d-pad clearly darker than the bezel too   */
        CHECK(dpad_v < 64);             /* d-pad reads as near-black                 */
        CHECK(case_v - button_v > 17);  /* A/B clearly darker than the case          */
        CHECK(case_v > pill_v);         /* Start/Select/MENU a LITTLE darker...      */
        CHECK(pill_v > button_v);       /* ...but nowhere near as dark as A/B        */
    }

    /* Speaker grille right margin, FOLLOWUPS #20 / task 15 item 4: `hline`
       draws its two columns INCLUSIVELY, so clamping only the loop's own
       bound against the margin still let the second, inclusive column land
       ON the boundary -- 7px of clearance shipped where
       KOBOY_CHROME_MARGIN requires 8. Swept over all four supported panels,
       not just Libra 2, since the old bug's margin was panel-width
       dependent (three of four panels showed it, per FOLLOWUPS).
       The grille's own y-range is recomputed here from the layout, the same
       independent-duplication style chrome_controls_top's guard uses below
       and for the same reason: a pixel scan restricted to the WRONG rows
       would either miss the grille or catch the bezel's unrelated DARK
       bands, which are not held to this margin at all (side_t can exceed
       KOBOY_CHROME_MARGIN by design). Detects DARK by sampling it fresh at
       a known bezel pixel rather than hard-coding chrome.c's private macro
       value, so a future palette retune cannot make this test silently
       compare against a stale constant. */
    {
        static const int panels[][2] = {
            { 1072, 1448 }, { 1264, 1680 }, { 1404, 1872 }, { 1440, 1920 },
        };
        koboy_config c; config_defaults(&c);
        static uint8_t gfb[1440 * 1920];
        int total_violations = 0;
        for (size_t pi = 0; pi < sizeof panels / sizeof panels[0]; pi++) {
            int W = panels[pi][0], H = panels[pi][1];
            koboy_profile p;
            CHECK(config_resolve_profile(&p, &c, W, H));
            memset(gfb, 0x7F, (size_t)W * H);
            chrome_render(gfb, W, &p, &c.layout);

            int dark_v = gfb[(size_t)(p.game_y + p.game_h / 2) * W + (p.game_x - 3)];

            int mcy = c.layout.menu_cy * H / 1000, mh = c.layout.menu_h * H / 1000;
            int gy0 = mcy + mh / 2 + 18 * H / 1000;   /* matches chrome.c's gap, round 2 */
            int gy1 = H - KOBOY_CHROME_MARGIN;

            int violation = 0;
            for (int y = gy0; y < gy1; y++)
                for (int x = W - KOBOY_CHROME_MARGIN; x < W; x++)
                    if (gfb[(size_t)y * W + x] == dark_v) violation++;
            if (violation)
                fprintf(stderr, "  grille right margin painted: %dx%d -> %d px\n",
                        W, H, violation);
            total_violations += violation;
        }
        CHECK_EQ_INT(total_violations, 0);
    }

    /* NO NINTENDO MARKS. This is a public GPLv3 repo; the faceplate is an
       homage to the industrial design and carries none of the word marks.
       Asserted on the source rather than the pixels, because that is where a
       future edit would add one. */
    {
        FILE *f = fopen("src/chrome.c", "rb");
        CHECK(f != NULL);
        static char src[200000];
        size_t n = f ? fread(src, 1, sizeof src - 1, f) : 0;
        if (f) fclose(f);
        src[n] = 0;
        for (size_t i = 0; i < n; i++)
            if (src[i] >= 'a' && src[i] <= 'z') src[i] = (char)(src[i] - 32);
        CHECK(strstr(src, "NINTENDO") == NULL);
        CHECK(strstr(src, "GAME BOY") == NULL);
        CHECK(strstr(src, "GAMEBOY") == NULL);
    }
})
