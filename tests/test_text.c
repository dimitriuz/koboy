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

    /* CLIPPING IS LIVE. Drawing past every edge must touch nothing outside the
       buffer. A guard band on both sides catches an unclamped write; this is
       checked by assertion on the band rather than by hoping a stray write
       lands somewhere observable. */
    static uint8_t guarded[16 + W * H + 16];
    memset(guarded, 0x5A, sizeof guarded);
    uint8_t *inner = guarded + 16;
    memset(inner, 0xFF, (size_t)W * H);
    text_draw(inner, W, W, H, -50, -50, "CLIP", 3, 0x00);
    text_draw(inner, W, W, H, W - 2, H - 2, "CLIP", 3, 0x00);
    text_draw(inner, W, W, H, 0, H + 5, "CLIP", 3, 0x00);
    int guard_ok = 1;
    for (int i = 0; i < 16; i++) if (guarded[i] != 0x5A) guard_ok = 0;
    for (int i = 0; i < 16; i++) if (guarded[16 + W * H + i] != 0x5A) guard_ok = 0;
    CHECK_EQ_INT(guard_ok, 1);

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
