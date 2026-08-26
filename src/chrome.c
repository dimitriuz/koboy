#include "chrome.h"
#include "text.h"
#include <string.h>

/* The faceplate is drawn with KOBOY_REFRESH_FULL -- GC16, sixteen levels. The
   four-level ceiling is a constraint on the GAME RECT only, and this file used
   three values out of sixteen (now nine). Depth is free here: chrome is
   drawn once, so elaborateness is an authoring question and not a
   performance one.

   The case tone is a WARM LIGHT GREY, not pure white -- the user's explicit
   choice against the reference photo, task 15. Round 1 of that task
   inverted the CONTROL tones by mistake (near-white d-pad/buttons/pills on
   a grey case read as holes punched in the case, not raised controls); this
   is the corrected ordering, checked against the photo directly: the case
   is the lightest thing on the lower half, every control sits darker than
   it, and the d-pad is the darkest thing on the case -- darker even than
   the screen bezel, which is why it gets its own tone (DPAD) instead of
   reusing DARK. "Clearly" below is pinned at more than one ~17-level GC16
   step, so relationships survive being quantised down to the panel's real
   16-level driver, not just on this exact 8-bit render:
     DPAD   < DARK (bezel)  -- so the d-pad and the bezel read as two
                                different objects, not one
     BUTTON < BG             -- A/B clearly darker than the case
     PILL   < BG             -- Start/Select/MENU a little darker than the
                                case (a smaller gap than BUTTON's, on
                                purpose -- the photo's pills are much closer
                                to the case tone than the magenta buttons)
   Everything else here is free to retune. */
#define BG        0xD0   /* case: warm light grey */
#define CASE_HI   0xE8   /* raised edge, lighter than the case            */
#define CASE_LO   0xA0   /* recess, darker than the case                  */
#define DPAD      0x20   /* d-pad: near-black -- the darkest thing on the case */
#define BUTTON    0x70   /* A/B discs: dark, clearly darker than the case */
#define PILL      0xB4   /* Start/Select/MENU: a little darker than the case */
#define DARK      0x55   /* bezel / d-pad ridge & hub highlight            */
#define INK       0x00
#define ACCENT_HI 0x30   /* strapline accent rule, upper: navy on the real DMG   */
#define ACCENT_LO 0x78   /* strapline accent rule, lower: maroon on the real DMG */

static int perm(int v, int total) { return v * total / 1000; }

static int min2(int a, int b) { return a < b ? a : b; }

/* --- the LCD layout's shared geometry. Contracts and rationale in chrome.h.
   Defined ABOVE chrome_controls_top because that function calls the first of
   them; the rest sit here with it so the three are read together. */

int chrome_lcd_strip_h(int panel_h)
{
    /* 250 permille. Why that number, and why the old 72 permille was wrong,
       is in chrome.h -- it is a measurement about what the strip has to hold,
       not a taste. No floor: see chrome.h. */
    return perm(250, panel_h);
}

/* Contract, and the rationale for the diamond's arrangement, in chrome.h.

   Two bands, because the strip carries two different KINDS of control and
   mixing them costs reachability. The UPPER band holds the things a thumb
   rests on -- the d-pad under the left one, the face diamond under the
   right, MENU parked in the dead centre column between them where neither
   thumb goes. The LOWER band holds the things pressed deliberately and
   rarely: L1, SELECT, START, R1 in one row, with the battery lamp at its
   left end.

   Every dimension is a fraction of the band it lives in rather than of the
   panel, so the whole strip scales with chrome_lcd_strip_h and one number
   governs the lot. The two extents that matter are bounded BY CONSTRUCTION
   rather than by a clamp, which is what lets tests assert containment as an
   equality rather than an inequality:
     d-pad  reaches 42/100 of the upper band from its centre  (< 1/2)
     diamond reaches face_r + face_off = 26/10 * face_r
                                       = 26/10 * 19/100 = 494/1000  (< 1/2)
   so neither can leave the band it belongs to however tall the strip is. */
void chrome_lcd_layout(const koboy_profile *p, chrome_lcd_controls *o)
{
    const int W = p->panel_w, H = p->panel_h;
    memset(o, 0, sizeof *o);

    /* No "strip taller than the panel" guard, and that is a proof rather than
       an oversight: chrome_lcd_strip_h is 250 permille of H, so strip <= H for
       every H >= 0 and sy below can never be negative. This file's own
       convention (see round_out_corner, and the corner-radius note in
       chrome_render) is that a guard which cannot fire is removed and replaced
       by the reason, not left lying around looking defensive. */
    int strip = chrome_lcd_strip_h(H);
    int sy = H - strip;
    o->strip.x = 0; o->strip.y = sy; o->strip.w = W; o->strip.h = strip;

    int upper_h = strip * 68 / 100;
    int lower_h = strip - upper_h;
    int ly      = sy + upper_h;

    /* --- upper band: d-pad, face diamond, MENU */
    o->dpad_r  = upper_h * 42 / 100;
    o->dpad_cx = W * 16 / 100;
    o->dpad_cy = sy + upper_h / 2;

    o->face_r   = upper_h * 19 / 100;
    o->face_off = o->face_r * 8 / 5;    /* > face_r * sqrt(2): see chrome.h */
    int fcx = W - W * 16 / 100;         /* mirrored, not a second constant */
    int fcy = o->dpad_cy;
    o->x_cx = fcx;                  o->x_cy = fcy - o->face_off;   /* NORTHEAST */
    o->y_cx = fcx - o->face_off;    o->y_cy = fcy;
    o->a_cx = fcx + o->face_off;    o->a_cy = fcy;
    o->b_cx = fcx;                  o->b_cy = fcy + o->face_off;   /* SOUTHEAST */

    /* The centre column is whatever the two thumb clusters leave. Derived
       from their real extents, not from a fixed fraction of the panel, so it
       cannot overlap them on a panel geometry nobody has measured. */
    int col_l = o->dpad_cx + o->dpad_r + KOBOY_CHROME_MARGIN;
    int col_r = fcx - (o->face_r + o->face_off) - KOBOY_CHROME_MARGIN;
    int col_cx = (col_l + col_r) / 2;
    int col_w  = col_r - col_l;

    o->menu.w = W * 22 / 100;
    if (o->menu.w > col_w) o->menu.w = col_w;
    /* LIVE GUARD, and it is about REACHABILITY, not about memory: col_w goes
       to nothing on a panel narrow enough for the two thumb clusters to meet
       in the middle, and a zero-width MENU is a device with no way back to
       the ROM browser. A cramped MENU that overlaps a button is recoverable;
       an absent one is not. */
    if (o->menu.w < 16) o->menu.w = 16;
    o->menu.h = upper_h * 34 / 100;
    o->menu.x = col_cx - o->menu.w / 2;
    o->menu.y = sy + upper_h * 12 / 100;
    if (o->menu.x < 0) o->menu.x = 0;

    /* --- lower band: the battery lamp, then four pills */
    o->bat_r  = lower_h * 22 / 100;
    if (o->bat_r < 4) o->bat_r = 4;
    o->bat_cx = 2 * KOBOY_CHROME_MARGIN + o->bat_r;
    /* Two fifths down the band, not centred: "BATTERY" is captioned BELOW the
       lamp (as it is in the DMG layout), so the pair has to be centred, not
       the disc alone. */
    o->bat_cy = ly + lower_h * 40 / 100;

    int px0  = o->bat_cx + o->bat_r + KOBOY_CHROME_MARGIN;
    int px1  = W - 2 * KOBOY_CHROME_MARGIN;
    int cell = (px1 - px0) / 4;
    if (cell < 8) cell = 8;             /* LIVE GUARD: see menu.w above */
    int pcy  = ly + lower_h / 2;
    int ph   = lower_h * 56 / 100;
    /* L1 and R1 are drawn NARROWER than SELECT and START inside cells of the
       same width, which is the whole of what makes them read as shoulder
       buttons rather than a fifth and sixth pill. Their CENTRES stay on the
       even grid, so the row is evenly spaced whatever the widths are. */
    int wide  = cell - W * 2 / 100;  if (wide  < 8) wide  = 8;
    int small = cell * 60 / 100;     if (small < 8) small = 8;
    const int ws[4] = { small, wide, wide, small };
    koboy_rect *slot[4] = { &o->l1, &o->select, &o->start, &o->r1 };
    for (int i = 0; i < 4; i++) {
        int cx = px0 + cell * i + cell / 2;
        slot[i]->w = ws[i];
        slot[i]->h = ph;
        slot[i]->x = cx - ws[i] / 2;
        slot[i]->y = pcy - ph / 2;
    }
}

void chrome_lcd_menu_rect(const koboy_profile *p, koboy_rect *out)
{
    chrome_lcd_controls c;
    chrome_lcd_layout(p, &c);
    *out = c.menu;
}

void chrome_lcd_battery(const koboy_profile *p, int *cx, int *cy, int *r)
{
    chrome_lcd_controls c;
    chrome_lcd_layout(p, &c);
    *cx = c.bat_cx; *cy = c.bat_cy; *r = c.bat_r;
}

/* Contract and rationale in chrome.h. Every term below is the exact expression
   the corresponding draw call in chrome_render() uses for its top edge, so the
   two cannot drift: box() spans cy - h/2, disc() spans cy - r, and frame() with
   thickness t reaches t-1 rows above its y.

   tests/test_chrome.c duplicates this exact chain independently (computing
   its own expected minimum from the layout, not by calling this function)
   and asserts EQUALITY, at all four supported panel sizes. That duplication
   is deliberate: an inequality check against a pixel sample only catches a
   deleted term when that term happens to be the chain's current binding
   minimum -- five of the seven terms here were provably unguarded that way
   before this comment was written (see the task report's mutant table). An
   independent equality check catches all seven, because deleting any one of
   them changes this function's return value away from what the test believes
   is correct, full stop. */
int chrome_controls_top(int layout_mode, const koboy_layout *l,
                        int panel_w, int panel_h)
{
    const int W = panel_w, H = panel_h;

    /* The LCD layout draws no permille controls at all -- see chrome.h. Its
       whole control band IS the bottom strip, so the game rect must stop
       exactly where the strip starts. */
    if (layout_mode == KOBOY_LAYOUT_LCD) {
        int top = H - chrome_lcd_strip_h(H);
        if (top < 0) top = 0;
        return top;
    }

    int dcy = perm(l->dpad_cy, H), dr = perm(l->dpad_r, W);
    int arm = dr / 3;

    int top = dcy - dr - 1;                    /* vertical arm + its INK frame */
    top = min2(top, dcy - arm / 2 - 1);        /* horizontal arm + its frame   */
    top = min2(top, perm(l->a_cy, H) - perm(l->a_r, W));
    top = min2(top, perm(l->b_cy, H) - perm(l->b_r, W));
    top = min2(top, perm(l->start_cy, H) - perm(l->start_h, H) / 2);
    top = min2(top, perm(l->select_cy, H) - perm(l->select_h, H) / 2);
    top = min2(top, perm(l->menu_cy, H) - perm(l->menu_h, H) / 2);
    if (top < 0) top = 0;
    return top;
}

/* The fine clamps below (x0 < 0 / x1 >= W in hline, y0 < 0 / y1 >= H in
   vline) are live, not dead code: frame() reaches them whenever a rect's
   horizontal and vertical margins differ, decoupling the row/column that
   trips the coarse skip from the range that needs trimming. The horizontal-
   and vertical-overflow guard-band tests in tests/test_chrome.c exercise
   exactly this path. Do not remove them as "unreachable". */
static void hline(uint8_t *fb, int stride, int W, int H, int x0, int x1, int y, uint8_t v)
{
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y < 0 || y >= H) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= W) x1 = W - 1;
    if (x0 > x1) return;
    memset(fb + (size_t)y * stride + x0, v, (size_t)(x1 - x0 + 1));
}

static void vline(uint8_t *fb, int stride, int W, int H, int x, int y0, int y1, uint8_t v)
{
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (x < 0 || x >= W) return;
    if (y0 < 0) y0 = 0;
    if (y1 >= H) y1 = H - 1;
    if (y0 > y1) return;
    for (int y = y0; y <= y1; y++) fb[(size_t)y * stride + x] = v;
}

static void frame(uint8_t *fb, int stride, int W, int H, int x, int y, int w, int h, int t, uint8_t v)
{
    for (int i = 0; i < t; i++) {
        hline(fb, stride, W, H, x - i, x + w - 1 + i, y - i, v);
        hline(fb, stride, W, H, x - i, x + w - 1 + i, y + h - 1 + i, v);
        vline(fb, stride, W, H, x - i, y - i, y + h - 1 + i, v);
        vline(fb, stride, W, H, x + w - 1 + i, y - i, y + h - 1 + i, v);
    }
}

/* A solid rectangle, via hline row by row -- frame() only ever drew a
   border, and the bezel bands below are moulded case, not a hairline. Bounds
   are clamped here too (not just inside hline), the same belt-and-braces
   reasoning chrome_bands documents: the caller passes raw game-rect
   arithmetic straight through, and this is the file that does the write. */
static void fill_rect(uint8_t *fb, int stride, int W, int H, int x0, int y0, int x1, int y1, uint8_t v)
{
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (x0 < 0) x0 = 0;
    if (x1 >= W) x1 = W - 1;
    if (y0 < 0) y0 = 0;
    if (y1 >= H) y1 = H - 1;
    /* LIVE GUARD, and it took the LCD layout to expose it. An EMPTY band --
       x0 > x1 after clamping, which is what "the game rect touches the panel
       edge, so the side band has no width" produces -- must draw nothing. It
       used to draw a one-pixel column INSIDE the rect: hline() swaps an
       unordered pair before clamping, so the empty range (0, -1) came back as
       (-1, 0) and painted column 0. Measured as 1530 stray pixels (two
       columns x 765 rows) on the full-width Mickey Mouse rect, straight
       through chrome_render's "never writes inside the game rect" contract.
       The DMG faceplate never has a zero-width band, which is why this sat
       here unexercised until now. */
    if (x0 > x1 || y0 > y1) return;
    for (int y = y0; y <= y1; y++) hline(fb, stride, W, H, x0, x1, y, v);
}

static void disc(uint8_t *fb, int stride, int W, int H, int cx, int cy, int r, uint8_t v)
{
    for (int y = -r; y <= r; y++) {
        int py = cy + y;
        if (py < 0 || py >= H) continue;
        for (int x = -r; x <= r; x++) {
            if (x * x + y * y <= r * r) {
                int px = cx + x;
                if (px >= 0 && px < W) fb[(size_t)py * stride + px] = v;
            }
        }
    }
}

/* disc()'s outline: same membership test, restricted to the outer two-pixel
   shell, so the battery lamp reads as a ring around its fill rather than a
   second solid disc sitting on top of it. */
static void ring(uint8_t *fb, int stride, int W, int H, int cx, int cy, int r, uint8_t v)
{
    int inner = r - 2;
    if (inner < 0) inner = 0;
    for (int y = -r; y <= r; y++) {
        int py = cy + y;
        if (py < 0 || py >= H) continue;
        for (int x = -r; x <= r; x++) {
            int d2 = x * x + y * y;
            if (d2 <= r * r && d2 > inner * inner) {
                int px = cx + x;
                if (px >= 0 && px < W) fb[(size_t)py * stride + px] = v;
            }
        }
    }
}

/* Cuts a quarter-disc of `v` into one outer corner of a rectangle, so a
   filled frame reads as a moulded, rounded case rather than a picture frame
   with square corners -- the biggest remaining shape mismatch against the
   reference photo once the band asymmetry itself was right.
   (qx, qy) points FROM the corner INTO the rectangle (e.g. qx=+1 for a LEFT
   corner, which rounds inward to the right; qy=+1 for a TOP corner). Every
   candidate pixel lies within the r x r box between the corner and
   (cx + qx*r, cy + qy*r), which is what makes the call sites' radius bound
   safe: a radius no larger than the thinner of the two bands meeting at that
   corner can only remove bezel material that band already owned, never
   reach past it -- see the call sites in chrome_render for the actual
   bound. This function does not enforce that bound itself; it draws
   whatever quarter-disc it is asked for, same as disc() and box() do not
   know about the game rect either.

   The circle is centred on the box's INNER corner -- (r-1, r-1) in the
   (dx, dy) frame below, i.e. the pixel deepest into the rectangle, not the
   true outer corner (cx, cy) itself. Centring it AT the outer corner was
   the first version's bug: with the circle at (0, 0), "distance > r" is
   true for almost the whole box except a blob near the box's FAR corner,
   which cuts a floating chevron out of the rectangle's interior and leaves
   the true corner sharp -- exactly backwards, and it shipped once, caught
   only by zooming into a rendered golden pixel-for-pixel (see the task 15
   report's corner crops). Centring on the inner corner instead makes the
   true corner the point FARTHEST from the centre,
   so it is reliably the first pixel cut, and the cut traces a proper
   quarter circle out to the two tangent points at (r-1, 0) and (0, r-1). */
static void round_out_corner(uint8_t *fb, int stride, int W, int H,
                             int cx, int cy, int r, int qx, int qy, uint8_t v)
{
    int cr = r - 1;
    for (int dy = 0; dy < r; dy++) {
        int py = cy + qy * dy;
        if (py < 0 || py >= H) continue;
        for (int dx = 0; dx < r; dx++) {
            int ex = dx - cr, ey = dy - cr;         /* offset from the INNER corner */
            if (ex * ex + ey * ey <= cr * cr) continue;   /* inside the round: leave the bezel alone */
            int px = cx + qx * dx;
            if (px >= 0 && px < W) fb[(size_t)py * stride + px] = v;
        }
    }
}

static void box(uint8_t *fb, int stride, int W, int H, int cx, int cy, int w, int h, uint8_t v)
{
    for (int y = cy - h / 2; y <= cy + h / 2; y++) {
        if (y < 0 || y >= H) continue;
        int x0 = cx - w / 2;
        int x1 = x0 + w - 1;
        if (x0 < 0) x0 = 0;
        if (x1 >= W) x1 = W - 1;
        if (x0 <= x1) memset(fb + (size_t)y * stride + x0, v, (size_t)(x1 - x0 + 1));
    }
}

/* text_draw_centred centres on the whole panel; the battery lamp needs to
   centre on an arbitrary x instead (the lamp's own cx, not the panel's), which
   is the same arithmetic with that one substitution. */
static void text_draw_centred_at(uint8_t *fb, int stride, int W, int H,
                                 int cx, int y, const char *s, int px, uint8_t ink)
{
    text_draw(fb, stride, W, H, cx - text_measure(s, px) / 2, y, s, px, ink);
}

/* The d-pad cross, drawn from a centre and an arm half-length. BOTH layouts
   call this: the DMG faceplate's cross and the Game & Watch strip's are the
   same object at different sizes and positions, and a Game & Watch title's
   own directions (Donkey Kong needs a full four-way; Mickey Mouse binds two
   diagonals to up/down) are steered with exactly the same shape. Extracted
   rather than copied for the reason chrome.h's header gives about the
   battery lamp: a second copy is a second chance to get it wrong.

   Solid DPAD (near-black) arms -- the darkest thing on the case, per the
   reference photo, and the single most recognisable shape on a DMG's lower
   half. Round 1 filled these with a near-white tone, which read as a hole
   punched in the case rather than a raised control; DPAD is chosen to sit
   clearly below DARK (the bezel tone) too, so the d-pad and the bezel do not
   blur into "the same dark thing" even though they are the two darkest tones
   on the panel. */
static void draw_dpad(uint8_t *fb, int stride, int W, int H, int dcx, int dcy, int dr)
{
    int arm = dr / 3;
    box(fb, stride, W, H, dcx, dcy, arm, 2 * dr, DPAD);
    box(fb, stride, W, H, dcx, dcy, 2 * dr, arm, DPAD);
    frame(fb, stride, W, H, dcx - arm / 2, dcy - dr, arm, 2 * dr, 2, INK);
    frame(fb, stride, W, H, dcx - dr, dcy - arm / 2, 2 * dr, arm, 2, INK);

    /* Centre boss and ridges, matching the photo's moulded cross rather
       than a flat one. Purely cosmetic pixels drawn INSIDE the cross just
       filled above -- dcx/dcy/dr/arm are untouched, so chrome_controls_top's
       d-pad terms (computed from those same four values, see this file's
       top) cannot drift from what is actually drawn, and input.c's touch
       zone, built from the same layout permille, stays exactly as wide as
       it always was.
       Round 1's ridges were full-width ink lines spanning the WHOLE arm at
       even intervals, which subdivided each arm into a row of visibly
       separate boxes -- a segmented grid, not a ridged surface. Fixed two
       ways: the ridge colour is now DARK (a lighter tone scored onto the
       near-black DPAD fill, a surface highlight rather than a same-colour
       divider line), and each ridge is SHORTER than the arm's own width
       (ridge_w, not the full arm) and clustered near the outer THIRD of
       each arm -- three per tip -- rather than evenly spaced hub-to-tip. */
    int hub_r = arm / 2;
    if (hub_r > 0) {
        disc(fb, stride, W, H, dcx, dcy, hub_r, DARK);
        ring(fb, stride, W, H, dcx, dcy, hub_r, INK);
    }
    int ridge_w = arm * 2 / 3;
    if (ridge_w < 1) ridge_w = 1;
    int tip_span = dr / 3;
    for (int k = 1; k <= 3; k++) {
        int off = dr - (tip_span * k) / 4;
        if (off <= hub_r + 2 || off >= dr - 1) continue;
        hline(fb, stride, W, H, dcx - ridge_w / 2, dcx + ridge_w / 2, dcy - off, DARK);
        hline(fb, stride, W, H, dcx - ridge_w / 2, dcx + ridge_w / 2, dcy + off, DARK);
        vline(fb, stride, W, H, dcx - off, dcy - ridge_w / 2, dcy + ridge_w / 2, DARK);
        vline(fb, stride, W, H, dcx + off, dcy - ridge_w / 2, dcy + ridge_w / 2, DARK);
    }
}

void chrome_bands(const koboy_profile *p, int panel_w, int *left, int *right_start)
{
    int lx = p->game_x;                     /* width of the left band */
    if (lx < 0) lx = 0;
    if (lx > panel_w) lx = panel_w;
    int rx = p->game_x + p->game_w;         /* first column right of the rect */
    if (rx < 0) rx = 0;
    if (rx > panel_w) rx = panel_w;
    *left = lx;
    *right_start = rx;
}

/* No audio has shipped: this build is silent end to end, and the historic DMG
   strapline ("DOT MATRIX WITH STEREO SOUND") would be a false claim printed on
   a public faceplate. The Bluetooth plan is written but not executed, so it
   buys nothing yet -- swap this back to the stereo-sound wording only once
   that task lands and audio is real, not when it merely exists on paper. */
static const char STRAPLINE[] = "DOT MATRIX ON ELECTRONIC PAPER";

/* Fits a label to a box and draws it centred, returning nothing: the exact
   loop the MENU pill used before the strip grew nine more labelled controls.
   Extracted because "pick the largest px that still clears the box" is the
   one sizing rule every label on this faceplate follows, and nine copies of
   it is nine chances for one of them to overflow its box unnoticed. `max_px`
   caps a short word in a big box, which is what keeps a two-character label
   from being drawn twice the height of a six-character one beside it. */
static void label_in_box(uint8_t *fb, int stride, int W, int H,
                         int bx, int by, int bw, int bh, const char *s,
                         int max_px, uint8_t ink)
{
    int px = 1;
    while (px < max_px &&
          text_measure(s, px + 1) <= bw - 8 &&
          TEXT_GLYPH_H * (px + 1) <= bh - 8)
        px++;
    text_draw_centred_at(fb, stride, W, H, bx + bw / 2,
                         by + (bh - TEXT_GLYPH_H * px) / 2, s, px, ink);
}

/* One pill: PILL fill, INK frame, label inside. Start/Select/MENU on the DMG
   faceplate are drawn this way and so is every pill in the LCD strip, which
   is the point -- someone who has used the DMG faceplate already knows what
   a pill with a word in it does. */
static void draw_pill(uint8_t *fb, int stride, int W, int H,
                      const koboy_rect *r, const char *label)
{
    fill_rect(fb, stride, W, H, r->x, r->y, r->x + r->w - 1, r->y + r->h - 1, PILL);
    frame(fb, stride, W, H, r->x, r->y, r->w, r->h, 2, INK);
    label_in_box(fb, stride, W, H, r->x, r->y, r->w, r->h, label, 4, INK);
}

/* One face-button disc with its RETROPAD name in it. The label goes INSIDE
   rather than below, unlike the DMG's A and B: those have a whole case band
   under them and these do not, and an unlabelled disc in a diamond of four
   is exactly the "four indistinguishable grey shapes" text.c was added to
   stop.

   The names are X / Y / A / B and NOT the DMG's own two, deliberately: the
   gw core's own overlay speaks retropad ("NORTHEAST" sits over the SNES
   pad's TOP button, which is X), and a label that disagreed with the overlay
   would send a user to the wrong button every time they consulted it. */
static void draw_face_button(uint8_t *fb, int stride, int W, int H,
                             int cx, int cy, int r, const char *label)
{
    disc(fb, stride, W, H, cx, cy, r, BUTTON);
    ring(fb, stride, W, H, cx, cy, r, INK);
    /* Inscribed square of the disc, so a label can never poke out of the
       circle it is centred in: half-side = r / sqrt(2), and 7/10 is just
       under 1/sqrt(2) = 0.7071. */
    int side = r * 7 / 10 * 2;
    label_in_box(fb, stride, W, H, cx - side / 2, cy - side / 2, side, side,
                 label, 4, BG);
}

/* The LCD faceplate: a case, a recess around a game rect that runs the full
   panel width, and one bottom strip carrying a FULL RETROPAD.

   The strip's controls are the correction this layout's first version
   needed. That version drew none, on the theory that a Game & Watch title
   exposes its own on-artwork buttons to a pointer. Measured against the
   shipped .mgw collection, that theory is false: those files route through
   gwlua's compat init, which has no pointer handling at all, so a pointer
   press anywhere on the artwork changes ZERO pixels while a joypad press
   changes 211k. Every one of these titles is driven by per-title RETROPAD
   bindings -- Mickey Mouse (Wide Screen) uses up/down/x/b for its four
   diagonals and l1/r1 for GAME A / GAME B; Donkey Kong (Multi Screen) uses
   the full cross plus b for JUMP -- and koboy cannot know which. So the
   strip exposes the WHOLE set rather than guessing: a d-pad, the X/Y/A/B
   diamond, SELECT and START (without which no round can be started at all),
   and L1/R1.

   No strapline. In the DMG layout it sits in the top bezel band, whose
   height that layout guarantees; here the game rect is allowed to reach
   y = 0, so there is no band to guarantee it a home.

   Obeys the same contract as chrome_render: it must never write inside the
   game rect. Every element below is either in the bottom strip or clipped to
   the recess band, and the game rect is filled by nothing at all. */
static void chrome_render_lcd(uint8_t *fb, int stride, const koboy_profile *p)
{
    const int W = p->panel_w, H = p->panel_h;
    int gx0 = p->game_x, gy0 = p->game_y;
    int gx1 = p->game_x + p->game_w - 1, gy1 = p->game_y + p->game_h - 1;

    /* Case everywhere except the game rect, exactly as the DMG path does it
       and with the same clamped band widths for the same heap-overrun
       reason -- see chrome_bands. */
    int lx, rx;
    chrome_bands(p, W, &lx, &rx);
    for (int y = 0; y < H; y++) {
        if (y >= gy0 && y <= gy1) {
            memset(fb + (size_t)y * stride, BG, (size_t)lx);
            memset(fb + (size_t)y * stride + rx, BG, (size_t)(W - rx));
        } else {
            memset(fb + (size_t)y * stride, BG, (size_t)W);
        }
    }

    /* A recess band around the rect. Symmetric here, unlike the DMG bezel's
       deliberate bottom-heavy asymmetry: that asymmetry exists to make a
       rectangle read as a DMG handheld, and this is not one. fill_rect clamps,
       which is what makes a full-width game rect (left/right bands of zero
       width, the Mickey Mouse case) draw correctly rather than needing its
       own branch. */
    int bez = perm(6, W);
    if (bez < 3) bez = 3;
    fill_rect(fb, stride, W, H, gx0 - bez, gy0 - bez, gx1 + bez, gy0 - 1, DARK);
    fill_rect(fb, stride, W, H, gx0 - bez, gy1 + 1, gx1 + bez, gy1 + bez, DARK);
    fill_rect(fb, stride, W, H, gx0 - bez, gy0, gx0 - 1, gy1, DARK);
    fill_rect(fb, stride, W, H, gx1 + 1, gy0, gx1 + bez, gy1, DARK);
    frame(fb, stride, W, H, gx0 - bez, gy0 - bez,
          (gx1 + bez) - (gx0 - bez) + 1, (gy1 + bez) - (gy0 - bez) + 1, 1, CASE_HI);

    chrome_lcd_controls c;
    chrome_lcd_layout(p, &c);

    /* One hairline to separate the strip from the case above -- the strip
       carries live controls and the case does not, and on a panel with no
       colour a tone change is the only way to say so. */
    hline(fb, stride, W, H, 0, W - 1, c.strip.y, CASE_LO);
    hline(fb, stride, W, H, 0, W - 1, c.strip.y + 1, CASE_HI);

    draw_dpad(fb, stride, W, H, c.dpad_cx, c.dpad_cy, c.dpad_r);

    /* The diamond, in the arrangement the core's own overlay uses. Drawn in
       the order X, Y, A, B purely so the source reads top, left, right,
       bottom; they do not overlap (see chrome.h on face_off). */
    draw_face_button(fb, stride, W, H, c.x_cx, c.x_cy, c.face_r, "X");
    draw_face_button(fb, stride, W, H, c.y_cx, c.y_cy, c.face_r, "Y");
    draw_face_button(fb, stride, W, H, c.a_cx, c.a_cy, c.face_r, "A");
    draw_face_button(fb, stride, W, H, c.b_cx, c.b_cy, c.face_r, "B");

    draw_pill(fb, stride, W, H, &c.l1,     "L1");
    draw_pill(fb, stride, W, H, &c.select, "SELECT");
    draw_pill(fb, stride, W, H, &c.start,  "START");
    draw_pill(fb, stride, W, H, &c.r1,     "R1");
    draw_pill(fb, stride, W, H, &c.menu,   "MENU");

    /* Wordmark, in the centre column under MENU -- the only decoration the
       strip has room for, and it goes where the original console puts its own
       logotype: below everything, centred. Sized to the gap actually left
       between MENU and the bottom of the upper band, and skipped outright --
       not crushed -- when there is none. It is decoration, so it is derived
       FROM the controls' geometry and never feeds back into it. */
    int deco_y0 = c.menu.y + c.menu.h + KOBOY_CHROME_MARGIN;
    int deco_y1 = c.l1.y - KOBOY_CHROME_MARGIN;
    if (deco_y1 - deco_y0 > TEXT_GLYPH_H + 2) {
        int avail_h = deco_y1 - deco_y0;
        int word_px = 1;
        while (word_px < 5 &&
              text_measure("koboy", word_px + 1) <= c.menu.w &&
              TEXT_GLYPH_H * (word_px + 1) <= avail_h - 2)
            word_px++;
        text_draw_centred_at(fb, stride, W, H, c.menu.x + c.menu.w / 2,
                             deco_y0 + (avail_h - TEXT_GLYPH_H * word_px) / 2,
                             "koboy", word_px, DARK);
    }
}

void chrome_render(uint8_t *fb, int stride, const koboy_profile *p,
                   const koboy_layout *l)
{
    const int W = p->panel_w, H = p->panel_h;

    if (p->layout_mode == KOBOY_LAYOUT_LCD) {
        /* `l` is genuinely unused by this branch -- the LCD faceplate has no
           permille controls to place. Not dropped from the signature, because
           chrome_render is called from five places that all have a layout and
           do not know which faceplate they are about to get. */
        (void)l;
        chrome_render_lcd(fb, stride, p);
        return;
    }

    /* The left/right band widths come back from chrome_bands already clamped
       into [0, W] before the cast to size_t, exactly as hline/vline clamp, and
       for the same reason: a plain int cast to size_t does not saturate, it
       wraps. A negative game_x or a
       game rect running past the right edge would turn a band width into a
       length near SIZE_MAX and memset the heap flat -- the same underflow
       mechanism that took four rounds and an ASan repro to get out of
       frame()/hline()/vline() in this file.
       config_resolve_profile does keep the invariant for every caller today, so
       the clamps look dead. They are not: they are the local defence, in the
       file that does the writing, so that a change to the resolver in another
       file can never reintroduce a heap overrun here. tests/test_chrome.c
       asserts chrome_bands' clamped output directly for every violating case,
       which is the only way to cover the right-hand band: watching for the
       overrun cannot, because an unclamped `W - rx` hands memset a length near
       SIZE_MAX and glibc on x86-64 then writes inside the panel rather than
       into the guard band, so the sentinel test passes either way. Do not
       remove the clamps as "unreachable". */
    int lx, rx;
    chrome_bands(p, W, &lx, &rx);

    /* background everywhere except the game rect */
    for (int y = 0; y < H; y++) {
        if (y >= p->game_y && y < p->game_y + p->game_h) {
            memset(fb + (size_t)y * stride, BG, (size_t)lx);
            memset(fb + (size_t)y * stride + rx, BG, (size_t)(W - rx));
        } else {
            memset(fb + (size_t)y * stride, BG, (size_t)W);
        }
    }

    /* Asymmetric DARK bezel around the screen recess, replacing the old
       uniform 6px INK hairline. A real DMG's bottom bezel is visibly taller
       than the other three sides, and that asymmetry, more than any other
       single choice, is what makes a rectangle read as a handheld console
       rather than "a screen with a border". Task 15's photo correction: the
       STRAPLINE lives in the TOP band, not the bottom one (the bottom band
       stays plain, and stays the taller of the two -- see top_t/bot_t below).
       Four filled bands, entirely outside the game rect by construction (the
       top band stops at game_y - 1, the bottom band starts at
       game_y + game_h, and the side bands are cut to exactly the rect's own
       y-span), so this can never intrude on it regardless of side_t/top_t/
       bot_t.
       side_t is kept modest on purpose: the mandatory game_h/12 addition to
       bot_t already consumes most of the gap above chrome_controls_top on
       the narrowest supported panel (Clara), and a fatter side_t would eat
       further into the band the wordmark needs below -- see the "free
       full-width band" comment further down. Nudged from 8 to 13 permille in
       task 15 so the rounded corners below have enough width to read as
       rounded rather than merely notched; verified against the sweep test
       that this still leaves the wordmark band non-empty on Clara. */
    int side_t = perm(13, W);
    if (side_t < 5) side_t = 5;
    /* Grown enough to seat the strapline (photo correction: it belongs
       ABOVE the screen, not below), but with a SMALLER bonus than bot_t's
       own -- /24 versus /12, so top_t < bot_t for every game_h > 0 and the
       bottom band is guaranteed to stay the taller of the two without a
       runtime clamp. top_t also never has to fight config_resolve_profile
       for headroom: game_y (the top margin) is fixed at panel_h/20
       regardless of scale, comfortably larger than top_t on every supported
       panel (by 30+ px at scale 5), so by0 below never goes negative and
       this band never eats into the resolver's scale-5 fit -- unlike a
       change to game_y itself would (see config.c's game_y comment for the
       fix-round history this task's brief calls out by name). */
    int top_t = side_t + p->game_h / 24;
    int bot_t = side_t + p->game_h / 12;      /* the asymmetry */

    int bx0 = p->game_x - side_t, bx1 = p->game_x + p->game_w - 1 + side_t;
    int by0 = p->game_y - top_t, by1 = p->game_y + p->game_h - 1 + bot_t;

    fill_rect(fb, stride, W, H, bx0, by0, bx1, p->game_y - 1, DARK);              /* top */
    fill_rect(fb, stride, W, H, bx0, p->game_y + p->game_h, bx1, by1, DARK);      /* bottom */
    fill_rect(fb, stride, W, H, bx0, p->game_y, p->game_x - 1,
             p->game_y + p->game_h - 1, DARK);                                   /* left */
    fill_rect(fb, stride, W, H, p->game_x + p->game_w, p->game_y, bx1,
             p->game_y + p->game_h - 1, DARK);                                   /* right */

    /* The recess lip (a shade lighter than the surrounding case, a shade
       darker than the bezel it sits on) and the bezel's own outer bevel: two
       cosmetic tones this file did not use before, spent here because chrome
       is drawn once and a tonal ramp costs nothing at that price. */
    frame(fb, stride, W, H, p->game_x - 1, p->game_y - 1, p->game_w + 2, p->game_h + 2, 2, CASE_LO);
    frame(fb, stride, W, H, bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1, 2, CASE_HI);

    /* Rounded bezel corners, larger at the bottom-right as the photo shows.
       Every radius here is a fraction of side_t, and that alone is what
       makes the bound safe: side_t is the width of every band (top, bottom
       AND both sides) at the point where two bands MEET a corner, since
       top_t = side_t + game_h/24 and bot_t = side_t + game_h/12 are both
       side_t PLUS a non-negative term, i.e. top_t >= side_t and
       bot_t >= side_t always. So a radius no larger than side_t itself can
       only remove bezel material the THINNER of the two meeting bands
       already owned, at every corner, without needing to re-derive which
       band is thinner per corner. Applied after both frame() calls above
       (not before) so the cut also removes the CASE_HI outer-bevel stroke
       at the corner, not just the DARK fill under it -- a square frame()
       drawn after the round would just redraw the sharp corner it was meant
       to remove. */
    /* No ">0" guard here: side_t floors at 5 above, so corner_small = side_t/2
       is at least 2 and corner_big = side_t is at least 5, for every panel
       this file has ever been asked to draw. A guard that can never fire is
       exactly what this file's own convention (see round_out_corner's and
       chrome_bands' comments) says NOT to leave lying around without a note
       explaining why it is still live -- and there is no such note to write
       here truthfully, so the guard is removed rather than dressed up as
       defensive. */
    int corner_small = side_t / 2;
    int corner_big   = side_t;         /* bottom-right: the photo's larger radius */
    round_out_corner(fb, stride, W, H, bx0, by0, corner_small, +1, +1, BG);
    round_out_corner(fb, stride, W, H, bx1, by0, corner_small, -1, +1, BG);
    round_out_corner(fb, stride, W, H, bx0, by1, corner_small, +1, -1, BG);
    round_out_corner(fb, stride, W, H, bx1, by1, corner_big, -1, -1, BG);

    /* Two short accent rules flank the strapline on each side (one pair
       left, one pair right), echoing the real DMG's two colour rules either
       side of its strap -- navy over maroon there, two distinct greys off
       this file's ramp here, since the panel has no colour. Their size is
       fixed BEFORE the strapline's own px is chosen, and that ordering
       matters: reserving `2 * rule_reserve` in the strapline's own width
       budget below guarantees room for both rules by construction, rather
       than hoping whatever px the text lands on happens to leave slack.
       Without the reservation the text greedily claims the width first
       (that was the task 15 first pass -- 90% of game_w gone to the string,
       leaving less on each side than one gap+rule needs), and the rules
       silently never draw on any supported panel, which no test here would
       catch since drawing nothing is not a crash. */
    int rule_gap = perm(6, W);  if (rule_gap < 2) rule_gap = 2;
    int rule_len = perm(20, W); if (rule_len < 6) rule_len = 6;
    int rule_margin = perm(10, W);
    int rule_reserve = rule_gap + rule_len + rule_margin;

    /* Strapline, centred in the TOP band -- the single biggest "reads as the
       real handheld" correction in task 15: it shipped in the bottom band,
       and the real one sits above the screen. Sized to the widest px that
       still clears the band's own height, the game rect's width (with a
       margin), AND the two rules' reserved width, so the one string fits
       every supported panel without a per-device table.
       Text stays "DOT MATRIX ON ELECTRONIC PAPER" -- the real DMG's strap
       claims stereo sound, and this build has none; see STRAPLINE's own
       comment. Do not "correct" the wording toward the photo. */
    int strap_px = 1;
    while (strap_px < 8 &&
          text_measure(STRAPLINE, strap_px + 1) <= p->game_w - perm(20, W) - 2 * rule_reserve &&
          TEXT_GLYPH_H * (strap_px + 1) <= top_t - 6)
        strap_px++;
    int strap_y = by0 + (top_t - TEXT_GLYPH_H * strap_px) / 2;
    text_draw_centred(fb, stride, W, H, strap_y, STRAPLINE, strap_px, BG);

    /* The rules sit LEVEL with the text rather than stacked above/below it,
       so they cost no extra vertical room and top_t's sizing above never has
       to account for them. The `if` guards stay -- belt and braces against
       the reservation above, and the only thing that keeps this "skipped,
       not crushed" at scales far below anything shipped, where strap_px
       never leaves 1 and the reservation was never actually exercised. */
    int strap_w = text_measure(STRAPLINE, strap_px);
    int strap_l = W / 2 - strap_w / 2;
    int strap_r = W / 2 + strap_w / 2;
    int rule_y = strap_y + TEXT_GLYPH_H * strap_px / 2;
    if (strap_l - rule_gap - rule_len >= bx0 + rule_margin) {
        int rx1 = strap_l - rule_gap, rx0 = rx1 - rule_len;
        hline(fb, stride, W, H, rx0, rx1, rule_y - 1, ACCENT_HI);
        hline(fb, stride, W, H, rx0, rx1, rule_y + 1, ACCENT_LO);
    }
    if (strap_r + rule_gap + rule_len <= bx1 - rule_margin) {
        int rx0 = strap_r + rule_gap, rx1 = rx0 + rule_len;
        hline(fb, stride, W, H, rx0, rx1, rule_y - 1, ACCENT_HI);
        hline(fb, stride, W, H, rx0, rx1, rule_y + 1, ACCENT_LO);
    }

    /* Wordmark: lives in the free full-width band between the bezel's
       bottom edge and the topmost drawn control -- chrome_controls_top()
       already computes exactly that boundary, so reusing it here means the
       wordmark can never grow into a button regardless of how the bezel or
       the layout change later. It does not feed back INTO that function, on
       purpose: it is decoration, not a drawn control or a live touch zone,
       so it has no business in its chain (chrome.h's contract is explicit
       that the chain covers controls and touch zones only).
       koboy sits lower-left, like the original console's own logotype below
       its screen, kept KOBOY_CHROME_MARGIN clear of the panel edge. On the
       tightest supported panel (Clara, 1072x1448) this band is under twenty
       pixels tall at the shipped scale, so the wordmark sizes itself to
       whatever room is actually there instead of assuming Libra 2
       (1264x1680) headroom, and the whole block is skipped -- not crushed
       into garbage -- if some future layout leaves no room at all. */
    int ctrl_top = chrome_controls_top(KOBOY_LAYOUT_DMG, l, W, H);
    int deco_y0 = p->game_y + p->game_h + bot_t;
    int deco_y1 = ctrl_top;
    if (deco_y1 - deco_y0 > TEXT_GLYPH_H + 2) {
        int pad = KOBOY_CHROME_MARGIN;
        int avail_h = deco_y1 - deco_y0;
        int word_avail_w = W - 2 * pad;
        int word_px = 1;
        while (word_px < 6 &&
              text_measure("koboy", word_px + 1) <= word_avail_w &&
              TEXT_GLYPH_H * (word_px + 1) <= avail_h - 2)
            word_px++;
        int word_x = pad;
        int word_y = deco_y0 + (avail_h - TEXT_GLYPH_H * word_px) / 2;
        text_draw(fb, stride, W, H, word_x, word_y, "koboy", word_px, DARK);
    }

    /* The cross, at the DMG layout's permille position. draw_dpad owns the
       shape; chrome_controls_top's d-pad terms are computed from these same
       three values (see this file's top), so the drawn control and the
       reserved band cannot drift. */
    int dcx = perm(l->dpad_cx, W), dcy = perm(l->dpad_cy, H), dr = perm(l->dpad_r, W);
    draw_dpad(fb, stride, W, H, dcx, dcy, dr);

    /* A and B: dark, clearly darker than the case (BUTTON), matching the
       photo's magenta discs read in greyscale -- round 1 shipped these
       near-white, the same "hole punched in the case" mistake as the
       d-pad. */
    int acx = perm(l->a_cx, W), acy = perm(l->a_cy, H), ar = perm(l->a_r, W);
    int bcx = perm(l->b_cx, W), bcy = perm(l->b_cy, H), br = perm(l->b_r, W);
    disc(fb, stride, W, H, acx, acy, ar, BUTTON);
    disc(fb, stride, W, H, bcx, bcy, br, BUTTON);

    /* Labels: A, B, Start and Select centred BELOW their control, exactly
       where the real DMG puts them -- before this task the four were
       indistinguishable grey shapes, and text.c exists so they no longer have
       to be. One px size, picked so the longest of the four ("SELECT") still
       clears its own pill with margin, keeps the row visually consistent. */
    int lbl_px = 1;
    while (lbl_px < 6 &&
          text_measure("SELECT", lbl_px + 1) <= perm(l->select_w, W) - perm(10, W) &&
          TEXT_GLYPH_H * (lbl_px + 1) <= perm(l->menu_h, H) - 6)
        lbl_px++;
    text_draw_centred_at(fb, stride, W, H, acx, acy + ar + perm(6, H), "A", lbl_px, INK);
    text_draw_centred_at(fb, stride, W, H, bcx, bcy + br + perm(6, H), "B", lbl_px, INK);

    /* Start and Select pills. The real ones sit at roughly a 20-degree
       angle; these stay AXIS-ALIGNED, a deliberate simplification recorded
       here rather than an oversight -- angling them needs a sheared fill
       primitive this file does not have AND rotated text, which text.c
       cannot do (its glyph table is drawn column-by-column at 0 degrees
       only). The tilted A/B pair already carries the diagonal feel the
       photo has; adding a shear primitive and a rotated glyph path for two
       pills was judged not worth it against that. */
    int scx = perm(l->start_cx, W), scy = perm(l->start_cy, H);
    int sw = perm(l->start_w, W), sh = perm(l->start_h, H);
    int tcx = perm(l->select_cx, W), tcy = perm(l->select_cy, H);
    int tw = perm(l->select_w, W), th = perm(l->select_h, H);
    /* PILL: a little darker than the case, not "clearly" darker like DPAD
       or BUTTON -- the photo's Start/Select pills sit much closer to the
       case tone than the magenta buttons do, and round 1's near-white MID
       had them backwards (lighter than the case) on top of being too far
       off tone either way. */
    box(fb, stride, W, H, scx, scy, sw, sh, PILL);
    box(fb, stride, W, H, tcx, tcy, tw, th, PILL);
    text_draw_centred_at(fb, stride, W, H, scx, scy + sh / 2 + perm(6, H), "START", lbl_px, INK);
    text_draw_centred_at(fb, stride, W, H, tcx, tcy + th / 2 + perm(6, H), "SELECT", lbl_px, INK);

    /* MENU. Drawn, not hidden behind a gesture: the drawn UI is the part
       people trust, and v1 already learned that the input model has to match
       the drawing -- a relative thumb-pad under a drawn absolute cross was
       unusable. Power still means quit, so a menu that fails to draw can never
       trap the user on a device where a stuck app looks like a brick. */
    int mcx = perm(l->menu_cx, W), mcy = perm(l->menu_cy, H);
    int mw = perm(l->menu_w, W), mh = perm(l->menu_h, H);
    box(fb, stride, W, H, mcx, mcy, mw, mh, PILL);   /* same tone as Start/Select: a koboy-only control, styled like its neighbours */
    frame(fb, stride, W, H, mcx - mw / 2, mcy - mh / 2, mw, mh, 2, INK);
    /* Label INSIDE the box -- exactly where a real DMG puts MENU-equivalent
       markings on its own controls -- rather than below it like the other
       four, which is the one placement rule this element does not share with
       them. */
    text_draw_centred_at(fb, stride, W, H, mcx, mcy - TEXT_GLYPH_H * lbl_px / 2, "MENU", lbl_px, INK);
}

/* One definition of the label and its scale, so the guard below cannot measure
   one string while text_draw_centred_at draws a different one. */
#define BATTERY_LABEL     "BATTERY"
#define BATTERY_LABEL_PX  1

/* The lamp itself, with no opinion about where it goes -- the two layouts put
   it in completely different places (the DMG's left-hand case band, the LCD's
   bottom strip) and this is the part they share. Split out when the LCD
   layout arrived rather than copied, because the fill geometry below has
   already been wrong once on the device ("the battery fill is a rectangle")
   and a second copy is a second chance to get it wrong again. */
static void battery_lamp(uint8_t *fb, int stride, int W, int H,
                         int cx, int cy, int r, int percent)
{
    disc(fb, stride, W, H, cx, cy, r, BG);
    ring(fb, stride, W, H, cx, cy, r, INK);
    if (percent >= 0) {
        /* Fill proportionally, so the lamp says something rather than merely
           existing.

           The fill is a CHORD OF THE LAMP, and the containment test below is
           what makes it one. The previous version ran hline from cx - r to
           cx + r, which is the disc's BOUNDING BOX: the level was painted full
           width and spilled outside the circle, and the ring() afterwards
           merely outlined a circle on top of the overspill. Reported from the
           device as "the battery fill is a rectangle". Same test as disc()
           above, deliberately -- there is one definition of what is inside
           this lamp. */
        int fill = r * 2 * percent / 100;
        int top  = cy + r - fill;
        for (int y = -r; y <= r; y++) {
            int py = cy + y;
            if (py < top || py < 0 || py >= H) continue;
            for (int x = -r; x <= r; x++) {
                if (x * x + y * y > r * r) continue;
                int px = cx + x;
                if (px >= 0 && px < W) fb[(size_t)py * stride + px] = DARK;
            }
        }
        ring(fb, stride, W, H, cx, cy, r, INK);
    }
    text_draw_centred_at(fb, stride, W, H, cx, cy + r + r / 2,
                         BATTERY_LABEL, BATTERY_LABEL_PX, INK);
}

void chrome_render_battery(uint8_t *fb, int stride, const koboy_profile *p,
                           const koboy_layout *l, int percent)
{
    (void)l;
    /* Defensive: percent arrives from a platform backend
       (kobo_battery_percent) that reads a raw sysfs node on hardware
       nobody has tested yet -- the only defined values are 0..100 or -1
       for "unknown", but nothing stops a stray value outside that range
       from arriving, and this file clamps everything else it draws from
       caller-supplied numbers (see chrome_bands' note). Any value below 0
       collapses to the same "unknown" the caller would send explicitly;
       anything above 100 is capped so the fill loop below can never paint
       past the lamp's own circle. Live guard, not dead code. */
    if (percent > 100) percent = 100;
    if (percent < 0) percent = -1;

    const int W = p->panel_w, H = p->panel_h;

    /* THE USER'S EXPLICIT REQUEST: in the LCD layout the battery moves UNDER
       the screen. It has to -- the DMG position is the case band left of the
       game rect, and in this layout that rect runs the full panel width, so
       there is no band to put it in and the DMG guard below (which refuses to
       draw anything that would reach game_x) would silently draw nothing at
       all. */
    if (p->layout_mode == KOBOY_LAYOUT_LCD) {
        int lcx, lcy, lr;
        chrome_lcd_battery(p, &lcx, &lcy, &lr);
        /* Same contract as the DMG guard below, tested on the axis that can
           actually go wrong here: the lamp lives BELOW the game rect, so what
           must be proved is that its topmost row clears the rect's bottom.
           Live: chrome_lcd_strip_h floors at 48 px while the resolver is free
           to hand this a panel so short that the strip is most of it. */
        if (lcy - lr <= p->game_y + p->game_h - 1) return;
        battery_lamp(fb, stride, W, H, lcx, lcy, lr, percent);
        return;
    }

    /* Left of the screen, like the DMG's power LED. */
    int cx = p->game_x / 2;
    int cy = p->game_y + p->game_h / 2;
    int r  = W / 60;
    if (r < 4) r = 4;

    /* Never inside the game rect: the contract chrome_render lives under, and
       this function is called from the same places.

       The extent tested is the LABEL's, not just the disc's. "BATTERY" at
       px = 1 is ~42 px wide while r is W/60 -- five pixels on a small panel --
       so a guard that only cleared the disc let the text run into the game
       rect. Swept against the real resolver, 7,737 panel/scale combinations
       wrote inside the rect that way. All four supported panels happened to be
       clear (the Clara family by 48 px), which is luck, not construction:
       nothing in the layout ties game_x to the width of a word. */
    /* Exactly the rightmost column text_draw_centred_at will touch, expressed
       as an offset from cx: it starts at cx - m/2 and writes m columns, so the
       last one is cx + m - m/2 - 1. Written that way rather than as m/2 so the
       odd-width case is not silently one pixel short. */
    int lw = text_measure(BATTERY_LABEL, BATTERY_LABEL_PX);
    int label_right = lw > 0 ? lw - lw / 2 - 1 : 0;
    int extent = r > label_right ? r : label_right;
    if (cx + extent >= p->game_x) return;

    battery_lamp(fb, stride, W, H, cx, cy, r, percent);
}
