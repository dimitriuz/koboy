#ifndef KOBOY_VIDEO_H
#define KOBOY_VIDEO_H
#include "koboy.h"

/* The RGB -> grey reduction under the selected koboy_gray_map (the enum is in
   koboy.h). These three MUST agree -- a core handing koboy XRGB8888 must not
   render differently from one handing it RGB565 -- so all three go through one
   static gray_of() in video.c rather than three copies of the weights.
   An out-of-range map renders as KOBOY_GRAY_DEFAULT. */
uint8_t video_rgb565_to_gray(uint16_t px, koboy_gray_map m);
uint8_t video_xrgb8888_to_gray(uint32_t px, koboy_gray_map m);
void    video_gray_lut_build(uint8_t lut[65536], koboy_gray_map m);

/* The range check, EXPORTED rather than left as an `if` at each use site, and
   that is not tidiness. koboy_gray_map reaches video.c through an `int` field
   set from an ini file and the in-game MENU, so an out-of-range value is one
   bad edit from indexing the weight table off its end -- but a guard whose
   only observable failure is an out-of-bounds read is a guard NO TEST CAN
   PROVE (CLAUDE.md: "if a test can only fail via UB, it is not a test").
   Exposing the clamp lets tests/test_video_gray.c assert the CLAMPED VALUE, so
   deleting the guard fails a check.

   LIVE: called on every gray_of and every video_gray_map_name. */
koboy_gray_map video_gray_map_clamp(int m);

/* The ini/menu spelling of a map, and the reverse. _name never returns NULL
   (an out-of-range map names the default), so main.c builds a menu label
   without a null check. _parse returns false and touches nothing for an
   unknown name, so a typo in koboy.ini keeps the previous value instead of
   silently becoming map 0. */
const char *video_gray_map_name(koboy_gray_map m);
bool        video_gray_map_parse(const char *s, koboy_gray_map *out);
void video_scale_gray(uint8_t *dst, int dst_stride, const uint8_t *src,
                      int src_w, int src_h, int src_stride, int scale);

/* Nearest-neighbour rescale to an ARBITRARY dst_w x dst_h -- the LCD layout's
   scaler, where "the largest integer multiple that fits" wastes most of the
   panel (a 654x396 Game & Watch unit at integer 1x uses about half a 1264-wide
   panel: the device's "too small").

   NEAREST NEIGHBOUR, not interpolation, and that is a decision: the output is
   quantised to four levels immediately afterwards, so a blended edge pixel
   lands on one of the same four values -- it just picks a different one, by
   smearing the hard edges of an LCD segment into its background.

   FIXED POINT, not floats: nothing in this pixel path uses floating point, and
   a 16.16 step per axis keeps the whole error budget over a 1560-row
   destination under one source pixel.

   Rows repeat under upscaling, so a destination row mapping to the same source
   row as its predecessor is memcpy'd rather than resampled -- the same
   collapse-into-block-copies trick the integer path uses, and it matters
   because video_submit is this pipeline's measured bottleneck.

   Preconditions, CHECKED not assumed: src/dst w and h all >= 1, dst_stride >=
   dst_w, and src_w/src_h under 65536 so the 16.16 step is representable. */
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

/* Up to this many rectangles per frame. Four separates a sprite corner from a
   status bar corner (the case this exists for) without unbounded merge
   bookkeeping; nothing requires exactly four, but callers size their
   koboy_rect arrays against it. NOT a disjointness guarantee -- see below. */
#define KOBOY_MAX_RECTS 4

/* Splits the changed region into up to max_out rectangles, or returns the
   single merged bounding box when splitting would not pay.

   Refresh cost here is roughly `fixed + area`, so a sprite in one corner and a
   status bar in the other merge into a near-full-rect refresh whose interior
   has not changed. `fixed_tiles` is that fixed cost in 8x8 tiles and comes
   from config, not a constant: every absolute timing this project measured
   moved by up to 2.2x between sessions, so a compiled-in threshold would be
   false precision.

   Returns 0 when nothing changed. THE UNION OF THE RETURNED RECTS ALWAYS
   COVERS EVERY CHANGED TILE -- a dropped region leaves a stale pixel that
   looks exactly like ghosting, the worst kind of e-ink bug because nobody
   reports it as a bug. tests/test_video_multirect.c asserts the union.

   The rects are NOT guaranteed disjoint: capping to max_out merges candidates
   pairwise by bounding-box union, which can make one a strict superset of
   another once it has absorbed a third. Contained rects are dropped after
   capping, but nothing else is deoverlapped. Coverage holds either way -- a
   union only grows -- and the cost is a rect refreshed twice, not a stale
   pixel.

   LIVE GUARDS: max_out < 1 degrades to the merged box in out[0] (returns 1)
   rather than refusing; out == NULL returns 0. */
int video_split_dirty(const uint8_t *prev, const uint8_t *cur,
                      int w, int h, int stride, int fixed_tiles,
                      koboy_rect *out, int max_out);

/* THE PIXEL ASPECT RATIO of the frame a core just delivered -- how wide one of
   its pixels is relative to how tall, in 16.16 -- from the DISPLAY aspect the
   core asked for and the frame's delivered size. KOBOY_ASPECT_ONE is square,
   which is what every scaler path here did unconditionally before this existed.

   A separate step rather than a field because the display aspect belongs to
   the CORE while the pixel aspect depends on the frame in hand, and one core
   varies that frame to frame (Game & Watch alternates between the whole unit
   and the LCD alone several times a second). Three integer operations per
   submit, not a per-pixel cost.

   THE DEADBAND IS LOAD-BEARING. A result within KOBOY_PAR_DEADBAND of 1.0
   snaps to exactly 1.0, because cores report ROUNDED aspects: race (Neo Geo
   Pocket) reports 1.05 for a 160x152 frame whose exact ratio is 1.0526, 0.25%
   off square. Without the snap that core -- and any writing 1.33 for 4/3 --
   leaves the integer block-copy scaler for the fractional one and resamples a
   picture that did not need it. 1/128 is eleven times below the smallest REAL
   anisotropy measured (Mega Drive, 32:35, 8.6% off square), so it cannot
   swallow a genuine one.

   LIVE GUARD: returns KOBOY_ASPECT_ONE for a degenerate frame or an absent
   aspect rather than dividing by it. */
#define KOBOY_PAR_DEADBAND 512u        /* 1/128 in 16.16 */
uint32_t video_pixel_aspect(uint32_t display_aspect, int frame_w, int frame_h);

/* Largest scale at which src_w x src_h fits p's reserved game rect, plus the
   destination width the pixel aspect asks for and the centring offsets. At the
   core's max geometry with square pixels this is p->scale at (0,0); below max
   it scales up to fill. Exposed for tests.

   THE SCALE IS THE VERTICAL ONE AND STAYS AN INTEGER; only the horizontal axis
   carries the pixel aspect. That asymmetry is a decision:

     - It makes a square-pixel core bit-identical: par == 1 gives
       dw == src_w * scale, and the pipeline takes video_scale_gray's block
       path exactly as it always did. The Game Boy is the only presentation
       verified on hardware, and the goldens pin it.
     - Uneven ROW replication is the worst artifact available here. A 160x210
       Atari frame fitted freely into a 960x768 rect is 960x720 -- every source
       row drawn 3 or 4 times in a repeating comb across a four-level panel.
       Integer vertical gives 840x630: exactly three rows per row, the shape
       docs/FOLLOWUPS.md #51 predicted by hand.

   The cost, taken deliberately, is that a tall frame with a wide pixel leaves
   more of the rect unused than a free fit would (PAL Atari, 160x250 at 25:12,
   lands on 666x500 of 960x768). */
void video_fit_par(const koboy_profile *p, int src_w, int src_h, uint32_t par,
                   int *scale_out, int *dw_out, int *ox_out, int *oy_out);

/* video_fit_par with square pixels -- what this function has always been.
   Kept as its own name because config.c's sibling and every pre-existing test
   talk about it. */
void video_fit(const koboy_profile *p, int src_w, int src_h,
               int *scale_out, int *ox_out, int *oy_out);

/* The largest dst_w x dst_h with src's aspect ratio that fits avail_w x
   avail_h. EXACT integer arithmetic, no fixed-point ratio and no float: the
   BINDING axis is filled to the pixel, which is what lets
   config_resolve_profile and video_fit_rect agree on the same rect instead of
   drifting apart by the rounding of a shared 16.16 ratio.

   BOTH axes are fitted -- min of the two ratios. Fitting width alone overflows
   the panel for a tall title: Donkey Kong is 606x748, which scaled to 1264
   wide is 1560 rows.

   Here and not in config.c so there is ONE definition of the aspect fit,
   shared by the resolver that sizes the rect and the per-frame fit that places
   a frame inside it. Writes nothing for a degenerate input (any argument
   < 1). */
void video_fit_frac(int src_w, int src_h, int avail_w, int avail_h,
                    int *dw_out, int *dh_out);

/* Where a src_w x src_h frame lands inside p's reserved game rect, in
   game-rect-relative pixels, for WHATEVER layout p carries: integer-vertical
   in KOBOY_LAYOUT_DMG (video_fit_par), fractional aspect-preserving in
   KOBOY_LAYOUT_LCD. Always centred on both axes and NEVER wider or taller than
   the rect -- video_pipeline_run's bounds guard exists because an overflowing
   frame corrupts memory rather than merely looking wrong, and this is what
   keeps it from firing.

   "NEVER WIDER OR TALLER" IS A PROMISE FOR EVERY FRAME IN [1, max], not an
   inherited property of the rect. It used to be the latter (the DMG rect was
   max times an integer). The rect now comes from BASE geometry, so a
   bigger-than-base frame CAN exceed it, and the DMG branch drops to the
   fractional fit when it does. That is the safety argument for the whole
   rect-sizing change, swept over every system's real geometry in
   tests/test_video_pipeline.c.

   KOBOY_ASPECT_ONE reproduces the pre-anisotropy answer exactly, both
   branches. */
void video_fit_rect(const koboy_profile *p, int src_w, int src_h, uint32_t par,
                    int *dw_out, int *dh_out, int *ox_out, int *oy_out);

typedef struct koboy_video koboy_video;

koboy_video   *video_create(const koboy_profile *p, bool force_dither,
                            koboy_gray_map map);

/* Quarter turns COUNTER-CLOCKWISE applied to every frame, 0..3, matching
   RETRO_ENVIRONMENT_SET_ROTATION. Defaults to 0, byte for byte the behaviour
   that existed before rotation did.

   Set it right after video_create and leave it alone: the profile this
   koboy_video was built from was resolved from ALREADY-TRANSPOSED geometry
   (core_get_geometry does the swap), so rotation and buffer dimensions must
   agree -- kept so by destroying and rebuilding on a change to either.
   Flipping it live without a video_invalidate leaves prev claiming that pixels
   which all moved did not. */
void           video_set_rotation(koboy_video *v, int rot);
int            video_get_rotation(const koboy_video *v);

/* The DISPLAY aspect the core asked for, 16.16, from core_display_aspect. Set
   right after video_create and again whenever the core re-announces, like
   video_set_rotation -- and like it, a change needs a video_invalidate,
   because every pixel moves.

   THE DEFAULT IS 0, MEANING "NO ANSWER", AND YIELDS SQUARE PIXELS. Not
   KOBOY_ASPECT_ONE: a display aspect of 1:1 is a real, non-neutral claim (a
   160x144 frame in a square gives pixels 0.9 as wide as tall), so it cannot
   double as the sentinel. */
void           video_set_aspect(koboy_video *v, uint32_t display_aspect);
uint32_t       video_get_aspect(const koboy_video *v);

/* Swaps the greyscale mapping of a LIVE koboy_video, rebuilding the
   65536-entry LUT in place -- microseconds once, nothing per frame, which is
   the point of a LUT: video_submit is the measured bottleneck and must not
   gain a branch per pixel.

   Does NOT invalidate; the caller must. `prev` still holds pixels from the OLD
   mapping, and the dirty diff would leave every unchanged tile showing them --
   on e-ink a half-old frame that persists until something else touches those
   tiles. main.c's return-from-MODE_MENU path invalidates unconditionally. */
void           video_set_gray_map(koboy_video *v, koboy_gray_map map);
koboy_gray_map video_get_gray_map(const koboy_video *v);

/* Whether the pipeline ends in video_dither_1bit (two output values) instead
   of video_quantise4 (four). Settable LIVE for the reason video_set_gray_map
   is: the in-game MOTION entry has to be judged while looking at motion.

   A change needs a video_invalidate -- every pixel's output value can move.
   main.c's return-from-menu path invalidates unconditionally.

   The getter lets the log report what the LIVE pipeline is doing rather than
   what config.c parsed, and is the only handle a host test has on this at all,
   since only the panel can see the difference the setting is for. */
void           video_set_dither(koboy_video *v, bool on);
bool           video_get_dither(const koboy_video *v);
void           video_destroy(koboy_video *v);
koboy_rect     video_submit(koboy_video *v, const void *src, int src_w, int src_h,
                            size_t src_pitch, koboy_pixfmt fmt);

/* As video_submit, but fills `out` with up to max_out rects (video_split_dirty
   decides how many) and returns the count. video_submit is the single-rect
   wrapper -- max_out 1.

   fixed_tiles is a PARAMETER rather than a koboy_video field so video_create's
   signature stays shared with the existing tests. video_submit passes 0: at
   max_out 1 video_split_dirty takes its merged-box path before fixed_tiles is
   read, so the value is dead there by construction, not by accident. */
int video_submit_rects(koboy_video *v, const void *src, int src_w, int src_h,
                       size_t src_pitch, koboy_pixfmt fmt, int fixed_tiles,
                       koboy_rect *out, int max_out);

/* Forces the next video_submit to report the entire game rect dirty. Called on
   the way back from any UI mode, which paints over the game rect so `prev`
   stops describing the panel. Without it the first frame back diffs against a
   screen that is no longer there and leaves the overpainted region stale --
   which looks exactly like ghosting. */
void video_invalidate(koboy_video *v);

/* The rect the LAST submitted frame occupies inside the reserved game rect --
   video_fit_rect's answer for that frame. Zero-sized until a frame arrives.

   For the LCD layout's POINTER input: a touch must be normalised against the
   pixels the artwork is actually on, which there are not the whole reserved
   rect. That rect comes from MAX geometry, and a Game & Watch title alternates
   between the whole unit and the LCD alone several times a second, normalising
   the pointer it receives against whatever it is showing
   (gw/gwlua/functions.c reads state->zoom). Normalising against the reserved
   rect would offset every touch by the centring margin. */
void video_frame_rect(const koboy_video *v, koboy_rect *out);

const uint8_t *video_buffer(const koboy_video *v);
int            video_stride(const koboy_video *v);
#endif
