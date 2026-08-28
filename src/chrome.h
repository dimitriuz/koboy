#ifndef KOBOY_CHROME_H
#define KOBOY_CHROME_H
#include "koboy.h"

/* Draws the static faceplate into a full-panel gray8 buffer. Must never write
   inside the profile's game rect. Called once at startup and refreshed with
   KOBOY_REFRESH_FULL, so it costs nothing per frame. */
void chrome_render(uint8_t *fb, int stride, const koboy_profile *p,
                   const koboy_layout *l);

/* The topmost panel row any drawn control -- or any live touch zone from the
   same layout -- occupies. config_resolve_profile calls it so the auto-fitted
   game rect stops above the control band, not merely above the bezel margin:
   reserving KOBOY_CHROME_MARGIN alone let scale = 0 fit a rect that swallowed
   the A button and the d-pad on every panel, with the touch zones still live
   underneath, so tapping the lower playfield pressed A.

   Here, beside the drawing code, so there is ONE definition of where controls
   start. input.c's hit zones come off the same permille, and where the two
   differ by a pixel (the d-pad's INK frame is drawn one row above its touch
   circle) this returns the smaller.

   `layout_mode` selects which control set. KOBOY_LAYOUT_LCD draws none of the
   permille controls, so its answer is the top of the bottom strip and `l` is
   unused -- still passed rather than splitting the function, because every
   caller asks the same question and one answer is what stops the resolver and
   the renderer disagreeing. */
int chrome_controls_top(int layout_mode, const koboy_layout *l,
                        int panel_w, int panel_h);

/* ---------------------------------------------------------- the LCD layout
 *
 * Three pieces of geometry the LCD faceplate needs, defined once rather than
 * recomputed: chrome.c DRAWS the bottom strip, config.c reserves the game rect
 * clear of it, input.c hit-tests MENU inside it. Three independent copies of
 * the same arithmetic is how a drawn control and its live touch zone drift
 * apart, which this project has already paid for once (input.h's note on the
 * faceplate's zones under a full-panel list).
 */

/* Height of the bottom strip: the band below the game rect carrying the game
   CONTROLS, the BATTERY lamp, the wordmark and the MENU zone. Permille of the
   panel, like every other control dimension here.

   250 permille -- 420 px on the verified 1264x1680 panel -- SET BY WHAT HAS TO
   FIT IN IT, not by what is left over. The first version was 72 permille
   (120 px), "the largest strip that still lets Donkey Kong fill the panel
   width", because it drew no game controls at all. MEASURED to be wrong: the
   shipped .mgw titles route through gwlua's compat init, which has no pointer
   handling, so a touch on the artwork changes ZERO pixels while a joypad press
   changes 211k. The strip has to carry a full retropad, and 120 px cannot.

   250 permille costs nothing that matters: the binding case is the tallest
   measured title, Donkey Kong (Multi Screen) 606x748, which fits 1021x1260 in
   the remaining 1264x1260 -- more than 1.6x, against the 1x the device
   reported before this layout. Widescreen titles are width-bound and fill all
   1264 columns either way.

   NO FLOOR, deliberately: the old 48 px floor was for a strip holding one MENU
   pill. A strip seating a d-pad, four face buttons and four pills has no
   meaningful minimum -- every control derives from this height, so a panel too
   short produces controls too small to hit whatever floor is applied, and
   inventing one would only hide that. */
int chrome_lcd_strip_h(int panel_h);

/* Every control the LCD strip carries, in PANEL coordinates, resolved once:
   chrome.c draws from this, input.c hit-tests it, config.c reserves the game
   rect clear of it.

   The d-pad is a four-way cross of half-length `dpad_r`, hit-tested exactly
   like the DMG faceplate's, so the two layouts share one implementation and
   one set of quirks.

   THREE FACE ARRANGEMENTS, chosen per system by the profile's `lcd_face`
   (koboy_lcd_face, koboy.h), because the arrangement is load-bearing: a player
   who has held the real pad already knows where its buttons are.

   DIAMOND: four discs, X top, Y left, A right, B bottom. Right for Game &
   Watch because the gw core's own overlay DRAWS a SNES pad and labels the TOP
   button NORTHEAST and the BOTTOM one SOUTHEAST; right for SNES because its
   real pad IS that diamond.

   ROWS6: six discs, two rows of three -- X Y Z above A B C, the six-button
   Mega Drive pad. That system has no shoulders, so `l1`/`r1` come back
   ZERO-SIZED and the lower band carries two pills instead of four.

   PAIR2: two discs on a north-east/south-west diagonal, a Game Boy Advance.
   `x_*`/`y_*` come back ZERO -- and a zero is a coordinate a real finger can
   reach, so BOTH the draw path and input.c's hit test gate on `face_n >= 4`
   rather than relying on the zero being harmless. The four pills stay: a GBA's
   L and R are the one genuinely left-and-right pair on this strip.

   `face_n` says which arrangement was resolved; every consumer switches on it
   rather than re-deriving it.

   THE FIELD NAMES ARE THE RETROPAD'S and stay so: they say which BIT each disc
   reports, one thing for every system. What each disc SAYS is a separate table
   (koboy_lcd_pad in koboy.h), because JOYPAD_A is the SNES's A and the Mega
   Drive's C.

   `face_r` is every disc's radius. `face_off` is DIAMOND's centre-to-centre
   distance out to each disc; `face_pitch` is ROWS6's step between neighbours.
   Only one is meaningful at a time and both are always filled, because a stale
   field is easier to read than a union.

   BOTH SPACINGS ARE PROVED, not clamped. face_off is face_r * 8/5, so two
   adjacent diamond discs (centres face_off * sqrt(2) apart) cannot merge.
   face_pitch is face_r * 11/5, so a grid neighbour is 2.2 * face_r away with a
   0.2 * face_r gap and the diagonals are 3.11 * face_r apart. PAIR2 reuses
   face_off -- its discs sit face_off/2 either side of the cluster centre on
   both axes, so face_off * sqrt(2) apart, the diamond's separation exactly. */
typedef struct {
    koboy_rect strip;                   /* the whole bottom strip */
    int dpad_cx, dpad_cy, dpad_r;
    int face_n;                         /* 2 = PAIR2, 4 = DIAMOND, 6 = ROWS6 */
    int face_r, face_off, face_pitch;
    int x_cx, x_cy;                     /* diamond: NORTHEAST. rows6: top mid.
                                           pair2: ZERO -- no such button */
    int y_cx, y_cy;                     /* diamond: west.      rows6: low left.
                                           pair2: ZERO -- no such button */
    int a_cx, a_cy;                     /* diamond: east.      rows6: low right.
                                           pair2: north-east */
    int b_cx, b_cy;                     /* diamond: SOUTHEAST. rows6: low mid.
                                           pair2: south-west */
    int l1_cx, l1_cy;                   /* rows6 only: top left  (zero otherwise) */
    int r1_cx, r1_cy;                   /* rows6 only: top right (zero otherwise) */
    koboy_rect l1, select, start, r1;   /* the lower band, left to right.
                                           l1/r1 are zero-sized under ROWS6 */
    koboy_rect menu;
    int bat_cx, bat_cy, bat_r;
} chrome_lcd_controls;

void chrome_lcd_layout(const koboy_profile *p, chrome_lcd_controls *out);

/* The MENU zone in panel coordinates -- chrome_lcd_layout's `menu`, named
   because MENU is the ONLY way back to the ROM browser once a game is running.
   It sits in the strip's CENTRE COLUMN, between the d-pad and the face
   cluster, the one part neither thumb rests on: a MENU that fires by accident
   mid-round is worse than one slightly further to reach. */
void chrome_lcd_menu_rect(const koboy_profile *p, koboy_rect *out);

/* The battery lamp's centre and radius, chrome_lcd_layout's answer under a
   usable name. The user asked for the battery under the screen; here that is
   the left end of the strip's LOWER band, beside the pills, rather than the
   DMG layout's left-hand case band -- which does not exist here, the game rect
   runs the full panel width. */
void chrome_lcd_battery(const koboy_profile *p, int *cx, int *cy, int *r);

/* The two background-band widths chrome_render fills either side of the game
   rect, CLAMPED into [0, panel_w]. Exposed only so the clamp can be tested
   DIRECTLY: a sentinel guard band cannot detect the right-hand case, because
   an unclamped `W - rx` is a length near SIZE_MAX and where glibc writes for
   such a length is an implementation detail that on x86-64 lands INSIDE the
   panel, so an overrun test passes with or without the clamp. Asserting the
   clamped values is deterministic on every libc and invokes no UB.
   `*left` is the band left of the rect; `*right_start` is the first column
   right of it, so the right band is panel_w - *right_start wide. */
void chrome_bands(const koboy_profile *p, int panel_w, int *left, int *right_start);

/* Draws the battery lamp for `percent` (0..100), or unfilled when percent < 0.
   Separate from chrome_render because it is the one element whose value
   changes: redrawn whenever the whole panel is already being repainted
   (startup, menu exit, chrome restore), so the faceplate keeps its
   zero-per-frame cost and needs no timer. A dedicated timer was REJECTED --
   on a panel where every refresh is visible, a periodic one for a number that
   changes over hours is a bad trade. */
void chrome_render_battery(uint8_t *fb, int stride, const koboy_profile *p,
                           const koboy_layout *l, int percent);
#endif
