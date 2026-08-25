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
   scale-1 times. Per-pixel work collapses into block copies. */
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

struct koboy_video {
    koboy_profile p;
    bool     dither;
    int      stride;
    uint8_t *cur, *prev, *gray;
    uint8_t  lut[65536];
};

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
    v->gray = calloc(1, (size_t)KOBOY_GB_W * KOBOY_GB_H);
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

koboy_rect video_submit(koboy_video *v, const void *src, int src_w, int src_h,
                        size_t src_pitch, koboy_pixfmt fmt)
{
    koboy_rect empty = { 0, 0, 0, 0 };
    if (!src) return empty;                       /* core signalled a duplicate */
    if (src_w != KOBOY_GB_W || src_h != KOBOY_GB_H) return empty;

    for (int y = 0; y < src_h; y++) {
        uint8_t *d = v->gray + (size_t)y * KOBOY_GB_W;
        if (fmt == KOBOY_PIXFMT_RGB565) {
            const uint16_t *s = (const uint16_t *)((const uint8_t *)src + (size_t)y * src_pitch);
            for (int x = 0; x < src_w; x++) d[x] = v->lut[s[x]];
        } else {
            const uint32_t *s = (const uint32_t *)((const uint8_t *)src + (size_t)y * src_pitch);
            for (int x = 0; x < src_w; x++) d[x] = video_xrgb8888_to_gray(s[x]);
        }
    }

    video_scale_gray(v->cur, v->stride, v->gray, KOBOY_GB_W, KOBOY_GB_H,
                     KOBOY_GB_W, v->p.scale);

    if (v->dither)
        video_dither_1bit(v->cur, v->p.game_w, v->p.game_h, v->stride,
                          v->p.game_x, v->p.game_y);
    else
        video_quantise4(v->cur, v->p.game_w, v->p.game_h, v->stride);

    koboy_rect r = video_dirty_rect(v->prev, v->cur, v->p.game_w, v->p.game_h, v->stride);
    /* cur holds the frame the caller will blit; prev becomes the baseline for
       the next diff. When nothing changed the buffers already match. */
    if (r.w) memcpy(v->prev, v->cur, (size_t)v->stride * (size_t)v->p.game_h);
    return r;
}
