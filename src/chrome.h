#ifndef KOBOY_CHROME_H
#define KOBOY_CHROME_H
#include "koboy.h"

/* Draws the static faceplate into a full-panel gray8 buffer. Must never write
   inside the profile's game rect. Called once at startup and refreshed with
   KOBOY_REFRESH_FULL, so it costs nothing per frame. */
void chrome_render(uint8_t *fb, int stride, const koboy_profile *p,
                   const koboy_layout *l);

/* The topmost panel row any drawn control -- or any live touch zone derived
   from the same layout -- occupies. config_resolve_profile calls it so the
   auto-fitted game rect stops above the control band instead of only above the
   bezel margin: reserving KOBOY_CHROME_MARGIN alone let scale = 0 fit a rect
   that swallowed the A button and the d-pad on every supported panel, and the
   touch zones stayed live underneath it, so tapping the lower playfield pressed
   A. It lives here, beside the code that draws the controls, so there is one
   definition of where they start; input.c's hit zones come off the same layout
   permille, and where the two differ by a pixel (the d-pad's INK frame is drawn
   one row above its touch circle) this returns the smaller. */
int chrome_controls_top(const koboy_layout *l, int panel_w, int panel_h);

/* The two background-band widths chrome_render fills either side of the game
   rect, clamped into [0, panel_w]. Exposed only so the clamp can be tested
   directly, which the sentinel guard band cannot do for the right-hand band:
   an unclamped `W - rx` is a length near SIZE_MAX, and where glibc actually
   writes for such a length is an implementation detail that on x86-64 lands
   INSIDE the panel rather than in the guard. A test that watches for the
   overrun therefore passes whether or not the clamp is present. Asserting the
   clamped values instead is deterministic on every libc, and invokes no
   undefined behaviour to do it. `*left` is the width of the band left of the
   rect; `*right_start` is the first column right of it, so the right band is
   panel_w - *right_start wide. */
void chrome_bands(const koboy_profile *p, int panel_w, int *left, int *right_start);

/* Draws the battery lamp for `percent` (0..100), or an unfilled lamp when
   percent < 0. Separate from chrome_render because it is the one element with
   a value that changes: it is redrawn whenever the whole panel is already
   being repainted -- startup, menu exit, a chrome restore -- so the faceplate
   keeps its zero-per-frame-cost property and needs no timer. A dedicated timer
   was rejected: on a panel where every refresh is visible, adding a periodic
   one to show a number that changes over hours is a bad trade. */
void chrome_render_battery(uint8_t *fb, int stride, const koboy_profile *p,
                           const koboy_layout *l, int percent);
#endif
