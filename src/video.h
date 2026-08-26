#ifndef KOBOY_VIDEO_H
#define KOBOY_VIDEO_H
#include "koboy.h"

uint8_t video_rgb565_to_gray(uint16_t px);
uint8_t video_xrgb8888_to_gray(uint32_t px);
void    video_gray_lut_build(uint8_t lut[65536]);
void video_scale_gray(uint8_t *dst, int dst_stride, const uint8_t *src,
                      int src_w, int src_h, int src_stride, int scale);

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

typedef struct koboy_video koboy_video;

koboy_video   *video_create(const koboy_profile *p, bool force_dither);
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

const uint8_t *video_buffer(const koboy_video *v);
int            video_stride(const koboy_video *v);
#endif
