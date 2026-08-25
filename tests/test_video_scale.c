#include "test.h"
#include "pgm.h"
#include "video.h"

TEST_MAIN({
    /* 2x2 source, scale 3 -> 6x6 of solid 3x3 blocks */
    const uint8_t src[4] = { 0, 85, 170, 255 };
    uint8_t dst[6 * 6];
    memset(dst, 0xAA, sizeof dst);
    video_scale_gray(dst, 6, src, 2, 2, 2, 3);
    for (int y = 0; y < 6; y++)
        for (int x = 0; x < 6; x++)
            CHECK_EQ_INT(dst[y * 6 + x], src[(y / 3) * 2 + (x / 3)]);

    /* scale 1 is a plain copy */
    uint8_t one[4];
    video_scale_gray(one, 2, src, 2, 2, 2, 1);
    CHECK(memcmp(one, src, 4) == 0);

    /* a dst_stride wider than the scaled width must leave the pad untouched */
    uint8_t pad[6 * 8];
    memset(pad, 0x11, sizeof pad);
    video_scale_gray(pad, 8, src, 2, 2, 2, 3);
    for (int y = 0; y < 6; y++) { CHECK_EQ_INT(pad[y*8+6], 0x11); CHECK_EQ_INT(pad[y*8+7], 0x11); }

    /* full Game Boy frame at the shipped 5x default, golden-checked */
    static uint8_t gb[KOBOY_GB_W * KOBOY_GB_H];
    for (int y = 0; y < KOBOY_GB_H; y++)
        for (int x = 0; x < KOBOY_GB_W; x++)
            gb[y * KOBOY_GB_W + x] = (uint8_t)(((x / 8) + (y / 8)) % 4 * 85);
    static uint8_t out[800 * 720];
    video_scale_gray(out, 800, gb, KOBOY_GB_W, KOBOY_GB_H, KOBOY_GB_W, 5);
    CHECK(pgm_compare_golden("scale_5x_checker", out, 800, 720, 800) == 1);
})
