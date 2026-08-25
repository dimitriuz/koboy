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

            int bad = 0;
            for (int y = q.game_y; y < q.game_y + q.game_h; y++)
                for (int x = q.game_x; x < q.game_x + q.game_w; x++)
                    if (fb[(size_t)y * W + x] != 0x7F) bad++;
            if (bad) {
                fprintf(stderr, "  chrome inside game rect: %dx%d scale=%d -> "
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

    /* The MENU zone is a LIVE TOUCH ZONE, so chrome_controls_top must account
       for it. That function's contract is "the topmost row any drawn control
       or live touch zone occupies", and it exists because a scale = 0
       auto-fitted rect once swallowed the A button while its touch zone stayed
       live underneath -- tapping the lower playfield pressed A. A new zone the
       function does not know about reintroduces exactly that. */
    {
        koboy_config c; config_defaults(&c);
        const int W = 1264, H = 1680;
        int top = chrome_controls_top(&c.layout, W, H);
        int menu_top = (c.layout.menu_cy * H / 1000) - (c.layout.menu_h * H / 1000) / 2;
        CHECK(top <= menu_top);
    }
})
