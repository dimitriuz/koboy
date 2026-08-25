#include "test.h"
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
})
