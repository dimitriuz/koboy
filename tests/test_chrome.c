#include "test.h"
#include "pgm.h"
#include "chrome.h"
#include "config.h"
#include "input.h"
#include <stdlib.h>

static int render(koboy_config *c, int W, int H, uint8_t *fb, koboy_profile *p)
{
    config_resolve_profile(p, c, W, H, KOBOY_GB_W, KOBOY_GB_H, KOBOY_GB_W, KOBOY_GB_H);
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
            CHECK(config_resolve_profile(&q, &cc, W, H, KOBOY_GB_W, KOBOY_GB_H, KOBOY_GB_W, KOBOY_GB_H));
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
        /* THREE layouts, not one: the shipped default (no extra discs), the
           one config_extra_buttons_for_rom builds for a Pokemon Mini (one
           disc) and the one it builds for a WonderSwan (two). The first is
           what catches an UNGUARDED extra term -- with r == 0 an
           unconditional `perm(cy) - perm(r)` computes 0 - 0 = 0 and collapses
           the whole reservation, so this loop would report 0 against an
           expected 1018. The other two are what prove the term is honoured
           when the discs really are there, at both array lengths. */
        koboy_config cfgs[3];
        config_defaults(&cfgs[0]);
        config_defaults(&cfgs[1]);
        config_defaults(&cfgs[2]);
        config_extra_buttons_for_rom(&cfgs[1].layout, "Pokemon Tetris.min");
        config_extra_buttons_for_rom(&cfgs[2].layout, "GunPey.ws");
        CHECK_EQ_INT(cfgs[0].layout.extra[0].r, 0);
        CHECK(cfgs[1].layout.extra[0].r > 0);
        CHECK_EQ_INT(cfgs[1].layout.extra[1].r, 0);
        CHECK(cfgs[2].layout.extra[0].r > 0);
        CHECK(cfgs[2].layout.extra[1].r > 0);

        for (size_t li = 0; li < 3; li++) {
        const koboy_layout *l = &cfgs[li].layout;

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
            /* Conditional here for the same reason it is conditional in
               chrome.c: r == 0 means an empty slot, not a button of zero
               radius sitting at the top of the panel. */
            for (int e = 0; e < KOBOY_MAX_EXTRA_BTNS; e++) {
                if (l->extra[e].r <= 0) continue;
                int e_top = l->extra[e].cy * H / 1000 - l->extra[e].r * W / 1000;
                if (e_top < expected) expected = e_top;
            }
            int start_top = l->start_cy * H / 1000 - (l->start_h * H / 1000) / 2;
            if (start_top < expected) expected = start_top;
            int select_top = l->select_cy * H / 1000 - (l->select_h * H / 1000) / 2;
            if (select_top < expected) expected = select_top;
            int menu_top = l->menu_cy * H / 1000 - (l->menu_h * H / 1000) / 2;
            if (menu_top < expected) expected = menu_top;
            if (expected < 0) expected = 0;

            int got = chrome_controls_top(KOBOY_LAYOUT_DMG, l, W, H);
            if (got != expected)
                fprintf(stderr, "  layout %d %dx%d: chrome_controls_top=%d, expected %d\n",
                        (int)li, W, H, got, expected);
            CHECK_EQ_INT(got, expected);
        }
        }

        /* And the number is the SAME with and without the C button, on every
           panel. That is a claim about the chosen position rather than about
           the code: C was placed in the pocket below A precisely so it never
           becomes the binding minimum, which is what lets a Pokemon Mini get
           the identical game rect and resolved scale a Game Boy gets. Move
           the disc up and this fails, which is the point -- the cost of that
           move is a smaller picture and it should not be payable silently. */
        for (size_t pi = 0; pi < sizeof panels / sizeof panels[0]; pi++) {
            int W = panels[pi][0], H = panels[pi][1];
            CHECK_EQ_INT(chrome_controls_top(KOBOY_LAYOUT_DMG, &cfgs[1].layout, W, H),
                         chrome_controls_top(KOBOY_LAYOUT_DMG, &cfgs[0].layout, W, H));
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

        struct { const char *name; int dpad, a, b, c, start, select, menu; } cases[] = {
            { "dpad",   1, 0, 0, 0, 0, 0, 0 },
            { "a",      0, 1, 0, 0, 0, 0, 0 },
            { "b",      0, 0, 1, 0, 0, 0, 0 },
            { "c",      0, 0, 0, 1, 0, 0, 0 },
            { "start",  0, 0, 0, 0, 1, 0, 0 },
            { "select", 0, 0, 0, 0, 0, 1, 0 },
            { "menu",   0, 0, 0, 0, 0, 0, 1 },
        };
        for (size_t ci = 0; ci < sizeof cases / sizeof cases[0]; ci++) {
            koboy_layout l;
            memset(&l, 0, sizeof l);
            /* Parked: negligible footprint, far down the panel. */
            l.dpad_cx = l.a_cx = l.b_cx = l.start_cx = l.select_cx = l.menu_cx = 500;
            l.dpad_cy = l.a_cy = l.b_cy = l.start_cy = l.select_cy = l.menu_cy = 990;
            l.extra[0].cx = l.extra[1].cx = 500;
            l.extra[0].cy = l.extra[1].cy = 990;
            /* The extra radii are parked NON-zero, unlike the parked cy/r
               pairs above, because zero is that field's "absent" sentinel: a
               parked 0 would make the term skip entirely rather than lose on
               merit, and the six non-extra cases would then be testing a
               layout with no extra disc in it at all. */
            l.dpad_r = l.a_r = l.b_r = 5;
            l.extra[0].r = l.extra[1].r = 5;
            l.start_w = l.select_w = l.menu_w = 10;
            l.start_h = l.select_h = l.menu_h = 10;

            /* Bring exactly one control up to a clearly dominant position. */
            if (cases[ci].dpad)   { l.dpad_cy   = 300; l.dpad_r   = 80; }
            if (cases[ci].a)      { l.a_cy      = 300; l.a_r      = 80; }
            if (cases[ci].b)      { l.b_cy      = 300; l.b_r      = 80; }
            /* Case "c" drives extra[1], the SECOND slot, deliberately: a loop
               that only ever read extra[0] would pass every other assertion
               in this file, because the Pokemon Mini -- the only system with
               exactly one disc -- fills slot 0. */
            if (cases[ci].c)      { l.extra[1].cy = 300; l.extra[1].r = 80; }
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
            } else if (cases[ci].c) {
                expected = l.extra[1].cy * H / 1000 - l.extra[1].r * W / 1000;
            } else if (cases[ci].start) {
                expected = l.start_cy * H / 1000 - (l.start_h * H / 1000) / 2;
            } else if (cases[ci].select) {
                expected = l.select_cy * H / 1000 - (l.select_h * H / 1000) / 2;
            } else {
                expected = l.menu_cy * H / 1000 - (l.menu_h * H / 1000) / 2;
            }

            int got = chrome_controls_top(KOBOY_LAYOUT_DMG, &l, W, H);
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
        config_resolve_profile(&p, &c, W, H, KOBOY_GB_W, KOBOY_GB_H, KOBOY_GB_W, KOBOY_GB_H);
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

    /* THE EXTRA DISCS. A Pokemon Mini has A, B and C; a WonderSwan has an A
       and a B that land on L1/R1 once its screen is rotated. In both cases a
       button that exists in the hardware and is unreachable on koboy is the
       exact bug the Game & Watch layout was fixed for, twice over now. So
       each disc has to be DRAWN (an invisible live zone is the same bug
       wearing a hat), drawn ONLY for the system that has it, and it has to
       clear everything already on the faceplate.

       Note what the golden-image check immediately above already proves on
       its own: the Game Boy faceplate is unchanged, pixel for pixel, by all
       of this. That golden was rendered before any extra disc existed. */
    {
        static const int panels[][2] = {
            { 1072, 1448 }, { 1264, 1680 }, { 1404, 1872 }, { 1440, 1920 },
        };
        /* Both systems, with the geometry each core actually reports -- 96x64
           for the Pokemon Mini (measured through probe_core with the core's
           video scale at 1x, not the 384x256 its internal 4x upscaler reports
           by default) and 224x224 for the WonderSwan (its MAX, which is square
           because the core reports one geometry that both orientations fit
           inside). `n` is how many discs that system must have; asserting the
           count is what stops a one-disc renderer from passing the WonderSwan
           case by drawing only extra[0]. */
        static const struct { const char *rom; int n, gw, gh; } sys[] = {
            { "/roms/Pokemon Tetris.min", 1,  96,  64 },
            { "/roms/GunPey.ws",          2, 224, 224 },
            /* The two systems added with this batch, at the MAX geometry
               their cores really report (measured through probe_core):
               FreeIntv is a fixed 352x224 with base == max, Gearcoleco
               declares 512x288 for its F18A modes while drawing 256x192.
               Both are much bigger rects than the Pokemon Mini's or the
               WonderSwan's, which is the point of listing them here -- a
               disc that clears the game rect at 96x64 can still be underneath
               it at 512x288. */
            { "/roms/Atlantis.int",       2, 352, 224 },
            { "/roms/BurgerTime.col",     2, 512, 288 },
            /* Arcade, at the LARGEST max geometry the author's 227-romset
               collection reports (Tapper and Popeye both hit 512x512;
               FinalBurn Neo reports a SQUARE max, side = max(w,h), so that a
               quarter-turned board fits the same buffer either way round).
               This is the biggest reserved rect any system in this table
               produces and therefore the hardest case for a disc to stay
               clear of. */
            { "/roms/galaga.zip",         2, 512, 512 },
        };
        static uint8_t fbc[1440 * 1920], fbn[1440 * 1920];

        for (size_t si = 0; si < sizeof sys / sizeof sys[0]; si++)
        for (size_t pi = 0; pi < sizeof panels / sizeof panels[0]; pi++) {
            const int W = panels[pi][0], H = panels[pi][1];

            koboy_config cc; config_defaults(&cc);
            config_extra_buttons_for_rom(&cc.layout, sys[si].rom);
            koboy_config cn; config_defaults(&cn);
            config_extra_buttons_for_rom(&cn.layout, "/roms/Metroid.nes"); /* clears */
            CHECK_EQ_INT(cn.layout.extra[0].r, 0);
            CHECK_EQ_INT(cn.layout.extra[1].r, 0);

            int have = 0;
            for (int e = 0; e < KOBOY_MAX_EXTRA_BTNS; e++)
                if (cc.layout.extra[e].r > 0) have++;
            CHECK_EQ_INT(have, sys[si].n);

            koboy_profile p;
            CHECK(config_resolve_profile(&p, &cc, W, H,
                                         sys[si].gw, sys[si].gh,
                                         sys[si].gw, sys[si].gh));

            memset(fbc, 0x7F, (size_t)W * H);
            chrome_render(fbc, W, &p, &cc.layout);
            memset(fbn, 0x7F, (size_t)W * H);
            chrome_render(fbn, W, &p, &cn.layout);

            int case_v = fbc[2 * (size_t)W + 2];

            for (int e = 0; e < sys[si].n; e++) {
                int ccx = cc.layout.extra[e].cx * W / 1000;
                int ccy = cc.layout.extra[e].cy * H / 1000;
                int cr  = cc.layout.extra[e].r  * W / 1000;

                /* Drawn, and drawn as a BUTTON: clearly darker than the case,
                   by more than one ~17-level GC16 step, exactly like A and B.
                   A probe just inside the rim rather than dead centre,
                   because the centre is where the label's ink lands. */
                int rim_v = fbc[(size_t)ccy * W + (ccx + cr * 4 / 5)];
                CHECK(case_v - rim_v > 17);

                /* ...and NOT drawn for a system with no extra discs. Without
                   this the assertion above is equally consistent with a
                   faceplate that grew these discs for every game. */
                CHECK_EQ_INT(fbn[(size_t)ccy * W + (ccx + cr * 4 / 5)], case_v);

                /* LABELLED. The disc must contain ink -- an unlabelled disc in
                   a cluster is the "indistinguishable grey shapes" problem
                   text.c was added to solve. */
                int ink = 0;
                for (int y = ccy - cr; y <= ccy + cr; y++)
                    for (int x = ccx - cr; x <= ccx + cr; x++)
                        if (fbc[(size_t)y * W + x] == 0x00) ink++;
                CHECK(ink > 0);

                /* CLEARANCES, re-derived here rather than trusted from the
                   comment in config.c: clear of the A disc, of the B disc, of
                   the d-pad, of the START/SELECT/MENU row below, and
                   KOBOY_CHROME_MARGIN clear of the panel's right edge. */
                int acx = cc.layout.a_cx * W / 1000, acy = cc.layout.a_cy * H / 1000;
                int ar  = cc.layout.a_r  * W / 1000;
                long dx = acx - ccx, dy = acy - ccy;
                CHECK(dx * dx + dy * dy > (long)(ar + cr) * (ar + cr));

                int bcx = cc.layout.b_cx * W / 1000, bcy = cc.layout.b_cy * H / 1000;
                int br  = cc.layout.b_r  * W / 1000;
                dx = bcx - ccx; dy = bcy - ccy;
                CHECK(dx * dx + dy * dy > (long)(br + cr) * (br + cr));

                int dcx = cc.layout.dpad_cx * W / 1000;
                int dcy = cc.layout.dpad_cy * H / 1000;
                int dr  = cc.layout.dpad_r  * W / 1000;
                dx = dcx - ccx; dy = dcy - ccy;
                CHECK(dx * dx + dy * dy > (long)(dr + cr) * (dr + cr));

                int row_top = cc.layout.menu_cy * H / 1000
                            - (cc.layout.menu_h * H / 1000) / 2;
                CHECK(ccy + cr < row_top);
                CHECK(W - (ccx + cr) >= KOBOY_CHROME_MARGIN);

                /* And it never reaches into the game rect, the contract every
                   other drawn control on this faceplate is held to. */
                CHECK(ccy - cr >= p.game_y + p.game_h ||
                      ccx - cr >= p.game_x + p.game_w ||
                      ccx + cr <  p.game_x);
            }

            /* Discs must not overlap EACH OTHER either -- two live zones on
               the same pixels report two buttons for one finger. */
            if (sys[si].n == 2) {
                int x0 = cc.layout.extra[0].cx * W / 1000;
                int y0 = cc.layout.extra[0].cy * H / 1000;
                int r0 = cc.layout.extra[0].r  * W / 1000;
                int x1 = cc.layout.extra[1].cx * W / 1000;
                int y1 = cc.layout.extra[1].cy * H / 1000;
                int r1 = cc.layout.extra[1].r  * W / 1000;
                long dx = x0 - x1, dy = y0 - y1;
                CHECK(dx * dx + dy * dy > (long)(r0 + r1) * (r0 + r1));
                /* Different BITS, too: two discs reporting the same button is
                   a copy-paste away and looks fine on the panel. */
                CHECK(cc.layout.extra[0].bit != cc.layout.extra[1].bit);
            }

            /* Nothing painted over the game rect at all, for any of them. */
            int intruded = 0;
            for (int y = p.game_y; y < p.game_y + p.game_h; y++)
                for (int x = p.game_x; x < p.game_x + p.game_w; x++)
                    if (fbc[(size_t)y * W + x] != 0x7F) intruded++;
            CHECK_EQ_INT(intruded, 0);

            /* The extras must not have MOVED chrome_controls_top: that is
               what lets these systems get the same game rect and resolved
               scale they would have had with no extra discs at all. */
            CHECK_EQ_INT(chrome_controls_top(KOBOY_LAYOUT_DMG, &cc.layout, W, H),
                         chrome_controls_top(KOBOY_LAYOUT_DMG, &cn.layout, W, H));
        }
    }

    /* config_extra_buttons_for_rom itself: which extensions get which discs,
       and -- the half that is easy to leave out -- that it CLEARS. The config
       outlives one game (MENU -> CHOOSE ROM reuses it), so a setter that only
       ever set would leave live, drawn discs on the next Game Boy. */
    {
        koboy_config c; config_defaults(&c);
        CHECK_EQ_INT(c.layout.extra[0].r, 0);

        config_extra_buttons_for_rom(&c.layout, "Pokemon Tetris.min");
        CHECK(c.layout.extra[0].r > 0);
        CHECK_EQ_INT(c.layout.extra[1].r, 0);
        /* The BIT is the core's, and it is asserted rather than assumed: the
           Pokemon Mini core binds C to RETRO_DEVICE_ID_JOYPAD_R. */
        CHECK_EQ_INT(c.layout.extra[0].bit, KOBOY_BTN_R1);
        CHECK(!strcmp(c.layout.extra[0].label, "C"));
        int set_cx = c.layout.extra[0].cx, set_cy = c.layout.extra[0].cy;
        int set_r  = c.layout.extra[0].r;

        /* Idempotent: a second call for the same ROM must not drift. */
        config_extra_buttons_for_rom(&c.layout, "Pokemon Tetris.min");
        CHECK_EQ_INT(c.layout.extra[0].cx, set_cx);
        CHECK_EQ_INT(c.layout.extra[0].cy, set_cy);
        CHECK_EQ_INT(c.layout.extra[0].r,  set_r);

        /* Case-insensitive, like every other extension test in this suite. */
        config_extra_buttons_for_rom(&c.layout, "/roms/POKEMON TETRIS.MIN");
        CHECK_EQ_INT(c.layout.extra[0].r, set_r);
        config_extra_buttons_for_rom(&c.layout, "/roms/Pokemon Tetris.MiN");
        CHECK_EQ_INT(c.layout.extra[0].r, set_r);

        /* A WONDERSWAN GETS TWO, on both extensions and in both cases, and
           they are L1 and R1 -- the bits beetle-wswan's rotated key map puts
           the console's own A and B on. Asserting the BITS is the point: two
           discs in the right places reporting the wrong buttons look exactly
           like two working discs. */
        static const char *ws[] = {
            "GunPey.ws", "/roms/Final Fantasy.wsc",
            "/roms/GUNPEY.WS", "/roms/FINAL FANTASY.WSC",
        };
        for (size_t i = 0; i < sizeof ws / sizeof ws[0]; i++) {
            config_extra_buttons_for_rom(&c.layout, ws[i]);
            CHECK(c.layout.extra[0].r > 0);
            CHECK(c.layout.extra[1].r > 0);
            CHECK_EQ_INT(c.layout.extra[0].bit, KOBOY_BTN_L1);
            CHECK_EQ_INT(c.layout.extra[1].bit, KOBOY_BTN_R1);
            CHECK(!strcmp(c.layout.extra[0].label, "L1"));
            CHECK(!strcmp(c.layout.extra[1].label, "R1"));
        }

        /* AN INTELLIVISION GETS TWO, and the bits matter more here than
           anywhere else in this function: extra[0] is JOYPAD_L, which is not
           a button on the hardware at all -- it is FreeIntv's "hold to show
           the mini keypad" modifier, and it is the ONLY route to the
           console's twelve keypad keys, several of which titles cannot be
           started without. A disc in the right place carrying the wrong bit
           would look identical and lock the user out of BurgerTime's player
           prompt. extra[1] is the hardware's third action button. */
        static const char *intv[] = {
            "Atlantis.int", "/roms/BURGERTIME.INT", "/roms/Bump 'n' Jump.Int",
        };
        for (size_t i = 0; i < sizeof intv / sizeof intv[0]; i++) {
            config_extra_buttons_for_rom(&c.layout, intv[i]);
            CHECK(c.layout.extra[0].r > 0);
            CHECK(c.layout.extra[1].r > 0);
            CHECK_EQ_INT(c.layout.extra[0].bit, KOBOY_BTN_L1);
            CHECK_EQ_INT(c.layout.extra[1].bit, KOBOY_BTN_Y);
            CHECK(!strcmp(c.layout.extra[0].label, "KEY"));
            CHECK(!strcmp(c.layout.extra[1].label, "TOP"));
        }

        /* A COLECOVISION GETS TWO, and they are keypad 1 and keypad 2 --
           JOYPAD_Y and JOYPAD_X, which is where Gearcoleco puts them. Same
           reasoning as the Intellivision above: the console's own BIOS asks
           for a keypad digit before any cartridge starts, so a wrong bit
           here is not a missing convenience, it is a system that cannot be
           played. */
        static const char *col[] = {
            "BurgerTime.col", "/roms/DONKEY KONG.COL", "/roms/Zaxxon.Col",
        };
        for (size_t i = 0; i < sizeof col / sizeof col[0]; i++) {
            config_extra_buttons_for_rom(&c.layout, col[i]);
            CHECK(c.layout.extra[0].r > 0);
            CHECK(c.layout.extra[1].r > 0);
            CHECK_EQ_INT(c.layout.extra[0].bit, KOBOY_BTN_Y);
            CHECK_EQ_INT(c.layout.extra[1].bit, KOBOY_BTN_X);
            CHECK(!strcmp(c.layout.extra[0].label, "K1"));
            CHECK(!strcmp(c.layout.extra[1].label, "K2"));
        }

        /* The two two-disc systems added together must not have been given
           the SAME two bits: a copy-pasted case that kept the WonderSwan's
           L1/R1 would satisfy every "extra[0].r > 0" above and quietly make
           an Intellivision keypad unreachable. */
        {
            koboy_layout a, b;
            memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
            config_extra_buttons_for_rom(&a, "Atlantis.int");
            config_extra_buttons_for_rom(&b, "BurgerTime.col");
            CHECK(a.extra[0].bit != b.extra[0].bit ||
                  a.extra[1].bit != b.extra[1].bit);
        }

        /* ARCADE GETS TWO, and they are JOYPAD_Y and JOYPAD_X -- Button 3
           and Button 4 in FinalBurn Neo's flat mapping, where the faceplate's
           own B and A are already Button 1 and Button 2. Chosen by COUNTING:
           of the 227 romsets in the author's set, 134 bind Y and 71 bind X,
           against 45-48 for the shoulder buttons there is no room for.
           Asserting the bits is the point, as everywhere else here: two discs
           in the right places carrying L1/R1 would look identical and press
           nothing on most boards. */
        static const char *arc[] = {
            "galaga.zip", "/roms/fbneo/MSPACMAN.ZIP", "/roms/arcade/dkong.Zip",
        };
        for (size_t i = 0; i < sizeof arc / sizeof arc[0]; i++) {
            config_extra_buttons_for_rom(&c.layout, arc[i]);
            CHECK(c.layout.extra[0].r > 0);
            CHECK(c.layout.extra[1].r > 0);
            CHECK_EQ_INT(c.layout.extra[0].bit, KOBOY_BTN_Y);
            CHECK_EQ_INT(c.layout.extra[1].bit, KOBOY_BTN_X);
            CHECK(!strcmp(c.layout.extra[0].label, "3"));
            CHECK(!strcmp(c.layout.extra[1].label, "4"));
        }
        /* Arcade and ColecoVision genuinely DO share both bits (Y and X), so
           the pairwise-distinctness check above cannot be extended to them --
           what distinguishes them is the LABEL, which is what the player
           reads off the panel. K1/K2 on a ColecoVision means "the keypad
           digit the BIOS is asking for"; 3/4 on an arcade board means "the
           third and fourth fire button". Asserted so that a copy-paste that
           carried K1/K2 into the arcade case is caught. */
        {
            koboy_layout a, b;
            memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
            config_extra_buttons_for_rom(&a, "galaga.zip");
            config_extra_buttons_for_rom(&b, "BurgerTime.col");
            CHECK_EQ_INT(a.extra[0].bit, b.extra[0].bit);   /* same bit, and that is fine */
            CHECK(strcmp(a.extra[0].label, b.extra[0].label) != 0);
            CHECK(strcmp(a.extra[1].label, b.extra[1].label) != 0);
        }

        /* A NEO GEO POCKET gets NONE -- its stick, A, B and OPTION are exactly
           what the DMG faceplate already draws. An ATARI 2600 and a MASTER
           SYSTEM / GAME GEAR likewise: one fire button plus Reset/Select, and
           two buttons plus Start/Pause, both of which the faceplate already
           carries. Checked from a SET state, so this is the clearing path
           too. */
        static const char *ngp[] = {
            "Sonic.ngp", "/roms/METAL SLUG.NGC",
            "Adventure.a26", "/roms/YARS' REVENGE.A26",
            "Alex Kidd.sms", "/roms/SONIC.SMS",
            "Crystal Warriors.gg", "/roms/BATTLETOADS.GG",
        };
        for (size_t i = 0; i < sizeof ngp / sizeof ngp[0]; i++) {
            config_extra_buttons_for_rom(&c.layout, "GunPey.ws");
            CHECK(c.layout.extra[0].r > 0);
            config_extra_buttons_for_rom(&c.layout, ngp[i]);
            CHECK_EQ_INT(c.layout.extra[0].r, 0);
            CHECK_EQ_INT(c.layout.extra[1].r, 0);
        }

        /* Cleared for everything else, from a SET state each time -- and from
           the TWO-disc state as well as the one-disc one, so a clear that
           only ever emptied slot 0 is caught. */
        static const char *others[] = {
            "Metroid.nes", "Tetris.gb", "Zelda.gbc", "BALL.mgw",
            "no-extension", "Tetris.minx", "Tetris.mi", "",
            "GunPey.wsx", "GunPey.w", "Sonic.ngpx",
            "Atlantis.intx", "Atlantis.in", "BurgerTime.colx",
            "BurgerTime.co", "Adventure.a26x", "Alex Kidd.smsx",
            "Crystal Warriors.ggx", "Crystal Warriors.g",
            "galaga.zipx", "galaga.zi", "galaga.7z",
        };
        for (size_t i = 0; i < sizeof others / sizeof others[0]; i++) {
            config_extra_buttons_for_rom(&c.layout, "GunPey.ws");
            CHECK(c.layout.extra[0].r > 0);
            CHECK(c.layout.extra[1].r > 0);
            config_extra_buttons_for_rom(&c.layout, others[i]);
            for (int e = 0; e < KOBOY_MAX_EXTRA_BTNS; e++) {
                CHECK_EQ_INT(c.layout.extra[e].cx, 0);
                CHECK_EQ_INT(c.layout.extra[e].cy, 0);
                CHECK_EQ_INT(c.layout.extra[e].r,  0);
                CHECK_EQ_INT(c.layout.extra[e].bit, 0);
            }
        }
        config_extra_buttons_for_rom(&c.layout, "GunPey.ws");
        config_extra_buttons_for_rom(&c.layout, NULL);
        CHECK_EQ_INT(c.layout.extra[0].r, 0);
        CHECK_EQ_INT(c.layout.extra[1].r, 0);
    }

    /* The battery lamp renders from a percentage, and an unknown battery (-1)
       is a valid input rather than a crash: the SDL backend has no battery and
       an unseen Kobo may not expose one either. */
    {
        koboy_config c; config_defaults(&c);
        koboy_profile p;
        const int W = 1264, H = 1680;
        static uint8_t a[1264 * 1680], b[1264 * 1680];
        config_resolve_profile(&p, &c, W, H, KOBOY_GB_W, KOBOY_GB_H, KOBOY_GB_W, KOBOY_GB_H);

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
                if (!config_resolve_profile(&q, &cc, W, H, KOBOY_GB_W, KOBOY_GB_H, KOBOY_GB_W, KOBOY_GB_H)) continue;
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
       (>= ~3*dr/4), for every dr > 0.
       Swept over all four supported panels, matching the sibling
       chrome_controls_top guard just above: the tones themselves are
       panel-size-independent constants (BG/DARK/DPAD/BUTTON/PILL never vary
       with W or H), so this was never a live coverage gap the way the
       margin/invariant tests were -- but sampling only 1264x1680 left it the
       one tonal check in this file that was not symmetric with the rest,
       and a future change that made a tone panel-size-dependent by accident
       would only be caught here if the sweep were already in place. */
    {
        static const int panels[][2] = {
            { 1072, 1448 }, { 1264, 1680 }, { 1404, 1872 }, { 1440, 1920 },
        };
        koboy_config c; config_defaults(&c);

        for (size_t pi = 0; pi < sizeof panels / sizeof panels[0]; pi++) {
            int W = panels[pi][0], H = panels[pi][1];
            koboy_profile p;
            CHECK(config_resolve_profile(&p, &c, W, H, KOBOY_GB_W, KOBOY_GB_H, KOBOY_GB_W, KOBOY_GB_H));
            memset(fb, 0x7F, (size_t)W * H);
            chrome_render(fb, W, &p, &c.layout);

            int case_v  = fb[2 * W + 2];
            int a_cx = c.layout.a_cx * W / 1000, a_cy = c.layout.a_cy * H / 1000;
            int button_v = fb[(size_t)a_cy * W + a_cx];
            int bezel_x = p.game_x - 3, bezel_y = p.game_y + p.game_h / 2;
            int bezel_v = fb[(size_t)bezel_y * W + bezel_x];
            int scx = c.layout.start_cx * W / 1000, scy = c.layout.start_cy * H / 1000;
            int pill_v = fb[(size_t)scy * W + scx];
            int dcx = c.layout.dpad_cx * W / 1000, dcy = c.layout.dpad_cy * H / 1000;
            int dr = c.layout.dpad_r * W / 1000;
            int dpad_v = fb[(size_t)(dcy - dr / 2) * W + dcx];

            if (case_v - bezel_v <= 17 || bezel_v - dpad_v <= 17 ||
                case_v - button_v <= 17 || pill_v <= button_v || case_v <= pill_v ||
                dpad_v >= 64)
                fprintf(stderr, "  case tone: %dx%d -> dpad=%d bezel=%d button=%d pill=%d case=%d\n",
                        W, H, dpad_v, bezel_v, button_v, pill_v, case_v);
            CHECK(case_v - bezel_v > 17);   /* bezel clearly darker than the case        */
            CHECK(bezel_v - dpad_v > 17);   /* d-pad clearly darker than the bezel too   */
            CHECK(dpad_v < 64);             /* d-pad reads as near-black                 */
            CHECK(case_v - button_v > 17);  /* A/B clearly darker than the case          */
            CHECK(case_v > pill_v);         /* Start/Select/MENU a LITTLE darker...      */
            CHECK(pill_v > button_v);       /* ...but nowhere near as dark as A/B        */
        }
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

    /* STRAPLINE WORDING. A deliberate honesty ruling: this build is silent
       end to end, and the historic DMG strap's "DOT MATRIX WITH STEREO
       SOUND" claim would be a false statement about the product if printed
       on the faceplate (see STRAPLINE's own comment in chrome.c). Before
       this check the wording was protected only indirectly, by the golden
       pixel-diff -- a real gate, but not a direct one, and not one that
       names what it is protecting against.
       This cannot reuse the trademark check's shape exactly (scan the
       WHOLE file, upper-cased, for the forbidden phrase): chrome.c's own
       comment quotes "STEREO SOUND" by name to explain why it was rejected,
       so a whole-file scan for that phrase would fail on the explanation
       forever, not on a real regression -- unlike NINTENDO/GAME BOY/GAMEBOY,
       which have no legitimate reason to appear anywhere in this file.
       So this reads the STRAPLINE string literal itself, not the whole
       file, and checks only what the faceplate actually prints: it must not
       read the historic stereo-sound claim, and it must read the wording
       this build ships. Case-sensitive and unmodified (no upper-casing),
       since STRAPLINE is already all caps in source and an accidental
       lower-case edit should also be caught. */
    {
        FILE *f = fopen("src/chrome.c", "rb");
        CHECK(f != NULL);
        static char src[200000];
        size_t n = f ? fread(src, 1, sizeof src - 1, f) : 0;
        if (f) fclose(f);
        src[n] = 0;

        const char *needle = "STRAPLINE[] = \"";
        const char *start = strstr(src, needle);
        CHECK(start != NULL);
        if (start) {
            start += strlen(needle);
            const char *end = strchr(start, '"');
            CHECK(end != NULL);
            if (end && (size_t)(end - start) < 128) {
                char strap[128];
                size_t len = (size_t)(end - start);
                memcpy(strap, start, len);
                strap[len] = 0;
                CHECK(strstr(strap, "STEREO SOUND") == NULL);
                CHECK(strcmp(strap, "DOT MATRIX ON ELECTRONIC PAPER") == 0);
            }
        }
    }

    /* ======================================================== the LCD layout
     *
     * A different faceplate for a different system: no d-pad, no A/B, a
     * full-width fractionally-scaled game rect, and one bottom strip holding
     * the battery lamp and MENU. Everything below is asserted against the
     * MEASURED Mickey Mouse geometry (654x396) on the one verified panel,
     * resolved by the real resolver -- not against a hand-built profile,
     * because the resolver's answer is what the device will actually draw.
     */
    {
        koboy_config lc; config_defaults(&lc);
        lc.layout_mode = KOBOY_LAYOUT_LCD;
        koboy_profile lp;
        CHECK(config_resolve_profile(&lp, &lc, 1264, 1680, 654, 396, 654, 396));

        memset(fb, 0x7F, (size_t)1264 * 1680);
        chrome_render(fb, 1264, &lp, &lc.layout);
        chrome_render_battery(fb, 1264, &lp, &lc.layout, 62);

        /* CRITICAL, and the same contract the DMG faceplate lives under: not
           one byte inside the game rect. In this layout the rect touches both
           panel edges, so every band around it is zero-width on two sides and
           the clamped fills are the only thing keeping them out. */
        int intruded = 0;
        for (int y = lp.game_y; y < lp.game_y + lp.game_h; y++)
            for (int x = lp.game_x; x < lp.game_x + lp.game_w; x++)
                if (fb[y * 1264 + x] != 0x7F) intruded++;
        if (intruded)
            fprintf(stderr, "  LCD chrome wrote %d px inside the game rect\n", intruded);
        CHECK_EQ_INT(intruded, 0);

        /* ...and it drew SOMETHING. Without this, a chrome_render_lcd that
           returned immediately would satisfy the check above perfectly. */
        int painted = 0;
        for (int y = 0; y < 1680; y++)
            for (int x = 0; x < 1264; x++)
                if (fb[y * 1264 + x] != 0x7F) painted++;
        CHECK(painted > 10000);

        /* Golden, so the whole faceplate is pinned and not just the three
           elements named below. */
        CHECK(pgm_compare_golden("chrome_lcd_1264x1680", fb, 1264, 1680, 1264) == 1);

        int strip = chrome_lcd_strip_h(1680);
        int sy    = 1680 - strip;
        CHECK_EQ_INT(strip, 420);          /* 250 permille of 1680 */
        CHECK_EQ_INT(sy, 1260);

        /* ================================= THE CONTROL STRIP
         *
         * The strip exists to carry a full retropad, because the shipped
         * .mgw titles are driven by per-title retropad bindings and ignore
         * the pointer entirely (measured: 0 pixels changed by a pointer
         * press, 211k by a joypad press). Everything below is about the
         * controls being THERE, being DRAWN, and not sitting on top of one
         * another -- the matching "a tap here presses THAT button and not a
         * neighbour" checks live in tests/test_input_touch.c.
         */
        chrome_lcd_controls ctl;
        memset(&ctl, 0, sizeof ctl);
        chrome_lcd_layout(&lp, &ctl);

        CHECK_EQ_INT(ctl.strip.y, sy);
        CHECK_EQ_INT(ctl.strip.h, strip);
        CHECK_EQ_INT(ctl.strip.w, 1264);

        /* THE DIAMOND'S ARRANGEMENT. Not decoration: the gw core's own
           overlay draws a SNES pad and labels its TOP button NORTHEAST,
           which is Mickey Mouse's `x` binding, so X must be the top one and
           B the bottom one or a user reading the overlay is sent to the
           wrong button. Asserted as strict inequalities between the four
           centres, which is the only form a rearrangement cannot satisfy. */
        CHECK(ctl.x_cy < ctl.y_cy);            /* X above the middle row */
        CHECK(ctl.x_cy < ctl.a_cy);
        CHECK(ctl.b_cy > ctl.y_cy);            /* B below it */
        CHECK(ctl.b_cy > ctl.a_cy);
        CHECK_EQ_INT(ctl.y_cy, ctl.a_cy);      /* Y and A share the row */
        CHECK(ctl.y_cx < ctl.a_cx);            /* Y left of A */
        CHECK_EQ_INT(ctl.x_cx, ctl.b_cx);      /* X and B share the column */
        CHECK(ctl.y_cx < ctl.x_cx && ctl.x_cx < ctl.a_cx);
        /* ...and adjacent discs do not merge into one blob, which is what
           face_off > face_r * sqrt(2) buys. Squared, to stay integer. */
        CHECK(2 * ctl.face_off * ctl.face_off > 4 * ctl.face_r * ctl.face_r);

        /* EVERY CONTROL IS INSIDE THE STRIP AND INSIDE THE PANEL, and none of
           them overlaps another. Expressed over one table so a control added
           later without a bounds check is caught by construction rather than
           by whoever remembers to add a line. The d-pad and the four discs
           enter as their bounding boxes -- a box that clears its neighbours
           certainly means the shape inside it does. */
        struct { koboy_rect r; const char *name; } zone[10] = {
            { { ctl.dpad_cx - ctl.dpad_r, ctl.dpad_cy - ctl.dpad_r,
                2 * ctl.dpad_r + 1, 2 * ctl.dpad_r + 1 }, "dpad" },
            { { ctl.x_cx - ctl.face_r, ctl.x_cy - ctl.face_r,
                2 * ctl.face_r + 1, 2 * ctl.face_r + 1 }, "X" },
            { { ctl.y_cx - ctl.face_r, ctl.y_cy - ctl.face_r,
                2 * ctl.face_r + 1, 2 * ctl.face_r + 1 }, "Y" },
            { { ctl.a_cx - ctl.face_r, ctl.a_cy - ctl.face_r,
                2 * ctl.face_r + 1, 2 * ctl.face_r + 1 }, "A" },
            { { ctl.b_cx - ctl.face_r, ctl.b_cy - ctl.face_r,
                2 * ctl.face_r + 1, 2 * ctl.face_r + 1 }, "B" },
            { ctl.l1,     "L1"     },
            { ctl.select, "SELECT" },
            { ctl.start,  "START"  },
            { ctl.r1,     "R1"     },
            { ctl.menu,   "MENU"   },
        };
        for (int i = 0; i < 10; i++) {
            const koboy_rect *r = &zone[i].r;
            if (r->x < 0 || r->y < sy || r->x + r->w > 1264 || r->y + r->h > 1680)
                fprintf(stderr, "  LCD control %s at (%d,%d) %dx%d escapes the strip\n",
                        zone[i].name, r->x, r->y, r->w, r->h);
            CHECK(r->w > 0 && r->h > 0);
            CHECK(r->x >= 0);
            CHECK(r->y >= sy);                       /* below the artwork */
            CHECK(r->x + r->w <= 1264);
            CHECK(r->y + r->h <= 1680);
            /* Big enough to hit with a thumb. 64 px is ~5.4 mm on this
               panel's 300 dpi -- a floor, not a target; the shipped sizes are
               75-108 px. The four discs' BOUNDING boxes are what is measured,
               which is the honest number for a round target. */
            CHECK(r->w >= 64 && r->h >= 64);
        }
        for (int i = 0; i < 10; i++)
            for (int j = i + 1; j < 10; j++) {
                const koboy_rect *a = &zone[i].r, *b2 = &zone[j].r;
                int over = !(a->x + a->w <= b2->x || b2->x + b2->w <= a->x ||
                             a->y + a->h <= b2->y || b2->y + b2->h <= a->y);
                /* The diamond's four discs are the ONE permitted overlap:
                   their bounding boxes touch at the corners even though the
                   circles inside them do not (asserted separately above). */
                int diamond = (i >= 1 && i <= 4 && j >= 1 && j <= 4);
                if (over && !diamond)
                    fprintf(stderr, "  LCD controls %s and %s overlap\n",
                            zone[i].name, zone[j].name);
                CHECK(!over || diamond);
            }

        /* EVERY CONTROL IS ACTUALLY DRAWN. A zone with nothing under it is an
           invisible button. Checked as a tone difference against the bare
           strip beside it, so a palette retune does not break it. */
        int case_v = fb[(sy + 4) * 1264 + 1264 / 2];   /* bare strip, top edge */
        for (int i = 0; i < 10; i++) {
            const koboy_rect *r = &zone[i].r;
            int painted_px = 0;
            for (int y = r->y; y < r->y + r->h; y++)
                for (int x = r->x; x < r->x + r->w; x++)
                    if (fb[y * 1264 + x] != case_v) painted_px++;
            /* AREA, not a single sample point: sampling the centre pixel is
               what a labelled control defeats -- the glyph is drawn there, in
               a tone that is neither the fill nor the case, and for the "B"
               disc it happened to equal the case tone exactly. (Real, caught
               here.) The three shapes fill very different fractions of their
               boxes -- a pill all of it, a disc pi/4 = 78%, the d-pad cross
               only 31% (two arms of width dr/3 crossing) -- so the threshold
               is set below the smallest of them. 25% is still five times what
               the largest LABEL alone covers (a 20x28 glyph in a 109x109 box
               is under 5%), which is the mutant it has to catch. */
            int area = r->w * r->h;
            if (painted_px * 100 < area * 25)
                fprintf(stderr, "  LCD control %s: painted=%d/%d\n",
                        zone[i].name, painted_px, area);
            CHECK(painted_px * 100 >= area * 25);
        }

        /* ...AND EVERY LABELLED ONE CARRIES ITS LABEL. Separate from the
           check above, and looking at a strictly INTERIOR region, because
           that is the only way to see a label at all: a disc's INK ring and a
           pill's INK frame are ink too, so counting ink over the whole zone
           passes for an unlabelled control. Deleting the diamond's
           label_in_box call was caught ONLY by the golden until this existed
           -- a real mutant, and the same class as v2's "MENU box not drawn".
           An unlabelled disc in a diamond of four is precisely the "four
           indistinguishable grey shapes" text.c was added to stop, and here
           it is worse than cosmetic: the labels are what tie koboy's buttons
           to the names the core's own overlay uses. */
        {
            /* The interior of each labelled control: the inscribed box for a
               disc (7/10 of r either way is just inside r/sqrt(2)), and a
               6 px inset for a pill, which clears its 2 px frame. The d-pad
               has no label and is not in this table. */
            int fr = ctl.face_r * 7 / 10;
            struct { koboy_rect r; const char *name; } lbl[9] = {
                { { ctl.x_cx - fr, ctl.x_cy - fr, 2 * fr, 2 * fr }, "X" },
                { { ctl.y_cx - fr, ctl.y_cy - fr, 2 * fr, 2 * fr }, "Y" },
                { { ctl.a_cx - fr, ctl.a_cy - fr, 2 * fr, 2 * fr }, "A" },
                { { ctl.b_cx - fr, ctl.b_cy - fr, 2 * fr, 2 * fr }, "B" },
                { { ctl.l1.x + 6,     ctl.l1.y + 6,     ctl.l1.w - 12,     ctl.l1.h - 12     }, "L1" },
                { { ctl.select.x + 6, ctl.select.y + 6, ctl.select.w - 12, ctl.select.h - 12 }, "SELECT" },
                { { ctl.start.x + 6,  ctl.start.y + 6,  ctl.start.w - 12,  ctl.start.h - 12  }, "START" },
                { { ctl.r1.x + 6,     ctl.r1.y + 6,     ctl.r1.w - 12,     ctl.r1.h - 12     }, "R1" },
                { { ctl.menu.x + 6,   ctl.menu.y + 6,   ctl.menu.w - 12,   ctl.menu.h - 12   }, "MENU" },
            };
            for (int i = 0; i < 9; i++) {
                const koboy_rect *r = &lbl[i].r;
                int glyph = 0;
                int fill_v = fb[(r->y + r->h - 2) * 1264 + r->x + 1];  /* a corner of the interior: the fill */
                for (int y = r->y; y < r->y + r->h; y++)
                    for (int x = r->x; x < r->x + r->w; x++)
                        if (fb[y * 1264 + x] != fill_v) glyph++;
                /* A 5x7 glyph at the smallest px this faceplate ever picks is
                   35 pixels; the shipped labels are hundreds. 20 is a floor
                   that only "nothing was drawn" can fall below. */
                if (glyph < 20)
                    fprintf(stderr, "  LCD control %s: %d label px inside it\n",
                            lbl[i].name, glyph);
                CHECK(glyph >= 20);
            }
        }

        /* MENU is REACHABLE: it is the only way back to the ROM browser once
           a game is running, so its zone must be inside the strip, clear of
           the game rect, inside the panel -- and actually drawn, which is
           checked as "the box is not all background". */
        koboy_rect m;
        memset(&m, 0, sizeof m);
        chrome_lcd_menu_rect(&lp, &m);
        CHECK(m.w > 0 && m.h > 0);
        CHECK(m.x >= 0 && m.x + m.w <= 1264);
        CHECK(m.y >= sy);
        CHECK(m.y + m.h <= 1680);
        CHECK(m.y >= lp.game_y + lp.game_h);          /* below the artwork */
        int menu_ink = 0;
        for (int y = m.y; y < m.y + m.h; y++)
            for (int x = m.x; x < m.x + m.w; x++)
                if (fb[y * 1264 + x] == 0x00) menu_ink++;   /* INK frame + label */
        CHECK(menu_ink > 100);
        /* ...and the BOX ITSELF is filled, not just its label drawn on bare
           case. Sampled as a tone comparison rather than against a literal,
           so it survives a palette retune: a point inside the box, clear of
           the frame and above the label, must differ from the strip
           background beside it. Without this, deleting the fill and the
           frame left menu_ink satisfied by the label alone and only the
           golden caught it -- a real mutant, confirmed. */
        {
            int inside_v  = fb[(m.y + 6) * 1264 + m.x + 6];
            int outside_v = fb[(m.y + 6) * 1264 + m.x - 20];
            CHECK(inside_v != outside_v);
        }

        /* THE BATTERY MOVED UNDER THE SCREEN -- the user's explicit request.
           Two halves, and both are needed: it is drawn in the strip, and it
           is NOT drawn where the DMG layout puts it (left of the game rect),
           which in this layout is inside the artwork and would violate the
           no-writes-in-the-rect contract above. */
        int bcx, bcy, br;
        bcx = bcy = br = 0;
        chrome_lcd_battery(&lp, &bcx, &bcy, &br);
        CHECK(br >= 4);
        CHECK(bcy - br >= sy);
        CHECK(bcy + br < 1680);
        CHECK(bcx - br >= 0);
        CHECK(bcy - br > lp.game_y + lp.game_h);
        int lamp_ink = 0;
        for (int y = bcy - br; y <= bcy + br; y++)
            for (int x = bcx - br; x <= bcx + br; x++)
                if (fb[y * 1264 + x] == 0x00) lamp_ink++;   /* the INK ring */
        CHECK(lamp_ink > 20);

        /* A partially-charged lamp really does show a level: the same
           chord-of-the-disc fill the DMG lamp uses, now reached through the
           LCD branch. 62% must differ from 0%. */
        static uint8_t fb0[1264 * 1680];
        memcpy(fb0, fb, (size_t)1264 * 1680);
        chrome_render_battery(fb, 1264, &lp, &lc.layout, 0);
        CHECK(memcmp(fb0, fb, (size_t)1264 * 1680) != 0);
    }

    /* EXTREME PANELS, where the strip's two live guards actually fire. Both
       are about REACHABILITY, not memory, and neither can be reached on any
       supported panel -- which is exactly why they need a case here rather
       than a comment saying they look dead.

       Hand-built profiles, not resolver output: config_resolve_profile is
       free to refuse geometry this silly, and the guards belong to
       chrome_lcd_layout regardless of who asks it. */
    {
        /* Tall and narrow enough that the d-pad and the diamond meet in the
           middle and leave no centre column at all. MENU is the ONLY way back
           to the ROM browser, so it must survive with a hittable size and
           inside the panel; a cramped MENU overlapping a button is
           recoverable, an absent one is not. */
        koboy_profile xp;
        memset(&xp, 0, sizeof xp);
        xp.panel_w = 400; xp.panel_h = 1600; xp.layout_mode = KOBOY_LAYOUT_LCD;
        chrome_lcd_controls xc;
        memset(&xc, 0, sizeof xc);
        chrome_lcd_layout(&xp, &xc);
        if (xc.menu.w < 16 || xc.menu.h < 16)
            fprintf(stderr, "  400x1600: MENU collapsed to %dx%d\n", xc.menu.w, xc.menu.h);
        CHECK(xc.menu.w >= 16 && xc.menu.h >= 16);
        CHECK(xc.menu.x >= 0 && xc.menu.x + xc.menu.w <= 400);
        CHECK(xc.menu.y >= xc.strip.y && xc.menu.y + xc.menu.h <= 1600);

        /* Narrower and taller still, until the centre column's midpoint itself
           lands on the panel's left edge. MENU's x clamp is what keeps the
           zone from starting at a negative column, where in_rect_xywh would
           quietly make it wider than it is drawn. Nothing resembling a device
           reaches here -- it is the guard's own boundary, and it is tested
           because the alternative is a guard nothing can fail. */
        koboy_profile zp;
        memset(&zp, 0, sizeof zp);
        zp.panel_w = 50; zp.panel_h = 4000; zp.layout_mode = KOBOY_LAYOUT_LCD;
        chrome_lcd_controls zc;
        memset(&zc, 0, sizeof zc);
        chrome_lcd_layout(&zp, &zc);
        if (zc.menu.x < 0)
            fprintf(stderr, "  50x4000: MENU starts at column %d\n", zc.menu.x);
        CHECK(zc.menu.x >= 0);

        /* Narrow enough that the battery lamp leaves the pill row almost no
           width. The four pills must still be four distinct zones -- without
           the cell floor their centres collapse onto one another and L1,
           SELECT, START and R1 become the same button. */
        koboy_profile yp;
        memset(&yp, 0, sizeof yp);
        yp.panel_w = 120; yp.panel_h = 1600; yp.layout_mode = KOBOY_LAYOUT_LCD;
        chrome_lcd_controls yc;
        memset(&yc, 0, sizeof yc);
        chrome_lcd_layout(&yp, &yc);
        const koboy_rect *row[4] = { &yc.l1, &yc.select, &yc.start, &yc.r1 };
        for (int i = 0; i < 4; i++) {
            CHECK(row[i]->w > 0 && row[i]->h > 0);
            for (int j = i + 1; j < 4; j++) {
                int over = !(row[i]->x + row[i]->w <= row[j]->x ||
                             row[j]->x + row[j]->w <= row[i]->x);
                if (over)
                    fprintf(stderr, "  120x1600: pill %d (%d..%d) and %d (%d..%d) collapsed\n",
                            i, row[i]->x, row[i]->x + row[i]->w,
                            j, row[j]->x, row[j]->x + row[j]->w);
                CHECK(!over);
            }
        }
    }

    /* Guard band, LCD layout: a profile whose rect runs off every edge must
       not write outside the panel. The DMG faceplate has three such tests;
       this branch draws a completely different set of shapes (full-width
       bands whose side pieces are zero- or negative-width) and needs its
       own. */
    {
        const int GUARD2 = 64;
        const uint8_t SENT = 0x42;
        int TW = 400, TH = 500;
        size_t bs = (size_t)(TW + 2 * GUARD2) * (TH + 2 * GUARD2);
        uint8_t *g2 = malloc(bs);
        CHECK(g2 != NULL);

        static const struct { int x, y, w, h; const char *name; } bad[] = {
            {   0,   0, 400, 500, "exactly the panel"        },
            {  -8,  -8, 420, 520, "over every edge"          },
            { 390,   0,  50, 500, "past the right edge"      },
            {   0, 480, 400,  60, "past the bottom, into the strip" },
            {   0,   0,   1,   1, "degenerate 1x1"           },
        };
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            memset(g2, SENT, bs);
            uint8_t *ps = g2 + (size_t)GUARD2 * (TW + 2 * GUARD2) + GUARD2;
            koboy_profile bp;
            memset(&bp, 0, sizeof bp);
            bp.layout_mode = KOBOY_LAYOUT_LCD;
            bp.scale = 1; bp.panel_w = TW; bp.panel_h = TH;
            bp.max_w = bad[i].w; bp.max_h = bad[i].h;
            bp.game_x = bad[i].x; bp.game_y = bad[i].y;
            bp.game_w = bad[i].w; bp.game_h = bad[i].h;
            chrome_render(ps, TW + 2 * GUARD2, &bp, &c.layout);
            chrome_render_battery(ps, TW + 2 * GUARD2, &bp, &c.layout, 50);

            int bad_px = 0;
            for (size_t j = 0; j < bs; j++) {
                int x = (int)(j % (size_t)(TW + 2 * GUARD2));
                int y = (int)(j / (size_t)(TW + 2 * GUARD2));
                int is_guard = (x < GUARD2 || x >= TW + GUARD2 ||
                                y < GUARD2 || y >= TH + GUARD2);
                if (is_guard && g2[j] != SENT) bad_px++;
            }
            if (bad_px)
                fprintf(stderr, "  LCD guard band: %s corrupted %d px\n",
                        bad[i].name, bad_px);
            CHECK_EQ_INT(bad_px, 0);
        }
        free(g2);
    }
})
