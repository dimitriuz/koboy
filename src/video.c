#include "video.h"
#include <string.h>
#include <stdlib.h>

/* The shadow-lift curve: out = v * (255 + K) / (v + K).

   A gamma of about 0.85 is what the measurements wanted, and this is that
   curve without a pow() or a float anywhere -- the pixel path is integer by
   constraint, and a table would need generating by something this project
   cannot reproduce in a test. Monotone, exactly 0 at 0 and exactly 255 at
   255 (so white stays paper-white and black stays black), and every value in
   between is one multiply and one divide a reviewer can redo by hand.

   WHY it is here at all, since a monotone remap looks like a no-op before a
   quantiser: it is not one, because the quantiser's thresholds are fixed at
   43/128/213. Lifting the shadows is exactly equivalent to lowering those
   thresholds, and lowering the first one is what stops a dark-but-coloured
   background collapsing to solid black. MEASURED over 38 gameplay frames from
   19 titles across NES/WonderSwan Color/Neo Geo Pocket Color: pixels that
   carry visible colour (max channel >= 24) yet quantise to level 0 fall from
   6.7% of all pixels to 2.5%. Kirby's Adventure's brick wall rgb(99,20,0)
   goes 41 -> 48, i.e. off level 0 and onto level 1, which is the difference
   between a solid black slab and a wall with a pattern in it.

   K = 1000 is not a tuned-to-death number: 700 and 1400 were measured too and
   move the same statistic by fractions of a percent. It is the value whose
   curve sits closest to gamma 0.85 (83 -> 96 against 98, 169 -> 181 against
   180) while keeping the Game Boy's two mid greys comfortably inside their
   original levels. */
#define KOBOY_GRAY_LIFT 1000u

static inline unsigned gray_lift(unsigned v)
{
    return (v * (255u + KOBOY_GRAY_LIFT)) / (v + KOBOY_GRAY_LIFT);
}

/* One row per koboy_gray_map, in the same order. The weights of every
   non-VALUE row sum to EXACTLY 256, which is what makes (w.r*255 + w.g*255 +
   w.b*255) >> 8 come out at exactly 255 -- white must stay white, or the
   quantiser's top level stops being reachable from white.

   KOBOY_GRAY_VALUE has no weights: it is max(R,G,B), which no weighted sum
   can express. Its row is present so the table stays indexable by the enum,
   and gray_of branches on the enum rather than on a sentinel weight. */
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

/* 8-bit channels in, one grey out. Callers expand 5/6-bit RGB565 channels by
   bit replication first, so 0x1F -> 0xFF exactly and the two entry points
   below cannot disagree about what a saturated channel means. */
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

/* Nearest-neighbour integer upscale. Each source pixel becomes a solid run of
   `scale` bytes; each source row is emitted once then memcpy-replicated
   scale-1 times. Per-pixel work collapses into block copies.

   Preconditions, satisfied by construction at the only call site and stated
   because nothing enforces them: scale >= 1, and dst_stride >= src_w * scale.
   A scale of 0 emits nothing; a short dst_stride overlaps rows. */
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
    /* LIVE GUARD, three separate things in one test. A non-positive extent
       has nothing to draw and would make the step divisions below a division
       by zero. And a source over 65535 px on an axis cannot be expressed as a
       16.16 step at all -- (src << 16) would overflow uint32_t and the scaler
       would silently sample the wrong pixels rather than fail. No core
       geometry is remotely near that (the largest measured here is 973), so
       this is the guard for a caller that has already gone wrong, not a case
       this project produces. */
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

    /* NO CLAMP ON sx/sy, and that is a proof rather than an oversight -- the
       kind of guard this project's convention says to remove outright rather
       than leave looking defensive (see chrome.c's corner-radius note). The
       step is FLOORED: xstep <= (src_w << 16) / dst_w, so after the last of
       dst_w increments the accumulator has reached at most
       (dst_w - 1) * xstep < src_w << 16, i.e. sx <= src_w - 1, always.
       Truncation can only make the sampled position too LOW, never too high.
       The same argument holds on y. Reintroduce a clamp if the step ever
       stops being floored (a rounded step CAN overshoot) -- but not before,
       because a clamp that cannot fire is a clamp no test can prove. */
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

/* WHY THE THRESHOLDS ARE NOT THE MATRIX, and why this is one line of
   arithmetic rather than an oversight either way.

   video_bayer_build produces a permutation of 0..255 -- that is what a 16x16
   Bayer matrix IS, and tests/test_video_quant.c pins it. Thresholding `v > m`
   against it makes the white count for a value v exactly v out of 256, which
   is right for every value except the one the panel renders best: v = 255
   lands on `255 > 255`, which is false, so ONE cell in every 16x16 tile of
   pure white comes out black. On an 800x720 game rect that is 2250 isolated
   black dots at 16px spacing over what should be a clean page -- and pure
   white is not a corner case here, it is the Game Boy's own lightest shade
   (gambatte emits exactly rgb(255,255,255)) and most HUD text on every other
   system.

   Scaling the matrix to 0..254 fixes it at the top without moving anything
   else that matters: 255 > 254 is true in every cell, 0 is still black in
   every cell (nothing is below zero), and the interior counts shift by at
   most one cell in 256. The one visible consequence is that m = 0 and m = 1
   both map to threshold 0, so v = 1 lights two cells instead of one -- a
   near-black getting 0.8% lighter, against a pure white that stops being
   speckled.

   Kept as a SECOND table rather than folded into video_bayer_build because
   the matrix is a mathematical object other code and tests ask for by name;
   this is a rendering decision about how to use it, and doing the scaling
   per pixel in the loop below would put a multiply and a divide in the
   measured bottleneck (CLAUDE.md: video_submit is 17 ms) to save 256 bytes.

   Lazy init, and single-threaded by construction: nothing in src/ creates a
   thread, and §6 of the design keeps the emulator single-threaded because
   non-blocking refresh submission removed the reason to add one. If a worker
   thread ever appears, this needs a once-guard. */
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

/* Bound on the tile grid video_split_dirty will attempt to split, in tiles per
   axis. Sized against the largest game rect this project has ever fitted (the
   spec's scale-5 800x720 is 100x90 tiles) with headroom for a hypothetical
   larger panel or scale; a request bigger than this falls back to the merged
   box in video_split_dirty rather than grow the scratch buffer, which keeps
   the buffer a fixed, auditable size instead of scaling with whatever a
   caller happens to pass. Coverage still holds on that fallback -- it is a
   missed optimisation, never a missed rect. */
#define KOBOY_SPLIT_MAX_TILES 300

/* Upper bound on how many band x column-run candidates video_split_dirty will
   generate before capping to max_out. 64 comfortably covers every shape this
   project's UI produces (a handful of sprites plus a status bar); a
   pathological checkerboard of single-tile changes could in principle demand
   more, and video_split_dirty bails to the merged box rather than overflow
   this array -- same reasoning as KOBOY_SPLIT_MAX_TILES above. */
#define KOBOY_SPLIT_MAX_CANDIDATES 64

/* Scratch tile-dirty bitmap for video_split_dirty. File-scope static, not a
   local, for the same reason g_bayer is: it would otherwise be
   KOBOY_SPLIT_MAX_TILES^2 bytes (90000) of stack pushed and popped every
   presented frame. Reused across calls, which is safe only because nothing in
   src/ creates a thread (see the note above bayer_ensure) -- a worker thread
   sharing this buffer would need a lock or its own copy. */
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

/* Is `b` entirely inside `a` (including equal)? Used to drop redundant
   candidates after the merge-cap below: unioning candidates in band-major
   order can, once a merged box has absorbed a third candidate, end up
   strictly containing some other candidate that was never unioned with it.
   Dropping the contained one is free (its area was already going to be
   blitted and refreshed as part of `a`) and turns a guaranteed-wasted
   double-refresh into nothing. */
static bool rect_contains(koboy_rect a, koboy_rect b)
{
    return b.x >= a.x && b.y >= a.y &&
           b.x + b.w <= a.x + a.w && b.y + b.h <= a.y + a.h;
}

/* Does the tile-row `r` (in the reduced tby0-relative grid, width sub_tw)
   contain any dirty tile? Column-segmentation and row-segmentation both need
   this, so it is one function rather than two copies that could drift. */
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
    /* LIVE GUARD: out == NULL has no slot to write a rect count into, so
       there is nothing this function can honestly report. Every caller in
       this codebase passes a real array, but video_split_dirty is also
       exercised directly by tests/test_video_multirect.c, and a NULL here
       must not be a silent no-op or a crash. */
    if (!out) return 0;

    koboy_rect merged = video_dirty_rect(prev, cur, w, h, stride);
    if (merged.w == 0) return 0;                  /* nothing changed */

    /* LIVE GUARD: max_out < 1 (a caller error, not a real request) and
       max_out == 1 (a real request for exactly the old single-rect
       behaviour) both degrade to the merged box; folding them into one
       branch means video_submit's wrapper -- which always asks for 1 --
       never touches the segmentation machinery below at all. */
    if (max_out <= 1) { out[0] = merged; return 1; }

    /* Tile-index bounding box of the merged rect. video_dirty_rect's own walk
       is tile-aligned (x, y, and the non-edge extent of w, h are all
       KOBOY_TILE multiples), so this recovers exactly the tile range that
       walk found dirty -- no tile outside it can be dirty by construction. */
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

    /* Row-band segmentation: maximal runs of tile-rows with at least one
       dirty tile, separated by at least one tile-row with none. Within each
       band, column segmentation by the same rule. Each band's first and last
       row, and each candidate rect's own bounding pixel box, come straight
       from the tile indices -- no further tightening -- which is always a
       safe (if sometimes not maximally tight) superset of the true dirty
       pixels in that piece. */
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

    /* A pathological shape (checkerboard-dense changes) ran the candidate
       array out of room. Coverage must not depend on how big that array is,
       so give up the split rather than silently drop whatever did not fit. */
    if (overflowed) { out[0] = merged; return 1; }

    /* Cap to max_out by merging candidates together -- never by dropping one.
       A union of rects only grows; it never stops covering what its inputs
       covered, so this preserves the coverage property regardless of which
       pairs get merged. Order doesn't matter for correctness, only for how
       tight the result is, so the simplest rule (merge the last two,
       repeatedly) is enough.

       This does NOT keep the emitted rects disjoint. Candidates are
       band-major ordered, and merging "the last two" can cross a band
       boundary: a box that has already absorbed one neighbour by union can,
       on the next merge, swallow a third candidate elsewhere in the list
       outright (measured on production geometry: one merged 664x536 rect
       fully contained an unrelated 80x72 one). Coverage still holds -- a
       union only grows -- but an emitted rect nested inside another is pure
       double-work downstream (blit_gray8 and refresh both run its area
       twice), which is exactly the cost this function exists to cut. The
       loop below removes that specific, cheaply-detectable case; it does not
       attempt general deoverlap. */
    while (ncand > max_out) {
        cand[ncand - 2] = rect_union(cand[ncand - 2], cand[ncand - 1]);
        ncand--;
    }

    /* Drop any candidate now fully contained in another (see above). O(n^2)
       over at most KOBOY_MAX_RECTS entries, so the cost is noise. */
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

    /* The decision: split only when it is actually cheaper. With only one
       candidate left (the common case once a shape has no internal gaps --
       see the full-screen-scroller case in tests/test_video_multirect.c) the
       candidate already equals the merged box, so this is never strictly
       less and the two branches produce the identical rect either way. */
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
    /* The frame size the last fit was computed for. A core whose base
       geometry changes mid-session (Game & Watch alternates between the whole
       unit and the LCD alone, several times a second) re-fits only when the
       size actually moves -- and the margin around the content has to be
       cleared on that transition, or the previous, larger frame stays visible
       around the edges of the new one. */
    int      fit_src_w, fit_src_h;
    /* Where the last frame landed inside the reserved rect -- see
       video_frame_rect. Recorded rather than recomputed by the caller so
       there is one answer, produced by the code that actually placed the
       pixels. */
    koboy_rect fit_rect;
    uint8_t  lut[65536];
    /* Which mapping `lut` was built with. Stored, not just applied, because
       the XRGB8888 path below does not go through the LUT at all -- it calls
       video_xrgb8888_to_gray per pixel and needs the same map, or a core that
       chose the other pixel format would render differently from one that
       chose RGB565. */
    koboy_gray_map map;
    /* Quarter turns counter-clockwise to apply to each incoming frame, 0..3.
       See video_set_rotation. 0 for every core that has ever run through this
       pipeline before FinalBurn Neo, and the rot == 0 path below is byte for
       byte the loop that existed before this field did. */
    int      rot;
    /* The core's DISPLAY aspect in 16.16, or 0 for "never told" -- see
       video_set_aspect. 0 for every core that ran through this pipeline
       before the Atari 2600 arrived, and 0 is what makes the square path
       below byte for byte the one that existed then. */
    uint32_t dar;
};

/* Contract in video.h. Snaps to exactly square inside the deadband, and the
   reason that matters -- a core that reports a ROUNDED aspect -- is there too. */
uint32_t video_pixel_aspect(uint32_t display_aspect, int frame_w, int frame_h)
{
    /* LIVE GUARD. display_aspect 0 is "the core never said", which is the
       state every koboy_video starts in; a non-positive extent has no ratio
       and would divide by zero just below. Both answer square, which is what
       this whole file did unconditionally before non-square pixels existed. */
    if (display_aspect == 0 || frame_w < 1 || frame_h < 1) return KOBOY_ASPECT_ONE;

    uint32_t par = (uint32_t)(((uint64_t)display_aspect * (uint64_t)frame_h)
                              / (uint64_t)frame_w);
    uint32_t d = par > KOBOY_ASPECT_ONE ? par - KOBOY_ASPECT_ONE
                                        : KOBOY_ASPECT_ONE - par;
    if (d <= KOBOY_PAR_DEADBAND) return KOBOY_ASPECT_ONE;
    return par;
}

/* Contract in video.h -- including why only the horizontal axis carries the
   pixel aspect.

   The reserved rect is sized from the core's MAX geometry (koboy.h), so a
   square-pixel frame at exactly max lands at p.scale with no offset -- which
   is every Game Boy frame ever, 160x144 into 800x720 at scale 5, offsets
   zero, bit for bit what this did before it could fit anything else. A frame
   SMALLER than max is the case that was wrong before: it used to be scaled by
   p.scale and parked in the top-left corner, so a 305x191 Game & Watch
   in-game view drew at 1:1 in the corner of a 654x396 rect with the rest left
   black. Fitting it instead puts it at 2x, centred, filling the rect.

   Never returns a scale below 1: src can't exceed the rect (game_w is the
   par-corrected max times the scale -- config_resolve_profile_par -- and
   video_pipeline_run rejects src > max before calling this), so the divisions
   cannot floor to 0. The clamp is a live guard against a caller that skips
   that check, not dead code. */
void video_fit_par(const koboy_profile *p, int src_w, int src_h, uint32_t par,
                   int *scale_out, int *dw_out, int *ox_out, int *oy_out)
{
    if (par == 0) par = KOBOY_ASPECT_ONE;

    /* A degenerate source has no fit; 1x1 keeps every consumer's arithmetic
       (the offsets below, the scaler's dst_w) inside the rect rather than
       handing back a zero or negative width. */
    int fs = 1, dw = 1;
    if (src_w > 0 && src_h > 0) {
        int fy = p->game_h / src_h;
        int fx;
        if (par == KOBOY_ASPECT_ONE) {
            /* THE SQUARE PATH, and it is written out separately rather than
               folded into the fixed-point one on purpose: this is the only
               path the Game Boy, and eight of the eleven systems, ever take,
               and it must produce the same integers the pre-anisotropy
               version produced -- not the same integers up to a rounding. */
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
           rejected (a frame too big for the reserved rect at any scale), and
           a dw wider than the rect is the case video_pipeline_run's bounds
           guard exists to stop from corrupting memory. It cannot fire for any
           frame that guard accepts -- floor(game_w / src_w) * src_w <= game_w
           on the square path, and the fixed-point ceiling above is floored for
           the same reason -- so this is the defence for a caller that skipped
           the check, not a correction of the arithmetic. */
    }
    if (dw > p->game_w) dw = p->game_w;
    if (dw < 1) dw = 1;
    int ox = (p->game_w - dw) / 2;
    int oy = (p->game_h - src_h * fs) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;
    *scale_out = fs; *dw_out = dw; *ox_out = ox; *oy_out = oy;
}

/* Contract in video.h: video_fit_par's square case, under the name the rest of
   this project already used for it. */
void video_fit(const koboy_profile *p, int src_w, int src_h,
               int *scale_out, int *ox_out, int *oy_out)
{
    int dw;
    video_fit_par(p, src_w, src_h, KOBOY_ASPECT_ONE, scale_out, &dw, ox_out, oy_out);
}

/* Contract in video.h. Deciding which axis binds by cross-multiplying, rather
   than by comparing two truncated ratios, is what makes the binding axis come
   out EXACT: the alternative (a shared 16.16 ratio) loses up to one part in
   65536 of it twice over -- once resolving the reserved rect, again fitting a
   frame into that rect -- and the two roundings compound into a couple of
   stray pixels of margin down one side. */
void video_fit_frac(int src_w, int src_h, int avail_w, int avail_h,
                    int *dw_out, int *dh_out)
{
    /* LIVE GUARD: a non-positive extent has no fit, and would divide by zero
       below. Callers check their own inputs; this is the local defence in the
       function that does the arithmetic. */
    if (src_w < 1 || src_h < 1 || avail_w < 1 || avail_h < 1) return;

    int dw, dh;
    if ((long)src_w * avail_h <= (long)src_h * avail_w) {
        dh = avail_h;                                   /* height binds */
        dw = (int)(((long)src_w * avail_h) / src_h);
    } else {
        dw = avail_w;                                   /* width binds */
        dh = (int)(((long)src_h * avail_w) / src_w);
    }
    /* A source far wider than tall (or the reverse) can floor the non-binding
       axis to nothing; one row of artwork beats none. */
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    *dw_out = dw; *dh_out = dh;
}

void video_fit_rect(const koboy_profile *p, int src_w, int src_h, uint32_t par,
                    int *dw_out, int *dh_out, int *ox_out, int *oy_out)
{
    /* 1x1, not undefined: video_fit_frac writes nothing for a degenerate
       input (its own live guard), and BOTH branches can now reach it, so the
       fallback value belongs at the declaration rather than being repeated in
       front of each call. One row of picture beats a garbage rect. */
    int dw = 1, dh = 1;
    if (par == 0) par = KOBOY_ASPECT_ONE;

    if (p->layout_mode == KOBOY_LAYOUT_LCD) {
        /* The invariant this shortcut states, and the DMG branch below gets
           for free: A SQUARE-PIXEL FRAME AT EXACTLY MAX GEOMETRY FILLS THE
           RESERVED RECT. game_w/game_h were themselves produced by
           video_fit_frac from max_w/max_h (config_resolve_profile), so
           re-fitting max into that result is asking the same question twice --
           and the second answer can be a pixel short, because the first one's
           non-binding axis was floored and re-deriving the ratio from a
           floored number can only lose. Saying so outright is both faster and
           exact, and it is the same property video_fit_par already has at max
           for the DMG layout.

           SQUARE-PIXEL, and the qualifier is new and load-bearing: the
           reserved rect was resolved from max_w x max_h with no pixel aspect
           in it (config_resolve_profile), so once par != 1 the shortcut's
           premise is simply false and the general fit below has to run. Today
           only .mgw reaches this layout and the Game & Watch core reports a
           square aspect, so the shortcut is still what actually executes --
           but a layout branch that silently ignores the aspect is exactly the
           kind of thing that gets found by rendering a frame and looking at
           it, two systems later.

           AND `rect_from_max`, which is that "two systems later" arriving:
           SNES and Mega Drive share this layout now and their rect comes from
           BASE, then possibly gets cut again by the per-system scale ceiling.
           For them the premise above is false whatever the aspect is -- the
           rect is not max's shape and is not max's size -- so a frame that
           happened to arrive at exactly max geometry would be STRETCHED to
           fill a rect it does not match. Guarded on the flag rather than on
           the layout, because "LCD" no longer means "Game & Watch". */
        if (p->rect_from_max &&
            par == KOBOY_ASPECT_ONE && src_w == p->max_w && src_h == p->max_h) {
            dw = p->game_w; dh = p->game_h;
        } else {
            /* The frame's DISPLAYED shape, which is what an aspect-preserving
               fit has to preserve. At par == 1 this is src_w and the call is
               identical to the one that was here. */
            int ew = par == KOBOY_ASPECT_ONE
                   ? src_w
                   : (int)((((uint64_t)src_w * par) + 32768u) >> 16);
            if (ew < 1) ew = 1;
            video_fit_frac(ew, src_h, p->game_w, p->game_h, &dw, &dh);
        }
    } else {
        /* The frame's DISPLAYED width, computed once and used twice: to ask
           whether an integer fit is possible at all, and (if it is not) as
           the source shape the fractional fit must preserve. Same rounding as
           video_fit_par's, on purpose -- the two must agree about whether a
           frame is one pixel too wide. */
        int ew = par == KOBOY_ASPECT_ONE
               ? src_w
               : (int)((((uint64_t)src_w * par) + 32768u) >> 16);
        if (ew < 1) ew = 1;

        if (ew > p->game_w || src_h > p->game_h) {
            /* THE FRAME DOES NOT FIT EVEN AT 1:1, so there is no integer fit
               to find: video_fit_par's scale floor is 1 (it must be, or a
               frame smaller than the rect would round to nothing), which
               means it cannot SHRINK -- it would hand back dh = src_h and the
               scaler would write past the bottom of v->cur.

               This became reachable when the DMG rect started being sized
               from the core's BASE geometry (config_resolve_profile_par): the
               rect no longer holds every frame in [1, max] by construction,
               and snes9x2005's 512x512 max against its 256x224 base is
               exactly the gap. Under the old max-sized rect it could not fire
               at all -- game_w >= ceil(max_w * par) >= ew and
               game_h >= max_h >= src_h for every frame the bounds guard in
               video_pipeline_run accepts -- which is why nothing here needed
               it before and why every existing fit is bit-for-bit unchanged.

               Fractional rather than refusing the frame: a hi-res mode
               presented slightly small is a picture, and a dropped frame is a
               frozen screen. */
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
    /* Sized to the core's max geometry, not a constant -- a core is free to
       submit any frame up to max_w x max_h (video_pipeline_run's bounds
       check below is what actually enforces the ceiling), and this buffer
       has to hold the largest one without a realloc mid-session. For the
       Game Boy, base and max are both always 160x144, so this allocates
       exactly what the old KOBOY_GB_W*KOBOY_GB_H constant did. */
    v->gray = calloc(1, (size_t)p->max_w * (size_t)p->max_h);
    if (!v->cur || !v->prev || !v->gray) { video_destroy(v); return NULL; }
    video_gray_lut_build(v->lut, v->map);
    /* prev starts as an impossible value so the first frame is fully dirty */
    memset(v->prev, 0x01, n);
    return v;
}

/* Contract in video.h. Stored raw -- there is nothing to validate here that
   core_display_aspect has not already validated, and 0 is a meaningful value
   (see video_pixel_aspect's live guard) rather than an error. */
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
    /* Masked, not validated: 0..3 is the whole domain (see core.c, which
       masks the core's answer the same way), and a caller that computed 4
       means 0. */
    v->rot = rot & 3;
    /* NOT invalidating prev here, deliberately. This is called immediately
       after video_create, whose prev is already seeded to an impossible
       value so the first frame is fully dirty -- and it is called nowhere
       else, because a rotation change arrives bundled with a geometry change
       (core.c sets geom_dirty for it) and main.c answers that by destroying
       and rebuilding the whole koboy_video. If a caller ever does flip this
       on a live pipeline, it owes video_invalidate afterwards: every pixel
       moves, and prev would otherwise claim most of them did not. */
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
   (and touches nothing) for frames that carry no pixels this pipeline can
   honestly handle: a duplicate signalled by a NULL src, and a size the core
   had no business sending. Split out of video_submit_rects so the
   dirty-diff and prev-copy below it are the only thing that differs between
   the single-rect and multi-rect entry points. */
static bool video_pipeline_run(koboy_video *v, const void *src, int src_w, int src_h,
                               size_t src_pitch, koboy_pixfmt fmt)
{
    if (!src) return false;                        /* core signalled a duplicate */
    /* LIVE GUARD: v->gray and v->cur are allocated at v->p.max_w x max_h (see
       video_create) and v->p.max_w*v->p.max_h*scale respectively -- a frame
       outside [1, max] is either nothing to draw (<= 0, which a well-behaved
       core never sends outside the duplicate-frame NULL convention already
       handled above) or bigger than what those buffers were sized for, and
       writing it would walk off the end of v->gray a row or a column past
       its real width. Rejecting it here, before a single byte is touched, is
       what makes "video_submit accepts any size up to the core's maximum"
       true instead of "any size the core claims, trusted on faith." A core
       reporting geometry it then contradicts on the very next frame is a
       broken core, not a koboy bug, and dropping the frame is the correct
       response -- the alternative is scribbling into whatever memory follows
       the buffer. */
    /* The bound is checked against the PRESENTED size, not the buffer the core
       handed over, because the presented size is what every buffer here was
       allocated for: config_resolve_profile was given core_get_geometry's
       already-transposed max (see core.h), so for a quarter-turned board
       p.max_w/max_h are 224x288 while the core's own frame is 288x224.
       Comparing the un-rotated w/h against them would reject every frame of
       Galaga -- a black game rect, not a crash, which is the worse of the two
       failures because it looks like a broken core. */
    const int fw = (v->rot & 1) ? src_h : src_w;   /* frame width  as presented */
    const int fh = (v->rot & 1) ? src_w : src_h;   /* frame height as presented */
    if (fw < 1 || fh < 1 || fw > v->p.max_w || fh > v->p.max_h) return false;

    /* rot == 0 is the pre-existing loop, unchanged and still the only path any
       core but FinalBurn Neo takes -- kept as its own branch rather than
       folded into a general addressing scheme, because this is the stage
       CLAUDE.md names as the pipeline's measured bottleneck (17 ms of a 23 ms
       frame) and a per-pixel indirection bought for a case nobody takes is
       exactly the wrong trade here.

       The three turning branches all write `d` SEQUENTIALLY and read the
       source strided, rather than the other way round. That is the cheaper
       half to make cache-hostile: the write side is a linear run the store
       buffer absorbs, while a strided WRITE would dirty a fresh cache line
       per pixel. The strided reads stay cheap in practice because a source
       column is one byte-pair every src_pitch and the whole source of a
       pre-1990 board is ~130 KB -- a Cortex-A9's L1 holds a column's worth of
       lines and the next column reuses every one of them.

       Rotation is COUNTER-CLOCKWISE quarter turns, matching libretro's
       SET_ROTATION (see libretro_min.h). Derivations, with (W,H) the source's
       own width and height:
         rot 1  out is (H,W):  out[y][x] = src[x][W-1-y]
         rot 2  out is (W,H):  out[y][x] = src[H-1-y][W-1-x]
         rot 3  out is (H,W):  out[y][x] = src[H-1-x][y]
       Verified against a rendered Galaga frame, not derived and trusted: at
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

    /* From here down the frame IS fw x fh: every consumer below -- the fit,
       the margin-clear trigger, the quantiser's extent -- is talking about the
       picture as it will be shown, and the core's own orientation has done its
       last job. Rebinding the two names rather than editing a dozen call sites
       is deliberate: it makes the rot == 0 case textually identical to what
       was here before. */
    src_w = fw;
    src_h = fh;

    /* Fitted and centred in the reserved rect, not parked in its top-left.
       See video_fit: at max geometry this is p.scale at offset (0,0) -- the
       Game Boy path, unchanged -- and below max it scales up to fill.

       The margin is cleared only when the frame SIZE changes, not every
       frame. It has to be cleared then, or the outgoing (larger) frame stays
       visible as a border around the incoming one; and it must not be cleared
       otherwise, because this rect is diffed against prev to find the dirty
       region and a memset of the full rect every frame would be pure cost on
       the stage that is already this pipeline's bottleneck. */
    /* The pixel aspect of THIS frame, derived from the core's display aspect
       and the size it actually delivered. Three integer ops, once per frame,
       on the stage CLAUDE.md names as the bottleneck -- which is why it is
       here and not per pixel. KOBOY_ASPECT_ONE for every square-pixel core,
       and that value takes every branch below down the path it took before
       this existed. */
    const uint32_t par = video_pixel_aspect(v->dar, src_w, src_h);

    int dw, dh, ox, oy;
    video_fit_rect(&v->p, src_w, src_h, par, &dw, &dh, &ox, &oy);
    if (src_w != v->fit_src_w || src_h != v->fit_src_h) {
        /* Cleared to the LIGHTEST of the four levels, not to 0. Until a core
           arrived whose frame is permanently smaller than its max in one
           axis, this margin was a transient and nobody looked at it; a
           WonderSwan makes it 36% of the reserved rect for the whole session.
           The core reports max 224x224 so that BOTH its orientations fit
           without re-fitting the rect (see main.c's base-only branch), which
           leaves a 224x80-equivalent band above and below a landscape title
           forever. At 0 that band is solid black -- unreadable-adjacent on
           reflective paper and the worst case for this panel's waveforms, the
           same reasoning that already picked the Pokemon Mini's inverted
           palette in core.c.

           KOBOY_DU4_LEVELS[3] specifically, not 0xFF-as-a-number: it is the
           value the quantiser below emits for white, so the margin is already
           quantised and stays byte-stable frame after frame -- which is what
           keeps it out of the dirty diff instead of flickering into it. */
        memset(v->cur, KOBOY_DU4_LEVELS[3], (size_t)v->stride * (size_t)v->p.game_h);
        v->fit_src_w = src_w; v->fit_src_h = src_h;
    }
    {
        koboy_rect fr = { ox, oy, dw, dh };
        v->fit_rect = fr;                /* see video_frame_rect */
    }
    uint8_t *dst = v->cur + (size_t)oy * v->stride + ox;
    /* WHICH SCALER, decided from the answer rather than from the layout, and
       that is the whole shape of the non-square-pixel fix.

       The block-copy scaler can only express ONE integer factor on both axes.
       When the fit produced exactly that -- dh a whole multiple of src_h and
       dw the SAME multiple of src_w -- it is used, and the result is byte for
       byte what this line produced before pixel aspects existed. That covers
       the Game Boy (160x144 -> 800x720 at 5) and every other square-pixel
       core, which is where the cost is: video_submit is this pipeline's
       measured bottleneck and the square path must not pay for the other one.

       Anything else -- the LCD layout, or a DMG frame whose pixels are not
       square -- goes to the fractional scaler, which already exists for the
       Game & Watch and already collapses repeated rows into memcpy. For an
       Atari 2600 frame that means the vertical is still an exact integer
       (video_fit_par keeps it so), so only one row in `scale` does per-pixel
       work and the rest are copies.

       THE GAME & WATCH FIT IS EXCLUDED EXPLICITLY, not left to fall out of
       the arithmetic, and it is not redundant: its fractional fit CAN land on
       an exact integer multiple by coincidence, and the two scalers do not
       agree to the pixel when it does. video_scale_gray_frac's step is
       floored, so at a scale that is not a power of two it samples up to one
       source pixel low at each boundary -- source column 0 gets one extra
       destination column and one other column loses one. That is a property
       the Game & Watch layout has always had and this change has no business
       altering; the Game Boy has never had it and must not acquire it, which
       is what the block path being unconditional for an exact DMG fit
       guarantees.

       KEYED ON `rect_from_max`, NOT ON THE LAYOUT, and the distinction
       arrived with SNES and Mega Drive: they are in the LCD layout too now,
       but their rect comes from base and their exact-multiple fits are the
       ordinary kind the block path is BETTER at. Excluding them along with
       Game & Watch would hand two systems the one-pixel sampling skew for no
       reason. As shipped neither reaches it anyway (both cores report a
       non-square pixel aspect, so no fit is an exact multiple); with
       `pixel_aspect = false` they do, and then they get the same scaler the
       Game Boy gets. */
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

    /* Quantise/dither run over the FITTED rect only -- the dw x dh the scaler
       just wrote -- not over the whole reserved rect. For the Game Boy those
       are the same region (base == max, so dw x dh IS game_w x game_h at
       offset 0,0) and this is unchanged behaviour bit for bit, which
       tests/test_video_pipeline.c's Game Boy cases are what actually prove.

       For a core whose frame is smaller than its max it is both cheaper and
       more correct. Cheaper: video_submit is this pipeline's measured
       bottleneck (CLAUDE.md), and a landscape WonderSwan's fitted rect is 64%
       of its 896x896 reserved one, so a third of the per-frame quantise pass
       was being spent on pixels no frame ever reaches. More correct: those
       pixels are the margin the size-change clear above just set to the
       lightest level, and re-quantising them every frame only risks moving
       them.

       The dither path keeps its Bayer phase by being handed the margin's
       PANEL coordinates (game_x + ox, game_y + oy) rather than the rect's
       origin -- the matrix is indexed by absolute screen position on purpose,
       so shifting the region must shift the phase with it or the pattern
       walks. */
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
    /* LIVE GUARD: out == NULL, checked here too (not just inside
       video_split_dirty below) so a NULL out skips the pipeline entirely
       rather than paying for a convert/scale/quantise pass whose result
       could never be reported. max_out is not re-validated here: it passes
       straight through to video_split_dirty, whose own live guard handles
       max_out < 1. */
    if (!out) return 0;
    if (!video_pipeline_run(v, src, src_w, src_h, src_pitch, fmt)) return 0;

    /* Diffed over the FULL reserved rect (game_w x game_h), not the current
       frame's own src_w*scale x src_h*scale -- the matching tradeoff to the
       one noted in video_pipeline_run. Keeping this at game_w/game_h rather
       than tracking a shrinking frame's smaller extent is what makes a
       Game Boy frame (src always == max) diff exactly as before; it also
       means a core that submits a smaller frame after a bigger one leaves
       the now-uncovered remainder exactly as it last was, in both cur and on
       the panel, rather than forcing it blank -- ghosting-adjacent, but not
       incorrect: nothing was asked to change there, so nothing needed to. */
    int n = video_split_dirty(v->prev, v->cur, v->p.game_w, v->p.game_h, v->stride,
                              fixed_tiles, out, max_out);
    /* cur holds the frame the caller will blit; prev becomes the baseline for
       the next diff. When nothing changed the buffers already match. */
    if (n > 0) memcpy(v->prev, v->cur, (size_t)v->stride * (size_t)v->p.game_h);
    return n;
}

koboy_rect video_submit(koboy_video *v, const void *src, int src_w, int src_h,
                        size_t src_pitch, koboy_pixfmt fmt)
{
    koboy_rect one = { 0, 0, 0, 0 };
    koboy_rect r[1];
    /* fixed_tiles is 0 here, not a real config value: max_out 1 makes
       video_split_dirty take its merged-box path before fixed_tiles is ever
       read, so nothing downstream of this call sees this placeholder. */
    int n = video_submit_rects(v, src, src_w, src_h, src_pitch, fmt, 0, r, 1);
    if (n > 0) one = r[0];
    return one;
}
