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

    /* ------------------------------------------- fractional scaling (LCD) */

    /* THE MAPPING ITSELF, pinned by hand-computed source indices rather than
       by a second implementation of the same formula. video.h specifies a
       16.16 step -- step = (src << 16) / dst, accumulated -- and truncation in
       that step is observable: 3 source columns into 2 destination columns
       samples 0 and 1, NOT 0 and 2, because the accumulator only reaches
       98304 (1.5 in 16.16) at dx = 1 and 1.5 floors to 1. Asserting the
       ROUNDED-to-nearest answer instead would silently accept a different
       resampler. */
    {
        const uint8_t s2[4] = { 10, 20, 30, 40 };     /* 2x2 */
        uint8_t d[16];

        /* 2x2 -> 4x4: an exact doubling, so the fractional path must agree
           with the integer path pixel for pixel. Without this, the two could
           drift and only the LCD layout would notice. */
        uint8_t di[16];
        memset(d, 0, sizeof d); memset(di, 0, sizeof di);
        video_scale_gray_frac(d, 4, s2, 2, 2, 2, 4, 4);
        video_scale_gray(di, 4, s2, 2, 2, 2, 2);
        CHECK(memcmp(d, di, 16) == 0);

        /* 2x2 -> 3x3: columns/rows map [0,0,1] (step 43690; 0, 43690, 87380). */
        static const int m23[3] = { 0, 0, 1 };
        memset(d, 0, sizeof d);
        video_scale_gray_frac(d, 3, s2, 2, 2, 2, 3, 3);
        for (int y = 0; y < 3; y++)
            for (int x = 0; x < 3; x++)
                CHECK_EQ_INT(d[y * 3 + x], s2[m23[y] * 2 + m23[x]]);

        /* 3x3 -> 2x2: a DOWNSCALE, mapping [0,1]. The direction in which a
           rounded (rather than floored) step would overshoot the source --
           see the proof in video_scale_gray_frac. */
        const uint8_t s3[9] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
        static const int m32[2] = { 0, 1 };
        memset(d, 0, sizeof d);
        video_scale_gray_frac(d, 2, s3, 3, 3, 3, 2, 2);
        for (int y = 0; y < 2; y++)
            for (int x = 0; x < 2; x++)
                CHECK_EQ_INT(d[y * 2 + x], s3[m32[y] * 3 + m32[x]]);
    }

    /* Degenerate extents draw NOTHING rather than something wrong: each of
       these would divide by zero or index backwards inside the scaler. */
    {
        const uint8_t s[4] = { 1, 2, 3, 4 };
        uint8_t d[16];
        memset(d, 0x5A, sizeof d);
        video_scale_gray_frac(d, 4, s, 0, 2, 2, 4, 4);
        video_scale_gray_frac(d, 4, s, 2, 0, 2, 4, 4);
        video_scale_gray_frac(d, 4, s, 2, 2, 2, 0, 4);
        video_scale_gray_frac(d, 4, s, 2, 2, 2, 4, 0);
        /* and the 16.16 representability ceiling: (src << 16) would wrap */
        video_scale_gray_frac(d, 4, s, 65536, 2, 2, 4, 4);
        video_scale_gray_frac(d, 4, s, 2, 65536, 2, 4, 4);
        for (size_t i = 0; i < sizeof d; i++) CHECK_EQ_INT(d[i], 0x5A);
    }

    /* THE ROW-REPEAT OPTIMISATION, against the same mapping written longhand
       with no memcpy shortcut -- the one part of the scaler that is an
       OPTIMISATION rather than a definition, and so the one part that can be
       wrong while every hand-computed case above passes (a shortcut copying
       the WRONG previous row, or copying when the source row advanced, only
       shows up on a real image).

       At the measured LCD geometry (654x396 Mickey Mouse -> the 1264x765 rect
       on a 1264x1680 panel) and at a downscale, covering the repeat-heavy and
       skip-heavy directions. The destination is guarded on both sides: a
       stride wider than dst_w must leave the pad untouched, and nothing may
       run past the end. */
    {
        static uint8_t src[654 * 396];
        for (int y = 0; y < 396; y++)
            for (int x = 0; x < 654; x++)
                src[y * 654 + x] = (uint8_t)((x * 7 + y * 13 + (x ^ y)) & 0xFF);

        static const struct { int dw, dh; } sizes[] = {
            { 1264, 765 },     /* the measured Mickey Mouse fit */
            { 327,  198 },     /* an exact halving */
            { 200,  300 },     /* a downscale that does not divide */
            { 654,  396 },     /* 1:1 */
        };
        static uint8_t got[1300 * 800], want[1300 * 800];
        const int gstride = 1300;

        for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
            int dw = sizes[i].dw, dh = sizes[i].dh;
            memset(got, 0xC3, sizeof got);
            memset(want, 0xC3, sizeof want);

            /* Longhand: identical 16.16 accumulator, no row reuse. No
               clamp either -- video.c's own comment proves the floored step
               cannot overshoot, and a reference that clamped would hide a
               step change that made it possible. */
            unsigned xstep = ((unsigned)654 << 16) / (unsigned)dw;
            unsigned ystep = ((unsigned)396 << 16) / (unsigned)dh;
            unsigned yfp = 0;
            for (int dy = 0; dy < dh; dy++, yfp += ystep) {
                int sy = (int)(yfp >> 16);
                unsigned xfp = 0;
                for (int dx = 0; dx < dw; dx++, xfp += xstep)
                    want[dy * gstride + dx] = src[sy * 654 + (int)(xfp >> 16)];
            }

            video_scale_gray_frac(got, gstride, src, 654, 396, 654, dw, dh);
            if (memcmp(got, want, sizeof got) != 0) {
                size_t j = 0;
                while (j < sizeof got && got[j] == want[j]) j++;
                fprintf(stderr, "  %dx%d: first mismatch at byte %zu"
                        " (row %zu col %zu): got %u want %u\n",
                        dw, dh, j, j / gstride, j % gstride,
                        (unsigned)got[j], (unsigned)want[j]);
            }
            CHECK(memcmp(got, want, sizeof got) == 0);
        }
    }
})
