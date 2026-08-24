#ifndef KOBOY_VIDEO_H
#define KOBOY_VIDEO_H
#include "koboy.h"

uint8_t video_rgb565_to_gray(uint16_t px);
uint8_t video_xrgb8888_to_gray(uint32_t px);
void    video_gray_lut_build(uint8_t lut[65536]);
void video_scale_gray(uint8_t *dst, int dst_stride, const uint8_t *src,
                      int src_w, int src_h, int src_stride, int scale);
#endif
