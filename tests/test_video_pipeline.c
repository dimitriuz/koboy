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
        video_fit_rect(&p, 160, 144, &dw, &dh, &ox, &oy);
        CHECK_EQ_INT(dw, 800);
        CHECK_EQ_INT(dh, 720);
        CHECK_EQ_INT(ox, 0);
        CHECK_EQ_INT(oy, 0);
        /* Below max, still an INTEGER multiple -- the DMG branch must not
           quietly acquire the fractional fit. 90x70 is chosen because the two
           fits DISAGREE there: integer takes min(800/90, 720/70) = 8, giving
           720x560, while a fractional fit would be width-bound at 800x622.
           A size where they agree (any exact divisor) would pass either way. */
        video_fit_rect(&p, 90, 70, &dw, &dh, &ox, &oy);
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
        video_fit_rect(&p, 654, 396, &dw, &dh, &ox, &oy);
        CHECK_EQ_INT(dw, 1264);
        CHECK_EQ_INT(dh, 765);
        CHECK_EQ_INT(ox, 0);
        CHECK_EQ_INT(oy, 0);

        /* LCD, below max: the zoomed LCD-only view a Game & Watch title
           alternates to. Fractional and centred, and -- unlike the DMG
           branch -- NOT rounded down to an integer multiple: 305x191 at
           integer scale would be 4x = 1220x764, and the fractional fit is
           height-bound at 765 rows. */
        video_fit_rect(&p, 305, 191, &dw, &dh, &ox, &oy);
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

        koboy_video *lv = video_create(&lp, false);
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
           margin outside it cleared (the size changed, so the clear fires). */
        CHECK_EQ_INT(video_buffer(lv)[fr.x + 4 + (size_t)400 * video_stride(lv)], 0x00);
        CHECK_EQ_INT(video_buffer(lv)[0], 0x00);   /* margin, cleared to black */

        video_destroy(lv);
    }
})


