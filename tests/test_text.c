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
