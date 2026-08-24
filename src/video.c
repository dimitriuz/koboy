#include "video.h"
#include <string.h>

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
