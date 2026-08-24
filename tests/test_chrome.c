#include "test.h"
#include "pgm.h"
#include "chrome.h"
#include "config.h"

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
})
