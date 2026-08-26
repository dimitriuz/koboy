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
   one row above its touch circle) this returns the smaller.

   `layout_mode` (a koboy_layout_mode) selects which set of controls that is.
   KOBOY_LAYOUT_LCD draws none of the permille controls at all -- a Game &
   Watch title draws its own buttons into the artwork -- so its answer is
   simply the top of the bottom strip, and `l` is unused there. It is still
   passed, rather than the function splitting in two, because every caller
   asks the same question ("where must the game rect stop?") and one answer
   is what stops the resolver and the renderer from disagreeing. */
int chrome_controls_top(int layout_mode, const koboy_layout *l,
                        int panel_w, int panel_h);

/* ---------------------------------------------------------- the LCD layout
 *
 * Three pieces of geometry the LCD faceplate needs, defined here rather than
 * recomputed at each use, for exactly the reason chrome_controls_top gives
 * above: chrome.c DRAWS the bottom strip, config.c has to reserve the game
 * rect clear of it, and input.c has to hit-test MENU inside it. Three
 * independent copies of the same arithmetic is how a drawn control and its
 * live touch zone drift apart, and this project has already paid for that
 * once (see input.h's note on the faceplate's zones under a full-panel list).
 */

/* Height of the bottom strip: the band below the game rect carrying the
   BATTERY lamp, the wordmark and the MENU zone. Permille of the panel, like
   every other control dimension here, with a floor so a hypothetical tiny
   panel still gets a strip a finger can hit.

   72 permille is not a free parameter. It is the largest strip that still
   lets the TALLEST measured Game & Watch title fill the panel width: Donkey
   Kong is 606x748, which at 1264 wide is 1560 rows, and 1680 - 1560 = 120 --
   which is what 72 permille of 1680 comes to. A taller strip would start
   shrinking that title away from full width for no gain. */
int chrome_lcd_strip_h(int panel_h);

/* The MENU zone, in panel coordinates. MENU is the ONLY way back to the ROM
   browser once a game is running, so in a layout with no drawn faceplate
   controls it is the one thing that must still be there. */
void chrome_lcd_menu_rect(const koboy_profile *p, koboy_rect *out);

/* The battery lamp's centre and radius, in panel coordinates. The user asked
   for the battery to move under the screen; in this layout that is the left
   end of the bottom strip, and chrome_render_battery draws it there instead
   of in the DMG layout's left-hand case band (which does not exist here --
   the game rect runs the full panel width). */
void chrome_lcd_battery(const koboy_profile *p, int *cx, int *cy, int *r);

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
