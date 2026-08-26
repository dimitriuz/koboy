#include "test.h"
#include "pgm.h"
#include "video.h"
#include "config.h"

static void fill565(uint16_t *fb, int w, int h, int shift)
{
    static const uint16_t dmg[4] = { 0x0000, 0x52AA, 0xA555, 0xFFFF };
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            fb[y * w + x] = dmg[((x + shift) / 8 + y / 8) % 4];
}

TEST_MAIN({
    koboy_config c; config_defaults(&c);
    koboy_profile p;
    CHECK(config_resolve_profile(&p, &c, 1264, 1680));

    koboy_video *v = video_create(&p, false);
    CHECK(v != NULL);

    static uint16_t fb[KOBOY_GB_W * KOBOY_GB_H];
    fill565(fb, KOBOY_GB_W, KOBOY_GB_H, 0);

    /* first frame is entirely new */
    koboy_rect r = video_submit(v, fb, KOBOY_GB_W, KOBOY_GB_H,
                                KOBOY_GB_W * sizeof(uint16_t), KOBOY_PIXFMT_RGB565);
    CHECK_EQ_INT(r.w, 800);
    CHECK_EQ_INT(r.h, 720);
    CHECK(pgm_compare_golden("pipeline_dmg_5x", video_buffer(v), 800, 720,
                             video_stride(v)) == 1);

    /* resubmitting identical content changes nothing */
    r = video_submit(v, fb, KOBOY_GB_W, KOBOY_GB_H,
                     KOBOY_GB_W * sizeof(uint16_t), KOBOY_PIXFMT_RGB565);
    CHECK_EQ_INT(r.w, 0);

    /* a NULL frame is the core's can-dupe signal: also nothing */
    r = video_submit(v, NULL, KOBOY_GB_W, KOBOY_GB_H, 0, KOBOY_PIXFMT_RGB565);
    CHECK_EQ_INT(r.w, 0);

    /* shifting the pattern dirties a real region */
    fill565(fb, KOBOY_GB_W, KOBOY_GB_H, 8);
    r = video_submit(v, fb, KOBOY_GB_W, KOBOY_GB_H,
                     KOBOY_GB_W * sizeof(uint16_t), KOBOY_PIXFMT_RGB565);
    CHECK(r.w > 0);

    /* DMG's four shades must survive as exactly four distinct output values,
       with no dithering, because DU4 is itself a four-level mode */
    int seen[256] = {0}, distinct = 0;
    const uint8_t *buf = video_buffer(v);
    for (int y = 0; y < 720; y++)
        for (int x = 0; x < 800; x++) seen[buf[y * video_stride(v) + x]] = 1;
    for (int i = 0; i < 256; i++) distinct += seen[i];
    CHECK_EQ_INT(distinct, 4);

    video_destroy(v);

    /* #4: force_dither=true was only ever exercised by calling
       video_dither_1bit directly (tests/test_video_quant.c) -- the
       `if (v->dither)` wiring in the pipeline itself, which is what an actual
       GBC user with force_dither on hits, had no end-to-end coverage. Same
       pattern as above (four-level check), but through the dithered path:
       the 1-bit disher's only two output values are 0x00 and 0xFF, so
       anything else surviving into the buffer is a real regression, not a
       rounding difference. */
    {
        koboy_video *dv = video_create(&p, true);
        CHECK(dv != NULL);

        fill565(fb, KOBOY_GB_W, KOBOY_GB_H, 0);
        koboy_rect dr = video_submit(dv, fb, KOBOY_GB_W, KOBOY_GB_H,
                                     KOBOY_GB_W * sizeof(uint16_t), KOBOY_PIXFMT_RGB565);
        CHECK(dr.w > 0);

        int dseen[256] = {0}, ddistinct = 0;
        const uint8_t *dbuf = video_buffer(dv);
        for (int y = 0; y < 720; y++)
            for (int x = 0; x < 800; x++) dseen[dbuf[y * video_stride(dv) + x]] = 1;
        for (int i = 0; i < 256; i++) ddistinct += dseen[i];
        CHECK_EQ_INT(ddistinct, 2);
        CHECK(dseen[0x00]);
        CHECK(dseen[0xFF]);

        video_destroy(dv);
    }
})
