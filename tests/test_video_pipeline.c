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

        /* THE SAME MARGIN, UNDER force_dither. The dither path is 1-bit and
           thresholds against a Bayer matrix whose top cell is 255, so
           `255 > 255` is false and a dither pass run over the CLEARED margin
           speckles it black -- a permanent, static, half-black band around
           every frame on a panel whose whole point is a clean page. The
           margin therefore has to stay outside the dithered region, exactly
           as it stays outside the quantised one. Asserted over the whole top
           band rather than at one probe, because a Bayer pattern is only
           visible if you look at more than one pixel. */
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

    /* ------------- the LCD layout end to end through video_submit ---------- */
    {
        /* A real .mgw geometry (Mickey Mouse, 654x396) resolved by the real
           resolver, so this exercises the same rect the device would get.
           Nothing else in this file runs a frame through the FRACTIONAL
           scaler; the pipeline could pick the integer path for the LCD
           layout and every assertion above would still pass. */
        koboy_config lc; config_defaults(&lc);
        lc.layout_mode = KOBOY_LAYOUT_LCD;
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
       A golden-age arcade board turned its MONITOR on its side: FinalBurn Neo
       renders Galaga into a 288x224 LANDSCAPE buffer and asks the frontend,
       through RETRO_ENVIRONMENT_SET_ROTATION, to turn it a quarter turn. This
       block checks the turn itself, on a source small enough to write out by
       hand, and then the two properties that a plausible-looking wrong
       implementation would still satisfy.

       The source is 4 wide by 2 tall with a DISTINCT VALUE PER PIXEL, so
       every one of the eight can be traced to exactly one destination. Values
       are picked to be already-quantised greys (0x0000 -> 0, 0xFFFF -> 255,
       and two mid RGB565 greys), and the profile below is built at scale 1 so
       the destination is the rotated source pixel for pixel with no scaling
       in the way. */
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

        /* A profile whose max is the SQUARE 4x4 -- which is what FBNeo really
           reports (side = max(w,h)) precisely so that both orientations fit
           one buffer -- resolved on a panel big enough that the scale search
           cannot demote below 1. Scale is pinned to 1 explicitly so the
           destination is the rotated frame with nothing added. */
        koboy_config rc; config_defaults(&rc);
        rc.scale = 1; rc.scale_explicit = true;
        koboy_profile rp;
        CHECK(config_resolve_profile(&rp, &rc, 1264, 1680, 4, 2, 4, 4));
        CHECK_EQ_INT(rp.scale, 1);

        /* Expected destinations, derived from libretro's own definition (the
           value is 90-degree COUNTER-CLOCKWISE steps) and NOT from the
           implementation:
             rot 0 -> 4x2, unchanged
             rot 1 -> 2x4, out[y][x] = src[x][3-y]
             rot 2 -> 4x2, out[y][x] = src[1-y][3-x]
             rot 3 -> 2x4, out[y][x] = src[1-x][y]            */
        for (int rot = 0; rot < 4; rot++) {
            koboy_video *rv = video_create(&rp, false, KOBOY_GRAY_DEFAULT);
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
           produces identical output for both. */
        {
            koboy_video *a = video_create(&rp, false, KOBOY_GRAY_DEFAULT);
            koboy_video *b = video_create(&rp, false, KOBOY_GRAY_DEFAULT);
            CHECK(a && b);
            video_set_rotation(a, 1);
            video_set_rotation(b, 3);
            video_submit(a, src, 4, 2, 4 * sizeof(uint16_t), KOBOY_PIXFMT_RGB565);
            video_submit(b, src, 4, 2, 4 * sizeof(uint16_t), KOBOY_PIXFMT_RGB565);
            size_t n = (size_t)video_stride(a) * (size_t)rp.game_h;
            CHECK(memcmp(video_buffer(a), video_buffer(b), n) != 0);
            video_destroy(a); video_destroy(b);
        }

        /* And rot 0 leaves the pipeline BYTE FOR BYTE what it was before
           rotation existed -- the property every other core koboy ships
           depends on. Compared against a koboy_video that was never told
           about rotation at all, not against a second one set to 0, so a
           video_set_rotation that corrupted state on the way to 0 is caught
           too. */
        {
            koboy_video *a = video_create(&rp, false, KOBOY_GRAY_DEFAULT);
            koboy_video *b = video_create(&rp, false, KOBOY_GRAY_DEFAULT);
            CHECK(a && b);
            video_set_rotation(a, 0);
            video_submit(a, src, 4, 2, 4 * sizeof(uint16_t), KOBOY_PIXFMT_RGB565);
            video_submit(b, src, 4, 2, 4 * sizeof(uint16_t), KOBOY_PIXFMT_RGB565);
            size_t n = (size_t)video_stride(a) * (size_t)rp.game_h;
            CHECK_EQ_INT(memcmp(video_buffer(a), video_buffer(b), n), 0);
            CHECK_EQ_INT(video_get_rotation(b), 0);   /* the default */
            video_destroy(a); video_destroy(b);
        }
    }
})


