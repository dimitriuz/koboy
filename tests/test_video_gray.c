#include "test.h"
#include "video.h"
#include "config.h"
#include <string.h>

/* The quantiser's answer for one grey, as a level index 0..3. Goes through
   video_quantise4 rather than reimplementing 43/128/213 here, so a test about
   "which of the four levels does this colour land on" cannot drift away from
   the thresholds the pipeline actually uses. */
static int level_of(uint8_t gray)
{
    uint8_t px = gray;
    video_quantise4(&px, 1, 1, 1);
    for (int i = 0; i < 4; i++) if (px == KOBOY_DU4_LEVELS[i]) return i;
    return -1;
}

static int level_of_rgb(koboy_gray_map m, unsigned r, unsigned g, unsigned b)
{
    return level_of(video_xrgb8888_to_gray((uint32_t)((r << 16) | (g << 8) | b), m));
}

TEST_MAIN({
    /* ---- properties every mapping must have ---------------------------- */
    for (int m = 0; m < KOBOY_GRAY_COUNT; m++) {
        koboy_gray_map g = (koboy_gray_map)m;

        /* Black stays black and white stays paper-white. Not decoration: the
           weight triples sum to exactly 256 so that (255,255,255) comes out
           at 255 and the quantiser's top level stays reachable from white. A
           triple summing to 255 would cap at 254 and every "white" pixel in
           the library would render one level down. */
        CHECK_EQ_INT(video_rgb565_to_gray(0x0000, g), 0);
        CHECK_EQ_INT(video_rgb565_to_gray(0xFFFF, g), 255);
        CHECK_EQ_INT(video_xrgb8888_to_gray(0x00000000u, g), 0);
        CHECK_EQ_INT(video_xrgb8888_to_gray(0x00FFFFFFu, g), 255);

        /* The LUT must agree with the scalar path at every one of its 65536
           inputs -- the LUT is the only thing the RGB565 hot loop reads. */
        static uint8_t lut[65536];
        video_gray_lut_build(lut, g);
        int mismatches = 0;
        for (int i = 0; i < 65536; i++)
            if (lut[i] != video_rgb565_to_gray((uint16_t)i, g)) mismatches++;
        CHECK_EQ_INT(mismatches, 0);

        /* THE TWO PIXEL FORMATS MUST AGREE. A core that requests XRGB8888
           does not touch the LUT at all (video_pipeline_run calls
           video_xrgb8888_to_gray per pixel), so nothing but this check stops
           the two paths drifting into rendering the same game differently
           depending on which format its core happened to ask for. Every
           RGB565 value, expanded to 8 bits exactly the way
           video_rgb565_to_gray expands it. */
        int fmt_disagree = 0;
        for (int i = 0; i < 65536; i++) {
            unsigned r5 = ((unsigned)i >> 11) & 0x1Fu;
            unsigned g6 = ((unsigned)i >> 5) & 0x3Fu;
            unsigned b5 = (unsigned)i & 0x1Fu;
            uint32_t x = (uint32_t)((((r5 << 3) | (r5 >> 2)) << 16) |
                                    (((g6 << 2) | (g6 >> 4)) << 8) |
                                     ((b5 << 3) | (b5 >> 2)));
            if (video_xrgb8888_to_gray(x, g) != video_rgb565_to_gray((uint16_t)i, g))
                fmt_disagree++;
        }
        CHECK_EQ_INT(fmt_disagree, 0);

        /* Monotonic along the neutral diagonal: a lighter grey never comes
           out darker. The shadow lift is a curve, and a curve that folded
           back would put two different greys on one level in the middle of a
           gradient. */
        int prev = -1, nonmono = 0;
        for (int v = 0; v < 256; v++) {
            if (video_xrgb8888_to_gray((uint32_t)((v << 16) | (v << 8) | v), g) < prev)
                nonmono++;
            prev = video_xrgb8888_to_gray((uint32_t)((v << 16) | (v << 8) | v), g);
        }
        CHECK_EQ_INT(nonmono, 0);
    }

    /* ---- THE GAME BOY IS UNAFFECTED, under every mapping ---------------- */
    /* These four are gambatte's real DMG palette, read off the host build of
       gambatte_libretro running Super Mario Land and Kirby's Dream Land: a
       frame contains exactly these four colours and nothing else. They are
       near-neutral, and every mapping here is the identity on a neutral grey
       up to the lift -- so all four land on the four levels they have always
       landed on, and tests/golden/pipeline_dmg_5x.pgm is byte-identical.

       THIS IS WHY THERE IS NO PER-SYSTEM EXEMPTION. An exemption keyed on
       160x144 geometry would also have caught the Game Gear, which is a
       COLOUR system and precisely the case the new mappings exist to fix. */
    {
        static const unsigned dmg[4][3] = {
            {   0,   0,   0 }, {  82,  85,  82 }, { 173, 170, 173 }, { 255, 255, 255 }
        };
        for (int m = 0; m < KOBOY_GRAY_COUNT; m++)
            for (int i = 0; i < 4; i++)
                CHECK_EQ_INT(level_of_rgb((koboy_gray_map)m,
                                          dmg[i][0], dmg[i][1], dmg[i][2]), i);
    }

    /* ---- the defect this exists to fix, in the pixels that showed it ---- */
    /* Sonic Pocket Adventure's sky, straight out of the RACE core. Rec.601
       weights blue at 29/256, so this bright cyan-blue comes out at 119 and
       the quantiser puts it on level 1 -- a DARK grey for the lightest thing
       on the screen. */
    CHECK_EQ_INT(level_of_rgb(KOBOY_GRAY_LUMA,     0, 154, 255), 1);
    CHECK(level_of_rgb(KOBOY_GRAY_DEFAULT,         0, 154, 255) >= 2);

    /* Castlevania's sky, out of fceumm: Rec.601 puts it on level 0. Black. */
    CHECK_EQ_INT(level_of_rgb(KOBOY_GRAY_LUMA,     0,  36, 140), 0);
    CHECK(level_of_rgb(KOBOY_GRAY_DEFAULT,         0,  36, 140) >= 1);

    /* Kirby's Adventure's brick wall: level 0 under Rec.601, so a wall with a
       pattern in it renders as a solid black slab. */
    CHECK_EQ_INT(level_of_rgb(KOBOY_GRAY_LUMA,    99,  20,   0), 0);
    CHECK(level_of_rgb(KOBOY_GRAY_DEFAULT,       99,  20,   0) >= 1);

    /* And the constraint that stops the fix overshooting: Sonic's OWN blue
       must stay a level BELOW the sky he is drawn against, or the sprite
       merges into the background -- the same failure from the other
       direction. The default puts his body at 108 and the sky at 141, one
       level apart with room on both sides; KOBOY_GRAY_EQUAL, which lifts blue
       as hard as any weighting can, brings the body to 127 against a
       threshold of 128. It still separates, by one. That one is the reason
       the default is BALANCED and not EQUAL, and it is not asserted here as
       a merge because a one-unit margin is a fact about these two colours,
       not a property of the mapping. */
    CHECK(level_of_rgb(KOBOY_GRAY_DEFAULT, 0, 85, 255) <
          level_of_rgb(KOBOY_GRAY_DEFAULT, 0, 154, 255));
    CHECK(video_xrgb8888_to_gray(0x000055FFu, KOBOY_GRAY_DEFAULT) + 20 <
          video_xrgb8888_to_gray(0x009AFFu, KOBOY_GRAY_DEFAULT));

    /* Rec.601's own ordering still holds where it was right: green dominates,
       blue contributes least. KOBOY_GRAY_LUMA is v1 byte for byte. */
    CHECK(video_rgb565_to_gray(0x07E0, KOBOY_GRAY_LUMA) >
          video_rgb565_to_gray(0xF800, KOBOY_GRAY_LUMA));
    CHECK(video_rgb565_to_gray(0xF800, KOBOY_GRAY_LUMA) >
          video_rgb565_to_gray(0x001F, KOBOY_GRAY_LUMA));
    CHECK_EQ_INT(video_rgb565_to_gray(0x07E0, KOBOY_GRAY_LUMA), 149);
    CHECK_EQ_INT(video_rgb565_to_gray(0xF800, KOBOY_GRAY_LUMA),  76);
    CHECK_EQ_INT(video_rgb565_to_gray(0x001F, KOBOY_GRAY_LUMA),  28);

    /* ---- the lift, on its own ------------------------------------------ */
    /* BRIGHT is LUMA with the shadow lift and nothing else, so it may never
       darken a pixel and must actually brighten some. "Never darkens" alone
       would pass with the lift deleted. */
    {
        int lifted = 0, darkened = 0;
        for (int v = 0; v < 256; v++) {
            uint32_t px = (uint32_t)((v << 16) | (v << 8) | v);
            int a = video_xrgb8888_to_gray(px, KOBOY_GRAY_LUMA);
            int b = video_xrgb8888_to_gray(px, KOBOY_GRAY_BRIGHT);
            if (b > a) lifted++;
            if (b < a) darkened++;
        }
        CHECK_EQ_INT(darkened, 0);
        CHECK(lifted > 200);
    }

    /* The enum is ordered darkest-rendering to lightest, which is what makes
       "the menu cycles forward" mean "brighter" to someone holding the
       device. Measured as the mean over all 65536 RGB565 colours. */
    {
        unsigned long mean[KOBOY_GRAY_COUNT];
        for (int m = 0; m < KOBOY_GRAY_COUNT; m++) {
            unsigned long sum = 0;
            for (int i = 0; i < 65536; i++)
                sum += video_rgb565_to_gray((uint16_t)i, (koboy_gray_map)m);
            mean[m] = sum;
        }
        for (int m = 1; m < KOBOY_GRAY_COUNT; m++) CHECK(mean[m] > mean[m - 1]);
    }

    /* ---- names, and the out-of-range guard ------------------------------ */
    {
        koboy_gray_map g;
        for (int m = 0; m < KOBOY_GRAY_COUNT; m++) {
            const char *n = video_gray_map_name((koboy_gray_map)m);
            CHECK(n != NULL);
            CHECK(video_gray_map_parse(n, &g));
            CHECK_EQ_INT((int)g, m);
        }
        CHECK(!strcmp(video_gray_map_name(KOBOY_GRAY_DEFAULT), "balanced"));

        /* An unknown name is refused and leaves the caller's value ALONE --
           config.c relies on that to keep the previous mapping rather than
           silently falling to entry 0, which is the very mapping the key
           exists to move away from. */
        g = KOBOY_GRAY_VALUE;
        CHECK(!video_gray_map_parse("balanced ", &g));   /* trailing space */
        CHECK(!video_gray_map_parse("", &g));
        CHECK(!video_gray_map_parse("BALANCED", &g));    /* case matters */
        CHECK(!video_gray_map_parse(NULL, &g));
        CHECK_EQ_INT((int)g, (int)KOBOY_GRAY_VALUE);
    }

    /* THE GAME GEAR IS A COLOUR SYSTEM AND MUST NOT BE GIVEN THE GAME BOY'S
       TREATMENT, and this is the check that says so.

       The trap is specific and it is already half-sprung elsewhere in this
       codebase: a Game Gear frame is 160x144, EXACTLY the Game Boy's
       geometry, and config_resolve_profile really does key its scale default
       on that geometry (`is_game_boy`). For SCALE that is right -- 5 was
       measured for 160x144 and a Game Gear looks good at it. For COLOUR it
       would be a disaster: the whole point of the gray_map work is that the
       DMG's four fixed shades need no mapping and a colour system does, so an
       exemption keyed on 160x144 would silently route every Game Gear title
       through the Game Boy's identity path and render Sonic's sky black.

       Two assertions, both of which a future exemption would have to break:

       1. Geometry does not reach the reduction at all. The same colour
          reduces to the same grey through a Game-Boy-shaped pipeline
          (max 160x144) and a Game-Gear-shaped one (base 160x144, max
          284x240 -- the numbers Genesis Plus GX actually reports, measured).
          Read out of the rendered buffer, not from the scalar helper, so an
          exemption applied anywhere in video_create/video_submit is caught
          and not just one in video_rgb565_to_gray.

       2. The presentation the Game Gear ends up with, stated as a number:
          its 160x144 frame is fitted to 800x720 -- pixel for pixel the
          Game Boy's scale-5 picture, reached by auto-fitting a rect sized
          from a 284x240 max rather than by any Game-Boy special case. That
          is the claim worth pinning, because it is what makes leaving
          config_resolve_profile alone the right call: a Game Gear needs no
          exemption to look right.

       MUTANT-VERIFIED: adding `if (p->max_w == KOBOY_GB_W && p->max_h ==
       KOBOY_GB_H) map = KOBOY_GRAY_LUMA;` to video_create makes the first
       block fail (level 0 against level 1) on Sonic's sky. */
    {
        koboy_config c; config_defaults(&c);
        koboy_profile gb, gg;
        CHECK(config_resolve_profile(&gb, &c, 1264, 1680, 160, 144, 160, 144));
        CHECK(config_resolve_profile(&gg, &c, 1264, 1680, 160, 144, 284, 240));

        /* The Game Boy keeps its measured 5 (auto-fitting it lands on 6 --
           mutate the is_game_boy branch off and this goes red). */
        CHECK_EQ_INT(gb.scale, 5);

        /* rgb(0,154,255) -- Sonic Pocket Adventure's sky, the measured colour
           that renders BLACK under Rec.601 luma and correctly under the
           shipped default. A whole 160x144 frame of it. */
        static uint16_t sky[160 * 144];
        const uint16_t px = (uint16_t)(((0u >> 3) << 11) | ((154u >> 2) << 5) | (255u >> 3));
        for (size_t i = 0; i < sizeof sky / sizeof sky[0]; i++) sky[i] = px;

        uint8_t out[2];
        koboy_profile *profs[2] = { &gb, &gg };
        for (int i = 0; i < 2; i++) {
            koboy_video *v = video_create(profs[i], false, KOBOY_GRAY_DEFAULT);
            CHECK(v != NULL);
            video_submit(v, sky, 160, 144, 160 * sizeof(uint16_t),
                         KOBOY_PIXFMT_RGB565);
            koboy_rect fr; video_frame_rect(v, &fr);
            /* One pixel from the middle of the fitted picture, so the
               size-change margin (which is deliberately paper-white) cannot
               be what is being read. */
            out[i] = video_buffer(v)[(size_t)(fr.y + fr.h / 2) * video_stride(v)
                                     + fr.x + fr.w / 2];
            /* Claim 2: both end up as the SAME 800x720 picture. The Game Boy
               gets there at its configured scale 5 with the rect exactly the
               frame's size; the Game Gear gets there by fitting 160x144 into
               a rect sized from 284x240. */
            CHECK_EQ_INT(fr.w, 800);
            CHECK_EQ_INT(fr.h, 720);
            video_destroy(v);
        }
        CHECK_EQ_INT(out[0], out[1]);
        /* And it is not black -- otherwise "both the same" would also pass
           against a pipeline that gave BOTH of them the Rec.601 treatment. */
        CHECK(out[0] != KOBOY_DU4_LEVELS[0]);
    }

    /* An out-of-range map arrives from an int field in koboy_config and must
       render as the default rather than index the weight table off its end.

       Asserted on the CLAMP ITSELF, not on what a rendered pixel happens to
       come out as. Deleting the guard would make the render undefined, and
       "undefined" on this host may well produce exactly the value the test
       expected -- a check that can only fail through UB is not a check
       (CLAUDE.md). video_gray_map_clamp returns a value, so the mutant
       "return m unchanged" fails here deterministically. */
    {
        CHECK_EQ_INT((int)video_gray_map_clamp(-1), (int)KOBOY_GRAY_DEFAULT);
        CHECK_EQ_INT((int)video_gray_map_clamp(KOBOY_GRAY_COUNT), (int)KOBOY_GRAY_DEFAULT);
        CHECK_EQ_INT((int)video_gray_map_clamp(KOBOY_GRAY_COUNT + 7), (int)KOBOY_GRAY_DEFAULT);
        CHECK_EQ_INT((int)video_gray_map_clamp(1 << 30), (int)KOBOY_GRAY_DEFAULT);
        /* and every in-range value passes through untouched, or the clamp
           would be a constant function that also passes the three above */
        for (int m = 0; m < KOBOY_GRAY_COUNT; m++)
            CHECK_EQ_INT((int)video_gray_map_clamp(m), m);

        /* Both users of the clamp actually go through it. */
        koboy_gray_map bad = (koboy_gray_map)(KOBOY_GRAY_COUNT + 7);
        int disagree = 0;
        for (int i = 0; i < 65536; i += 7)
            if (video_rgb565_to_gray((uint16_t)i, bad) !=
                video_rgb565_to_gray((uint16_t)i, KOBOY_GRAY_DEFAULT)) disagree++;
        CHECK_EQ_INT(disagree, 0);
        CHECK(!strcmp(video_gray_map_name(bad), "balanced"));
    }
})
