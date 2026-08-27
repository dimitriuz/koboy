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

/* Height of the bottom strip: the band below the game rect carrying the game
   CONTROLS, the BATTERY lamp, the wordmark and the MENU zone. Permille of the
   panel, like every other control dimension here.

   250 permille -- 420 px on the verified 1264x1680 panel -- and the number is
   set by what has to FIT IN it, not by what is left over. The first version
   of this layout was 72 permille (120 px), chosen as "the largest strip that
   still lets Donkey Kong fill the panel width", because that version drew no
   game controls at all. That was a mistake with a measurement behind it: the
   shipped .mgw titles route through gwlua's compat init, which has no pointer
   handling whatsoever, so a touch on the artwork changes ZERO pixels while a
   joypad press changes 211k. Every one of these titles is driven by retropad
   buttons, so the strip has to carry a full retropad, and 120 px cannot.

   What 250 permille costs is nothing that matters: the binding case is the
   tallest measured title, Donkey Kong (Multi Screen) at 606x748, which fits
   1021x1260 in the remaining 1264x1260 -- still more than 1.6x, against the
   1x the device reported before this layout existed. The widescreen titles
   (Mickey Mouse 654x396) are width-bound and unaffected: they fill all 1264
   columns either way.

   No floor any more, and that is deliberate rather than an omission: the old
   48 px floor existed for a strip holding one MENU pill. A strip that must
   seat a d-pad, four face buttons and four pills has no meaningful minimum --
   chrome_lcd_layout derives every control from this height, so a panel too
   short for them produces controls too small to hit whatever floor is
   applied, and inventing one would only hide that. */
int chrome_lcd_strip_h(int panel_h);

/* Every control the LCD strip carries, in PANEL coordinates, resolved once.
   chrome.c draws from this, input.c hit-tests from it, and config.c reserves
   the game rect clear of the strip that holds it -- one definition, for the
   reason the section header above gives.

   The d-pad is a four-way cross of half-length `dpad_r`, hit-tested exactly
   like the DMG faceplate's (a circle of that radius, then the shared
   deadzone/hysteresis decode), so the two layouts share one implementation
   and one set of quirks.

   THE FACE BUTTONS COME IN TWO ARRANGEMENTS, chosen per system by the
   profile's `lcd_face` (koboy_lcd_face, koboy.h), because the arrangement is
   load-bearing rather than decorative -- a player who has held the real pad
   already knows where its buttons are.

   DIAMOND: four discs, X top, Y left, A right, B bottom. Right for Game &
   Watch because the gw core's own overlay (START with no cursor active)
   DRAWS a SNES pad and labels the TOP button NORTHEAST, the BOTTOM one
   SOUTHEAST, so a user reading that overlay finds the same button here;
   right for SNES because its real pad IS that diamond. Rearranging either
   into a row would break the correspondence silently.

   ROWS6: six discs, two rows of three, which is the six-button Mega Drive
   pad -- X Y Z above A B C. That system has no shoulders, so `l1` and `r1`
   come back ZERO-SIZED in this arrangement and the lower band carries two
   pills (MODE, START) instead of four: a shoulder pill for a bit that
   already has a disc would be two controls with one name. `face_n` says
   which arrangement was resolved, and every consumer switches on it rather
   than re-deriving it.

   PAIR2: two discs on a north-east/south-west diagonal, A above and right of
   B, which is a Game Boy Advance. `x_*` and `y_*` come back ZERO in this
   arrangement -- that machine has two face buttons and drawing four would
   invent two, see koboy_lcd_face in koboy.h -- and a zero is a coordinate a
   real finger can reach, so BOTH the draw path and input.c's hit test are
   gated on `face_n >= 4` rather than relying on the zero to be harmless. The
   four pills stay: a GBA has L and R, and they are the one control pair on
   this strip that is genuinely left-and-right.

   The FIELD NAMES here are the retropad's, and they stay the retropad's:
   they say which BIT each disc reports, which is one thing for every system
   and every arrangement. What each disc SAYS is a separate table
   (koboy_lcd_pad in koboy.h), because JOYPAD_A is the SNES's A and the Mega
   Drive's C.

   `face_r` is every disc's radius in both arrangements. `face_off` is the
   DIAMOND's centre-to-centre distance out to each disc; `face_pitch` is
   ROWS6's centre-to-centre step between neighbours. Only one of the two is
   meaningful at a time and both are always filled, because a stale field is
   easier to read than a union.

   Both spacings are proved rather than clamped. face_off > face_r * sqrt(2)
   by construction (it is face_r * 8/5), so two ADJACENT diamond discs cannot
   merge -- their centres are face_off * sqrt(2) apart. face_pitch is
   face_r * 11/5, so a grid neighbour's centre is 2.2 * face_r away and the
   discs keep a 0.2 * face_r gap; the diagonal neighbours are 3.11 * face_r
   apart. PAIR2 reuses face_off: its two discs sit face_off/2 either side of
   the cluster centre on both axes, so their centres are face_off * sqrt(2)
   apart -- the diamond's adjacent-pair separation exactly, and therefore
   proved by the same construction. */
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

/* The MENU zone, in panel coordinates -- chrome_lcd_layout's `menu`, kept as
   a named accessor because MENU is the ONLY way back to the ROM browser once
   a game is running and every caller that wants it wants nothing else. It
   sits in the strip's CENTRE COLUMN, between the d-pad and the face diamond,
   which is the one part of the strip neither thumb rests on: a MENU that fires
   by accident mid-round is worse than one that is slightly further to reach. */
void chrome_lcd_menu_rect(const koboy_profile *p, koboy_rect *out);

/* The battery lamp's centre and radius, in panel coordinates -- again just
   chrome_lcd_layout's answer under a name its callers can use. The user asked
   for the battery to move under the screen; in this layout that is the left
   end of the strip's LOWER band, beside the L1/SELECT/START/R1 pills, and
   chrome_render_battery draws it there instead of in the DMG layout's
   left-hand case band (which does not exist here -- the game rect runs the
   full panel width). */
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
