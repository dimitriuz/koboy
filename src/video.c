#include "video.h"

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
