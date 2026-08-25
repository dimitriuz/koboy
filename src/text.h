#ifndef KOBOY_TEXT_H
#define KOBOY_TEXT_H
#include <stdint.h>

/* A 5x7 bitmap font, one byte per column, bit 0 = top row.
   Lifted out of main.c, where it existed for two calibration prompts, because
   v2 has three screens that render arbitrary strings -- and the old table was
   A-Z plus space, so a ROM filename lost every digit without saying so.
   Pulling in a font library for this would still be absurd. */

#define TEXT_GLYPH_W 5
#define TEXT_GLYPH_H 7
#define TEXT_ADVANCE 6          /* 5 columns plus one blank */

/* Width in panel pixels of `s` rendered at scale `px`. */
int  text_measure(const char *s, int px);

/* Draws `s` with its top-left at (x, y), clipped to the W x H buffer.
   Characters outside the table render as blank space. */
void text_draw(uint8_t *fb, int stride, int W, int H, int x, int y,
               const char *s, int px, uint8_t ink);

void text_draw_centred(uint8_t *fb, int stride, int W, int H, int y,
                       const char *s, int px, uint8_t ink);
#endif
