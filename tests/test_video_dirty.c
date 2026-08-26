#include "test.h"
#include "config.h"
#include "video.h"

#define W 64
#define H 48

TEST_MAIN({
    static uint8_t a[W * H], b[W * H];
    memset(a, 0, sizeof a); memset(b, 0, sizeof b);

    /* identical buffers: empty rect, so presentation is skipped entirely */
    koboy_rect r = video_dirty_rect(a, b, W, H, W);
    CHECK_EQ_INT(r.w, 0);

    /* one pixel changed: rect snaps out to its whole 8x8 tile */
    b[10 * W + 11] = 255;
    r = video_dirty_rect(a, b, W, H, W);
    CHECK_EQ_INT(r.x, 8); CHECK_EQ_INT(r.y, 8);
    CHECK_EQ_INT(r.w, 8); CHECK_EQ_INT(r.h, 8);

    /* two distant changes: bounding box spans both tiles */
    memset(b, 0, sizeof b);
    b[0] = 255;
    b[(H - 1) * W + (W - 1)] = 255;
    r = video_dirty_rect(a, b, W, H, W);
    CHECK_EQ_INT(r.x, 0); CHECK_EQ_INT(r.y, 0);
    CHECK_EQ_INT(r.w, W); CHECK_EQ_INT(r.h, H);

    /* whole buffer changed */
    memset(b, 7, sizeof b);
    r = video_dirty_rect(a, b, W, H, W);
    CHECK_EQ_INT(r.w, W); CHECK_EQ_INT(r.h, H);

    /* a change confined to one row of tiles does not grow vertically */
    memset(b, 0, sizeof b);
    for (int x = 0; x < W; x++) b[20 * W + x] = 255;
    r = video_dirty_rect(a, b, W, H, W);
    CHECK_EQ_INT(r.y, 16); CHECK_EQ_INT(r.h, 8);
    CHECK_EQ_INT(r.w, W);

    /* video_invalidate forces the NEXT submit to report the whole game rect
       dirty. Required because a UI mode paints over the game rect, so the
       prev buffer no longer describes what is on the panel -- without this the
       first frame back diffs against a screen that is gone and silently leaves
       chrome-covered pixels stale. */
    {
        koboy_profile p; koboy_config c;
        config_defaults(&c);
        config_resolve_profile(&p, &c, 1264, 1680, KOBOY_GB_W, KOBOY_GB_H, KOBOY_GB_W, KOBOY_GB_H);
        koboy_video *v = video_create(&p, false);
        CHECK(v != NULL);

        static uint16_t frame[KOBOY_GB_W * KOBOY_GB_H];
        for (int i = 0; i < KOBOY_GB_W * KOBOY_GB_H; i++) frame[i] = 0x0000;

        koboy_rect r1 = video_submit(v, frame, KOBOY_GB_W, KOBOY_GB_H,
                                     KOBOY_GB_W * 2, KOBOY_PIXFMT_RGB565);
        CHECK_EQ_INT(r1.w, p.game_w);          /* first frame is fully dirty */

        /* The same frame again changes nothing. */
        koboy_rect r2 = video_submit(v, frame, KOBOY_GB_W, KOBOY_GB_H,
                                     KOBOY_GB_W * 2, KOBOY_PIXFMT_RGB565);
        CHECK_EQ_INT(r2.w, 0);

        /* After invalidation the identical frame is fully dirty again. */
        video_invalidate(v);
        koboy_rect r3 = video_submit(v, frame, KOBOY_GB_W, KOBOY_GB_H,
                                     KOBOY_GB_W * 2, KOBOY_PIXFMT_RGB565);
        CHECK_EQ_INT(r3.w, p.game_w);
        CHECK_EQ_INT(r3.h, p.game_h);
        CHECK_EQ_INT(r3.x, 0);
        CHECK_EQ_INT(r3.y, 0);

        video_destroy(v);
    }
})
