#include "test.h"
#include "video.h"

TEST_MAIN({
    /* Bayer 16x16 must be a permutation of 0..255 starting at 0 */
    uint8_t m[16][16];
    video_bayer_build(m);
    int seen[256] = {0};
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) seen[m[y][x]]++;
    int bad = 0;
    for (int i = 0; i < 256; i++) if (seen[i] != 1) bad++;
    CHECK_EQ_INT(bad, 0);
    CHECK_EQ_INT(m[0][0], 0);

    /* DMG's four shades survive quantisation as four distinct, ordered levels */
    uint8_t dmg[4] = { 15, 90, 165, 240 };
    video_quantise4(dmg, 4, 1, 4);
    CHECK_EQ_INT(dmg[0], KOBOY_DU4_LEVELS[0]);
    CHECK_EQ_INT(dmg[1], KOBOY_DU4_LEVELS[1]);
    CHECK_EQ_INT(dmg[2], KOBOY_DU4_LEVELS[2]);
    CHECK_EQ_INT(dmg[3], KOBOY_DU4_LEVELS[3]);
    CHECK(dmg[0] < dmg[1] && dmg[1] < dmg[2] && dmg[2] < dmg[3]);

    /* exact levels are idempotent under requantisation */
    uint8_t lv[4]; memcpy(lv, KOBOY_DU4_LEVELS, 4);
    video_quantise4(lv, 4, 1, 4);
    CHECK(memcmp(lv, KOBOY_DU4_LEVELS, 4) == 0);

    /* dither output is 1-bit */
    uint8_t buf[32 * 32];
    for (int i = 0; i < 32 * 32; i++) buf[i] = 128;
    video_dither_1bit(buf, 32, 32, 32, 0, 0);
    int nonbinary = 0, black = 0;
    for (int i = 0; i < 32 * 32; i++) {
        if (buf[i] != 0 && buf[i] != 255) nonbinary++;
        if (buf[i] == 0) black++;
    }
    CHECK_EQ_INT(nonbinary, 0);
    CHECK(black > 300 && black < 724);   /* mid grey dithers to roughly half */

    /* PURE WHITE IS PURE WHITE, and pure black is pure black. Ordered
       dithering compares against a threshold, and the naive choice of
       threshold -- the Bayer matrix itself, a permutation of 0..255 -- makes
       `255 > 255` false, so one cell in every 16x16 tile of white comes out
       BLACK. Over an 800x720 game rect that is 2250 isolated black dots on
       what should be a clean page, and white is not an edge case: it is the
       Game Boy's own lightest shade and most HUD text on every other system.
       Checked over 32x32 = four whole tiles, so a single stray cell cannot
       hide between samples. */
    uint8_t white[32 * 32];
    for (int i = 0; i < 32 * 32; i++) white[i] = 255;
    video_dither_1bit(white, 32, 32, 32, 0, 0);
    int white_dark = 0;
    for (int i = 0; i < 32 * 32; i++) if (white[i] != 255) white_dark++;
    CHECK_EQ_INT(white_dark, 0);

    /* The other end, which the scaling must NOT have broken: nothing is below
       threshold 0, so a black field stays entirely black. Without this the
       obvious "fix" of comparing >= instead would pass the check above and
       speckle black fields WHITE -- the same defect upside down. */
    uint8_t black_fld[32 * 32];
    for (int i = 0; i < 32 * 32; i++) black_fld[i] = 0;
    video_dither_1bit(black_fld, 32, 32, 32, 0, 0);
    int black_light = 0;
    for (int i = 0; i < 32 * 32; i++) if (black_fld[i] != 0) black_light++;
    CHECK_EQ_INT(black_light, 0);

    /* And the middle still dithers: a fix that returned white for everything
       would satisfy both of the above. 254 is one below pure white and must
       still produce SOME black, or the top end has been flattened rather
       than corrected. */
    uint8_t near[32 * 32];
    for (int i = 0; i < 32 * 32; i++) near[i] = 254;
    video_dither_1bit(near, 32, 32, 32, 0, 0);
    int near_dark = 0;
    for (int i = 0; i < 32 * 32; i++) if (near[i] == 0) near_dark++;
    CHECK(near_dark > 0);
    CHECK(near_dark < 32);      /* ...but only a trace of it */

    /* SCREEN-COORDINATE INDEXING: identical content at the same screen
       position must dither identically. This is what makes dirty-rect
       skipping possible, so it is a correctness property, not cosmetics. */
    uint8_t a[16 * 16], b[16 * 16];
    for (int i = 0; i < 256; i++) { a[i] = 100; b[i] = 100; }
    video_dither_1bit(a, 16, 16, 16, 48, 32);
    video_dither_1bit(b, 16, 16, 16, 48, 32);
    CHECK(memcmp(a, b, 256) == 0);

    /* and the same content at a different screen position generally differs */
    uint8_t c[16 * 16];
    for (int i = 0; i < 256; i++) c[i] = 100;
    video_dither_1bit(c, 16, 16, 16, 49, 32);
    CHECK(memcmp(a, c, 256) != 0);
})
