#include "test.h"
#include "pgm.h"
#include "chrome.h"
#include "config.h"
#include <stdlib.h>

static int render(koboy_config *c, int W, int H, uint8_t *fb, koboy_profile *p)
{
    config_resolve_profile(p, c, W, H);
    memset(fb, 0x7F, (size_t)W * H);
    chrome_render(fb, W, p, &c->layout);
    return 1;
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
    /* Bezel from (1,1) size (398,498), frame expands by max 5:
       Leftmost: x = 1 - 5 = -4 (OUT); Topmost: y = 1 - 5 = -4 (OUT)
       Rightmost: x = 1 + 398 + 5 = 404 > 400 (OUT); Bottommost: y = 1 + 498 + 5 = 504 > 500 (OUT)
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
    /* Bezel from (1,49) size (398,202), frame expands by max 5:
       y-range: 49 - 5 = 44 to 251 + 5 = 256 (all in [0, 500))
       x-range: 1 - 5 = -4 to 399 + 5 = 404 (extends beyond [0, 400))
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
    /* Bezel from (49,1) size (202,500), frame expands by max 5:
       x-range: 49 - 5 = 44 to 251 + 5 = 256 (all in [0, 400))
       y-range: 1 - 5 = -4 to 501 + 5 = 506 (extends beyond [0, 500))
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

    free(guarded);
})
