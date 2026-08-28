#include "test.h"
#include "text.h"

/* Counts ink pixels, which is enough to distinguish "drew a glyph" from
   "drew nothing" without pinning down the exact bitmap. */
static int ink_count(const uint8_t *fb, int n)
{
    int c = 0;
    for (int i = 0; i < n; i++) if (fb[i] == 0x00) c++;
    return c;
}

/* Renders a single character alone into an exact TEXT_GLYPH_W x
   TEXT_GLYPH_H buffer and packs its pixels into a signature: bit
   (row * TEXT_GLYPH_W + col) set iff that pixel is ink. TEXT_GLYPH_W *
   TEXT_GLYPH_H is 35, well under 64, so this is an exact, order-independent
   fingerprint of the glyph -- no scaling, no second character, no clipping
   in play, comparable with plain ==. */
static uint64_t glyph_signature(char ch)
{
    enum { GW = TEXT_GLYPH_W, GH = TEXT_GLYPH_H };
    uint8_t g[GW * GH];
    char s[2] = { ch, 0 };
    memset(g, 0xFF, sizeof g);
    text_draw(g, GW, GW, GH, 0, 0, s, 1, 0x00);
    uint64_t sig = 0;
    for (int i = 0; i < GW * GH; i++)
        if (g[i] == 0x00) sig |= (uint64_t)1 << i;
    return sig;
}

TEST_MAIN({
    enum { W = 200, H = 40 };
    static uint8_t fb[W * H];

    CHECK_EQ_INT(text_measure("", 1), 0);
    CHECK_EQ_INT(text_measure("A", 1), TEXT_ADVANCE);
    CHECK_EQ_INT(text_measure("ABC", 2), 3 * TEXT_ADVANCE * 2);

    /* Digits must render. This is the regression the extraction exists for:
       the old table was A-Z plus space, so every digit came out blank and a
       ROM filename lost its numbers silently. */
    memset(fb, 0xFF, sizeof fb);
    text_draw(fb, W, W, H, 0, 0, "0123456789", 1, 0x00);
    int digits = ink_count(fb, sizeof fb);
    CHECK(digits > 0);

    /* Punctuation a filename actually contains. */
    memset(fb, 0xFF, sizeof fb);
    text_draw(fb, W, W, H, 0, 0, ".,:-_/()'", 1, 0x00);
    CHECK(ink_count(fb, sizeof fb) > 0);

    /* Regression: TEXT_GLYPH_H is 7, so text_draw's row loop only ever
       visits bits 0..6 of a glyph column -- bit 7 (0x80) is silently never
       drawn, with no clip warning anywhere. ',' and ';' used to set bit 7
       for their descender tail, so it vanished: ',' rendered pixel-identical
       to '.', and ';' lost its tail and read as a malformed ':'. Every
       No-Intro ROM name ("Game (USA, Europe)") showed the collision as
       "USA. EUROPE". The ink-count checks above cannot catch this kind of
       bug -- both glyphs still have SOME ink, just the SAME ink as a
       different character. This asserts the distinction directly, per
       CLAUDE.md's testing-culture note: a test that only pins a value, and
       not a difference from its neighbours, does not catch a collision. */
    CHECK(glyph_signature(',') != glyph_signature('.'));
    CHECK(glyph_signature(';') != glyph_signature(':'));

    /* Generalised: sweep every printable ASCII character the font can be
       asked to render and require that no two DIFFERENT characters produce
       the same non-blank bitmap. Two exclusions, both intentional rather
       than gaps in the sweep:
         - Signature 0 (fully blank): glyph() maps every character outside
           the table to BLANK on purpose, so many different characters are
           *supposed* to share the blank glyph. A collision between two
           characters that both have real ink is never intentional.
         - Lowercase a-z: glyph() folds these to their uppercase glyph on
           purpose (asserted above), so 'a' colliding with 'A' is the
           designed behaviour, not the bug this test exists to catch. */
    {
        int collisions = 0, inked = 0;
        for (int a = 0x20; a <= 0x7E; a++) {
            if (a >= 'a' && a <= 'z') continue;
            uint64_t sig_a = glyph_signature((char)a);
            if (sig_a == 0) continue;
            inked++;
            for (int b = a + 1; b <= 0x7E; b++) {
                if (b >= 'a' && b <= 'z') continue;
                uint64_t sig_b = glyph_signature((char)b);
                if (sig_b == 0) continue;
                if (sig_a == sig_b) {
                    fprintf(stderr, "text glyph collision: '%c' (0x%02x) == "
                            "'%c' (0x%02x)\n", a, a, b, b);
                    collisions++;
                }
            }
        }
        /* THE SWEEP MUST HAVE HAD SOMETHING TO COMPARE, and without this it
           did not. `collisions` is asserted against its own initialiser, and
           BOTH loops skip a signature of 0 -- so a font table that had gone
           blank makes every character take the `continue`, runs the
           comparison zero times, and passes on a check that reads as if it
           had examined 95 glyphs. Found while adding `make coverage`; it is
           the same shape as tests/test_video_aspect.c's `swept` guard and
           the review that named that one did not name this one.
           50 rather than the exact count, for the same reason as there:
           adding a glyph to the table must not look like a regression. The
           number that matters is "the font is not empty". */
        CHECK(inked > 50);
        CHECK_EQ_INT(collisions, 0);
    }

    /* Lowercase folds to uppercase rather than vanishing. */
    memset(fb, 0xFF, sizeof fb);
    text_draw(fb, W, W, H, 0, 0, "abc", 1, 0x00);
    int lower = ink_count(fb, sizeof fb);
    memset(fb, 0xFF, sizeof fb);
    text_draw(fb, W, W, H, 0, 0, "ABC", 1, 0x00);
    CHECK_EQ_INT(lower, ink_count(fb, sizeof fb));

    /* An unknown character renders as blank space, never as garbage and never
       out of bounds. */
    memset(fb, 0xFF, sizeof fb);
    text_draw(fb, W, W, H, 0, 0, "\x01\x7F", 1, 0x00);
    CHECK_EQ_INT(ink_count(fb, sizeof fb), 0);

    /* The clip, asserted directly: proves the predicate itself is correct.
       The block below proves text_draw actually calls it -- see its comment
       for why that second check needs positive-only overflow to stay
       crash-safe under the same mutant that breaks this one. */
    CHECK_EQ_INT(text_pixel_visible(0, 0, W, H), 1);
    CHECK_EQ_INT(text_pixel_visible(W - 1, H - 1, W, H), 1);
    CHECK_EQ_INT(text_pixel_visible(-1, 0, W, H), 0);
    CHECK_EQ_INT(text_pixel_visible(0, -1, W, H), 0);
    CHECK_EQ_INT(text_pixel_visible(W, 0, W, H), 0);
    CHECK_EQ_INT(text_pixel_visible(0, H, W, H), 0);

    /* End-to-end proof that text_draw actually CONSULTS the clip, not merely
       that the predicate is right. Overflow is to the RIGHT and BOTTOM only,
       and that restriction is the whole point: a negative coordinate cast to
       size_t wraps to near SIZE_MAX, so an unclamped write would be undefined
       behaviour and the process would simply crash -- which is what the
       previous version of this block did, and why it could not be trusted.
       Positive overflow into padding that is inside the same allocation is
       fully defined, so a missing clamp is observed rather than survived.

       This is the check the direct text_pixel_visible assertions cannot make:
       they prove the predicate, this proves the call site. */
    {
        enum { GW = 64, GH = 32, PAD = 32 };
        enum { GSTRIDE = GW + PAD, GROWS = GH + PAD };
        static uint8_t g[GSTRIDE * GROWS];
        memset(g, 0xFF, sizeof g);

        /* Positioned so the glyphs run past both the right and bottom edges of
           the declared GW x GH region. */
        text_draw(g, GSTRIDE, GW, GH, GW - 2, GH - 2, "WW", 3, 0x00);

        int outside_touched = 0, inside_painted = 0;
        for (int y = 0; y < GROWS; y++)
            for (int x = 0; x < GSTRIDE; x++) {
                uint8_t v = g[(size_t)y * GSTRIDE + x];
                if (x >= GW || y >= GH) { if (v != 0xFF) outside_touched++; }
                else if (v != 0xFF)     inside_painted++;
            }
        CHECK_EQ_INT(outside_touched, 0);
        /* And it really did draw, so "nothing outside" is not vacuous. */
        CHECK(inside_painted > 0);
    }

    /* Centring puts equal-ish margins either side. */
    memset(fb, 0xFF, sizeof fb);
    text_draw_centred(fb, W, W, H, 0, "AB", 1, 0x00);
    int first = -1, last = -1;
    for (int x = 0; x < W; x++)
        for (int y = 0; y < H; y++)
            if (fb[y * W + x] == 0x00) { if (first < 0) first = x; last = x; }
    CHECK(first > 0);
    CHECK(W - 1 - last > 0);
    CHECK(first - (W - 1 - last) <= TEXT_ADVANCE);
})
