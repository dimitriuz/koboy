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

static void fill_solid565(uint16_t *fb, int w, int h, int pitch_px, uint16_t color)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            fb[y * pitch_px + x] = color;
}

TEST_MAIN({
    koboy_config c; config_defaults(&c);
    koboy_profile p;
    CHECK(config_resolve_profile(&p, &c, 1264, 1680, KOBOY_GB_W, KOBOY_GB_H, KOBOY_GB_W, KOBOY_GB_H));

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

    /* Item 4 of the task: video_submit must accept any frame up to the
       core's max, and reject only what genuinely does not fit -- and item 3:
       the buffer is sized off max_w/max_h, so a core rendering smaller than
       its maximum (every measured Game & Watch title against a core whose
       max spans a whole collection) must not be treated as an error. None of
       this is reachable through the Game Boy above, where base always
       equals max always equals the one frame size ever submitted. */
    {
        koboy_config gc; config_defaults(&gc);
        /* base != max on purpose (matches the config-level sweep in
           tests/test_config.c): the buffer is allocated at 200x150 (max),
           the first real frame submitted is smaller (100x75, base), and
           accepting it is exactly what "may legitimately render smaller
           than its maximum" means. */
        koboy_profile gp;
        CHECK(config_resolve_profile(&gp, &gc, 1264, 1680, 100, 75, 200, 150));
        CHECK_EQ_INT(gp.scale, 5);
        CHECK_EQ_INT(gp.game_w, 1000);   /* 200 * 5, from max -- not 100 * 5 */
        CHECK_EQ_INT(gp.game_h, 750);

        koboy_video *gv = video_create(&gp, false);
        CHECK(gv != NULL);

        /* Sized for the 201-wide "oversized" fill below, not just the
           200x150 max -- that fill is staging data for a submit video.c
           must reject for being too wide, not an out-of-bounds write into
           this test's own source buffer, which would be a bug in the test
           and not the property under test. */
        static uint16_t gfb[201 * 150];
        /* First frame: full max size, solid white. Quantises to the top
           DU4 level (0xFF) everywhere -- checked at a corner far from
           anything the later, smaller frame will touch. */
        fill_solid565(gfb, 200, 150, 200, 0xFFFF);
        koboy_rect gr = video_submit(gv, gfb, 200, 150,
                                     200 * sizeof(uint16_t), KOBOY_PIXFMT_RGB565);
        CHECK_EQ_INT(gr.w, 1000);
        CHECK_EQ_INT(gr.h, 750);
        CHECK_EQ_INT(video_buffer(gv)[0], 0xFF);
        CHECK_EQ_INT(video_buffer(gv)[999 + (size_t)749 * video_stride(gv)], 0xFF);

        /* Second frame: base-sized (100x75, half the max in each axis),
           solid black -- accepted (item 3), not merely tolerated: the
           returned rect must be non-empty (the near corner really did
           change from white to black), and the newly-written area must
           read back as the new colour. */
        fill_solid565(gfb, 100, 75, 200, 0x0000);
        gr = video_submit(gv, gfb, 100, 75,
                          200 * sizeof(uint16_t), KOBOY_PIXFMT_RGB565);
        CHECK(gr.w > 0);
        CHECK_EQ_INT(video_buffer(gv)[0], 0x00);                 /* new corner: black */
        /* Far corner, outside the 100x75 frame's 500x375-px scaled
           footprint, is untouched by this submit -- still the white the
           FIRST frame put there. This is the documented tradeoff in
           video_submit_rects' comment: a smaller frame does not blank the
           area a bigger one used to cover, it just leaves it alone. */
        CHECK_EQ_INT(video_buffer(gv)[999 + (size_t)749 * video_stride(gv)], 0xFF);

        /* LIVE GUARD, exercised end to end: 201 is one column past max_w
           (200). Rejected outright -- an empty rect, and (checked via the
           two reads above already having happened, and repeated below)
           nothing in the buffer disturbed by the attempt. Without the bounds
           check in video_pipeline_run, this would walk one row past the end
           of a max_w-wide scratch buffer for every one of 150 rows. */
        fill_solid565(gfb, 201, 150, 201, 0xAAAA);
        gr = video_submit(gv, gfb, 201, 150,
                          201 * sizeof(uint16_t), KOBOY_PIXFMT_RGB565);
        CHECK_EQ_INT(gr.w, 0);
        CHECK_EQ_INT(video_buffer(gv)[0], 0x00);   /* unchanged: still the black corner */

        /* And the object is still usable after a rejected frame -- a real
           submit right afterward works normally, proving the rejection did
           not leave anything corrupted for the next call to trip over. */
        fill_solid565(gfb, 200, 150, 200, 0xFFFF);
        gr = video_submit(gv, gfb, 200, 150,
                          200 * sizeof(uint16_t), KOBOY_PIXFMT_RGB565);
        CHECK(gr.w > 0);
        CHECK_EQ_INT(video_buffer(gv)[0], 0xFF);

        video_destroy(gv);
    }
})
