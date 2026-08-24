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
