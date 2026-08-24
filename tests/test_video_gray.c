#include "test.h"
#include "video.h"

TEST_MAIN({
    CHECK_EQ_INT(video_rgb565_to_gray(0x0000), 0);
    CHECK_EQ_INT(video_rgb565_to_gray(0xFFFF), 255);
    CHECK_EQ_INT(video_xrgb8888_to_gray(0x00000000u), 0);
    CHECK_EQ_INT(video_xrgb8888_to_gray(0x00FFFFFFu), 255);

    /* green dominates luma, blue contributes least */
    CHECK(video_rgb565_to_gray(0x07E0) > video_rgb565_to_gray(0xF800));
    CHECK(video_rgb565_to_gray(0xF800) > video_rgb565_to_gray(0x001F));

    /* the LUT must agree with the scalar path at every input */
    static uint8_t lut[65536];
    video_gray_lut_build(lut);
    int mismatches = 0;
    for (int i = 0; i < 65536; i++)
        if (lut[i] != video_rgb565_to_gray((uint16_t)i)) mismatches++;
    CHECK_EQ_INT(mismatches, 0);

    /* monotonic along the grey diagonal */
    int prev = -1, nonmono = 0;
    for (int v = 0; v < 32; v++) {
        uint16_t px = (uint16_t)((v << 11) | ((v * 2) << 5) | v);
        int g = video_rgb565_to_gray(px);
        if (g < prev) nonmono++;
        prev = g;
    }
    CHECK_EQ_INT(nonmono, 0);
})
