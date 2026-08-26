#ifndef KOBOY_VIDEO_H
#define KOBOY_VIDEO_H
#include "koboy.h"

/* The RGB -> grey reduction, under whichever koboy_gray_map is selected (the
   enum, and why it exists at all, are in koboy.h).

   These three MUST agree: video_gray_lut_build is just 65536 calls to
   video_rgb565_to_gray, and video_xrgb8888_to_gray is the same arithmetic for
   the other pixel format a core may request. A core that hands koboy
   XRGB8888 must not render differently from one that hands it RGB565, so all
   three go through one static gray_of() in video.c rather than three copies
   of the weights.

   An out-of-range map renders as KOBOY_GRAY_DEFAULT rather than reading past
   the weight table -- see the guard's comment in video.c. */
uint8_t video_rgb565_to_gray(uint16_t px, koboy_gray_map m);
uint8_t video_xrgb8888_to_gray(uint32_t px, koboy_gray_map m);
void    video_gray_lut_build(uint8_t lut[65536], koboy_gray_map m);

/* The range check, exported and named rather than left as an `if` at each use
   site, and that is deliberate rather than tidiness.

   koboy_gray_map reaches video.c through an `int` field in koboy_config --
   set from an ini file and from the in-game MENU -- so an out-of-range value
   is one bad edit away, and unguarded it would index the weight table off its
   end. But a guard whose only observable failure is reading past an array is
   a guard NO TEST CAN PROVE: what such a read returns is undefined, and on
   this host it may well land on a plausible value inside the next object and
   look like the guard working. That exact trap is written up in CLAUDE.md
   ("if a test can only fail via UB, it is not a test"). Exposing the clamp
   lets tests/test_video_gray.c assert the CLAMPED VALUE directly, so deleting
   the guard fails a check instead of merely becoming undefined.

   LIVE: video.c calls it on every gray_of and every video_gray_map_name. */
koboy_gray_map video_gray_map_clamp(int m);

/* The ini/menu spelling of a map, and the reverse. video_gray_map_name never
   returns NULL (an out-of-range map names the default), which is what lets
   main.c build a menu label without a null check. video_gray_map_parse
   returns false and touches nothing for a name it does not know, so a typo in
   koboy.ini keeps the previous value instead of silently becoming map 0. */
const char *video_gray_map_name(koboy_gray_map m);
bool        video_gray_map_parse(const char *s, koboy_gray_map *out);
void video_scale_gray(uint8_t *dst, int dst_stride, const uint8_t *src,
                      int src_w, int src_h, int src_stride, int scale);

/* Nearest-neighbour rescale to an ARBITRARY dst_w x dst_h -- the LCD layout's
   scaler, where "the largest integer multiple that fits" wastes most of the
   panel (a 654x396 Game & Watch unit at integer scale 1 uses about half the
   width of a 1264x1680 panel, which is what the device reported as "too
   small").

   Nearest neighbour, NOT interpolation, and that is a decision rather than a
   shortcut: the output is quantised to four grey levels immediately
   afterwards (video_quantise4), so a blended edge pixel lands on one of the
   same four values a nearest-neighbour pixel would -- it just picks a
   different one, and does it by smearing the hard edges of an LCD segment
   into its background. Blending buys nothing here and costs legibility.

   Fixed point, NOT floats: nothing in this project's pixel path uses floating
   point and the device's armhf toolchain is not where anyone wants to find
   out what that costs. A 16.16 step per axis is exact enough that the whole
   error budget over a 1560-row destination is a fraction of one source pixel.

   Rows repeat under upscaling, so a destination row that maps to the same
   source row as its predecessor is memcpy'd from it rather than resampled --
   the same "collapse per-pixel work into block copies" trick the integer path
   above uses, and it matters because video_submit is this pipeline's measured
   bottleneck (CLAUDE.md).

   Preconditions, stated because they are checked rather than assumed:
   src_w/src_h/dst_w/dst_h all >= 1 (anything else draws nothing), and
   dst_stride >= dst_w. src_w and src_h must be under 65536 for the 16.16
   step to be representable -- far above any core geometry, and guarded. */
void video_scale_gray_frac(uint8_t *dst, int dst_stride, const uint8_t *src,
                           int src_w, int src_h, int src_stride,
                           int dst_w, int dst_h);

extern const uint8_t KOBOY_DU4_LEVELS[4];

void video_bayer_build(uint8_t m[16][16]);
void video_quantise4(uint8_t *buf, int w, int h, int stride);
void video_dither_1bit(uint8_t *buf, int w, int h, int stride,
                       int screen_x, int screen_y);

#define KOBOY_TILE 8
koboy_rect video_dirty_rect(const uint8_t *prev, const uint8_t *cur,
                           int w, int h, int stride);

/* Up to this many rectangles per frame. Four was enough to separate a sprite
   corner from a status bar corner (the case this exists for) without the
   candidate/merge bookkeeping growing unbounded; nothing about the algorithm
   requires exactly four, but callers size their koboy_rect arrays against
   this constant so it is fixed at compile time.
   NOT a guarantee of disjointness -- see video_split_dirty below. */
#define KOBOY_MAX_RECTS 4

/* Splits the changed region into up to max_out rectangles, or returns the
   single merged bounding box when splitting would not pay.

   Refresh cost on this hardware is roughly `fixed + area`, so a sprite in the
   top-left and a status bar in the bottom-right merge into a near-full-rect
   refresh whose interior has not changed. `fixed_tiles` expresses the fixed
   cost in 8x8 tiles and comes from config, not from a constant: every absolute
   timing this project has measured moved by up to a factor of 2.2 between
   sessions, so a compiled-in threshold would be false precision.

   Returns 0 when nothing changed. The union of the returned rects ALWAYS covers
   every changed tile -- a dropped region leaves a stale pixel that looks exactly
   like ghosting, which is the worst kind of e-ink bug because nobody reports it
   as a bug. tests/test_video_multirect.c asserts that union directly.

   The rects are NOT guaranteed disjoint. When the candidate list is capped to
   max_out, candidates are merged pairwise by bounding-box union (src/video.c),
   which can make one candidate a strict superset of another once it has
   absorbed a third; video_split_dirty drops any rect fully contained in
   another after capping, but does not otherwise deoverlap. Coverage still
   holds either way -- a union only grows -- the cost is a rect blitted and
   refreshed twice, not a stale pixel.

   max_out < 1 degrades to the merged box (written to out[0], returns 1) rather
   than refusing to answer: a caller that mis-passes 0 still gets a correct,
   if unsplit, answer as long as out itself has room for one rect -- this is
   the live guard for that case. out == NULL has nowhere to write anything, so
   that returns 0 -- the live guard for that case. */
int video_split_dirty(const uint8_t *prev, const uint8_t *cur,
                      int w, int h, int stride, int fixed_tiles,
                      koboy_rect *out, int max_out);

/* Largest integer scale at which src_w x src_h fits p's reserved game rect,
   plus the offsets that centre it. At the core's max geometry this is
   p->scale at (0,0); below max it scales up to fill rather than leaving the
   frame at 1:1 in a corner. Exposed for tests.

   This is the DMG layout's fit and stays integer-only. The Game Boy must
   still resolve to exactly scale 5 -- 800x720 at (232,84) on a 1264x1680
   panel -- and an integer scale is also what makes video_scale_gray's
   memset/memcpy block path applicable at all. */
void video_fit(const koboy_profile *p, int src_w, int src_h,
               int *scale_out, int *ox_out, int *oy_out);

/* The largest dst_w x dst_h with src's aspect ratio that fits avail_w x
   avail_h. Exact integer arithmetic, no fixed-point ratio and no float: the
   BINDING axis is filled to the pixel, which is the property that lets
   config_resolve_profile and video_fit_rect agree on the same rect instead of
   drifting apart by the rounding of a shared 16.16 ratio.

   BOTH axes are fitted -- min of the two ratios, not just width. Fitting
   width alone overflows the panel for a tall title: Donkey Kong is 606x748,
   and scaled to a 1264-wide panel that is 1560 rows, which only fits at all
   because the bottom strip is subtracted first.

   Lives here, not in config.c, so there is ONE definition of the aspect fit
   shared by the resolver that sizes the reserved rect and the per-frame fit
   that places a frame inside it. Writes nothing and returns silently for a
   degenerate input (any argument < 1), which the callers' own guards already
   exclude. */
void video_fit_frac(int src_w, int src_h, int avail_w, int avail_h,
                    int *dw_out, int *dh_out);

/* Where a src_w x src_h frame lands inside p's reserved game rect, in
   game-rect-relative pixels, for WHATEVER layout p carries: an integer
   multiple in KOBOY_LAYOUT_DMG (video_fit above, times src), a fractional
   aspect-preserving fit in KOBOY_LAYOUT_LCD. Always centred. */
void video_fit_rect(const koboy_profile *p, int src_w, int src_h,
                    int *dw_out, int *dh_out, int *ox_out, int *oy_out);

typedef struct koboy_video koboy_video;

koboy_video   *video_create(const koboy_profile *p, bool force_dither,
                            koboy_gray_map map);

/* Swaps the greyscale mapping of a live koboy_video, rebuilding the 65536-entry
   LUT in place. Costs one LUT build (~65k multiply/divide pairs, microseconds)
   and nothing per frame afterwards, which is the whole point of keeping this a
   LUT: video_submit is this pipeline's measured bottleneck and must not gain a
   branch per pixel.

   Does NOT invalidate: the caller must follow this with video_invalidate(),
   because `prev` still holds pixels produced by the OLD mapping and the dirty
   diff would leave every unchanged tile showing them. On e-ink that is a
   half-old, half-new frame that persists until something else happens to touch
   those tiles. main.c's return-from-MODE_MENU path already invalidates
   unconditionally, which is why the menu entry does not have to. */
void           video_set_gray_map(koboy_video *v, koboy_gray_map map);
koboy_gray_map video_get_gray_map(const koboy_video *v);
void           video_destroy(koboy_video *v);
koboy_rect     video_submit(koboy_video *v, const void *src, int src_w, int src_h,
                            size_t src_pitch, koboy_pixfmt fmt);

/* As video_submit, but fills `out` with up to max_out rects (video_split_dirty
   above decides how many) and returns the count. video_submit remains a
   single-rect wrapper -- max_out 1 -- so existing callers and Task 6's
   tests/test_video_dirty.c keep working unchanged.

   fixed_tiles is threaded through as a parameter rather than stored on
   koboy_video at video_create time: video_create's signature is shared with
   tests/test_video_dirty.c and tests/test_video_pipeline.c, which the task
   brief requires stay untouched, so the config value that only main.c knows
   about travels through the call instead of through the struct. video_submit
   passes 0: with max_out 1, video_split_dirty takes its max_out<1-equivalent
   merged-box path before fixed_tiles is ever read, so the value is dead on
   that path by construction, not by accident. */
int video_submit_rects(koboy_video *v, const void *src, int src_w, int src_h,
                       size_t src_pitch, koboy_pixfmt fmt, int fixed_tiles,
                       koboy_rect *out, int max_out);

/* Forces the next video_submit to report the entire game rect dirty.
   Called on the way back from any UI mode: those modes paint over the game
   rect, so `prev` stops describing what is on the panel. Without this the
   first frame back diffs against a screen that is no longer there and leaves
   the overpainted region stale -- which looks exactly like ghosting and is
   therefore the kind of bug nobody reports as a bug. */
void video_invalidate(koboy_video *v);

/* The rect the LAST submitted frame actually occupies inside the reserved
   game rect, game-rect-relative -- i.e. video_fit_rect's answer for that
   frame. Zero-sized until a frame has been submitted.

   It exists for the LCD layout's pointer input: a touch has to be normalised
   against the pixels the artwork is ACTUALLY drawn on, and in that layout
   those are not the whole reserved rect. The reserved rect is sized from the
   core's MAX geometry, and a Game & Watch title alternates between the whole
   unit (max) and the LCD alone (smaller), several times a second -- and the
   core normalises the pointer it receives against whatever it is currently
   showing (third_party/gw/gwlua/functions.c reads state->zoom for exactly
   this). Normalising against the reserved rect instead would leave every
   touch offset by the centring margin whenever the frame is not at max. */
void video_frame_rect(const koboy_video *v, koboy_rect *out);

const uint8_t *video_buffer(const koboy_video *v);
int            video_stride(const koboy_video *v);
#endif
