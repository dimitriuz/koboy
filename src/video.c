#include "video.h"
#include <string.h>
#include <stdlib.h>

/* Rec.601 luma with integer weights; 8-bit channels expanded from 5/6 bits by
   bit replication so 0x1F -> 0xFF exactly. */
static inline uint8_t luma(unsigned r, unsigned g, unsigned b)
{
    return (uint8_t)((77u * r + 150u * g + 29u * b) >> 8);
}

uint8_t video_rgb565_to_gray(uint16_t px)
{
    unsigned r5 = (px >> 11) & 0x1Fu, g6 = (px >> 5) & 0x3Fu, b5 = px & 0x1Fu;
    unsigned r = (r5 << 3) | (r5 >> 2);
    unsigned g = (g6 << 2) | (g6 >> 4);
    unsigned b = (b5 << 3) | (b5 >> 2);
    return luma(r, g, b);
}

uint8_t video_xrgb8888_to_gray(uint32_t px)
{
    return luma((px >> 16) & 0xFFu, (px >> 8) & 0xFFu, px & 0xFFu);
}

void video_gray_lut_build(uint8_t lut[65536])
{
    for (int i = 0; i < 65536; i++) lut[i] = video_rgb565_to_gray((uint16_t)i);
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

/* Four evenly spaced levels for the DU4 waveform. These may need retuning
   against a real panel; they are deliberately in one place. */
const uint8_t KOBOY_DU4_LEVELS[4] = { 0x00, 0x55, 0xAA, 0xFF };

static uint8_t g_bayer[16][16];
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

/* Lazy init, and single-threaded by construction: nothing in src/ creates a
   thread, and §6 of the design keeps the emulator single-threaded because
   non-blocking refresh submission removed the reason to add one. If a worker
   thread ever appears, this needs a once-guard. */
static void bayer_ensure(void)
{
    if (!g_bayer_ready) { video_bayer_build(g_bayer); g_bayer_ready = 1; }
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
        const uint8_t *brow = g_bayer[(unsigned)(screen_y + y) & 15u];
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
    uint8_t  lut[65536];
};

/* Largest integer scale at which a src_w x src_h frame fits the reserved rect,
   and the offsets that centre it there.

   The reserved rect is sized from the core's MAX geometry (koboy.h), so a
   frame at exactly max lands at p.scale with no offset -- which is every Game
   Boy frame ever, 160x144 into 800x720 at scale 5, offsets zero, bit for bit
   what this did before it could fit anything else. A frame SMALLER than max
   is the case that was wrong: it used to be scaled by p.scale and parked in
   the top-left corner, so a 305x191 Game & Watch in-game view drew at 1:1 in
   the corner of a 654x396 rect with the rest left black. Fitting it instead
   puts it at 2x, centred, filling the rect it was given.

   Never returns less than 1: src can't exceed the rect (game_w = max_w*scale
   and video_pipeline_run rejects src > max before calling this), so the
   divisions cannot floor to 0 -- the clamp is a live guard against a caller
   that skips that check, not dead code. */
void video_fit(const koboy_profile *p, int src_w, int src_h,
               int *scale_out, int *ox_out, int *oy_out)
{
    int fs = 1;
    if (src_w > 0 && src_h > 0) {
        int fx = p->game_w / src_w, fy = p->game_h / src_h;
        fs = fx < fy ? fx : fy;
        if (fs < 1) fs = 1;
    }
    int ox = (p->game_w - src_w * fs) / 2;
    int oy = (p->game_h - src_h * fs) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;
    *scale_out = fs; *ox_out = ox; *oy_out = oy;
}

koboy_video *video_create(const koboy_profile *p, bool force_dither)
{
    koboy_video *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->p = *p;
    v->dither = force_dither;
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
    video_gray_lut_build(v->lut);
    /* prev starts as an impossible value so the first frame is fully dirty */
    memset(v->prev, 0x01, n);
    return v;
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
    if (src_w < 1 || src_h < 1 || src_w > v->p.max_w || src_h > v->p.max_h) return false;

    for (int y = 0; y < src_h; y++) {
        uint8_t *d = v->gray + (size_t)y * v->p.max_w;
        if (fmt == KOBOY_PIXFMT_RGB565) {
            const uint16_t *s = (const uint16_t *)((const uint8_t *)src + (size_t)y * src_pitch);
            for (int x = 0; x < src_w; x++) d[x] = v->lut[s[x]];
        } else {
            const uint32_t *s = (const uint32_t *)((const uint8_t *)src + (size_t)y * src_pitch);
            for (int x = 0; x < src_w; x++) d[x] = video_xrgb8888_to_gray(s[x]);
        }
    }

    /* Fitted and centred in the reserved rect, not parked in its top-left.
       See video_fit: at max geometry this is p.scale at offset (0,0) -- the
       Game Boy path, unchanged -- and below max it scales up to fill.

       The margin is cleared only when the frame SIZE changes, not every
       frame. It has to be cleared then, or the outgoing (larger) frame stays
       visible as a border around the incoming one; and it must not be cleared
       otherwise, because this rect is diffed against prev to find the dirty
       region and a memset of the full rect every frame would be pure cost on
       the stage that is already this pipeline's bottleneck. */
    int fs, ox, oy;
    video_fit(&v->p, src_w, src_h, &fs, &ox, &oy);
    if (src_w != v->fit_src_w || src_h != v->fit_src_h) {
        memset(v->cur, 0, (size_t)v->stride * (size_t)v->p.game_h);
        v->fit_src_w = src_w; v->fit_src_h = src_h;
    }
    video_scale_gray(v->cur + (size_t)oy * v->stride + ox, v->stride, v->gray,
                     src_w, src_h, v->p.max_w, fs);

    /* Quantise/dither still run over the FULL reserved rect (v->p.game_w x
       game_h = max_w*scale x max_h*scale), exactly as before generalisation.
       For the Game Boy that is exactly the area video_scale_gray just wrote,
       so this is unchanged behaviour bit for bit. For a core whose current
       frame is smaller than its max, the area outside the just-scaled
       corner is whatever was quantised into it last -- v->cur/prev start
       life zero-filled (video_create's calloc), which quantises to the
       darkest of the four levels, and nothing here paints over that until a
       frame actually reaches that pixel. That is a known rough edge for a
       shrinking-frame core, not something this task's target (fixed-size
       Game & Watch artwork per title) exercises, and is called out rather
       than silently accepted -- see the video_submit_rects comment on the
       matching dirty-rect tradeoff. */
    if (v->dither)
        video_dither_1bit(v->cur, v->p.game_w, v->p.game_h, v->stride,
                          v->p.game_x, v->p.game_y);
    else
        video_quantise4(v->cur, v->p.game_w, v->p.game_h, v->stride);
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
