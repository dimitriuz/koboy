#include "video.h"
#include <string.h>
#include <stdlib.h>

/* The shadow-lift curve: out = v * (255 + K) / (v + K).

   Gamma ~0.85 is what the measurements wanted, and this is that curve with no
   pow() and no float -- the pixel path is integer by constraint. Monotone,
   exactly 0 at 0 and exactly 255 at 255 (white stays paper-white, black stays
   black), one multiply and one divide a reviewer can redo by hand.

   IT IS NOT A NO-OP BEFORE A QUANTISER, which is what it looks like: the
   quantiser's thresholds are FIXED at 43/128/213, so lifting the shadows is
   exactly equivalent to lowering them, and lowering the first is what stops a
   dark-but-coloured background collapsing to solid black. MEASURED over 38
   gameplay frames from 19 titles (NES / WonderSwan Color / Neo Geo Pocket
   Color): pixels carrying visible colour (max channel >= 24) that quantise to
   level 0 fall from 6.7% to 2.5%. Kirby's Adventure's brick wall rgb(99,20,0)
   goes 41 -> 48, off level 0 onto level 1 -- a solid black slab becoming a
   wall with a pattern.

   K = 1000 is not tuned to death: 700 and 1400 were measured too and move the
   statistic by fractions of a percent. 1000 is the value whose curve sits
   closest to gamma 0.85 (83 -> 96 against 98, 169 -> 181 against 180) while
   keeping the Game Boy's two mid greys inside their original levels. */
#define KOBOY_GRAY_LIFT 1000u

static inline unsigned gray_lift(unsigned v)
{
    return (v * (255u + KOBOY_GRAY_LIFT)) / (v + KOBOY_GRAY_LIFT);
}

/* One row per koboy_gray_map, in enum order. Every non-VALUE row's weights sum
   to EXACTLY 256, which makes (w.r*255 + w.g*255 + w.b*255) >> 8 come out at
   exactly 255 -- white must stay white or the quantiser's top level stops
   being reachable.

   KOBOY_GRAY_VALUE has no weights (it is max(R,G,B), which no weighted sum
   expresses); its row exists so the table stays indexable by the enum.

   WHY BALANCED IS NOT EQUAL: (R+G+B)/3 is the obvious answer to "Rec.601
   crushes blue skies to black" and it is the WRONG one. Measured over the same
   38 frames from 19 colour titles, equal weights crush MORE pixels to level 0
   than Rec.601 does -- 8.9% against 6.7% -- because what they hand back to
   blue they take from green (they turn Kirby's Adventure's floor solid black,
   which Rec.601 did not). What removes the crushing is the shadow lift, not
   the weights. BALANCED's blue is about twice Rec.601's, which raises a sky,
   and its green stays high enough that Sonic's own blue body still lands a
   level BELOW the sky he is drawn against -- the failure EQUAL gets close to.
   Both halves are needed. */
static const struct { uint16_t wr, wg, wb; uint8_t lift; } GRAY_MAPS[KOBOY_GRAY_COUNT] = {
    /* LUMA     */ {  77, 150,  29, 0 },
    /* BRIGHT   */ {  77, 150,  29, 1 },
    /* BALANCED */ {  81, 118,  57, 1 },
    /* EQUAL    */ {  85,  85,  86, 1 },
    /* VALUE    */ {   0,   0,   0, 0 },
};

static const char *const GRAY_NAMES[KOBOY_GRAY_COUNT] = {
    "luma", "bright", "balanced", "equal", "value"
};

/* Contract, and why this is a separate exported function rather than an `if`
   inside gray_of, in video.h. */
koboy_gray_map video_gray_map_clamp(int m)
{
    if (m < 0 || m >= KOBOY_GRAY_COUNT) return KOBOY_GRAY_DEFAULT;
    return (koboy_gray_map)m;
}

/* 8-bit channels in, one grey out. Callers expand 5/6-bit RGB565 by bit
   replication first, so 0x1F -> 0xFF exactly and the two entry points cannot
   disagree about what a saturated channel means. */
static uint8_t gray_of(koboy_gray_map m_in, unsigned r, unsigned g, unsigned b)
{
    koboy_gray_map m = video_gray_map_clamp((int)m_in);
    if (m == KOBOY_GRAY_VALUE) {
        unsigned v = r > g ? r : g;
        if (b > v) v = b;
        return (uint8_t)v;
    }
    {
        unsigned v = (GRAY_MAPS[m].wr * r + GRAY_MAPS[m].wg * g +
                      GRAY_MAPS[m].wb * b) >> 8;
        return (uint8_t)(GRAY_MAPS[m].lift ? gray_lift(v) : v);
    }
}

const char *video_gray_map_name(koboy_gray_map m)
{
    return GRAY_NAMES[video_gray_map_clamp((int)m)];
}

bool video_gray_map_parse(const char *s, koboy_gray_map *out)
{
    if (!s || !out) return false;
    for (int i = 0; i < KOBOY_GRAY_COUNT; i++)
        if (!strcmp(s, GRAY_NAMES[i])) { *out = (koboy_gray_map)i; return true; }
    return false;
}

uint8_t video_rgb565_to_gray(uint16_t px, koboy_gray_map m)
{
    unsigned r5 = (px >> 11) & 0x1Fu, g6 = (px >> 5) & 0x3Fu, b5 = px & 0x1Fu;
    unsigned r = (r5 << 3) | (r5 >> 2);
    unsigned g = (g6 << 2) | (g6 >> 4);
    unsigned b = (b5 << 3) | (b5 >> 2);
    return gray_of(m, r, g, b);
}

uint8_t video_xrgb8888_to_gray(uint32_t px, koboy_gray_map m)
{
    return gray_of(m, (px >> 16) & 0xFFu, (px >> 8) & 0xFFu, px & 0xFFu);
}

void video_gray_lut_build(uint8_t lut[65536], koboy_gray_map m)
{
    for (int i = 0; i < 65536; i++) lut[i] = video_rgb565_to_gray((uint16_t)i, m);
}

/* Nearest-neighbour integer upscale: each source pixel becomes a solid run of
   `scale` bytes, each source row emitted once then memcpy-replicated. Per-pixel
   work collapses into block copies.

   PRECONDITIONS, satisfied by construction at the only call site and NOT
   enforced here: scale >= 1, dst_stride >= src_w * scale. A scale of 0 emits
   nothing; a short dst_stride overlaps rows. */
void video_scale_gray(uint8_t *dst, int dst_stride, const uint8_t *src,
                      int src_w, int src_h, int src_stride, int scale)
{
    const int out_w = src_w * scale;
    for (int sy = 0; sy < src_h; sy++) {
        uint8_t *row = dst + (size_t)sy * scale * dst_stride;
        const uint8_t *s = src + (size_t)sy * src_stride;
        if (scale == 1) {
            memcpy(row, s, (size_t)src_w);
        } else {
            uint8_t *o = row;
            for (int sx = 0; sx < src_w; sx++, o += scale) memset(o, s[sx], (size_t)scale);
        }
        for (int r = 1; r < scale; r++)
            memcpy(row + (size_t)r * dst_stride, row, (size_t)out_w);
    }
}

/* Contract, and every reason behind the choices here, in video.h. */
void video_scale_gray_frac(uint8_t *dst, int dst_stride, const uint8_t *src,
                           int src_w, int src_h, int src_stride,
                           int dst_w, int dst_h)
{
    /* LIVE GUARD, two things. A non-positive extent has nothing to draw and
       makes the step divisions a division by zero. A source over 65535 px on
       an axis cannot be expressed as a 16.16 step at all -- (src << 16) would
       overflow uint32_t and the scaler would silently sample the wrong pixels.
       No core geometry is near that (the largest measured is 973). */
    if (src_w < 1 || src_h < 1 || dst_w < 1 || dst_h < 1) return;
    if (src_w > 65535 || src_h > 65535) return;

    const uint32_t xstep = ((uint32_t)src_w << 16) / (uint32_t)dst_w;
    const uint32_t ystep = ((uint32_t)src_h << 16) / (uint32_t)dst_h;

    /* Neither accumulator can overflow: after dst_w steps xfp has reached at
       most dst_w * ((src_w << 16) / dst_w) <= src_w << 16 <= 0xFFFF0000, and
       the same bound holds on the y axis. */
    int      prev_sy  = -1;
    uint8_t *prev_row = NULL;
    uint32_t yfp = 0;

    /* NO CLAMP ON sx/sy, and that is a PROOF, not an oversight. The step is
       FLOORED: xstep <= (src_w << 16) / dst_w, so after the last of dst_w
       increments the accumulator has reached at most (dst_w - 1) * xstep <
       src_w << 16, i.e. sx <= src_w - 1 always; truncation can only sample too
       LOW. Same on y. REINTRODUCE A CLAMP IF THE STEP EVER STOPS BEING FLOORED
       (a rounded step CAN overshoot) -- but not before, because a clamp that
       cannot fire is a clamp no test can prove. */
    for (int dy = 0; dy < dst_h; dy++, yfp += ystep) {
        int sy = (int)(yfp >> 16);
        uint8_t *row = dst + (size_t)dy * dst_stride;
        if (sy == prev_sy) {                  /* repeated source row: copy it */
            memcpy(row, prev_row, (size_t)dst_w);
            continue;
        }

        const uint8_t *s = src + (size_t)sy * src_stride;
        uint32_t xfp = 0;
        for (int dx = 0; dx < dst_w; dx++, xfp += xstep)
            row[dx] = s[xfp >> 16];
        prev_sy = sy; prev_row = row;
    }
}

/* Four evenly spaced levels for the DU4 waveform. These may need retuning
   against a real panel; they are deliberately in one place. */
const uint8_t KOBOY_DU4_LEVELS[4] = { 0x00, 0x55, 0xAA, 0xFF };

static uint8_t g_bayer[16][16];
/* The THRESHOLDS video_dither_1bit actually compares against, which are not
   the Bayer matrix itself. See bayer_ensure. */
static uint8_t g_thresh[16][16];
static int     g_bayer_ready = 0;

void video_bayer_build(uint8_t m[16][16])
{
    /* Recursive Bayer construction: double a 1x1 seed to 16x16. */
    static const int quad[2][2] = { { 0, 2 }, { 3, 1 } };
    int cur[16][16], nxt[16][16];
    int n = 1;
    cur[0][0] = 0;
    while (n < 16) {
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++)
                for (int dy = 0; dy < 2; dy++)
                    for (int dx = 0; dx < 2; dx++)
                        nxt[y + dy * n][x + dx * n] = 4 * cur[y][x] + quad[dy][dx];
        n *= 2;
        for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) cur[y][x] = nxt[y][x];
    }
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) m[y][x] = (uint8_t)cur[y][x];
}

/* WHY THE THRESHOLDS ARE NOT THE MATRIX.

   video_bayer_build produces a permutation of 0..255 -- what a 16x16 Bayer
   matrix IS, pinned by tests/test_video_quant.c. Thresholding `v > m` against
   it makes the white count for value v exactly v out of 256, which is right
   for every value except the one the panel renders best: v = 255 lands on
   `255 > 255`, false, so ONE cell in every 16x16 tile of pure white comes out
   black -- 2250 isolated dots at 16 px spacing on an 800x720 rect. Pure white
   is no corner case: it is the Game Boy's own lightest shade (gambatte emits
   exactly rgb(255,255,255)) and most HUD text everywhere else.

   Scaling to 0..254 fixes the top without moving anything else: 255 > 254 in
   every cell, 0 still black in every cell, interior counts shift by at most
   one cell in 256. The one visible consequence is m = 0 and m = 1 both mapping
   to threshold 0, so v = 1 lights two cells -- a near-black 0.8% lighter,
   against a pure white that stops being speckled.

   A SECOND table rather than folded into video_bayer_build: the matrix is a
   mathematical object other code and tests ask for by name, and scaling per
   pixel would put a multiply and a divide in the measured bottleneck
   (video_submit, 17 ms) to save 256 bytes.

   LAZY INIT, single-threaded by construction: nothing in src/ creates a
   thread. If a worker thread ever appears, this needs a once-guard. */
static void bayer_ensure(void)
{
    if (g_bayer_ready) return;
    video_bayer_build(g_bayer);
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            g_thresh[y][x] = (uint8_t)(((unsigned)g_bayer[y][x] * 255u) / 256u);
    g_bayer_ready = 1;
}

void video_quantise4(uint8_t *buf, int w, int h, int stride)
{
    /* Nearest of the four levels. Thresholds sit midway between them. */
    for (int y = 0; y < h; y++) {
        uint8_t *row = buf + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            unsigned v = row[x];
            unsigned i = v < 43 ? 0 : v < 128 ? 1 : v < 213 ? 2 : 3;
            row[x] = KOBOY_DU4_LEVELS[i];
        }
    }
}

void video_dither_1bit(uint8_t *buf, int w, int h, int stride,
                       int screen_x, int screen_y)
{
    bayer_ensure();
    for (int y = 0; y < h; y++) {
        uint8_t *row = buf + (size_t)y * stride;
        const uint8_t *brow = g_thresh[(unsigned)(screen_y + y) & 15u];
        for (int x = 0; x < w; x++)
            row[x] = row[x] > brow[(unsigned)(screen_x + x) & 15u] ? 255 : 0;
    }
}

koboy_rect video_dirty_rect(const uint8_t *prev, const uint8_t *cur,
                           int w, int h, int stride)
{
    koboy_rect r = { 0, 0, 0, 0 };
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int ty = 0; ty < h; ty += KOBOY_TILE) {
        int th = (ty + KOBOY_TILE <= h) ? KOBOY_TILE : h - ty;
        for (int tx = 0; tx < w; tx += KOBOY_TILE) {
            int tw = (tx + KOBOY_TILE <= w) ? KOBOY_TILE : w - tx;
            int changed = 0;
            for (int y = 0; y < th && !changed; y++) {
                size_t off = (size_t)(ty + y) * stride + (size_t)tx;
                if (memcmp(prev + off, cur + off, (size_t)tw) != 0) changed = 1;
            }
            if (changed) {
                if (tx < x0) x0 = tx;
                if (ty < y0) y0 = ty;
                if (tx + tw - 1 > x1) x1 = tx + tw - 1;
                if (ty + th - 1 > y1) y1 = ty + th - 1;
            }
        }
    }
    if (x1 < 0) return r;                /* nothing changed */
    r.x = x0; r.y = y0; r.w = x1 - x0 + 1; r.h = y1 - y0 + 1;
    return r;
}

/* Bound on the tile grid video_split_dirty will attempt to split, tiles per
   axis. Sized against the largest rect ever fitted (800x720 is 100x90 tiles)
   with headroom. A bigger request falls back to the merged box rather than
   growing the scratch buffer, keeping it a fixed auditable size. Coverage
   still holds -- a missed optimisation, never a missed rect. */
#define KOBOY_SPLIT_MAX_TILES 300

/* Upper bound on band x column-run candidates before capping to max_out. 64
   covers every shape this UI produces; a pathological checkerboard could
   demand more, and video_split_dirty bails to the merged box rather than
   overflow -- same reasoning as KOBOY_SPLIT_MAX_TILES. */
#define KOBOY_SPLIT_MAX_CANDIDATES 64

/* Scratch tile-dirty bitmap. File-scope static, not a local: it would
   otherwise be 90000 bytes of stack pushed and popped every presented frame.
   Reused across calls, which is safe ONLY because nothing in src/ creates a
   thread -- a worker thread sharing it would need a lock or its own copy. */
static uint8_t g_tile_dirty[KOBOY_SPLIT_MAX_TILES][KOBOY_SPLIT_MAX_TILES];

static int tiles_in(koboy_rect r)
{
    return ((r.w + KOBOY_TILE - 1) / KOBOY_TILE) * ((r.h + KOBOY_TILE - 1) / KOBOY_TILE);
}

static koboy_rect rect_union(koboy_rect a, koboy_rect b)
{
    int x0 = a.x < b.x ? a.x : b.x;
    int y0 = a.y < b.y ? a.y : b.y;
    int x1 = (a.x + a.w) > (b.x + b.w) ? a.x + a.w : b.x + b.w;
    int y1 = (a.y + a.h) > (b.y + b.h) ? a.y + a.h : b.y + b.h;
    koboy_rect u = { x0, y0, x1 - x0, y1 - y0 };
    return u;
}

/* Is `b` entirely inside `a` (including equal)? Drops redundant candidates
   after the merge-cap below: unioning in band-major order can, once a merged
   box has absorbed a third candidate, strictly contain another that was never
   unioned with it. Dropping it is free and turns a guaranteed double-refresh
   into nothing. */
static bool rect_contains(koboy_rect a, koboy_rect b)
{
    return b.x >= a.x && b.y >= a.y &&
           b.x + b.w <= a.x + a.w && b.y + b.h <= a.y + a.h;
}

/* Does tile-row `r` (in the reduced tby0-relative grid, width sub_tw) contain
   any dirty tile? One function so row- and column-segmentation cannot drift. */
static bool row_has_dirty(int r, int sub_tw)
{
    for (int c = 0; c < sub_tw; c++) if (g_tile_dirty[r][c]) return true;
    return false;
}

static bool col_has_dirty_in_band(int c, int r0, int r1)
{
    for (int r = r0; r <= r1; r++) if (g_tile_dirty[r][c]) return true;
    return false;
}

int video_split_dirty(const uint8_t *prev, const uint8_t *cur,
                      int w, int h, int stride, int fixed_tiles,
                      koboy_rect *out, int max_out)
{
    /* LIVE GUARD: out == NULL has nowhere to write a rect, so there is nothing
       to honestly report. Exercised directly by
       tests/test_video_multirect.c. */
    if (!out) return 0;

    koboy_rect merged = video_dirty_rect(prev, cur, w, h, stride);
    if (merged.w == 0) return 0;                  /* nothing changed */

    /* LIVE GUARD: max_out < 1 (a caller error) and max_out == 1 (a real
       request for the single-rect behaviour) both degrade to the merged box.
       One branch, so video_submit's wrapper never touches the segmentation
       machinery below at all. */
    if (max_out <= 1) { out[0] = merged; return 1; }

    /* Tile-index bounding box of the merged rect. video_dirty_rect's walk is
       tile-aligned, so this recovers exactly the tile range it found dirty --
       no tile outside it can be dirty by construction. */
    int tbx0 = merged.x / KOBOY_TILE;
    int tby0 = merged.y / KOBOY_TILE;
    int tbx1 = (merged.x + merged.w - 1) / KOBOY_TILE;
    int tby1 = (merged.y + merged.h - 1) / KOBOY_TILE;
    int sub_tw = tbx1 - tbx0 + 1;
    int sub_th = tby1 - tby0 + 1;

    if (sub_tw > KOBOY_SPLIT_MAX_TILES || sub_th > KOBOY_SPLIT_MAX_TILES) {
        out[0] = merged; return 1;      /* see the comment on the constant */
    }

    /* Tile-dirty bitmap over just the merged rect's tile range, reusing
       video_dirty_rect's own memcmp walk per tile. */
    for (int r = 0; r < sub_th; r++) {
        int ty = (tby0 + r) * KOBOY_TILE;
        int th_ = (ty + KOBOY_TILE <= h) ? KOBOY_TILE : h - ty;
        for (int c = 0; c < sub_tw; c++) {
            int tx = (tbx0 + c) * KOBOY_TILE;
            int tw_ = (tx + KOBOY_TILE <= w) ? KOBOY_TILE : w - tx;
            int changed = 0;
            for (int y = 0; y < th_ && !changed; y++) {
                size_t off = (size_t)(ty + y) * stride + (size_t)tx;
                if (memcmp(prev + off, cur + off, (size_t)tw_) != 0) changed = 1;
            }
            g_tile_dirty[r][c] = (uint8_t)changed;
        }
    }

    /* Row-band segmentation: maximal runs of tile-rows containing a dirty
       tile, separated by a clean row; within each band, columns by the same
       rule. Bounds come straight from the tile indices with no tightening --
       always a safe, sometimes loose, superset of the true dirty pixels. */
    koboy_rect cand[KOBOY_SPLIT_MAX_CANDIDATES];
    int ncand = 0;
    bool overflowed = false;

    int r = 0;
    while (r < sub_th && !overflowed) {
        if (!row_has_dirty(r, sub_tw)) { r++; continue; }
        int band_r0 = r;
        while (r < sub_th && row_has_dirty(r, sub_tw)) r++;
        int band_r1 = r - 1;

        int py0 = (tby0 + band_r0) * KOBOY_TILE;
        int py1 = (tby0 + band_r1 + 1) * KOBOY_TILE; if (py1 > h) py1 = h;

        int c = 0;
        while (c < sub_tw) {
            if (!col_has_dirty_in_band(c, band_r0, band_r1)) { c++; continue; }
            int col_c0 = c;
            while (c < sub_tw && col_has_dirty_in_band(c, band_r0, band_r1)) c++;
            int col_c1 = c - 1;

            if (ncand >= KOBOY_SPLIT_MAX_CANDIDATES) { overflowed = true; break; }
            int px0 = (tbx0 + col_c0) * KOBOY_TILE;
            int px1 = (tbx0 + col_c1 + 1) * KOBOY_TILE; if (px1 > w) px1 = w;
            koboy_rect rect = { px0, py0, px1 - px0, py1 - py0 };
            cand[ncand++] = rect;
        }
    }

    /* The candidate array ran out (checkerboard-dense changes). Coverage must
       not depend on its size, so give up the split rather than silently drop
       what did not fit. */
    if (overflowed) { out[0] = merged; return 1; }

    /* Cap to max_out by MERGING candidates, never by dropping one: a union only
       grows, so coverage survives whichever pairs get merged. Order affects
       tightness, not correctness, so "merge the last two, repeatedly" suffices.

       This does NOT keep the emitted rects disjoint. Candidates are band-major
       ordered and merging the last two can cross a band boundary, so a box
       that already absorbed one neighbour can swallow a third outright
       (measured on production geometry: a merged 664x536 rect fully contained
       an unrelated 80x72 one). Coverage holds, but a nested rect is pure
       double-work downstream. The loop below removes that cheaply-detectable
       case only; it does not attempt general deoverlap. */
    while (ncand > max_out) {
        cand[ncand - 2] = rect_union(cand[ncand - 2], cand[ncand - 1]);
        ncand--;
    }

    /* Drop any candidate now fully contained in another. O(n^2) over at most
       KOBOY_MAX_RECTS entries. */
    for (int i = 0; i < ncand; i++) {
        for (int j = 0; j < ncand; j++) {
            if (i == j) continue;
            if (rect_contains(cand[j], cand[i])) {
                for (int k = i; k < ncand - 1; k++) cand[k] = cand[k + 1];
                ncand--;
                i--;
                break;
            }
        }
    }

    /* Split only when it is actually cheaper. With one candidate left (a shape
       with no internal gaps -- the full-screen-scroller case in
       tests/test_video_multirect.c) it already equals the merged box, so both
       branches produce the identical rect. */
    long sum = 0;
    for (int i = 0; i < ncand; i++) sum += (long)fixed_tiles + tiles_in(cand[i]);
    long cost_merged = (long)fixed_tiles + tiles_in(merged);

    if (ncand > 1 && sum < cost_merged) {
        memcpy(out, cand, (size_t)ncand * sizeof *out);
        return ncand;
    }
    out[0] = merged;
    return 1;
}

struct koboy_video {
    koboy_profile p;
    bool     dither;
    int      stride;
    uint8_t *cur, *prev, *gray;
    /* The frame size the last fit was computed for. A core whose base geometry
       changes mid-session (Game & Watch alternates between the whole unit and
       the LCD alone, several times a second) re-fits only when the size moves
       -- and the margin must be cleared on that transition, or the previous,
       larger frame stays visible around the edges of the new one. */
    int      fit_src_w, fit_src_h;
    /* Where the last frame landed inside the reserved rect (video_frame_rect).
       Recorded rather than recomputed by the caller so there is one answer,
       from the code that actually placed the pixels. */
    koboy_rect fit_rect;
    uint8_t  lut[65536];
    /* Which mapping `lut` was built with. STORED, not just applied: the
       XRGB8888 path does not go through the LUT at all and needs the same map,
       or the two pixel formats would render differently. */
    koboy_gray_map map;
    /* Quarter turns counter-clockwise per frame, 0..3 (video_set_rotation). 0
       for every core before FinalBurn Neo, and the rot == 0 path below is byte
       for byte the loop that existed before this field. */
    int      rot;
    /* The core's DISPLAY aspect in 16.16, or 0 for "never told"
       (video_set_aspect). 0 makes the square path below byte for byte the one
       that existed before the Atari 2600 arrived. */
    uint32_t dar;
};

/* Contract in video.h, including why the deadband matters. */
uint32_t video_pixel_aspect(uint32_t display_aspect, int frame_w, int frame_h)
{
    /* LIVE GUARD. 0 is "the core never said", the state every koboy_video
       starts in; a non-positive extent has no ratio and would divide by zero.
       Both answer square. */
    if (display_aspect == 0 || frame_w < 1 || frame_h < 1) return KOBOY_ASPECT_ONE;

    uint32_t par = (uint32_t)(((uint64_t)display_aspect * (uint64_t)frame_h)
                              / (uint64_t)frame_w);
    uint32_t d = par > KOBOY_ASPECT_ONE ? par - KOBOY_ASPECT_ONE
                                        : KOBOY_ASPECT_ONE - par;
    if (d <= KOBOY_PAR_DEADBAND) return KOBOY_ASPECT_ONE;
    return par;
}

/* Contract in video.h, including why only the horizontal axis carries the
   pixel aspect.

   A square-pixel frame at exactly max lands at p.scale with no offset -- every
   Game Boy frame ever, 160x144 into 800x720 at scale 5, bit for bit what this
   did before it could fit anything else. A frame SMALLER than max used to be
   scaled by p.scale and parked in the top-left, so a 305x191 Game & Watch
   in-game view drew at 1:1 in the corner of a 654x396 rect. Fitting it puts it
   at 2x, centred.

   Never returns a scale below 1, and the clamp is a LIVE GUARD rather than
   dead code: src cannot exceed the rect for any caller that went through
   video_pipeline_run's check, so the divisions cannot floor to 0 -- this is
   the defence for a caller that skipped it. */
void video_fit_par(const koboy_profile *p, int src_w, int src_h, uint32_t par,
                   int *scale_out, int *dw_out, int *ox_out, int *oy_out)
{
    if (par == 0) par = KOBOY_ASPECT_ONE;

    /* A degenerate source has no fit; 1x1 keeps every consumer's arithmetic
       inside the rect rather than handing back a zero or negative width. */
    int fs = 1, dw = 1;
    if (src_w > 0 && src_h > 0) {
        int fy = p->game_h / src_h;
        int fx;
        if (par == KOBOY_ASPECT_ONE) {
            /* THE SQUARE PATH, written out separately rather than folded into
               the fixed-point one: it is the only path most systems ever take
               and it must produce the SAME INTEGERS the pre-anisotropy version
               produced, not the same integers up to a rounding. */
            fx = p->game_w / src_w;
        } else {
            /* The widest integer vertical scale whose PAR-corrected width
               still fits. Floored, so src_w * fx * par <= game_w << 16 and the
               rounding below cannot push dw past game_w. */
            fx = (int)(((uint64_t)p->game_w << 16) / ((uint64_t)src_w * par));
        }
        fs = fx < fy ? fx : fy;
        if (fs < 1) fs = 1;

        if (par == KOBOY_ASPECT_ONE) {
            dw = src_w * fs;
        } else {
            dw = (int)((((uint64_t)src_w * (uint64_t)fs * par) + 32768u) >> 16);
            if (dw < 1) dw = 1;
        }
        /* LIVE, and unconditional rather than only on the fixed-point path:
           the fs >= 1 clamp above can force a scale the width test already
           rejected, and a dw wider than the rect is what
           video_pipeline_run's bounds guard exists to stop. It cannot fire for
           any frame that guard accepts, so this is the defence for a caller
           that skipped the check, not a correction of the arithmetic. */
    }
    if (dw > p->game_w) dw = p->game_w;
    if (dw < 1) dw = 1;
    int ox = (p->game_w - dw) / 2;
    int oy = (p->game_h - src_h * fs) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;
    *scale_out = fs; *dw_out = dw; *ox_out = ox; *oy_out = oy;
}

/* Contract in video.h: video_fit_par's square case, under its older name. */
void video_fit(const koboy_profile *p, int src_w, int src_h,
               int *scale_out, int *ox_out, int *oy_out)
{
    int dw;
    video_fit_par(p, src_w, src_h, KOBOY_ASPECT_ONE, scale_out, &dw, ox_out, oy_out);
}

/* Contract in video.h. CROSS-MULTIPLYING to decide which axis binds, rather
   than comparing two truncated ratios, is what makes the binding axis EXACT: a
   shared 16.16 ratio loses up to one part in 65536 twice over -- resolving the
   rect, then fitting a frame into it -- and the roundings compound into a
   couple of stray pixels of margin down one side. */
void video_fit_frac(int src_w, int src_h, int avail_w, int avail_h,
                    int *dw_out, int *dh_out)
{
    /* LIVE GUARD: a non-positive extent has no fit and would divide by zero
       below. Callers check their own inputs; this is the local defence. */
    if (src_w < 1 || src_h < 1 || avail_w < 1 || avail_h < 1) return;

    int dw, dh;
    if ((long)src_w * avail_h <= (long)src_h * avail_w) {
        dh = avail_h;                                   /* height binds */
        dw = (int)(((long)src_w * avail_h) / src_h);
    } else {
        dw = avail_w;                                   /* width binds */
        dh = (int)(((long)src_h * avail_w) / src_w);
    }
    /* A source far wider than tall can floor the non-binding axis to nothing;
       one row of artwork beats none. */
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    *dw_out = dw; *dh_out = dh;
}

void video_fit_rect(const koboy_profile *p, int src_w, int src_h, uint32_t par,
                    int *dw_out, int *dh_out, int *ox_out, int *oy_out)
{
    /* 1x1, not undefined: video_fit_frac writes nothing for a degenerate input
       and BOTH branches can reach it, so the fallback belongs at the
       declaration. One row of picture beats a garbage rect. */
    int dw = 1, dh = 1;
    if (par == 0) par = KOBOY_ASPECT_ONE;

    if (p->layout_mode == KOBOY_LAYOUT_LCD) {
        /* THE INVARIANT: A SQUARE-PIXEL FRAME AT EXACTLY MAX GEOMETRY FILLS
           THE RESERVED RECT. game_w/game_h were themselves produced by
           video_fit_frac from max_w/max_h, so re-fitting max into that result
           asks the same question twice -- and the second answer can be a pixel
           short, because the first's non-binding axis was floored. Saying so
           outright is faster and exact.

           SQUARE-PIXEL is load-bearing: the rect was resolved with no pixel
           aspect in it, so once par != 1 the premise is false and the general
           fit must run.

           `rect_from_max` likewise: SNES and Mega Drive share this layout and
           their rect comes from BASE, then possibly gets cut by the scale
           ceiling -- so the premise is false for them whatever the aspect is,
           and a frame arriving at exactly max would be STRETCHED to a rect it
           does not match. Guarded on the FLAG, not the layout, because "LCD"
           no longer means "Game & Watch". */
        if (p->rect_from_max &&
            par == KOBOY_ASPECT_ONE && src_w == p->max_w && src_h == p->max_h) {
            dw = p->game_w; dh = p->game_h;
        } else {
            /* The frame's DISPLAYED shape, which is what an aspect-preserving
               fit must preserve. At par == 1 this is src_w. */
            int ew = par == KOBOY_ASPECT_ONE
                   ? src_w
                   : (int)((((uint64_t)src_w * par) + 32768u) >> 16);
            if (ew < 1) ew = 1;
            video_fit_frac(ew, src_h, p->game_w, p->game_h, &dw, &dh);
        }
    } else {
        /* The frame's DISPLAYED width, used twice: to ask whether an integer
           fit is possible, and if not as the shape the fractional fit must
           preserve. SAME ROUNDING as video_fit_par's on purpose -- the two must
           agree about whether a frame is one pixel too wide. */
        int ew = par == KOBOY_ASPECT_ONE
               ? src_w
               : (int)((((uint64_t)src_w * par) + 32768u) >> 16);
        if (ew < 1) ew = 1;

        if (ew > p->game_w || src_h > p->game_h) {
            /* THE FRAME DOES NOT FIT EVEN AT 1:1, so there is no integer fit:
               video_fit_par's scale floor is 1 (it must be, or a frame smaller
               than the rect would round to nothing), so it cannot SHRINK -- it
               would hand back dh = src_h and the scaler would write past the
               bottom of v->cur.

               Reachable only since the DMG rect came from BASE geometry: the
               rect no longer holds every frame in [1, max] by construction, and
               snes9x2005's 512x512 max against its 256x224 base is the gap.
               Under the old max-sized rect it could not fire at all, which is
               why every existing fit is bit-for-bit unchanged.

               Fractional rather than refusing: a hi-res mode presented slightly
               small is a picture, a dropped frame is a frozen screen. */
            video_fit_frac(ew, src_h, p->game_w, p->game_h, &dw, &dh);
        } else {
            int fs, ox, oy;
            video_fit_par(p, src_w, src_h, par, &fs, &dw, &ox, &oy);
            dh = src_h * fs;
        }
    }

    int ox = (p->game_w - dw) / 2;
    int oy = (p->game_h - dh) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;
    *dw_out = dw; *dh_out = dh; *ox_out = ox; *oy_out = oy;
}

void video_frame_rect(const koboy_video *v, koboy_rect *out)
{
    if (!v || !out) return;
    *out = v->fit_rect;
}

koboy_video *video_create(const koboy_profile *p, bool force_dither,
                          koboy_gray_map map)
{
    koboy_video *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->p = *p;
    v->dither = force_dither;
    v->map = map;
    v->stride = p->game_w;
    size_t n = (size_t)v->stride * (size_t)p->game_h;
    v->cur  = calloc(1, n);
    v->prev = calloc(1, n);
    /* Sized to the core's MAX geometry, not a constant: a core may submit any
       frame up to max_w x max_h (video_pipeline_run's bounds check enforces
       the ceiling) and this must hold the largest without a mid-session
       realloc. For the Game Boy that is exactly 160x144. */
    v->gray = calloc(1, (size_t)p->max_w * (size_t)p->max_h);
    if (!v->cur || !v->prev || !v->gray) { video_destroy(v); return NULL; }
    video_gray_lut_build(v->lut, v->map);
    /* prev starts as an impossible value so the first frame is fully dirty */
    memset(v->prev, 0x01, n);
    return v;
}

/* Contract in video.h. Stored raw: core_display_aspect already validated, and
   0 is a meaningful value (video_pixel_aspect's live guard), not an error. */
void video_set_aspect(koboy_video *v, uint32_t display_aspect)
{
    if (!v) return;
    v->dar = display_aspect;
}

uint32_t video_get_aspect(const koboy_video *v)
{
    return v ? v->dar : 0u;
}

void video_set_rotation(koboy_video *v, int rot)
{
    if (!v) return;
    /* Masked, not validated: 0..3 is the whole domain (core.c masks the core's
       answer the same way) and a caller that computed 4 means 0. */
    v->rot = rot & 3;
    /* NOT invalidating prev, deliberately: this is called right after
       video_create, whose prev is already seeded impossible, and nowhere else
       -- a rotation change arrives with a geometry change and main.c rebuilds
       the whole koboy_video. A caller that DOES flip this live owes
       video_invalidate: every pixel moves, and prev would claim otherwise. */
}

int video_get_rotation(const koboy_video *v)
{
    return v ? v->rot : 0;
}

void video_set_gray_map(koboy_video *v, koboy_gray_map map)
{
    if (!v || v->map == map) return;     /* rebuilding an identical LUT is pure cost */
    v->map = map;
    video_gray_lut_build(v->lut, v->map);
}

koboy_gray_map video_get_gray_map(const koboy_video *v)
{
    return v ? v->map : (koboy_gray_map)KOBOY_GRAY_DEFAULT;
}

/* Contract in video.h. No invalidate on purpose: this cannot know what the
   caller is about to do, and video_invalidate costs a memset of the whole game
   rect. The one mid-session caller (main.c's MOTION row) is on the
   return-from-menu path, which invalidates unconditionally. */
void video_set_dither(koboy_video *v, bool on)
{
    if (!v) return;
    v->dither = on;
}

bool video_get_dither(const koboy_video *v)
{
    return v ? v->dither : false;
}

void video_destroy(koboy_video *v)
{
    if (!v) return;
    free(v->cur); free(v->prev); free(v->gray); free(v);
}

const uint8_t *video_buffer(const koboy_video *v) { return v->cur; }
int            video_stride(const koboy_video *v) { return v->stride; }

void video_invalidate(koboy_video *v)
{
    if (!v) return;
    /* 0x01 is not a value the four-level quantiser can ever emit, which is the
       same trick video_create uses to make the first frame fully dirty. */
    memset(v->prev, 0x01, (size_t)v->stride * (size_t)v->p.game_h);
}

/* Convert/scale/quantise-or-dither one core frame into v->cur. Returns false
   and touches nothing for a duplicate (NULL src) or a size the core had no
   business sending. Split out of video_submit_rects so the dirty-diff and
   prev-copy are the only difference between the two entry points. */
static bool video_pipeline_run(koboy_video *v, const void *src, int src_w, int src_h,
                               size_t src_pitch, koboy_pixfmt fmt)
{
    if (!src) return false;                        /* core signalled a duplicate */
    /* LIVE GUARD: v->gray is allocated at max_w x max_h, so a frame outside
       [1, max] is either nothing to draw or bigger than the buffer, and writing
       it walks off the end. A core that contradicts its own reported geometry
       is a broken core; dropping the frame is the correct response, and the
       alternative is scribbling into whatever follows the buffer.

       Checked against the PRESENTED size, not the buffer the core handed over:
       config_resolve_profile was given core_get_geometry's already-transposed
       max, so for a quarter-turned board p.max_w/max_h are 224x288 while the
       core's frame is 288x224. Comparing un-rotated w/h would reject every
       frame of Galaga -- a black game rect, which looks like a broken core. */
    const int fw = (v->rot & 1) ? src_h : src_w;   /* frame width  as presented */
    const int fh = (v->rot & 1) ? src_w : src_h;   /* frame height as presented */
    if (fw < 1 || fh < 1 || fw > v->p.max_w || fh > v->p.max_h) return false;

    /* rot == 0 is the pre-existing loop, unchanged, and still the only path
       any core but FinalBurn Neo takes. Its own branch rather than a general
       addressing scheme, because this is the pipeline's measured bottleneck
       (17 ms of a 23 ms frame) and a per-pixel indirection bought for a case
       nobody takes is the wrong trade.

       The three turning branches write `d` SEQUENTIALLY and read the source
       strided, not the reverse: the write side is a linear run the store
       buffer absorbs, while a strided WRITE would dirty a fresh cache line per
       pixel. The strided reads stay cheap because a whole pre-1990 board's
       source is ~130 KB and a Cortex-A9's L1 holds a column's worth of lines,
       which the next column reuses.

       COUNTER-CLOCKWISE quarter turns, matching libretro's SET_ROTATION. With
       (W,H) the source's own width and height:
         rot 1  out is (H,W):  out[y][x] = src[x][W-1-y]
         rot 2  out is (W,H):  out[y][x] = src[H-1-y][W-1-x]
         rot 3  out is (H,W):  out[y][x] = src[H-1-x][y]
       VERIFIED against a rendered Galaga frame, not derived and trusted: at
       rot 3 the score line reads left-to-right along the top and "CREDIT 0"
       sits at the bottom; at rot 1 the same frame is upside down. */
#define KOBOY_ROT_LOOP(TYPE, CONVERT)                                          \
    do {                                                                       \
        const uint8_t *base = (const uint8_t *)src;                            \
        for (int y = 0; y < fh; y++) {                                         \
            uint8_t *d = v->gray + (size_t)y * v->p.max_w;                     \
            switch (v->rot) {                                                  \
            case 0: {                                                          \
                const TYPE *s = (const TYPE *)(base + (size_t)y * src_pitch);  \
                for (int x = 0; x < fw; x++) d[x] = CONVERT(s[x]);             \
            } break;                                                           \
            case 1: {                                                          \
                const int sx = src_w - 1 - y;                                  \
                for (int x = 0; x < fw; x++) {                                 \
                    const TYPE *s = (const TYPE *)(base + (size_t)x * src_pitch); \
                    d[x] = CONVERT(s[sx]);                                     \
                }                                                              \
            } break;                                                           \
            case 2: {                                                          \
                const TYPE *s = (const TYPE *)(base + (size_t)(src_h - 1 - y) * src_pitch); \
                for (int x = 0; x < fw; x++) d[x] = CONVERT(s[src_w - 1 - x]); \
            } break;                                                           \
            default: {                                                         \
                for (int x = 0; x < fw; x++) {                                 \
                    const TYPE *s = (const TYPE *)(base + (size_t)(src_h - 1 - x) * src_pitch); \
                    d[x] = CONVERT(s[y]);                                      \
                }                                                              \
            } break;                                                           \
            }                                                                  \
        }                                                                      \
    } while (0)

    if (fmt == KOBOY_PIXFMT_RGB565) {
#define KOBOY_CONV565(px) v->lut[(px)]
        KOBOY_ROT_LOOP(uint16_t, KOBOY_CONV565);
#undef KOBOY_CONV565
    } else {
#define KOBOY_CONV8888(px) video_xrgb8888_to_gray((px), v->map)
        KOBOY_ROT_LOOP(uint32_t, KOBOY_CONV8888);
#undef KOBOY_CONV8888
    }
#undef KOBOY_ROT_LOOP

    /* From here down the frame IS fw x fh: every consumer below talks about
       the picture as it will be SHOWN. Rebinding the names rather than editing
       a dozen call sites keeps the rot == 0 case textually identical to what
       was here before. */
    src_w = fw;
    src_h = fh;

    /* The pixel aspect of THIS frame, from the core's display aspect and the
       size it delivered. Three integer ops ONCE PER FRAME, on the bottleneck
       stage -- which is why it is here and not per pixel. KOBOY_ASPECT_ONE for
       every square-pixel core, which takes every branch below down the path it
       took before this existed. */
    const uint32_t par = video_pixel_aspect(v->dar, src_w, src_h);

    /* Fitted and CENTRED in the reserved rect, not parked in its top-left: at
       max geometry this is p.scale at (0,0) -- the Game Boy path, unchanged --
       and below max it scales up to fill.

       The margin is cleared ONLY when the frame SIZE changes. It must be
       cleared then, or the outgoing larger frame stays visible as a border
       around the incoming one; and it must NOT be cleared otherwise, because
       this rect is diffed against prev and a full memset every frame would be
       pure cost on the bottleneck stage. */
    int dw, dh, ox, oy;
    video_fit_rect(&v->p, src_w, src_h, par, &dw, &dh, &ox, &oy);
    if (src_w != v->fit_src_w || src_h != v->fit_src_h) {
        /* Cleared to the LIGHTEST of the four levels, not to 0. A WonderSwan
           makes this margin 36% of the reserved rect for the whole session --
           its core reports max 224x224 so both orientations fit without
           re-fitting the rect, leaving a 224x80-equivalent band above and
           below a landscape title forever. At 0 that band is solid black:
           unreadable-adjacent on reflective paper and the worst case for this
           panel's waveforms.

           KOBOY_DU4_LEVELS[3] specifically, not 0xFF as a number: it is what
           the quantiser emits for white, so the margin is already quantised
           and stays byte-stable frame after frame, which keeps it OUT of the
           dirty diff instead of flickering into it. */
        memset(v->cur, KOBOY_DU4_LEVELS[3], (size_t)v->stride * (size_t)v->p.game_h);
        v->fit_src_w = src_w; v->fit_src_h = src_h;
    }
    {
        koboy_rect fr = { ox, oy, dw, dh };
        v->fit_rect = fr;                /* see video_frame_rect */
    }
    uint8_t *dst = v->cur + (size_t)oy * v->stride + ox;
    /* WHICH SCALER, decided from the ANSWER rather than from the layout.

       The block-copy scaler can express only ONE integer factor on both axes.
       When the fit produced exactly that (dh a whole multiple of src_h, dw the
       SAME multiple of src_w) it is used, and the result is byte for byte what
       this produced before pixel aspects existed -- the Game Boy (160x144 ->
       800x720 at 5) and every other square-pixel core, which is where the cost
       is: the square path must not pay for the other one.

       Anything else goes to the fractional scaler, which already collapses
       repeated rows into memcpy. For an Atari 2600 frame the vertical is still
       an exact integer (video_fit_par keeps it so), so only one row in `scale`
       does per-pixel work.

       THE GAME & WATCH FIT IS EXCLUDED EXPLICITLY, and it is not redundant:
       its fractional fit CAN land on an exact integer multiple by coincidence,
       and the two scalers do not agree to the pixel when it does.
       video_scale_gray_frac's step is floored, so at a non-power-of-two scale
       it samples up to one source pixel low at each boundary. That is a
       property the Game & Watch layout has always had; the Game Boy has never
       had it and MUST NOT acquire it.

       KEYED ON `rect_from_max`, NOT ON THE LAYOUT: SNES and Mega Drive are in
       the LCD layout too now, but their rect comes from base and their
       exact-multiple fits are the ordinary kind the block path is BETTER at.
       As shipped neither reaches it (both cores report a non-square aspect);
       with `pixel_aspect = false` they do, and get the Game Boy's scaler. */
    const int fs = dh / src_h;
    const bool block_ok = !v->p.rect_from_max &&
                          fs >= 1 && dh == src_h * fs && dw == src_w * fs;
    if (block_ok) {
        video_scale_gray(dst, v->stride, v->gray,
                         src_w, src_h, v->p.max_w, fs);
    } else {
        video_scale_gray_frac(dst, v->stride, v->gray,
                              src_w, src_h, v->p.max_w, dw, dh);
    }

    /* Quantise/dither runs over the FITTED rect only, not the whole reserved
       rect. For the Game Boy those are the same region (base == max), so this
       is unchanged bit for bit -- tests/test_video_pipeline.c's Game Boy cases
       prove it.

       For a smaller-than-max frame it is cheaper AND more correct. Cheaper: a
       landscape WonderSwan's fitted rect is 64% of its 896x896 reserved one, so
       a third of the per-frame pass was spent on pixels no frame reaches. More
       correct: those pixels are the margin the size-change clear just set to
       the lightest level, and re-quantising them only risks moving them.

       The dither path keeps its Bayer phase by receiving PANEL coordinates
       (game_x + ox, game_y + oy) rather than the rect's origin -- the matrix is
       indexed by absolute screen position, so shifting the region must shift
       the phase or the pattern walks. */
    uint8_t *fit = v->cur + (size_t)oy * v->stride + ox;
    if (v->dither)
        video_dither_1bit(fit, dw, dh, v->stride,
                          v->p.game_x + ox, v->p.game_y + oy);
    else
        video_quantise4(fit, dw, dh, v->stride);
    return true;
}

int video_submit_rects(koboy_video *v, const void *src, int src_w, int src_h,
                       size_t src_pitch, koboy_pixfmt fmt, int fixed_tiles,
                       koboy_rect *out, int max_out)
{
    /* LIVE GUARD: out == NULL checked here too, not just in
       video_split_dirty, so a NULL skips the pipeline entirely rather than
       paying for a convert/scale/quantise pass nothing could report. max_out
       is not re-validated: video_split_dirty's own guard handles it. */
    if (!out) return 0;
    if (!video_pipeline_run(v, src, src_w, src_h, src_pitch, fmt)) return 0;

    /* Diffed over the FULL reserved rect, not the current frame's own extent.
       That is what makes a Game Boy frame (src always == max) diff exactly as
       before, and it means a core submitting a smaller frame after a bigger one
       leaves the uncovered remainder exactly as it was in both cur and on the
       panel -- ghosting-adjacent but not incorrect: nothing asked it to
       change. */
    int n = video_split_dirty(v->prev, v->cur, v->p.game_w, v->p.game_h, v->stride,
                              fixed_tiles, out, max_out);
    /* cur is what the caller blits; prev becomes the next diff's baseline.
       When nothing changed the buffers already match. */
    if (n > 0) memcpy(v->prev, v->cur, (size_t)v->stride * (size_t)v->p.game_h);
    return n;
}

koboy_rect video_submit(koboy_video *v, const void *src, int src_w, int src_h,
                        size_t src_pitch, koboy_pixfmt fmt)
{
    koboy_rect one = { 0, 0, 0, 0 };
    koboy_rect r[1];
    /* fixed_tiles is 0, not a real config value: at max_out 1
       video_split_dirty takes its merged-box path before fixed_tiles is read. */
    int n = video_submit_rects(v, src, src_w, src_h, src_pitch, fmt, 0, r, 1);
    if (n > 0) one = r[0];
    return one;
}
