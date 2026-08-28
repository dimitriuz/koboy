#include "test.h"
#include "pgm.h"
#include "video.h"
#include "config.h"

/* How many distinct byte values the whole game rect holds. The four-level
   quantiser emits exactly four and the 1-bit ditherer exactly two, so this one
   number separates the two renderings without asserting which pixel is which
   -- which is the point, since the Bayer phase is a screen-position detail
   this test has no business pinning. */
static int distinct_levels(const koboy_video *v)
{
    int seen[256] = {0}, n = 0;
    const uint8_t *buf = video_buffer(v);
    for (int y = 0; y < 720; y++)
        for (int x = 0; x < 800; x++) seen[buf[(size_t)y * video_stride(v) + x]] = 1;
    for (int i = 0; i < 256; i++) n += seen[i];
    return n;
}

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

    koboy_video *v = video_create(&p, false, KOBOY_GRAY_DEFAULT);
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
        koboy_video *dv = video_create(&p, true, KOBOY_GRAY_DEFAULT);
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

    /* THE SAME CLAIM through video_set_dither on a LIVE pipeline -- the path
       the in-game MOTION row takes. The construction argument above proves the
       flag is honoured at video_create; nothing proved it could be flipped
       afterwards, and "only takes effect next launch" makes the row useless.

       Counted BOTH WAYS, four levels -> two -> four, so a setter that latched
       on and never off fails. Each flip is followed by video_invalidate + a
       resubmit of the SAME frame, the discipline main.c's return-from-menu
       path uses: without it the dirty diff suppresses the frame entirely and
       the buffer keeps the old rendering. */
    {
        koboy_video *lv = video_create(&p, false, KOBOY_GRAY_DEFAULT);
        CHECK(lv != NULL);
        CHECK(!video_get_dither(lv));

        fill565(fb, KOBOY_GB_W, KOBOY_GB_H, 0);
        koboy_rect lr = video_submit(lv, fb, KOBOY_GB_W, KOBOY_GB_H,
                                     KOBOY_GB_W * sizeof(uint16_t), KOBOY_PIXFMT_RGB565);
        CHECK(lr.w > 0);
        CHECK_EQ_INT(distinct_levels(lv), 4);

        video_set_dither(lv, true);
        CHECK(video_get_dither(lv));
        video_invalidate(lv);
        lr = video_submit(lv, fb, KOBOY_GB_W, KOBOY_GB_H,
                          KOBOY_GB_W * sizeof(uint16_t), KOBOY_PIXFMT_RGB565);
        CHECK(lr.w > 0);
        CHECK_EQ_INT(distinct_levels(lv), 2);

        video_set_dither(lv, false);
        CHECK(!video_get_dither(lv));
        video_invalidate(lv);
        lr = video_submit(lv, fb, KOBOY_GB_W, KOBOY_GB_H,
                          KOBOY_GB_W * sizeof(uint16_t), KOBOY_PIXFMT_RGB565);
        CHECK(lr.w > 0);
        CHECK_EQ_INT(distinct_levels(lv), 4);

        /* NULL is a no-op, not a crash: both are public. */
        video_set_dither(NULL, true);
        CHECK(!video_get_dither(NULL));

        video_destroy(lv);
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
        /* Pinned explicitly, because this block is about the PIPELINE (buffers
           sized from max, a smaller frame accepted) and not about scale
           policy. Without the pin these numbers move whenever that policy
           does: a non-Game-Boy geometry now auto-fits, so 200x150 chose 6 and
           every hardcoded 1000x750 below failed. Pinning keeps the test
           measuring its own subject. */
        gc.scale_explicit = true;
        /* TEN, not the 5 that was pinned here while the rect came from max.
           Every number in this block is expressed against a 1000x750 rect,
           and holding that rect fixed across the sizing-rule change is what
           keeps the block measuring its own subject (buffers from max,
           smaller frames accepted, the margin clear, the bounds guard)
           instead of turning into a second copy of the rect-sizing test.
           100 * 10 is the same 1000 that 200 * 5 was.

           It is not a free relabelling either: under the OLD max-sized rule
           scale 10 would have asked for 2000x1500, which does not fit a
           1264x1680 panel, so the fitting loop would have demoted it. The
           two rules genuinely disagree here. */
        gc.scale = 10;
        /* base != max on purpose (matches the config-level sweep in
           tests/test_config.c): the buffer is allocated at 200x150 (max),
           the first real frame submitted is the MAXIMUM one and the rect is
           sized for the smaller base -- which is the case the fit has to
           shrink rather than spill, and it is asserted directly below. */
        koboy_profile gp;
        CHECK(config_resolve_profile(&gp, &gc, 1264, 1680, 100, 75, 200, 150));
        CHECK_EQ_INT(gp.scale, 10);
        CHECK_EQ_INT(gp.game_w, 1000);   /* 100 * 10, from base -- not 200 * 10 */
        CHECK_EQ_INT(gp.game_h, 750);

        koboy_video *gv = video_create(&gp, false, KOBOY_GRAY_DEFAULT);
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
        /* Far corner is now BLACK too -- the regression test for a bug a real
           user hit on a real device.

           This used to assert 0xFF, on the reasoning that a smaller frame
           "leaves alone" the area a bigger one covered. It was a DEFECT: a
           Game & Watch title alternates between the whole unit (654x396) and
           the LCD alone (305x191) several times a second, and under the old
           rule the smaller view drew at 1:1 in the corner of the rect the
           bigger one sized -- a postage stamp in an empty bezel.

           video_fit now scales the smaller frame UP to fill the rect: 100x75
           into 1000x750 is 10x at zero offset, so every pixel including this
           corner is the new frame's black. The first frame (200x150, the max)
           still fits at exactly p.scale, the unchanged Game Boy path. */
        CHECK_EQ_INT(video_buffer(gv)[999 + (size_t)749 * video_stride(gv)], 0x00);

        /* THE ASSERTION THAT ACTUALLY DISCRIMINATES; the one above is kept only
           as documentation of the old behaviour's shape.

           The check above CANNOT FAIL on its own: a size change clears the
           margin before scaling, so with the fit reverted to the old
           corner-parked p.scale the rect is memset to 0 and a solid BLACK
           frame is drawn into its corner -- 0x00 either way. Confirmed by
           running that mutant.

           A same-size frame in a DIFFERENT colour separates them: no margin
           clear fires (100x75 both times), so the far corner can only become
           white if the frame scaled up to reach it. Fitted, 100x75 becomes
           1000x750; corner-parked it is 500x375 and this corner keeps the
           previous black. */
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

           Two frames, and BOTH are needed. First a full-rect BLACK one, which
           is not a size change and so does not clear: it just guarantees the
           margin is dark before the interesting frame arrives, because the
           clear now writes the LIGHTEST of the four levels and a clear-to-
           white is invisible against a buffer that was already white. */
        fill_solid565(gfb, 200, 150, 200, 0x0000);
        gr = video_submit(gv, gfb, 200, 150,
                          200 * sizeof(uint16_t), KOBOY_PIXFMT_RGB565);
        CHECK(gr.w > 0);
        CHECK_EQ_INT(video_buffer(gv)[0], 0x00);

        /* Then 90x70, which does not divide evenly: 1000/90 = 11 but
           750/70 = 10, so the height limits it to 10x = 900x700, centred at
           (50,25) with a real margin. The margin must come out WHITE against
           the black it is replacing -- delete the clear and it stays 0x00. */
        fill_solid565(gfb, 90, 70, 200, 0xFFFF);
        gr = video_submit(gv, gfb, 90, 70,
                          200 * sizeof(uint16_t), KOBOY_PIXFMT_RGB565);
        CHECK(gr.w > 0);
        CHECK_EQ_INT(video_buffer(gv)[500 + (size_t)375 * video_stride(gv)], 0xFF);
        CHECK_EQ_INT(video_buffer(gv)[0], 0xFF);                   /* margin, cleared */
        CHECK_EQ_INT(video_buffer(gv)[999 + (size_t)749 * video_stride(gv)], 0xFF);

        /* And once more with a BLACK frame at a new size, which is what pins
           the clear to the light level rather than to "whatever the frame
           happens to be": 80x60 fits at 12x = 960x720 centred at (20,15), so
           the margin and the picture must now differ. A clear that wrote 0,
           or none at all, leaves them identical. */
        fill_solid565(gfb, 80, 60, 200, 0x0000);
        gr = video_submit(gv, gfb, 80, 60,
                          200 * sizeof(uint16_t), KOBOY_PIXFMT_RGB565);
        CHECK(gr.w > 0);
        CHECK_EQ_INT(video_buffer(gv)[500 + (size_t)375 * video_stride(gv)], 0x00);
        CHECK_EQ_INT(video_buffer(gv)[0], 0xFF);
        CHECK_EQ_INT(video_buffer(gv)[999 + (size_t)749 * video_stride(gv)], 0xFF);

        video_destroy(gv);

        /* THE SAME MARGIN UNDER force_dither: lightest level, and it stays
           there.

           THIS CHECK'S TEETH MOVED. It was written when the ditherer
           thresholded against the raw Bayer matrix, where `255 > 255` is false
           and one cell per 16x16 tile of the cleared margin came out black --
           a static speckled band around every frame. Fixed at the source
           (video.c's g_thresh), so this no longer distinguishes "the margin is
           outside the dithered region" from "inside it and dithers to white
           anyway"; keeping the margin out of the pass is now a COST argument.

           It stays because what it asserts can still fail: the margin must be
           the lightest level and must not acquire content, so a size-change
           clear writing the wrong value or a fit at the wrong offset shows up
           here. Over the whole top band rather than one probe, because a
           pattern needs more than one pixel to see. */
        koboy_video *dv2 = video_create(&gp, true, KOBOY_GRAY_DEFAULT);
        CHECK(dv2 != NULL);
        fill_solid565(gfb, 90, 70, 200, 0xFFFF);
        koboy_rect dr2 = video_submit(dv2, gfb, 90, 70,
                                      200 * sizeof(uint16_t), KOBOY_PIXFMT_RGB565);
        CHECK(dr2.w > 0);
        int margin_dark = 0;
        for (int y = 0; y < 25; y++)                    /* rows above the fit */
            for (int x = 0; x < 1000; x++)
                if (video_buffer(dv2)[x + (size_t)y * video_stride(dv2)] != 0xFF)
                    margin_dark++;
        CHECK_EQ_INT(margin_dark, 0);
        video_destroy(dv2);
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

    /* ------------------------------- video_fit_frac: the LCD aspect fit ---- */
    {
        int dw, dh;

        /* THE BINDING AXIS IS EXACT. This is the property the whole LCD
           layout rests on -- config_resolve_profile sizes the reserved rect
           with this function and video_fit_rect then fits frames INTO that
           result, so an approximate answer compounds into stray pixels of
           margin down one side. Mickey Mouse's measured 654x396 into the
           1264x1560 a 1264x1680 panel leaves above the bottom strip is
           WIDTH-bound: 1264 exactly, and 396*1264/654 = 765 rows. */
        dw = dh = -1;
        video_fit_frac(654, 396, 1264, 1560, &dw, &dh);
        CHECK_EQ_INT(dw, 1264);
        CHECK_EQ_INT(dh, 765);

        /* Donkey Kong's measured 606x748 is the case that proves BOTH axes
           are fitted. Width alone would give 606 -> 1264 and 748 -> 1560.2,
           i.e. 1560 rows -- which is exactly the height available, so this
           title lands on the boundary and comes out HEIGHT-bound by a
           hair. A fit that only ever looked at width would overflow the
           strip for any title even slightly taller. */
        dw = dh = -1;
        video_fit_frac(606, 748, 1264, 1560, &dw, &dh);
        CHECK_EQ_INT(dh, 1560);
        CHECK(dw <= 1264);
        CHECK_EQ_INT(dw, 606 * 1560 / 748);

        /* A deliberately tall source: height must bind and the result must
           NOT exceed the available width. */
        dw = dh = -1;
        video_fit_frac(100, 1000, 1264, 500, &dw, &dh);
        CHECK_EQ_INT(dh, 500);
        CHECK_EQ_INT(dw, 50);

        /* ASPECT, asserted as a relation rather than as two numbers, so a
           fit that happened to produce the right width for the wrong reason
           still fails. The error is at most one pixel on the non-binding
           axis (one truncated division), which is what this bounds. */
        static const struct { int sw, sh, aw, ah; } cases[] = {
            { 654, 396, 1264, 1560 },
            { 606, 748, 1264, 1560 },
            { 973, 532, 1264, 1560 },
            { 305, 191, 1264,  765 },
            { 128, 128, 1264, 1560 },
        };
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            int w = -1, h = -1;
            video_fit_frac(cases[i].sw, cases[i].sh, cases[i].aw, cases[i].ah, &w, &h);
            CHECK(w >= 1 && h >= 1);
            CHECK(w <= cases[i].aw);
            CHECK(h <= cases[i].ah);
            /* |w*sh - h*sw| <= max(sw, sh): one pixel of truncation on the
               non-binding axis, scaled by the other side of the cross
               product. */
            long lhs = (long)w * cases[i].sh - (long)h * cases[i].sw;
            long tol = cases[i].sw > cases[i].sh ? cases[i].sw : cases[i].sh;
            if (lhs > tol || lhs < -tol)
                fprintf(stderr, "  %dx%d into %dx%d -> %dx%d: aspect error %ld > %ld\n",
                        cases[i].sw, cases[i].sh, cases[i].aw, cases[i].ah, w, h, lhs, tol);
            CHECK(lhs <= tol && lhs >= -tol);
            /* And the binding axis is filled EXACTLY -- one of the two, never
               neither. */
            CHECK(w == cases[i].aw || h == cases[i].ah);
        }

        /* LIVE GUARD: a degenerate extent writes nothing at all, so a caller
           that ignored the guard sees its own initialisation rather than a
           divide-by-zero or a garbage rect. */
        dw = dh = -7;
        video_fit_frac(0, 396, 1264, 1560, &dw, &dh);
        video_fit_frac(654, 0, 1264, 1560, &dw, &dh);
        video_fit_frac(654, 396, 0, 1560, &dw, &dh);
        video_fit_frac(654, 396, 1264, 0, &dw, &dh);
        CHECK_EQ_INT(dw, -7);
        CHECK_EQ_INT(dh, -7);
    }

    /* ------------------------- video_fit_rect: both layouts, one function -- */
    {
        koboy_profile p;
        memset(&p, 0, sizeof p);
        int dw, dh, ox, oy;

        /* DMG, at max: 160x144 at scale 5 is 800x720 with no offset. Spelled
           out here as well as in the video_fit block above because
           video_pipeline_run now calls THIS function, not video_fit, and a
           DMG branch that lost the integer scale would still pass every
           video_fit assertion in this file. */
        p.layout_mode = KOBOY_LAYOUT_DMG;
        p.max_w = 160; p.max_h = 144; p.scale = 5;
        p.game_w = 800; p.game_h = 720;
        video_fit_rect(&p, 160, 144, KOBOY_ASPECT_ONE, &dw, &dh, &ox, &oy);
        CHECK_EQ_INT(dw, 800);
        CHECK_EQ_INT(dh, 720);
        CHECK_EQ_INT(ox, 0);
        CHECK_EQ_INT(oy, 0);
        /* Below max, still an INTEGER multiple -- the DMG branch must not
           quietly acquire the fractional fit. 90x70 is chosen because the two
           fits DISAGREE there: integer takes min(800/90, 720/70) = 8, giving
           720x560, while a fractional fit would be width-bound at 800x622.
           A size where they agree (any exact divisor) would pass either way. */
        video_fit_rect(&p, 90, 70, KOBOY_ASPECT_ONE, &dw, &dh, &ox, &oy);
        CHECK_EQ_INT(dw, 720);            /* 90 * 8, not 800 */
        CHECK_EQ_INT(dh, 560);            /* 70 * 8, not 622 */
        CHECK_EQ_INT(ox, (800 - 720) / 2);
        CHECK_EQ_INT(oy, (720 - 560) / 2);

        /* LCD, at max: fills the reserved rect EXACTLY and sits at (0,0).
           Re-deriving the ratio from an already-floored game_w/game_h can
           only lose, so the invariant is stated outright in video_fit_rect --
           and pinned here, because a one-pixel shortfall is invisible by eye
           and shifts every pointer coordinate. */
        p.layout_mode = KOBOY_LAYOUT_LCD;
        p.rect_from_max = true;           /* a Game & Watch rect: see koboy.h */
        p.max_w = 654; p.max_h = 396; p.scale = 1;
        p.game_w = 1264; p.game_h = 765;
        video_fit_rect(&p, 654, 396, KOBOY_ASPECT_ONE, &dw, &dh, &ox, &oy);
        CHECK_EQ_INT(dw, 1264);
        CHECK_EQ_INT(dh, 765);
        CHECK_EQ_INT(ox, 0);
        CHECK_EQ_INT(oy, 0);

        /* LCD, below max: the zoomed LCD-only view a Game & Watch title
           alternates to. Fractional and centred, and -- unlike the DMG
           branch -- NOT rounded down to an integer multiple: 305x191 at
           integer scale would be 4x = 1220x764, and the fractional fit is
           height-bound at 765 rows. */
        video_fit_rect(&p, 305, 191, KOBOY_ASPECT_ONE, &dw, &dh, &ox, &oy);
        CHECK_EQ_INT(dh, 765);
        CHECK_EQ_INT(dw, 305 * 765 / 191);
        CHECK(dw > 1220);                 /* strictly better than integer 4x */
        CHECK_EQ_INT(ox, (1264 - dw) / 2);
        CHECK_EQ_INT(oy, 0);
        /* Containment, the property video_pipeline_run's writes depend on. */
        CHECK(ox >= 0 && oy >= 0);
        CHECK(ox + dw <= p.game_w);
        CHECK(oy + dh <= p.game_h);
    }

    /* ---- CONTAINMENT: every frame in [1, max] fits the base-sized rect ---- */
    /* THE SAFETY ARGUMENT FOR THE WHOLE RECT-SIZING CHANGE, SWEPT rather than
       argued.

       The DMG rect used to be max times an integer, so "any frame the core may
       send fits the rect it is drawn into" held by construction. The rect now
       comes from BASE geometry, which buys a SNES 4x its picture area and
       gives that up: a frame between base and max is bigger than the rect, and
       video_fit_par's scale floor of 1 means the integer fit cannot shrink it.
       video_fit_rect falls back to the fractional fit for exactly those
       frames, and without that video_pipeline_run's scaler writes past the end
       of v->cur -- SILENT MEMORY CORRUPTION, not a wrong-looking picture.

       Swept over every system at its MEASURED base/max/display aspect, on all
       four supported panels, against a grid of frame sizes covering the whole
       legal range including both corners. The pixel aspect is re-derived per
       FRAME, as video_pipeline_run does.

       THE ROW THAT ACTUALLY FIRES THE FALLBACK is FBNeo's Tapper: base 512x480
       at 5:4 pixels is a 640x480 rect at scale 1 on the Libra 2, and FBNeo
       declares a SQUARE max of 512x512 -- 32 rows taller than the rect. A
       shipped board, not a hypothetical, asserted by name below the sweep so a
       future geometry change cannot quietly make the sweep vacuous. */
    {
        static const struct {
            const char *name; int bw, bh, mw, mh; double dar;
        } sys[] = {
            { "Game Boy",      160, 144, 160, 144, 1.11111 },
            { "NES",           256, 240, 256, 240, 1.21905 },
            { "Pokemon Mini",  384, 256, 384, 256, 1.5     },
            { "WonderSwan",    224, 144, 224, 224, 1.55556 },
            { "WSwan portrait",144, 224, 224, 224, 1.55556 },
            { "Neo Geo Pocket",160, 152, 160, 152, 1.05    },
            { "Atari NTSC",    320, 210, 320, 256, 1.33333 },
            { "Atari PAL",     320, 250, 320, 256, 1.33333 },
            { "ColecoVision",  256, 192, 512, 288, 0.0     },
            { "Intellivision", 352, 224, 352, 224, 1.57143 },
            { "Master System", 256, 192, 284, 240, 1.52381 },
            { "Game Gear",     160, 144, 284, 240, 1.33333 },
            { "Mega Drive",    320, 224, 348, 240, 1.30612 },
            { "SNES",          256, 224, 512, 512, 1.33333 },
            { "SNES hi-res",   512, 448, 512, 512, 1.33333 },
            { "PC Engine 256", 256, 243, 512, 243, 1.2     },
            { "PC Engine 352", 352, 243, 512, 243, 1.2     },
            { "Galaga (rot 3)",224, 288, 288, 288, 0.75    },
            { "Defender",      292, 240, 292, 292, 1.33333 },
            { "Tapper",        512, 480, 512, 512, 1.33333 },
        };
        static const struct { int w, h; } panels[] = {
            { 1072, 1448 }, { 1264, 1680 }, { 1404, 1872 }, { 1440, 1920 }
        };
        int spills = 0, shrunk = 0;
        const char *first_spill = NULL;
        for (size_t i = 0; i < sizeof sys / sizeof sys[0]; i++) {
            /* dar == 0 is libretro's "no answer" and falls back to
               base_w/base_h, exactly as core_display_aspect does. */
            uint32_t dar = sys[i].dar > 0.0
                         ? (uint32_t)(sys[i].dar * 65536.0 + 0.5)
                         : (uint32_t)(((uint64_t)sys[i].bw << 16) / (uint64_t)sys[i].bh);
            for (size_t q = 0; q < sizeof panels / sizeof panels[0]; q++) {
                koboy_config sc; config_defaults(&sc);
                koboy_profile sp;
                CHECK(config_resolve_profile_par(&sp, &sc, panels[q].w, panels[q].h,
                                                 sys[i].bw, sys[i].bh,
                                                 sys[i].mw, sys[i].mh,
                                                 video_pixel_aspect(dar, sys[i].bw, sys[i].bh)));
                /* Both corners of the legal range plus a grid through it.
                   1x1 is what a core sends on a bad frame and the fit still
                   has to land inside the rect. */
                for (int fh = 1; fh <= sys[i].mh; fh += 17)
                for (int fw = 1; fw <= sys[i].mw; fw += 19) {
                    int w = fw, h = fh;
                    if (fw + 19 > sys[i].mw) w = sys[i].mw;
                    if (fh + 17 > sys[i].mh) h = sys[i].mh;
                    int dw, dh, ox, oy;
                    video_fit_rect(&sp, w, h, video_pixel_aspect(dar, w, h),
                                   &dw, &dh, &ox, &oy);
                    if (dw < 1 || dh < 1 || ox < 0 || oy < 0 ||
                        ox + dw > sp.game_w || oy + dh > sp.game_h) {
                        if (!spills) first_spill = sys[i].name;
                        spills++;
                    }
                    /* Did this frame have to be SHRUNK -- i.e. did the
                       fallback do any work? Counted so the sweep cannot pass
                       by never reaching the branch it exists to guard. */
                    if (h > sp.game_h) shrunk++;
                }
            }
        }
        if (spills) fprintf(stderr, "  first spill: %s\n", first_spill);
        CHECK_EQ_INT(spills, 0);
        CHECK(shrunk > 0);

        /* Tapper by name, with the arithmetic written out, because "shrunk >
           0" above is satisfied by any row and this is the one that carries
           the property. 512x512 is square-pixel at this frame size (4:3
           display aspect over a 512x512 frame), so it wants 683x512 shown; the
           rect is 640x480, the width binds, and the fractional fit gives
           640x479 rather than the 683x512 the integer path's 1x floor would
           have written -- 43 columns and 32 rows past the buffer. */
        {
            koboy_config tc; config_defaults(&tc);
            koboy_profile tp;
            uint32_t dar = (uint32_t)(1.33333 * 65536.0 + 0.5);
            CHECK(config_resolve_profile_par(&tp, &tc, 1264, 1680, 512, 480,
                                             512, 512,
                                             video_pixel_aspect(dar, 512, 480)));
            CHECK_EQ_INT(tp.game_w, 640);
            CHECK_EQ_INT(tp.game_h, 480);
            int dw, dh, ox, oy;
            video_fit_rect(&tp, 512, 512, video_pixel_aspect(dar, 512, 512),
                           &dw, &dh, &ox, &oy);
            CHECK_EQ_INT(dw, 640);
            CHECK(dh <= 480);
            CHECK(dh >= 470);            /* aspect kept, not squashed to nothing */
            CHECK(ox + dw <= tp.game_w);
            CHECK(oy + dh <= tp.game_h);
        }
    }

    /* ------------- the LCD layout end to end through video_submit ---------- */
    {
        /* A real .mgw geometry (Mickey Mouse, 654x396) resolved by the real
           resolver, so this exercises the same rect the device would get.
           Nothing else in this file runs a frame through the FRACTIONAL
           scaler; the pipeline could pick the integer path for the LCD
           layout and every assertion above would still pass. */
        koboy_config lc; config_defaults(&lc);
        lc.layout_mode = KOBOY_LAYOUT_LCD;
        lc.lcd_rect_from_max = true;
        koboy_profile lp;
        CHECK(config_resolve_profile(&lp, &lc, 1264, 1680, 654, 396, 654, 396));
        CHECK_EQ_INT(lp.game_w, 1264);
        CHECK_EQ_INT(lp.game_h, 765);

        koboy_video *lv = video_create(&lp, false, KOBOY_GRAY_DEFAULT);
        CHECK(lv != NULL);

        static uint16_t lfb[654 * 396];
        fill_solid565(lfb, 654, 396, 654, 0xFFFF);
        koboy_rect lr = video_submit(lv, lfb, 654, 396,
                                     654 * sizeof(uint16_t), KOBOY_PIXFMT_RGB565);
        /* The WHOLE reserved rect is dirty and white: a frame at max fills
           it. An integer-scale fit would have covered 654x396 of a 1264x765
           rect -- a quarter of it -- and left the far corner black. */
        CHECK_EQ_INT(lr.w, 1264);
        CHECK_EQ_INT(lr.h, 765);
        CHECK_EQ_INT(video_buffer(lv)[0], 0xFF);
        CHECK_EQ_INT(video_buffer(lv)[1263 + (size_t)764 * video_stride(lv)], 0xFF);
        CHECK_EQ_INT(video_buffer(lv)[700 + (size_t)400 * video_stride(lv)], 0xFF);

        /* video_frame_rect reports where that frame landed -- the input the
           LCD layout's touch-to-pointer normalisation is built on. */
        koboy_rect fr;
        memset(&fr, 0, sizeof fr);
        video_frame_rect(lv, &fr);
        CHECK_EQ_INT(fr.x, 0);
        CHECK_EQ_INT(fr.y, 0);
        CHECK_EQ_INT(fr.w, 1264);
        CHECK_EQ_INT(fr.h, 765);

        /* The zoomed view: smaller than max, so it is fitted and centred --
           and video_frame_rect must follow it, or every touch would be
           normalised against a rect the artwork no longer fills. */
        static uint16_t zfb[305 * 191];
        fill_solid565(zfb, 305, 191, 305, 0x0000);
        lr = video_submit(lv, zfb, 305, 191,
                          305 * sizeof(uint16_t), KOBOY_PIXFMT_RGB565);
        CHECK(lr.w > 0);
        video_frame_rect(lv, &fr);
        CHECK_EQ_INT(fr.h, 765);
        CHECK_EQ_INT(fr.w, 305 * 765 / 191);
        CHECK_EQ_INT(fr.x, (1264 - fr.w) / 2);
        CHECK_EQ_INT(fr.y, 0);
        /* Drawn where it says it is: black inside the reported rect, and the
           margin outside it cleared to the LIGHTEST of the four levels (the
           size changed, so the clear fires). White, not black, and that is
           the point of asserting both probes: a WonderSwan reserves a square
           rect for a landscape frame and lives with that margin for a whole
           session, so it has to read as blank paper rather than as a solid
           black band on a reflective panel. */
        CHECK_EQ_INT(video_buffer(lv)[fr.x + 4 + (size_t)400 * video_stride(lv)], 0x00);
        CHECK_EQ_INT(video_buffer(lv)[0], 0xFF);   /* margin, cleared to white */

        video_destroy(lv);
    }

    /* ---- the greyscale mapping, through the real pipeline ---------------- */
    {
        koboy_config gc; config_defaults(&gc);
        koboy_profile gp2;
        CHECK(config_resolve_profile(&gp2, &gc, 1264, 1680,
                                     KOBOY_GB_W, KOBOY_GB_H, KOBOY_GB_W, KOBOY_GB_H));

        /* A colour picture, not the DMG greys: every mapping is the identity
           on neutral grey, so a grey test frame could not tell two mappings
           apart and would assert nothing. These four are the measured
           problem colours -- Sonic's sky and body, Castlevania's sky, Kirby's
           wall -- so the frame exercises exactly the range that moves. */
        static const uint32_t colours[4] = {
            0x009AFFu,   /* rgb(0,154,255)  Sonic Pocket Adventure sky */
            0x0055FFu,   /* rgb(0,85,255)   Sonic himself */
            0x00248Cu,   /* rgb(0,36,140)   Castlevania sky */
            0x631400u    /* rgb(99,20,0)    Kirby's Adventure brick */
        };
        static uint32_t fb32[KOBOY_GB_W * KOBOY_GB_H];
        static uint16_t fb16[KOBOY_GB_W * KOBOY_GB_H];
        for (int y = 0; y < KOBOY_GB_H; y++)
            for (int x = 0; x < KOBOY_GB_W; x++) {
                uint32_t c = colours[((x / 8) + (y / 8)) % 4];
                fb32[y * KOBOY_GB_W + x] = c;
                /* The same colour as RGB565. Chosen so the 5/6-bit truncation
                   round-trips exactly (each channel is already representable),
                   which is what makes the two submissions below comparable at
                   all rather than differing by quantisation. */
                fb16[y * KOBOY_GB_W + x] =
                    (uint16_t)(((c >> 19) & 0x1Fu) << 11 |
                               ((c >> 10) & 0x3Fu) << 5  |
                               ((c >>  3) & 0x1Fu));
            }

        /* A CORE'S CHOICE OF PIXEL FORMAT MUST NOT CHANGE WHAT IS DRAWN.
           The RGB565 path reads the LUT; the XRGB8888 path calls
           video_xrgb8888_to_gray per pixel and never touches it. Asserted
           under a NON-DEFAULT map, because that is what distinguishes "the
           XRGB path uses v->map" from "the XRGB path hardcodes something that
           happens to be the default". */
        for (int m = 0; m < KOBOY_GRAY_COUNT; m++) {
            koboy_video *a = video_create(&gp2, false, (koboy_gray_map)m);
            koboy_video *b = video_create(&gp2, false, (koboy_gray_map)m);
            CHECK(a && b);
            video_submit(a, fb16, KOBOY_GB_W, KOBOY_GB_H,
                         KOBOY_GB_W * sizeof(uint16_t), KOBOY_PIXFMT_RGB565);
            video_submit(b, fb32, KOBOY_GB_W, KOBOY_GB_H,
                         KOBOY_GB_W * sizeof(uint32_t), KOBOY_PIXFMT_XRGB8888);
            CHECK_EQ_INT(video_stride(a), video_stride(b));
            CHECK_EQ_INT(memcmp(video_buffer(a), video_buffer(b),
                                (size_t)video_stride(a) * (size_t)gp2.game_h), 0);
            video_destroy(a); video_destroy(b);
        }

        /* video_set_gray_map has to actually change the pixels. Same frame,
           same koboy_video, different map: the buffer must differ afterwards,
           and match what a video CREATED with that map produces. The second
           half is what rules out "it changed to something", which a
           corrupted LUT would also satisfy. */
        {
            koboy_video *v2 = video_create(&gp2, false, KOBOY_GRAY_LUMA);
            koboy_video *ref = video_create(&gp2, false, KOBOY_GRAY_VALUE);
            CHECK(v2 && ref);
            size_t n = (size_t)video_stride(v2) * (size_t)gp2.game_h;
            video_submit(v2, fb16, KOBOY_GB_W, KOBOY_GB_H,
                         KOBOY_GB_W * sizeof(uint16_t), KOBOY_PIXFMT_RGB565);
            static uint8_t before[1264 * 1680];
            CHECK(n <= sizeof before);
            memcpy(before, video_buffer(v2), n);

            video_set_gray_map(v2, KOBOY_GRAY_VALUE);
            CHECK_EQ_INT((int)video_get_gray_map(v2), (int)KOBOY_GRAY_VALUE);
            /* The diff is against `prev`, which still holds the OLD mapping's
               pixels -- so this resubmission is exactly the half-old-half-new
               hazard video_set_gray_map's comment warns about, and the reason
               main.c pairs it with video_invalidate. Invalidating here makes
               the comparison total rather than only over the dirty rect. */
            video_invalidate(v2);
            video_submit(v2, fb16, KOBOY_GB_W, KOBOY_GB_H,
                         KOBOY_GB_W * sizeof(uint16_t), KOBOY_PIXFMT_RGB565);
            CHECK(memcmp(before, video_buffer(v2), n) != 0);

            video_submit(ref, fb16, KOBOY_GB_W, KOBOY_GB_H,
                         KOBOY_GB_W * sizeof(uint16_t), KOBOY_PIXFMT_RGB565);
            CHECK_EQ_INT(memcmp(video_buffer(ref), video_buffer(v2), n), 0);

            video_destroy(v2); video_destroy(ref);
        }
    }

    /* ------------------------------------------------------------ rotation
       An arcade board turned its MONITOR on its side: FBNeo renders Galaga
       into a 288x224 LANDSCAPE buffer and asks, through SET_ROTATION, for a
       quarter turn. This checks the turn itself on a source small enough to
       write out by hand, then the two properties a plausible-looking wrong
       implementation would still satisfy.

       The source is 4x2 with a DISTINCT VALUE PER PIXEL, so all eight trace to
       exactly one destination. Values are already-quantised greys, and the
       profile is built at scale 1 so the destination is the rotated source
       pixel for pixel. */
    {
        /* RGB565 greys the four-level quantiser maps to 0/85/170/255. Chosen
           by asking video_rgb565_to_gray rather than by assuming, because a
           wrong constant here would make every assertion below trivially
           true-or-false for the wrong reason. */
        const uint16_t GREY[4] = { 0x0000, 0x52AA, 0xA555, 0xFFFF };
        uint8_t lvl[4];
        for (int i = 0; i < 4; i++) {
            uint8_t g = video_rgb565_to_gray(GREY[i], KOBOY_GRAY_DEFAULT);
            lvl[i] = g < 43 ? 0 : g < 128 ? 85 : g < 213 ? 170 : 255;
        }
        /* Eight source pixels drawn from four levels, in a layout chosen for
           one property that is easy to get wrong and was: it must NOT be
           symmetric under a half turn. Written out:
             src (4x2):   0 1 2 3
                          0 3 1 2
           rot 3 is rot 1 turned 180 degrees, so a half-turn-symmetric pattern
           renders IDENTICALLY at both and the "the two directions differ"
           check below passes against an implementation that cannot tell them
           apart. The first pattern tried here was { 0 1 2 3 / 3 2 1 0 }, which
           is exactly that symmetry, and the check duly failed against correct
           code -- the pattern was the bug, not the pipeline. */
        static const int SRC[2][4] = { { 0, 1, 2, 3 }, { 0, 3, 1, 2 } };
        uint16_t src[2 * 4];
        for (int y = 0; y < 2; y++)
            for (int x = 0; x < 4; x++) src[y * 4 + x] = GREY[SRC[y][x]];

        /* A profile whose max is the SQUARE 4x4 -- what FBNeo really reports
           (side = max(w,h)), so both orientations fit one buffer -- on a panel
           big enough that the scale search cannot demote below 1. Scale is
           pinned to 1 so the destination is the rotated frame with nothing
           added.

           BASE IS PER-ROTATION, and that is not a convenience: core_get_geometry
           TRANSPOSES base and max for an odd rotation so every consumer sees
           the picture AS PRESENTED, and main.c re-resolves when the rotation
           changes. So a rot-1 board really does reach config_resolve_profile as
           2x4, and resolving ONE profile for all four rotations models a call
           sequence koboy cannot produce. Unnoticed while the rect came from the
           square max, which is the same either way. */
        koboy_config rc; config_defaults(&rc);
        rc.scale = 1; rc.scale_explicit = true;
        /* The two shapes the four rotations resolve to, hoisted because the
           blocks below the loop need one each: rp_land for the un-turned
           4x2 and rp_port for the quarter-turned 2x4. */
        koboy_profile rp_land, rp_port;
        CHECK(config_resolve_profile(&rp_land, &rc, 1264, 1680, 4, 2, 4, 4));
        CHECK(config_resolve_profile(&rp_port, &rc, 1264, 1680, 2, 4, 4, 4));

        /* Expected destinations, derived from libretro's own definition (the
           value is 90-degree COUNTER-CLOCKWISE steps) and NOT from the
           implementation:
             rot 0 -> 4x2, unchanged
             rot 1 -> 2x4, out[y][x] = src[x][3-y]
             rot 2 -> 4x2, out[y][x] = src[1-y][3-x]
             rot 3 -> 2x4, out[y][x] = src[1-x][y]            */
        for (int rot = 0; rot < 4; rot++) {
            const koboy_profile *rp = (rot & 1) ? &rp_port : &rp_land;
            CHECK_EQ_INT(rp->scale, 1);
            koboy_video *rv = video_create(rp, false, KOBOY_GRAY_DEFAULT);
            CHECK(rv != NULL);
            video_set_rotation(rv, rot);
            CHECK_EQ_INT(video_get_rotation(rv), rot);

            koboy_rect rr = video_submit(rv, src, 4, 2,
                                         4 * sizeof(uint16_t), KOBOY_PIXFMT_RGB565);
            CHECK(rr.w > 0);          /* the frame must not have been dropped */

            koboy_rect fit; video_frame_rect(rv, &fit);
            const int ew = (rot & 1) ? 2 : 4;
            const int eh = (rot & 1) ? 4 : 2;
            CHECK_EQ_INT(fit.w, ew);
            CHECK_EQ_INT(fit.h, eh);

            const uint8_t *out = video_buffer(rv);
            const int st = video_stride(rv);
            for (int y = 0; y < eh; y++)
                for (int x = 0; x < ew; x++) {
                    int want;
                    switch (rot) {
                    case 0:  want = SRC[y][x];            break;
                    case 1:  want = SRC[x][3 - y];        break;
                    case 2:  want = SRC[1 - y][3 - x];    break;
                    default: want = SRC[1 - x][y];        break;
                    }
                    CHECK_EQ_INT(out[(size_t)(fit.y + y) * st + fit.x + x],
                                 lvl[want]);
                }
            video_destroy(rv);
        }

        /* THE BOUND IS CHECKED AGAINST THE ROTATED SIZE, and this is the case
           that separates a working rotation from one that silently presents
           nothing. The profile's max is what core_get_geometry reported --
           ALREADY transposed -- so for a quarter-turned board it is 2x4 while
           the core's own frame is 4x2. A guard comparing the core's w/h
           against that max rejects 4 > 2 and drops EVERY FRAME: a black game
           rect, which looks like a broken core rather than a broken
           front-end.

           Built at max 2x4 (not the square above) precisely so the two
           comparisons disagree. */
        {
            koboy_config tc; config_defaults(&tc);
            tc.scale = 1; tc.scale_explicit = true;
            koboy_profile tp;
            CHECK(config_resolve_profile(&tp, &tc, 1264, 1680, 2, 4, 2, 4));
            koboy_video *tv = video_create(&tp, false, KOBOY_GRAY_DEFAULT);
            CHECK(tv != NULL);
            video_set_rotation(tv, 3);
            koboy_rect tr = video_submit(tv, src, 4, 2,
                                         4 * sizeof(uint16_t), KOBOY_PIXFMT_RGB565);
            CHECK(tr.w > 0);
            /* And the mirror image: at rot 0 the SAME frame does not fit that
               same profile and must be refused, so the guard is still a guard
               rather than having been deleted. */
            video_invalidate(tv);
            video_set_rotation(tv, 0);
            koboy_rect tr0 = video_submit(tv, src, 4, 2,
                                          4 * sizeof(uint16_t), KOBOY_PIXFMT_RGB565);
            CHECK_EQ_INT(tr0.w, 0);
            video_destroy(tv);
        }

        /* Rotation is not a no-op that happens to produce the right size: a
           rot-1 and a rot-3 render of the same frame must DIFFER. An
           implementation that transposed without reflecting -- the easiest
           thing to get wrong, and invisible on a symmetric test pattern --
           produces identical output for both. Both sides get the PORTRAIT
           profile, which is what a quarter-turned board resolves to. */
        {
            koboy_video *a = video_create(&rp_port, false, KOBOY_GRAY_DEFAULT);
            koboy_video *b = video_create(&rp_port, false, KOBOY_GRAY_DEFAULT);
            CHECK(a && b);
            video_set_rotation(a, 1);
            video_set_rotation(b, 3);
            video_submit(a, src, 4, 2, 4 * sizeof(uint16_t), KOBOY_PIXFMT_RGB565);
            video_submit(b, src, 4, 2, 4 * sizeof(uint16_t), KOBOY_PIXFMT_RGB565);
            size_t n = (size_t)video_stride(a) * (size_t)rp_port.game_h;
            CHECK(memcmp(video_buffer(a), video_buffer(b), n) != 0);
            video_destroy(a); video_destroy(b);
        }

        /* And rot 0 leaves the pipeline BYTE FOR BYTE what it was before
           rotation existed -- the property every other core koboy ships
           depends on. Compared against a koboy_video that was never told
           about rotation at all, not against a second one set to 0, so a
           video_set_rotation that corrupted state on the way to 0 is caught
           too. Landscape profile: rot 0 is the un-turned case. */
        {
            koboy_video *a = video_create(&rp_land, false, KOBOY_GRAY_DEFAULT);
            koboy_video *b = video_create(&rp_land, false, KOBOY_GRAY_DEFAULT);
            CHECK(a && b);
            video_set_rotation(a, 0);
            video_submit(a, src, 4, 2, 4 * sizeof(uint16_t), KOBOY_PIXFMT_RGB565);
            video_submit(b, src, 4, 2, 4 * sizeof(uint16_t), KOBOY_PIXFMT_RGB565);
            size_t n = (size_t)video_stride(a) * (size_t)rp_land.game_h;
            CHECK_EQ_INT(memcmp(video_buffer(a), video_buffer(b), n), 0);
            CHECK_EQ_INT(video_get_rotation(b), 0);   /* the default */
            video_destroy(a); video_destroy(b);
        }
    }
})


