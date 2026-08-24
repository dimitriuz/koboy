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

    /* Guard-band test: the primitives must clip their writes to the panel
       bounds. Construct pathological geometry that would write far outside
       the panel if not clamped: bezel extends 6px beyond game rect on all
       sides, so with tight margins, writes reach beyond [0, panel_w/h). */
    const int GUARD = 64;
    const uint8_t SENTINEL = 0x42;
    int TW = 400, TH = 500;  /* tight panel dimensions */
    size_t buf_size = (size_t)(TW + 2 * GUARD) * (TH + 2 * GUARD);
    uint8_t *guarded = malloc(buf_size);
    memset(guarded, SENTINEL, buf_size);
    uint8_t *panel_start = guarded + (size_t)GUARD * (TW + 2 * GUARD) + GUARD;

    /* Construct a pathological profile where the bezel overflows on all
       sides. game_x=2, game_y=2, game_w=396, game_h=496.
       Bezel drawn from (1,1) with size (398,498), frame expands ±6:
       - Left writes reach x = 1 - 6 = -5 (OUT OF BOUNDS)
       - Top writes reach y = 1 - 6 = -5 (OUT OF BOUNDS)
       - Right writes reach x = 1 + 398 + 6 = 405 > 400 (OUT OF BOUNDS)
       - Bottom writes reach y = 1 + 498 + 6 = 505 > 500 (OUT OF BOUNDS)
       The background fill and other primitives (d-pad, A/B, Start/Select)
       will also reach outside bounds with this geometry.
       Clamping is essential here; without it, this test would corrupt all
       four guard regions. */
    koboy_profile tight;
    memset(&tight, 0, sizeof tight);  /* zero-initialize */
    tight.scale = 1;
    tight.panel_w = TW;
    tight.panel_h = TH;
    tight.game_x = 2;
    tight.game_y = 2;
    tight.game_w = TW - tight.game_x - 2;  /* 396 */
    tight.game_h = TH - tight.game_y - 2;  /* 496 */

    /* Render with the stride covering the full guarded buffer */
    chrome_render(panel_start, TW + 2 * GUARD, &tight, &c.layout);

    /* Verify all padding bytes remain untouched. Without clamping in the
       primitives, the bezel would write at x = -5 to 405 and y = -5 to 505,
       corrupting all four guard regions. */
    int corrupted = 0;
    uint8_t *p_start = guarded;
    for (size_t i = 0; i < buf_size; i++) {
        int x = (int)(i % (size_t)(TW + 2 * GUARD));
        int y = (int)(i / (size_t)(TW + 2 * GUARD));
        int is_guard = (x < GUARD || x >= TW + GUARD || y < GUARD || y >= TH + GUARD);
        if (is_guard && p_start[i] != SENTINEL) corrupted++;
    }
    CHECK_EQ_INT(corrupted, 0);
    free(guarded);
})
