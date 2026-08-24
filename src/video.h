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
#endif
