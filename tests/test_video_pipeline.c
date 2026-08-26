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
        /* Far corner is now BLACK too, and this assertion is the regression
           test for a bug a real user hit on a real device.

           This used to assert 0xFF, on the reasoning that a smaller frame
           "does not blank the area a bigger one used to cover, it just leaves
           it alone" -- called a documented tradeoff. It was a defect. A Game
           & Watch title alternates between the whole unit (654x396) and the
           LCD alone (305x191) several times a second; under the old rule the
           smaller view drew at 1:1 in the top-left corner of the rect the
           bigger one had sized, with the rest left black. On the panel that
           is a postage stamp in the corner of an empty bezel.

           video_fit now scales the smaller frame UP to fill the rect it was
           given: 100x75 into a 1000x750 rect is 10x, offsets zero, so every
           pixel including this far corner is the new frame's black. The
           first frame (200x150, the max) still fits at exactly p.scale with
           zero offset, which is the Game Boy path and is unchanged. */
        CHECK_EQ_INT(video_buffer(gv)[999 + (size_t)749 * video_stride(gv)], 0x00);

        /* THE ASSERTION THAT ACTUALLY DISCRIMINATES, and the one above is
           kept only as documentation of the old behaviour's shape.

           The check above cannot fail on its own. A size change clears the
           margin before scaling, so with the fit reverted to the old
           corner-parked p.scale the rect is memset to 0 and a solid BLACK
           frame is drawn into its corner -- far corner reads 0x00 either way,
           and the mutant passes. Confirmed by running exactly that mutant.

           A same-size frame in a DIFFERENT colour separates them: no margin
           clear fires (100x75 both times), so the far corner can only become
           white if the frame was scaled up to reach it. Fitted, 100x75 goes
           to 10x = 1000x750 and covers the whole rect. Corner-parked at
           p.scale it is 500x375 in the top-left and this corner keeps the
           previous frame's black. */
        fill_solid565(gfb, 100, 75, 200, 0xFFFF);
        gr = video_submit(gv, gfb, 100, 75,
                          200 * sizeof(uint16_t), KOBOY_PIXFMT_RGB565);
        CHECK(gr.w > 0);
        CHECK_EQ_INT(video_buffer(gv)[0], 0xFF);
        CHECK_EQ_INT(video_buffer(gv)[999 + (size_t)749 * video_stride(gv)], 0xFF);
        /* And the centre, so a fit that covered only the far edges would not
           slip through either. */
        CHECK_EQ_INT(video_buffer(gv)[500 + (size_t)375 * video_stride(gv)], 0xFF);

        /* Put the buffer back to all-black before the guard checks below,
           which were written against a black buffer and assert on both the
           corner value and on a later white submit producing a non-empty
           rect. Same size again (100x75), so still no margin clear. */
        fill_solid565(gfb, 100, 75, 200, 0x0000);
        gr = video_submit(gv, gfb, 100, 75,
                          200 * sizeof(uint16_t), KOBOY_PIXFMT_RGB565);
        CHECK(gr.w > 0);
        CHECK_EQ_INT(video_buffer(gv)[0], 0x00);

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

        /* THE MARGIN CLEAR, which needs a fit that does NOT exactly fill.
           Every size change above happened to divide evenly (100x75 -> 10x
           -> exactly 1000x750), so the incoming frame covered the whole rect
           and deleting the clear changed nothing -- confirmed by mutating it
           away and watching the suite stay green.

           90x70 does not divide evenly: 1000/90 = 11 but 750/70 = 10, so the
           height limits it to 10x = 900x700, centred at (50,25) with a real
           margin. Coming from an all-white buffer, a WHITE frame makes the
           margin the only thing that can distinguish "cleared" from "stale":
           without the clear it keeps the previous frame's white. */
        fill_solid565(gfb, 90, 70, 200, 0xFFFF);
        gr = video_submit(gv, gfb, 90, 70,
                          200 * sizeof(uint16_t), KOBOY_PIXFMT_RGB565);
        CHECK(gr.w > 0);
        CHECK_EQ_INT(video_buffer(gv)[500 + (size_t)375 * video_stride(gv)], 0xFF);
        CHECK_EQ_INT(video_buffer(gv)[0], 0x00);                   /* margin, cleared */
        CHECK_EQ_INT(video_buffer(gv)[999 + (size_t)749 * video_stride(gv)], 0x00);

        video_destroy(gv);
    }

    /* ---- video_fit: centring a sub-max frame, and not moving the Game Boy ---- */
    {
    koboy_profile p;
    memset(&p, 0, sizeof p);

    /* THE GAME BOY PATH, and it must not move. 160x144 at scale 5 fills an
       800x720 rect exactly: the fit is p.scale and both offsets are zero, so
       every Game Boy frame lands exactly where it always did. */
    p.max_w = 160; p.max_h = 144; p.scale = 5;
    p.game_w = 800; p.game_h = 720;
    int fs, ox, oy;
    video_fit(&p, 160, 144, &fs, &ox, &oy);
    CHECK_EQ_INT(fs, 5);
    CHECK_EQ_INT(ox, 0);
    CHECK_EQ_INT(oy, 0);

    /* THE BUG THIS EXISTS FOR. A Game & Watch reserved rect at max geometry
       (654x396, scale 1), receiving the in-game view (305x191). The old code
       drew this at p.scale -- 1 -- in the top-left corner, leaving 80% of the
       bezel black; a user reported exactly that as "really small, sits at top
       left corner". 654/305 = 2 and 396/191 = 2, so it fits at 2x (610x382)
       centred with a 22/7 px margin. */
    p.max_w = 654; p.max_h = 396; p.scale = 1;
    p.game_w = 654; p.game_h = 396;
    video_fit(&p, 305, 191, &fs, &ox, &oy);
    CHECK_EQ_INT(fs, 2);
    CHECK_EQ_INT(ox, (654 - 305 * 2) / 2);   /* 22 */
    CHECK_EQ_INT(oy, (396 - 191 * 2) / 2);   /* 7  */
    /* Asserted as real containment, not just as numbers: the fitted content
       must sit wholly inside the rect, or video_pipeline_run writes past the
       buffer it was promised. */
    CHECK(ox >= 0 && oy >= 0);
    CHECK(ox + 305 * fs <= p.game_w);
    CHECK(oy + 191 * fs <= p.game_h);

    /* The limiting axis wins. A frame that is relatively wider than the rect
       must not be scaled by the height's larger factor and overflow. */
    p.max_w = 400; p.max_h = 400; p.scale = 1;
    p.game_w = 400; p.game_h = 400;
    video_fit(&p, 200, 40, &fs, &ox, &oy);
    CHECK_EQ_INT(fs, 2);                      /* 400/200 = 2, not 400/40 = 10 */
    CHECK(ox + 200 * fs <= p.game_w);
    CHECK(oy + 40 * fs <= p.game_h);

    /* LIVE GUARD: a frame at the rect's exact size fits at 1x with no
       margin, and never at 0x. */
    video_fit(&p, 400, 400, &fs, &ox, &oy);
    CHECK_EQ_INT(fs, 1);
    CHECK_EQ_INT(ox, 0);
    CHECK_EQ_INT(oy, 0);
}

})


