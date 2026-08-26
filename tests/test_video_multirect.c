#include "test.h"
#include "video.h"
#include <string.h>

/* Marks an 8x8-aligned block as changed. */
static void dirty_block(uint8_t *cur, int stride, int bx, int by, int bw, int bh)
{
    for (int y = by * KOBOY_TILE; y < (by + bh) * KOBOY_TILE; y++)
        for (int x = bx * KOBOY_TILE; x < (bx + bw) * KOBOY_TILE; x++)
            cur[(size_t)y * stride + x] = 0x00;
}

/* Every changed tile must be covered by SOME emitted rect. A split that drops
   a region leaves a stale pixel on the panel, which is indistinguishable from
   ghosting and therefore the kind of bug nobody reports. */
static int covers_all_dirty(const uint8_t *prev, const uint8_t *cur,
                            int w, int h, int stride,
                            const koboy_rect *r, int n)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            size_t off = (size_t)y * stride + (size_t)x;
            if (prev[off] == cur[off]) continue;
            int covered = 0;
            for (int i = 0; i < n && !covered; i++)
                if (x >= r[i].x && x < r[i].x + r[i].w &&
                    y >= r[i].y && y < r[i].y + r[i].h) covered = 1;
            if (!covered) return 0;
        }
    }
    return 1;
}

static long total_area(const koboy_rect *r, int n)
{
    long a = 0;
    for (int i = 0; i < n; i++) a += (long)r[i].w * r[i].h;
    return a;
}

TEST_MAIN({
    enum { W = 320, H = 288, S = 320 };
    static uint8_t prev[W * H], cur[W * H];
    koboy_rect out[KOBOY_MAX_RECTS];

    /* Nothing changed: zero rects, and the caller refreshes nothing. */
    memset(prev, 0xFF, sizeof prev);
    memset(cur, 0xFF, sizeof cur);
    CHECK_EQ_INT(video_split_dirty(prev, cur, W, H, S, 40, out, KOBOY_MAX_RECTS), 0);

    /* One small change: one rect, and it must be small. Splitting a single
       compact region would only add fixed cost. */
    memcpy(cur, prev, sizeof cur);
    dirty_block(cur, S, 1, 1, 2, 2);
    int n = video_split_dirty(prev, cur, W, H, S, 40, out, KOBOY_MAX_RECTS);
    CHECK_EQ_INT(n, 1);
    CHECK(covers_all_dirty(prev, cur, W, H, S, out, n));
    CHECK(total_area(out, n) < (long)W * H / 4);

    /* THE WIN CASE: a sprite top-left and a status bar bottom-right. Merged,
       the bounding box is nearly the whole rect and its interior has not
       changed. Split, the two pieces are tiny. */
    memcpy(cur, prev, sizeof cur);
    dirty_block(cur, S, 0, 0, 2, 2);
    dirty_block(cur, S, W / KOBOY_TILE - 2, H / KOBOY_TILE - 2, 2, 2);
    n = video_split_dirty(prev, cur, W, H, S, 40, out, KOBOY_MAX_RECTS);
    CHECK(n >= 2);
    CHECK(covers_all_dirty(prev, cur, W, H, S, out, n));
    CHECK(total_area(out, n) < (long)W * H / 2);

    /* THE SCROLLER CASE: everything changed. One rect is correct -- splitting
       would pay N times the fixed cost for the same area. This is the case the
       spec is honest about: no rectangle strategy helps a full-screen scroller. */
    memcpy(cur, prev, sizeof cur);
    dirty_block(cur, S, 0, 0, W / KOBOY_TILE, H / KOBOY_TILE);
    n = video_split_dirty(prev, cur, W, H, S, 40, out, KOBOY_MAX_RECTS);
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_INT(out[0].w, W);
    CHECK_EQ_INT(out[0].h, H);
    CHECK(covers_all_dirty(prev, cur, W, H, S, out, n));

    /* A very large fixed cost must suppress splitting entirely: at that point
       one big refresh genuinely is cheaper than several small ones. This is
       why the constant is configurable rather than compiled in. */
    memcpy(cur, prev, sizeof cur);
    dirty_block(cur, S, 0, 0, 2, 2);
    dirty_block(cur, S, W / KOBOY_TILE - 2, H / KOBOY_TILE - 2, 2, 2);
    n = video_split_dirty(prev, cur, W, H, S, 1000000, out, KOBOY_MAX_RECTS);
    CHECK_EQ_INT(n, 1);
    CHECK(covers_all_dirty(prev, cur, W, H, S, out, n));

    /* max_out is honoured, and coverage still holds when the split is capped. */
    memcpy(cur, prev, sizeof cur);
    for (int i = 0; i < 8; i++) dirty_block(cur, S, i * 4, i * 3, 1, 1);
    n = video_split_dirty(prev, cur, W, H, S, 40, out, 2);
    CHECK(n >= 1 && n <= 2);
    CHECK(covers_all_dirty(prev, cur, W, H, S, out, n));

    /* max_out of 1 always degrades to the merged bounding box. */
    n = video_split_dirty(prev, cur, W, H, S, 40, out, 1);
    CHECK_EQ_INT(n, 1);
    CHECK(covers_all_dirty(prev, cur, W, H, S, out, n));

    /* THE BOUNDARY CASE. Every check above uses either a fixed_tiles nowhere
       near the decision (40 with tiny blocks, or 1000000) or a shape with no
       decision to make at all (the scroller). None of them can tell a live
       cost gate from a stuck-open or stuck-shut one: a mutant that deletes
       the comparison only fails the 1000000 case above (see task-13-report.md,
       Step 9), and mutants that bias the gate toward merging more -- which is
       the failure mode that matters, since 20/40/80 is exactly the range
       Step 10's on-device tuning run will try -- pass everything else green.
       This block picks a shape whose merged-vs-split cost actually crosses
       zero inside that range, so the outcome differs by fixed_tiles alone:
       two 2x2-tile blocks with a 3-tile gap between them (tile (0,0) and
       tile (5,5)) merge into a 7x7-tile (49-tile) bounding box; each block is
       4 tiles, so splitting saves 49 - 2*4 = 41 tiles of fixed-cost-free area.
       fixed_tiles < 41 must split; fixed_tiles > 41 must merge. 20 and 80
       straddle that on either side of the same 20/40/80 sweep Step 10 uses. */
    memcpy(cur, prev, sizeof cur);
    dirty_block(cur, S, 0, 0, 2, 2);
    dirty_block(cur, S, 5, 5, 2, 2);
    koboy_rect blow[KOBOY_MAX_RECTS], bhigh[KOBOY_MAX_RECTS];
    int nlow  = video_split_dirty(prev, cur, W, H, S, 20, blow,  KOBOY_MAX_RECTS);
    int nhigh = video_split_dirty(prev, cur, W, H, S, 80, bhigh, KOBOY_MAX_RECTS);
    CHECK(nlow >= 2);
    CHECK(covers_all_dirty(prev, cur, W, H, S, blow, nlow));
    CHECK_EQ_INT(nhigh, 1);
    CHECK(covers_all_dirty(prev, cur, W, H, S, bhigh, nhigh));
})
