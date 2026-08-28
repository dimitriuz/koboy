/* NON-SQUARE PIXELS. Every core koboy runs but one hands over frames whose
   pixels are meant to be shown square; the Atari 2600's are ~1.75:1, and
   until this file existed every one of its 82 titles rendered about that much
   too tall (docs/FOLLOWUPS.md #51).

   The numbers in here are MEASURED, from the shipped cores through
   scripts/probe_core.c, not derived:

     core            base      max       aspect    delivered   pixel aspect
     gambatte        160x144   160x144   1.11111   160x144     1.0000  square
     gearcoleco      256x192   512x288   0         256x192     1.0000  square
     freeintv        352x224   352x224   1.57143   352x224     1.0000  square
     pokemini        384x256   384x256   1.5       384x256     1.0000  square
     race (NGP)      160x152   160x152   1.05      160x152     0.9975  ROUNDED
     beetle-wswan    224x144   224x224   1.55556   224x144     1.0000  square
     gw              606x748   606x748   0         606x748     1.0000  square
     fceumm (NES)    256x240   256x240   1.21905   256x240     1.1429  8:7
     gpgx (SMS)      256x192   284x240   1.52381   256x192     1.1429  8:7
     gpgx (GG)       160x144   284x240   1.33333   160x144     1.2000
     gpgx (MD)       320x224   348x240   1.30612   320x224     0.9143  32:35
     stella (NTSC)   320x210   320x256   1.33333   160x210     1.7500
     stella (PAL)    320x250   320x256   1.33333   160x250     2.0833
     fbneo galaga    288x224   288x288   0.75      288x224     0.9643  (rot 3)
     fbneo defender  292x240   292x292   1.33333   292x240     1.0959

   Two of those rows are why this file is shaped the way it is. race reports a
   ROUNDED aspect -- 1.05 for a ratio that is 1.0526 -- so "not exactly square"
   cannot be the trigger. And stella's base_width is a LIE (320 declared, 160
   delivered), which is why the aspect_ratio field, not the base geometry, is
   the signal. */
#include "test.h"
#include "video.h"
#include "config.h"

/* A core's reported float aspect, as core.c converts it: 16.16, rounded. */
#define DAR(x) ((uint32_t)((x) * 65536.0 + 0.5))

static void fill_cols(uint16_t *fb, int w, int h, uint16_t a, uint16_t b)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) fb[y * w + x] = (x & 1) ? b : a;
}

/* Width of the run of equal bytes starting at (x,y) in a gray buffer. */
static int run_w(const uint8_t *buf, int stride, int x, int y, int limit)
{
    uint8_t v = buf[(size_t)y * stride + x];
    int n = 0;
    while (x + n < limit && buf[(size_t)y * stride + x + n] == v) n++;
    return n;
}

TEST_MAIN({
    /* ================================================ video_pixel_aspect == */

    /* NO ANSWER IS SQUARE. 0 is what a koboy_video starts at and what
       core_display_aspect could never return for a loaded core, so this is
       the value every test that predates non-square pixels sees. */
    CHECK_EQ_INT(video_pixel_aspect(0, 160, 144), (int)KOBOY_ASPECT_ONE);
    CHECK_EQ_INT(video_pixel_aspect(0, 999, 1),   (int)KOBOY_ASPECT_ONE);

    /* LIVE GUARD: a degenerate frame would divide by zero. */
    CHECK_EQ_INT(video_pixel_aspect(DAR(1.3333), 0, 210),  (int)KOBOY_ASPECT_ONE);
    CHECK_EQ_INT(video_pixel_aspect(DAR(1.3333), 160, 0),  (int)KOBOY_ASPECT_ONE);
    CHECK_EQ_INT(video_pixel_aspect(DAR(1.3333), -1, 210), (int)KOBOY_ASPECT_ONE);

    /* THE GAME BOY. gambatte reports 1.11111 for a 160x144 frame, and 160/144
       is 1.1111..., so the pixels are square -- exactly, after the deadband
       absorbs the float's last bit. This is the assertion the whole change
       turns on: it is what makes tests/golden/pipeline_dmg_5x.pgm and the
       chrome goldens still pass. */
    CHECK_EQ_INT(video_pixel_aspect(DAR(1.11111), 160, 144), (int)KOBOY_ASPECT_ONE);

    /* The other square-pixel cores, each with its own real numbers, because
       "square" reached four different ways is four different chances to be
       wrong. freeintv and pokemini state their ratio exactly; gearcoleco and
       gw report 0 and core.c substitutes base_w/base_h. */
    CHECK_EQ_INT(video_pixel_aspect(DAR(1.57143), 352, 224), (int)KOBOY_ASPECT_ONE);
    CHECK_EQ_INT(video_pixel_aspect(DAR(1.5),     384, 256), (int)KOBOY_ASPECT_ONE);
    CHECK_EQ_INT(video_pixel_aspect(((uint32_t)256 << 16) / 192, 256, 192),
                 (int)KOBOY_ASPECT_ONE);                       /* gearcoleco */
    CHECK_EQ_INT(video_pixel_aspect(((uint32_t)606 << 16) / 748, 606, 748),
                 (int)KOBOY_ASPECT_ONE);                       /* Game & Watch */
    CHECK_EQ_INT(video_pixel_aspect(DAR(1.55556), 224, 144), (int)KOBOY_ASPECT_ONE);
    /* THE GAME BOY ADVANCE. gpSP reports exactly 1.5 for a 240x160 frame and
       240/160 IS 1.5, so this one needs no deadband and no substitution -- it
       is the cleanest square-pixel case in the table. Asserted because the
       claim "a GBA needs no aspect correction" is load-bearing for the
       integer 4x its ceiling produces: an anisotropic par would drop the fit
       a whole step, which is measured on this exact mechanism in
       config_resolve_profile_par's comment. */
    CHECK_EQ_INT(video_pixel_aspect(DAR(1.5), 240, 160), (int)KOBOY_ASPECT_ONE);

    /* THE DEADBAND, and the core it exists for. race reports 1.05 for a
       160x152 screen whose exact ratio is 1.0526 -- 0.25% off square. Without
       the snap the Neo Geo Pocket would leave the block-copy scaler and be
       resampled for a quarter of one percent. */
    {
        uint32_t raw = (uint32_t)(((uint64_t)DAR(1.05) * 152u) / 160u);
        CHECK(raw != KOBOY_ASPECT_ONE);                 /* genuinely not square... */
        CHECK(KOBOY_ASPECT_ONE - raw < KOBOY_PAR_DEADBAND);  /* ...but inside */
        CHECK_EQ_INT(video_pixel_aspect(DAR(1.05), 160, 152), (int)KOBOY_ASPECT_ONE);
    }

    /* Both edges of the deadband, from the inside and the outside, so it is
       pinned rather than "somewhere around there". A frame of w x w makes the
       pixel aspect equal the display aspect exactly, which is what lets these
       be written as a plain offset. */
    CHECK_EQ_INT(video_pixel_aspect(KOBOY_ASPECT_ONE + KOBOY_PAR_DEADBAND, 64, 64),
                 (int)KOBOY_ASPECT_ONE);
    CHECK_EQ_INT(video_pixel_aspect(KOBOY_ASPECT_ONE - KOBOY_PAR_DEADBAND, 64, 64),
                 (int)KOBOY_ASPECT_ONE);
    CHECK_EQ_INT(video_pixel_aspect(KOBOY_ASPECT_ONE + KOBOY_PAR_DEADBAND + 1, 64, 64),
                 (int)(KOBOY_ASPECT_ONE + KOBOY_PAR_DEADBAND + 1));
    CHECK_EQ_INT(video_pixel_aspect(KOBOY_ASPECT_ONE - KOBOY_PAR_DEADBAND - 1, 64, 64),
                 (int)(KOBOY_ASPECT_ONE - KOBOY_PAR_DEADBAND - 1));

    /* THE NON-SQUARE CORES. Each is checked to within a couple of parts in
       65536, which is the float round-trip through retro_game_geometry's
       `float aspect_ratio` -- not a loose window: the nearest wrong answer in
       every one of these rows is thousands of units away. */
    {
        struct { const char *what; uint32_t dar; int w, h; uint32_t want; } rows[] = {
            { "Atari NTSC 160x210 @ 4:3",  DAR(1.33333), 160, 210, 114688 },  /* 1.75   */
            { "Atari PAL  160x250 @ 4:3",  DAR(1.33333), 160, 250, 136533 },  /* 2.0833 */
            { "NES        256x240",        DAR(1.21905), 256, 240,  74898 },  /* 8:7    */
            { "SMS        256x192",        DAR(1.52381), 256, 192,  74898 },  /* 8:7    */
            { "Game Gear  160x144",        DAR(1.33333), 160, 144,  78643 },  /* 6:5    */
            { "Mega Drive 320x224",        DAR(1.30612), 320, 224,  59918 },  /* 32:35  */
            { "Galaga     224x288 (rot)",  DAR(0.75),    224, 288,  63195 },  /* 27:28  */
            { "Defender   292x240",        DAR(1.33333), 292, 240,  71820 },
        };
        for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
            uint32_t got = video_pixel_aspect(rows[i].dar, rows[i].w, rows[i].h);
            long d = (long)got - (long)rows[i].want;
            if (d < 0) d = -d;
            CHECK(d <= 4);
            /* And none of them is square, which is the whole point. */
            CHECK(got != KOBOY_ASPECT_ONE);
        }
    }

    /* ==================================================== video_fit_par ==== */

    koboy_config c; config_defaults(&c);

    /* THE GAME BOY, unmoved. 160x144 into the 800x720 rect at scale 5, offsets
       zero -- the numbers the design spec measured and the one presentation
       verified on real hardware. */
    {
        koboy_profile p;
        CHECK(config_resolve_profile(&p, &c, 1264, 1680, 160, 144, 160, 144));
        int fs, dw, ox, oy;
        video_fit_par(&p, 160, 144, KOBOY_ASPECT_ONE, &fs, &dw, &ox, &oy);
        CHECK_EQ_INT(fs, 5); CHECK_EQ_INT(dw, 800);
        CHECK_EQ_INT(ox, 0); CHECK_EQ_INT(oy, 0);
        /* And through the aspect gambatte actually reports, which is the path
           the running emulator takes. */
        video_fit_par(&p, 160, 144, video_pixel_aspect(DAR(1.11111), 160, 144),
                      &fs, &dw, &ox, &oy);
        CHECK_EQ_INT(fs, 5); CHECK_EQ_INT(dw, 800);
        CHECK_EQ_INT(ox, 0); CHECK_EQ_INT(oy, 0);
    }

    /* video_fit IS video_fit_par at par 1, over a sweep rather than at one
       point -- the property, not an example. If these ever disagree the
       "square pixels are untouched" claim is false somewhere. */
    {
        koboy_profile p;
        CHECK(config_resolve_profile(&p, &c, 1264, 1680, 256, 240, 320, 256));
        const int sizes[][2] = { {160,144},{320,256},{96,64},{305,191},{1,1},
                                 {319,255},{400,400},{200,40},{7,301} };
        for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
            int fs1, ox1, oy1, fs2, dw2, ox2, oy2;
            video_fit(&p, sizes[i][0], sizes[i][1], &fs1, &ox1, &oy1);
            video_fit_par(&p, sizes[i][0], sizes[i][1], KOBOY_ASPECT_ONE,
                          &fs2, &dw2, &ox2, &oy2);
            CHECK_EQ_INT(fs1, fs2); CHECK_EQ_INT(ox1, ox2); CHECK_EQ_INT(oy1, oy2);
            CHECK_EQ_INT(dw2, sizes[i][0] * fs2);
        }
        /* 0 is accepted as "square" too -- a caller that never set an aspect. */
        int fs, dw, ox, oy;
        video_fit_par(&p, 160, 144, 0, &fs, &dw, &ox, &oy);
        CHECK_EQ_INT(dw, 160 * fs);
    }

    /* THE ATARI. A rect resolved for the 2600's real numbers, and the frame
       fitted into it: 4:3 to the pixel, exactly the shape docs/FOLLOWUPS.md
       #51 predicted by hand before any of this code was written. The
       vertical stays an exact integer.

       The numbers grew by one whole step when the rect started coming from
       the core's BASE geometry rather than its max: stella2014 declares
       320x256 (PAL's tallest) and draws 210 lines on an NTSC title, so the
       rect used to reserve 46 rows of scale that nothing was ever going to
       fill, and the height is what bound the fit. 840x630 in a 840x768 rect
       became 1120x840 in a 1120x840 one -- the same picture, a third larger,
       and now with no dead band above and below it (oy == 0). */
    {
        koboy_profile p;
        uint32_t par_base = video_pixel_aspect(DAR(1.33333), 320, 210);
        CHECK(config_resolve_profile_par(&p, &c, 1264, 1680, 320, 210, 320, 256, par_base));
        CHECK_EQ_INT(p.game_w, 1120); CHECK_EQ_INT(p.game_h, 840);

        int fs, dw, ox, oy;
        uint32_t par = video_pixel_aspect(DAR(1.33333), 160, 210);
        video_fit_par(&p, 160, 210, par, &fs, &dw, &ox, &oy);
        CHECK_EQ_INT(fs, 4);           /* vertical: an exact integer, 840 rows */
        CHECK_EQ_INT(dw, 1120);        /* horizontal: 160 * 4 * 1.75          */
        CHECK_EQ_INT(ox, 0);
        CHECK_EQ_INT(oy, 0);           /* the rect IS the picture now         */
        /* 1120:840 IS 4:3. Stated as the ratio and not just as two numbers,
           because two numbers can be right for the wrong reason. */
        CHECK_EQ_INT(dw * 3, (840) * 4);

        /* What it used to do, kept as the counter-example: square scaling of
           the same frame in the same rect is 640x840, i.e. 1.75x too tall
           for its width. */
        int sfs, sox, soy;
        video_fit(&p, 160, 210, &sfs, &sox, &soy);
        CHECK_EQ_INT(160 * sfs, 640);
        CHECK_EQ_INT(210 * sfs, 840);
    }

    /* A PIXEL NARROWER THAN IT IS TALL, which is half the measured population
       (Mega Drive 32:35, Galaga 27:28) and the direction it would be easy to
       get backwards. The width must SHRINK relative to square, not grow. */
    {
        koboy_profile p;
        uint32_t par_base = video_pixel_aspect(DAR(1.30612), 320, 224);
        CHECK(config_resolve_profile_par(&p, &c, 1264, 1680, 320, 224, 348, 240, par_base));
        int fs, dw, ox, oy;
        video_fit_par(&p, 320, 224, par_base, &fs, &dw, &ox, &oy);
        /* 4, not the 3 this asserted while the rect came from Genesis Plus
           GX's 348x240 max -- a geometry no Mega Drive title renders. */
        CHECK_EQ_INT(fs, 4);
        CHECK(dw < 320 * fs);                       /* narrower than square */
        CHECK(dw > 320 * fs * 9 / 10);              /* but only by ~8.6%    */
        CHECK(ox > 0);                              /* centred, not flush   */
    }

    /* BOUNDS AND CENTRING, swept rather than sampled. The bounds guard in
       video_pipeline_run exists because a frame that overflows the rect
       corrupts memory rather than looking wrong; this is what keeps it from
       ever having to fire. Both axes are checked because with two scale
       factors there are two ways to lose centring, and the old single-factor
       code could only get one of them wrong. */
    {
        koboy_profile p;
        CHECK(config_resolve_profile(&p, &c, 1264, 1680, 256, 240, 320, 256));
        const uint32_t pars[] = { 1u, 4096u, 32768u, 65535u, KOBOY_ASPECT_ONE,
                                  65537u, 78643u, 114688u, 262143u, 1048576u };
        const int sizes[][2] = { {160,144},{320,256},{1,1},{320,1},{1,256},
                                 {160,210},{319,255},{2000,2000} };
        for (size_t i = 0; i < sizeof pars / sizeof pars[0]; i++)
            for (size_t j = 0; j < sizeof sizes / sizeof sizes[0]; j++) {
                int fs, dw, ox, oy;
                video_fit_par(&p, sizes[j][0], sizes[j][1], pars[i], &fs, &dw, &ox, &oy);
                int dh = sizes[j][1] * fs;
                CHECK(fs >= 1);
                CHECK(dw >= 1 && dw <= p.game_w);
                CHECK(ox >= 0 && oy >= 0);
                CHECK(ox + dw <= p.game_w);
                /* Centred to within the odd pixel an integer halving leaves. */
                CHECK(p.game_w - dw - ox <= ox + 1);
                if (dh <= p.game_h) {
                    CHECK(oy + dh <= p.game_h);
                    CHECK(p.game_h - dh - oy <= oy + 1);
                }
            }
    }

    /* ================================================== video_fit_rect ==== */

    /* The DMG branch just relays video_fit_par; the LCD branch has its own
       aspect fit and its own max-geometry shortcut, and the shortcut had to
       learn that its premise (the rect was sized from max) only holds for
       square pixels -- and then, when SNES and Mega Drive joined this layout,
       that it only holds for GAME & WATCH at all. */
    {
        koboy_config lc; config_defaults(&lc);
        lc.layout_mode = KOBOY_LAYOUT_LCD;
        lc.lcd_rect_from_max = true;            /* .mgw: see config.c */
        koboy_profile lp;
        CHECK(config_resolve_profile(&lp, &lc, 1264, 1680, 654, 396, 654, 396));
        CHECK(lp.rect_from_max);

        int dw, dh, ox, oy;
        /* Square pixels at max: the shortcut, exact, filling the rect. This is
           every Game & Watch title -- the gw core reports aspect 0. */
        video_fit_rect(&lp, 654, 396, KOBOY_ASPECT_ONE, &dw, &dh, &ox, &oy);
        CHECK_EQ_INT(dw, lp.game_w); CHECK_EQ_INT(dh, lp.game_h);

        /* THE SAME FRAME, THE SAME SQUARE PIXELS, ON A CONSOLE'S RECT: also
           not the shortcut. The rect there is fitted from BASE and may then
           be cut by the per-system scale ceiling, so max geometry is neither
           its shape nor its size -- taking the shortcut would stretch the
           frame to fill a rect it does not match. This is the case that
           `rect_from_max` exists for, and it cannot be reached through
           layout_mode: both profiles below are KOBOY_LAYOUT_LCD.

           Built from the SAME geometry as the run above so the only thing
           that differs is the flag, which is what makes this measure the
           flag. base 305x191 is Donkey Kong zoomed to its LCD alone; a
           console rect fitted from that is 1264x791, and a frame arriving at
           the 654x396 max must be fitted INTO it, not handed all of it. */
        {
            koboy_config bc = lc;
            bc.lcd_rect_from_max = false;
            koboy_profile bp;
            CHECK(config_resolve_profile(&bp, &bc, 1264, 1680, 305, 191, 654, 396));
            CHECK(!bp.rect_from_max);
            int bdw, bdh, box, boy;
            video_fit_rect(&bp, 654, 396, KOBOY_ASPECT_ONE, &bdw, &bdh, &box, &boy);
            CHECK(bdw <= bp.game_w && bdh <= bp.game_h);
            /* The rect is TALLER than 654:396, so an honest fit is width-bound
               and leaves height over. Handing over the whole rect would give
               game_h exactly, which is the mutant. */
            CHECK(bdh < bp.game_h);
            CHECK(boy > 0);
            /* ...and the shape that came out is the frame's, not the rect's. */
            CHECK(bdw * 396 / 654 >= bdh - 1 && bdw * 396 / 654 <= bdh + 1);
        }

        /* The SAME frame with wide pixels must NOT fill the rect: it has a
           different shape now, so the shortcut's premise is gone. */
        video_fit_rect(&lp, 654, 396, 114688u, &dw, &dh, &ox, &oy);
        CHECK(dw <= lp.game_w && dh <= lp.game_h);
        CHECK(dh < lp.game_h);                  /* width binds once widened */
        CHECK(oy > 0);
        /* And the result is the widened aspect, to within the fit's rounding:
           654 * 1.75 : 396 is 2.89:1. */
        CHECK((long)dw * 396 * 100 > (long)dh * 654 * 174);
        CHECK((long)dw * 396 * 100 < (long)dh * 654 * 176);
    }

    /* ========================================== the pipeline, end to end == */

    /* THE INERTNESS CLAIM, and it is deliberately not written as "the square
       case still matches a golden". An assertion of that shape passes
       trivially when the anisotropic path is never reached at all -- which is
       exactly how this defect survived every numeric check in the batch that
       introduced it. So all three buffers are produced here: two that must be
       IDENTICAL and one that must be DIFFERENT. Delete the anisotropic path
       and the third check fails; break the square path and the first two do. */
    {
        koboy_profile p;
        CHECK(config_resolve_profile(&p, &c, 1264, 1680, 160, 144, 160, 144));
        static uint16_t fb[160 * 144];
        fill_cols(fb, 160, 144, 0x0000, 0xFFFF);

        size_t n = (size_t)p.game_w * (size_t)p.game_h;
        uint8_t *never = malloc(n), *reported = malloc(n), *wide = malloc(n);
        CHECK(never && reported && wide);

        koboy_video *v = video_create(&p, false, KOBOY_GRAY_DEFAULT);
        CHECK(v != NULL);
        /* (1) never told about an aspect -- every test written before this */
        video_submit(v, fb, 160, 144, 320, KOBOY_PIXFMT_RGB565);
        for (int y = 0; y < p.game_h; y++)
            memcpy(never + (size_t)y * p.game_w,
                   video_buffer(v) + (size_t)y * video_stride(v), (size_t)p.game_w);

        /* (2) told what gambatte actually reports */
        video_set_aspect(v, DAR(1.11111));
        video_invalidate(v);
        video_submit(v, fb, 160, 144, 320, KOBOY_PIXFMT_RGB565);
        for (int y = 0; y < p.game_h; y++)
            memcpy(reported + (size_t)y * p.game_w,
                   video_buffer(v) + (size_t)y * video_stride(v), (size_t)p.game_w);

        /* (3) told the Atari's aspect for the same frame: 1.2 pixel aspect */
        video_set_aspect(v, DAR(1.33333));
        video_invalidate(v);
        video_submit(v, fb, 160, 144, 320, KOBOY_PIXFMT_RGB565);
        for (int y = 0; y < p.game_h; y++)
            memcpy(wide + (size_t)y * p.game_w,
                   video_buffer(v) + (size_t)y * video_stride(v), (size_t)p.game_w);

        CHECK_EQ_INT(memcmp(never, reported, n), 0);   /* square: bit for bit */
        CHECK(memcmp(never, wide, n) != 0);            /* and the path IS live */
        free(never); free(reported); free(wide);
        video_destroy(v);
    }

    /* THE PIXELS THEMSELVES GET WIDER, which no rect assertion can prove. A
       one-source-pixel column becomes a run of `scale` bytes under square
       scaling and a run of `scale * par` bytes under a wide pixel aspect.
       Checked on the panel buffer, after the scaler, because that is where
       the defect lived. */
    {
        koboy_profile p;
        uint32_t par_base = video_pixel_aspect(DAR(1.33333), 320, 210);
        CHECK(config_resolve_profile_par(&p, &c, 1264, 1680, 320, 210, 320, 256, par_base));
        static uint16_t fb[160 * 210];
        fill_cols(fb, 160, 210, 0x0000, 0xFFFF);

        koboy_video *v = video_create(&p, false, KOBOY_GRAY_DEFAULT);
        CHECK(v != NULL);
        video_set_aspect(v, DAR(1.33333));
        video_submit(v, fb, 160, 210, 320, KOBOY_PIXFMT_RGB565);

        koboy_rect fr; video_frame_rect(v, &fr);
        CHECK_EQ_INT(fr.w, 1120); CHECK_EQ_INT(fr.h, 840);

        const uint8_t *buf = video_buffer(v);
        int st = video_stride(v);
        int mid = fr.y + fr.h / 2;
        /* 160 source columns across 1120 destination ones is EXACTLY 7, and
           that is new: the base-sized rect put NTSC stella on 4 vertical
           steps, and 4 * 1.75 is a whole number, so the horizontal comb this
           block was written to catch is not present on this geometry any
           more. Square scaling would make every run 4, which is the value
           this rejects -- and the run count is still what does the
           rejecting, not the rect width, because a rect assertion cannot
           tell a wide pixel from a bigger scale. The genuinely fractional
           case moved to the PAL block below. */
        int seven = 0, other = 0;
        /* The first run at an arbitrary x is a PARTIAL one -- the scan starts
           in the middle of it -- so it is stepped over rather than counted,
           and the loop stops far enough from the right edge that the last
           counted run is whole. */
        int x0 = fr.x + 12;
        x0 += run_w(buf, st, x0, mid, fr.x + fr.w);
        for (int x = x0; x + 8 < fr.x + fr.w - 12; ) {
            int r = run_w(buf, st, x, mid, fr.x + fr.w);
            if (r == 7) seven++; else other++;
            x += r;
        }
        CHECK(seven > 0); CHECK_EQ_INT(other, 0);

        /* And the ROWS are exactly 4 deep -- the vertical scale stayed an
           integer, which is the property that keeps a 210-line frame from
           acquiring a 3/4-row comb. */
        {
            int col = fr.x + fr.w / 2;
            int r = 0;
            uint8_t v0 = buf[(size_t)fr.y * st + col];
            while (fr.y + r < fr.y + fr.h && buf[(size_t)(fr.y + r) * st + col] == v0) r++;
            CHECK_EQ_INT(r % 4, 0);
        }
        video_destroy(v);
    }

    /* THE SAME PROPERTY WHERE THE RATIO IS NOT A WHOLE NUMBER, which is what
       the NTSC block above used to test before its rect grew. PAL stella:
       320x250 declared, 160x250 delivered, 25:12 pixels -- 1000 destination
       columns over 160 source ones is 6.25, so the nearest-neighbour scaler
       must produce a MIX of 6-wide and 7-wide runs and nothing else. Square
       scaling would make every one of them exactly 3.

       Kept as its own block rather than folded into the sweep below: a comb
       is only visible if you count the teeth. */
    {
        koboy_profile p;
        uint32_t par_base = video_pixel_aspect(DAR(1.33333), 320, 250);
        CHECK(config_resolve_profile_par(&p, &c, 1264, 1680, 320, 250, 320, 256, par_base));
        static uint16_t fb[160 * 250];
        fill_cols(fb, 160, 250, 0x0000, 0xFFFF);

        koboy_video *v = video_create(&p, false, KOBOY_GRAY_DEFAULT);
        CHECK(v != NULL);
        video_set_aspect(v, DAR(1.33333));
        video_submit(v, fb, 160, 250, 320, KOBOY_PIXFMT_RGB565);

        koboy_rect fr; video_frame_rect(v, &fr);
        CHECK_EQ_INT(fr.w, 1000); CHECK_EQ_INT(fr.h, 750);

        const uint8_t *buf = video_buffer(v);
        int st = video_stride(v);
        int mid = fr.y + fr.h / 2;
        int six = 0, seven = 0, other = 0;
        int x0 = fr.x + 12;
        x0 += run_w(buf, st, x0, mid, fr.x + fr.w);
        for (int x = x0; x + 8 < fr.x + fr.w - 12; ) {
            int r = run_w(buf, st, x, mid, fr.x + fr.w);
            if (r == 6) six++; else if (r == 7) seven++; else other++;
            x += r;
        }
        CHECK(six > 0); CHECK(seven > 0); CHECK_EQ_INT(other, 0);
        video_destroy(v);
    }

    /* THE LCD LAYOUT KEEPS THE FRACTIONAL SCALER even when its fit lands on
       an exact integer multiple, and this is the case that proves the
       exclusion in video_pipeline_run is not redundant.

       400x420 into the LCD layout's 1264x1260 available area fits at exactly
       3x on both axes (height binds: 420 * 3 == 1260). The two scalers do NOT
       agree there. video_scale_gray_frac's step is floored -- (400 << 16) /
       1200 is 21845, not 21845.33 -- so source column 0 covers FOUR
       destination columns and the rest cover three, while the block scaler
       gives every column exactly three. That one-pixel difference is a
       property the Game & Watch presentation has always had; this change has
       no business altering it, and the Game Boy must never acquire it.

       Written as a first-run width because that is where the two disagree,
       not as a buffer comparison against a golden: a golden would go stale for
       any reason at all, and this asserts the actual mechanism. */
    {
        koboy_config lc; config_defaults(&lc);
        lc.layout_mode = KOBOY_LAYOUT_LCD;
        lc.lcd_rect_from_max = true;            /* Game & Watch: see config.c */
        koboy_profile lp;
        CHECK(config_resolve_profile(&lp, &lc, 1264, 1680, 400, 420, 400, 420));
        CHECK_EQ_INT(lp.game_w, 1200);          /* exactly 3x, both axes */
        CHECK_EQ_INT(lp.game_h, 1260);

        static uint16_t fb[400 * 420];
        fill_cols(fb, 400, 420, 0x0000, 0xFFFF);
        koboy_video *v = video_create(&lp, false, KOBOY_GRAY_DEFAULT);
        CHECK(v != NULL);
        video_submit(v, fb, 400, 420, 800, KOBOY_PIXFMT_RGB565);
        koboy_rect fr; video_frame_rect(v, &fr);
        CHECK_EQ_INT(fr.w, 1200); CHECK_EQ_INT(fr.h, 1260);
        CHECK_EQ_INT(run_w(video_buffer(v), video_stride(v),
                           fr.x, fr.y + fr.h / 2, fr.x + fr.w), 4);
        video_destroy(v);

        /* AND THE SAME EXACT-MULTIPLE FIT ON A CONSOLE'S RECT TAKES THE BLOCK
           PATH, which is the other half of that decision and the half that
           arrived with SNES and Mega Drive. The skew above is a property the
           Game & Watch presentation has always had and must keep; there is no
           reason to hand it to two systems that only share the layout, and
           the block scaler is the better one -- it is what the Game Boy gets.
           Same panel, same geometry, same exactly-3x rect: the ONLY
           difference is which geometry the rect was fitted from, so runs of 3
           against runs of 4 is the block path against the fractional one and
           nothing else.

           Mutant that made this necessary: keying block_ok on
           `layout_mode != KOBOY_LAYOUT_LCD` (its shape before this change)
           instead of on `!rect_from_max`. Nothing else in the suite noticed. */
        koboy_config bc = lc;
        bc.lcd_rect_from_max = false;
        koboy_profile bp;
        CHECK(config_resolve_profile(&bp, &bc, 1264, 1680, 400, 420, 400, 420));
        CHECK_EQ_INT(bp.game_w, 1200);
        CHECK_EQ_INT(bp.game_h, 1260);
        CHECK(!bp.rect_from_max);
        koboy_video *bv = video_create(&bp, false, KOBOY_GRAY_DEFAULT);
        CHECK(bv != NULL);
        video_submit(bv, fb, 400, 420, 800, KOBOY_PIXFMT_RGB565);
        koboy_rect br; video_frame_rect(bv, &br);
        CHECK_EQ_INT(br.w, 1200); CHECK_EQ_INT(br.h, 1260);
        CHECK_EQ_INT(run_w(video_buffer(bv), video_stride(bv),
                           br.x, br.y + br.h / 2, br.x + br.w), 3);
        video_destroy(bv);
    }

    /* ========================================= config_resolve_profile_par = */

    /* par 1 is the old function, over a sweep of geometries. */
    {
        const int geo[][4] = { {160,144,160,144}, {256,240,256,240},
                               {256,192,284,240}, {224,144,224,224},
                               {320,210,320,256}, {96,64,96,64},
                               {606,748,606,748}, {292,240,292,292} };
        for (size_t i = 0; i < sizeof geo / sizeof geo[0]; i++) {
            koboy_profile a, b;
            bool ra = config_resolve_profile(&a, &c, 1264, 1680,
                                             geo[i][0], geo[i][1], geo[i][2], geo[i][3]);
            bool rb = config_resolve_profile_par(&b, &c, 1264, 1680,
                                                 geo[i][0], geo[i][1], geo[i][2], geo[i][3],
                                                 KOBOY_ASPECT_ONE);
            CHECK_EQ_INT(ra, rb);
            CHECK_EQ_INT(memcmp(&a, &b, sizeof a), 0);
        }
    }

    /* THE NES, which is the case that made the rect aspect-aware at all.
       256x240 at 8:7 is 292.6 source columns of rect, so the rect is 879 wide
       at scale 3 and the frame lands at 878x720 -- correctly shaped AND
       BIGGER than the 768x720 square scaling gave it. Both halves are
       asserted, because "bigger" without "correctly shaped" is the original
       bug and "correctly shaped" without "bigger" is the regression that
       sizing the rect for square pixels produced (585x480, measured). */
    {
        koboy_profile p;
        uint32_t par = video_pixel_aspect(DAR(1.21905), 256, 240);
        CHECK(config_resolve_profile_par(&p, &c, 1264, 1680, 256, 240, 256, 240, par));
        CHECK_EQ_INT(p.game_w, 879);
        CHECK_EQ_INT(p.game_h, 720);

        int fs, dw, ox, oy;
        video_fit_par(&p, 256, 240, par, &fs, &dw, &ox, &oy);
        CHECK_EQ_INT(fs, 3);                 /* NOT 2 -- the round-up bought it */
        CHECK_EQ_INT(dw, 878);
        CHECK(dw > 768);                     /* bigger than square scaling was */
        /* 878:720 is 8/7 * 256 : 240, i.e. 1.219 -- the 4:3-ish NES shape. */
        CHECK((long)dw * 240 * 1000 > (long)720 * 256 * 1140);
        CHECK((long)dw * 240 * 1000 < (long)720 * 256 * 1146);
    }

    /* THE WONDERSWAN, which is why the rect's aspect comes from BASE and not
       from MAX. Its core reports 1.55556 for a 224x144 LANDSCAPE frame while
       declaring a SQUARE 224x224 max (so both orientations fit one buffer).
       Read against max that aspect says "widen by 56%"; read against base it
       says "square", which is the truth. The rect must not move. */
    {
        koboy_profile square, got;
        CHECK(config_resolve_profile(&square, &c, 1264, 1680, 224, 144, 224, 224));
        uint32_t par = video_pixel_aspect(DAR(1.55556), 224, 144);
        CHECK_EQ_INT(par, (int)KOBOY_ASPECT_ONE);
        CHECK(config_resolve_profile_par(&got, &c, 1264, 1680, 224, 144, 224, 224, par));
        CHECK_EQ_INT(memcmp(&square, &got, sizeof got), 0);
    }

    /* THE ROUNDING MODE, as an invariant rather than as an example.

       THE RECT IS SIZED FOR MAX GEOMETRY, SO A FRAME AT MAX MUST REACH THE
       RECT'S OWN SCALE. That is the property the whole two-step arrangement
       rests on -- config_resolve_profile_par picks a scale, video_fit_par has
       to be able to reach it -- and it is exactly what a rounded-DOWN rect
       width breaks: one source column short is `fx` one whole integer step
       short, which is the 585x480 regression again, reintroduced by a
       rounding mode rather than by ignoring the aspect.

       Swept over ten real core geometries and the whole plausible aspect
       range rather than asserted at a point, because which (geometry, aspect)
       pairs land on a fraction below a half is not something worth predicting
       by hand. Measured: 0 misses rounding up, 4672 rounding to nearest. The
       counter rather than a CHECK per iteration keeps ~14000 combinations
       from swamping this file's check count with one fact. */
    {
        const int geo[][2] = { {160,144},{256,240},{284,240},{320,256},{224,224},
                               {292,292},{288,288},{96,64},{348,240},{160,152} };
        int misses = 0, first_par = 0, first_g = -1, swept = 0;
        for (size_t g = 0; g < sizeof geo / sizeof geo[0]; g++)
            for (uint32_t par = 32768u; par <= 196608u; par += 1373u) {
                koboy_profile p;
                if (!config_resolve_profile_par(&p, &c, 1264, 1680,
                                                geo[g][0], geo[g][1],
                                                geo[g][0], geo[g][1], par)) continue;
                swept++;
                int fs, dw, ox, oy;
                video_fit_par(&p, geo[g][0], geo[g][1], par, &fs, &dw, &ox, &oy);
                if (fs != p.scale) {
                    if (!misses) { first_par = (int)par; first_g = (int)g; }
                    misses++;
                }
                /* And the fitted width fills the rect it was sized for, to
                   within the one pixel the ceiling adds. */
                if (fs == p.scale && dw > p.game_w) misses++;
            }
        /* THE SWEEP MUST ACTUALLY HAVE SWEPT, and without this line it did
           not have to. All three assertions below are satisfied by the
           INITIALISERS, so a config_resolve_profile_par that returned false
           for every combination took the `continue` 1200 times, ran the body
           zero times and passed on three checks -- the file's own comment
           above even states the counts it expects and never asserted one.
           tests/test_chrome.c (search `checked > 1000`) and
           tests/test_video_pipeline.c (`shrunk > 0`) already carry exactly
           this guard; this sweep was the sibling without it.
           1000 rather than the exact 1200: ten geometries times the 120
           `par` steps in [32768, 196608] is the count today, and pinning it
           exactly would make adding a geometry to the table look like a
           regression. The number that matters is "not zero, and not three". */
        CHECK(swept > 1000);
        CHECK_EQ_INT(misses, 0);
        CHECK_EQ_INT(first_g, -1);
        CHECK_EQ_INT(first_par, 0);
    }

    /* And the Atari's rect, the other end of the same formula: 320 declared
       columns at 0.875 (the aspect read against the DECLARED base, which is
       the one that is a lie) is 280, times scale 4. Narrower per unit of
       scale than square scaling reserved -- and it holds a bigger picture,
       which is the whole point. */
    {
        koboy_profile p;
        uint32_t par = video_pixel_aspect(DAR(1.33333), 320, 210);
        CHECK(config_resolve_profile_par(&p, &c, 1264, 1680, 320, 210, 320, 256, par));
        CHECK_EQ_INT(p.game_w, 1120);
        CHECK_EQ_INT(p.max_w, 320);      /* max itself is NOT rewritten */
        CHECK_EQ_INT(p.max_h, 256);
    }
})
