#include "chrome.h"
#include "text.h"
#include <string.h>

/* The faceplate is drawn with KOBOY_REFRESH_FULL -- GC16, sixteen levels. The
   four-level ceiling constrains the GAME RECT only, and chrome is drawn once,
   so depth is free here.

   THE CASE TONE IS A WARM LIGHT GREY, not pure white -- the user's explicit
   choice against the reference photo. An earlier round INVERTED the control
   tones (near-white d-pad/buttons/pills on a grey case read as holes punched
   in the case, not raised controls). The corrected ordering: the case is the
   lightest thing on the lower half, every control is darker, and the d-pad is
   the darkest thing on it -- darker even than the bezel, which is why it has
   its own tone rather than reusing DARK. "Clearly" is pinned at more than one
   ~17-level GC16 step, so the relationships survive quantisation to the
   panel's real 16-level driver:
     DPAD   < DARK (bezel)  -- two different objects, not one
     BUTTON < BG            -- A/B clearly darker than the case
     PILL   < BG            -- a SMALLER gap than BUTTON's, on purpose: the
                               photo's pills sit much closer to the case tone
                               than the magenta buttons
   Everything else is free to retune. */
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

/* --- the LCD layout's shared geometry. Contracts in chrome.h. ABOVE
   chrome_controls_top because that function calls the first of them. */

int chrome_lcd_strip_h(int panel_h)
{
    /* 250 permille, and NO FLOOR -- chrome.h has the measurement behind
       both. */
    return perm(250, panel_h);
}

/* Contract in chrome.h.

   TWO BANDS, because the strip carries two KINDS of control and mixing them
   costs reachability. The UPPER band holds what a thumb rests on -- d-pad
   left, face cluster right, MENU in the dead centre column between them. The
   LOWER band holds what is pressed deliberately and rarely: L1, SELECT,
   START, R1 in one row, battery lamp at its left end.

   Every dimension is a fraction of its own band rather than of the panel, so
   one number (chrome_lcd_strip_h) governs the lot. The two extents that matter
   are bounded BY CONSTRUCTION rather than clamped, which lets tests assert
   containment as an EQUALITY:
     d-pad   reaches 42/100 of the upper band from its centre  (< 1/2)
     diamond reaches face_r + face_off = 26/10 * 19/100 = 494/1000  (< 1/2) */
void chrome_lcd_layout(const koboy_profile *p, chrome_lcd_controls *o)
{
    const int W = p->panel_w, H = p->panel_h;
    memset(o, 0, sizeof *o);

    /* NO "strip taller than the panel" guard, and that is a PROOF:
       chrome_lcd_strip_h is 250 permille of H, so strip <= H for every H >= 0
       and sy can never be negative. This file's convention is that a guard
       which cannot fire is removed and replaced by the reason. */
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

    o->face_r     = upper_h * 19 / 100;
    o->face_off   = o->face_r * 8 / 5;   /* > face_r * sqrt(2): see chrome.h */
    o->face_pitch = o->face_r * 11 / 5;  /* > 2 * face_r:       see chrome.h */
    int fcx = W - W * 16 / 100;         /* mirrored, not a second constant */
    int fcy = o->dpad_cy;
    o->face_n = (p->lcd_face == KOBOY_LCD_FACE_ROWS6)  ? 6
              : (p->lcd_face == KOBOY_LCD_FACE_PAIR2)  ? 2
                                                       : 4;

    /* How far left of fcx the cluster reaches, which the centre column must
       clear: the diamond puts one disc at face_off, the grid a whole column at
       face_pitch, the pair only half of face_off. */
    int face_reach;
    if (o->face_n == 2) {
        /* TWO DISCS, a GBA: A north-east, B south-west, the diagonal the
           hardware puts them on. (Spelt GBA deliberately -- test_chrome.c
           forbids the console's word mark anywhere in this file, comments
           included.) x_* and y_* are LEFT AT ZERO by the memset, and that
           absence is load-bearing: a GBA has no third or fourth face button,
           and inventing two would hand a player controls the machine never had
           (gpSP binds those bits to Turbo A / Turbo B, worse than inert). Both
           consumers gate on face_n, not on the zero -- chrome.h.

           face_off/2 on each axis puts the two centres face_off * sqrt(2)
           apart, the diamond's adjacent-pair spacing, so the same construction
           proves they cannot merge. */
        int d = o->face_off / 2;
        o->a_cx = fcx + d;  o->a_cy = fcy - d;   /* NORTH-EAST */
        o->b_cx = fcx - d;  o->b_cy = fcy + d;   /* SOUTH-WEST */
        face_reach = d + o->face_r;
    } else if (o->face_n == 6) {
        /* TWO ROWS OF THREE, the six-button Mega Drive's own arrangement:
               X  Y  Z      JOYPAD_L  JOYPAD_X  JOYPAD_R
               A  B  C      JOYPAD_Y  JOYPAD_B  JOYPAD_A
           The field names are the retropad BITS, so the assignment below IS
           that table: "the disc that reports JOYPAD_L sits top-left". */
        int step = o->face_pitch;
        int row  = step / 2;
        o->l1_cx = fcx - step; o->l1_cy = fcy - row;
        o->x_cx  = fcx;        o->x_cy  = fcy - row;
        o->r1_cx = fcx + step; o->r1_cy = fcy - row;
        o->y_cx  = fcx - step; o->y_cy  = fcy + row;
        o->b_cx  = fcx;        o->b_cy  = fcy + row;
        o->a_cx  = fcx + step; o->a_cy  = fcy + row;
        face_reach = step + o->face_r;
    } else {
        o->x_cx = fcx;                  o->x_cy = fcy - o->face_off;   /* NORTHEAST */
        o->y_cx = fcx - o->face_off;    o->y_cy = fcy;
        o->a_cx = fcx + o->face_off;    o->a_cy = fcy;
        o->b_cx = fcx;                  o->b_cy = fcy + o->face_off;   /* SOUTHEAST */
        face_reach = o->face_off + o->face_r;
    }

    /* The centre column is whatever the two thumb clusters leave, derived from
       their REAL extents rather than a fixed fraction of the panel: it cannot
       overlap them on an unmeasured geometry, and the wider ROWS6 cluster
       narrows the column instead of colliding with MENU. */
    int col_l = o->dpad_cx + o->dpad_r + KOBOY_CHROME_MARGIN;
    int col_r = fcx - face_reach - KOBOY_CHROME_MARGIN;
    int col_cx = (col_l + col_r) / 2;
    int col_w  = col_r - col_l;

    o->menu.w = W * 22 / 100;
    if (o->menu.w > col_w) o->menu.w = col_w;
    /* LIVE GUARD, about REACHABILITY not memory: col_w goes to nothing on a
       panel narrow enough for the two thumb clusters to meet, and a zero-width
       MENU is a device with no way back to the ROM browser. A cramped MENU
       that overlaps a button is recoverable; an absent one is not. */
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
       lamp, so the PAIR has to be centred, not the disc alone. */
    o->bat_cy = ly + lower_h * 40 / 100;

    int px0  = o->bat_cx + o->bat_r + KOBOY_CHROME_MARGIN;
    int px1  = W - 2 * KOBOY_CHROME_MARGIN;
    /* FOUR PILLS, OR TWO. Under ROWS6 the shoulder bits already have discs in
       the face grid and a Mega Drive has no shoulders anyway -- two controls
       under one name is the labelling bug this layout exists to avoid -- so
       that arrangement carries MODE and START alone. PAIR2 keeps all four: a
       GBA does have L and R, and these outermost slots are the strip's only
       left/right pair. */
    const int np = (o->face_n == 6) ? 2 : 4;
    int cell = (px1 - px0) / np;
    if (cell < 8) cell = 8;             /* LIVE GUARD: see menu.w above */
    int pcy  = ly + lower_h / 2;
    int ph   = lower_h * 56 / 100;
    /* L1 and R1 are drawn NARROWER than SELECT and START inside cells of the
       same width, which is what makes them read as shoulder buttons rather
       than a fifth and sixth pill. Their CENTRES stay on the even grid, so the
       row is evenly spaced whatever the widths are. */
    int wide  = cell - W * 2 / 100;  if (wide  < 8) wide  = 8;
    int small = cell * 60 / 100;     if (small < 8) small = 8;
    const int ws4[4] = { small, wide, wide, small };
    koboy_rect *slot4[4] = { &o->l1, &o->select, &o->start, &o->r1 };
    const int ws2[2] = { wide, wide };
    koboy_rect *slot2[2] = { &o->select, &o->start };
    const int *ws = (np == 2) ? ws2 : ws4;
    koboy_rect **slot = (np == 2) ? slot2 : slot4;
    for (int i = 0; i < np; i++) {
        int cx = px0 + cell * i + cell / 2;
        slot[i]->w = ws[i];
        slot[i]->h = ph;
        slot[i]->x = cx - ws[i] / 2;
        slot[i]->y = pcy - ph / 2;
    }
    /* Explicitly zeroed rather than left as the memset's zeros, so a reader
       sees "absent" is intended and not an unwritten field. Belt and braces:
       in_rect_xywh cannot match a zero-width rect AND the draw path skips them
       by face_n. */
    if (np == 2) {
        memset(&o->l1, 0, sizeof o->l1);
        memset(&o->r1, 0, sizeof o->r1);
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

/* Contract in chrome.h. Every term below is the exact expression the
   corresponding draw call uses for its top edge, so the two cannot drift:
   box() spans cy - h/2, disc() spans cy - r, frame() with thickness t reaches
   t-1 rows above its y.

   tests/test_chrome.c duplicates this chain INDEPENDENTLY (computing its own
   expected minimum from the layout, not by calling this) and asserts EQUALITY
   at all four panel sizes. Deliberate: an inequality check against a pixel
   sample only catches a deleted term when that term is the chain's current
   binding minimum, and five of the seven here were provably unguarded that
   way. An equality check catches all seven. */
int chrome_controls_top(int layout_mode, const koboy_layout *l,
                        int panel_w, int panel_h)
{
    const int W = panel_w, H = panel_h;

    /* The LCD layout draws no permille controls: its whole control band IS the
       bottom strip, so the game rect stops where the strip starts. */
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
    /* The extra discs, the ONLY conditional terms in this chain. An empty slot
       has r == 0, and an unguarded term would compute 0 - 0 = 0 and collapse
       the whole reservation to the top of the panel for every DMG title. The
       guard is the PRESENCE TEST, not a clamp. */
    for (int i = 0; i < KOBOY_MAX_EXTRA_BTNS; i++)
        if (l->extra[i].r > 0)
            top = min2(top, perm(l->extra[i].cy, H) - perm(l->extra[i].r, W));
    top = min2(top, perm(l->start_cy, H) - perm(l->start_h, H) / 2);
    top = min2(top, perm(l->select_cy, H) - perm(l->select_h, H) / 2);
    top = min2(top, perm(l->menu_cy, H) - perm(l->menu_h, H) / 2);
    if (top < 0) top = 0;
    return top;
}

/* The fine clamps below (x0 < 0 / x1 >= W in hline, y0 < 0 / y1 >= H in
   vline) are LIVE, not dead code: frame() reaches them whenever a rect's
   horizontal and vertical margins differ, decoupling the row/column that trips
   the coarse skip from the range needing trimming. tests/test_chrome.c's
   overflow guard-band tests exercise exactly this. DO NOT REMOVE AS
   "UNREACHABLE".

   x1 IS INCLUSIVE, and that is a bug this file has already shipped once: the
   removed speaker grille drew each slash two columns wide behind a length
   clamp testing only the FIRST, so when the unclamped last column landed on
   the right edge the second painted one past it. The equality tests could not
   see it -- only a term-by-term audit did. A caller passing x1 = x + n must
   reserve n, not n - 1. */
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

/* A solid rectangle, via hline row by row -- frame() only draws a border, and
   the bezel bands are moulded case. Bounds are clamped here too, not just in
   hline: the caller passes raw game-rect arithmetic straight through. */
static void fill_rect(uint8_t *fb, int stride, int W, int H, int x0, int y0, int x1, int y1, uint8_t v)
{
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (x0 < 0) x0 = 0;
    if (x1 >= W) x1 = W - 1;
    if (y0 < 0) y0 = 0;
    if (y1 >= H) y1 = H - 1;
    /* LIVE GUARD, exposed by the LCD layout. An EMPTY band (x0 > x1 after
       clamping -- what "the game rect touches the panel edge" produces) must
       draw NOTHING. It used to draw a one-pixel column INSIDE the rect: hline()
       swaps an unordered pair before clamping, so the empty range (0, -1) came
       back as (-1, 0) and painted column 0. MEASURED as 1530 stray pixels on
       the full-width Mickey Mouse rect, straight through chrome_render's
       "never writes inside the game rect" contract. The DMG faceplate never
       has a zero-width band, which is why this sat unexercised. */
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
   shell, so the battery lamp reads as a ring around its fill. */
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

/* Cuts a quarter-disc of `v` into one outer corner of a rectangle, so a filled
   frame reads as a moulded rounded case rather than a picture frame.
   (qx, qy) points FROM the corner INTO the rectangle (qx=+1 for a LEFT corner,
   qy=+1 for a TOP one). Every candidate pixel lies within the r x r box
   between the corner and (cx + qx*r, cy + qy*r), which is what makes the call
   sites' radius bound safe: a radius no larger than the THINNER of the two
   bands meeting at that corner can only remove material that band already
   owned. This function does not enforce that bound; the call sites do.

   THE CIRCLE IS CENTRED ON THE BOX'S INNER CORNER -- (r-1, r-1) in the
   (dx, dy) frame -- not on the true outer corner. Centring it AT the outer
   corner was the first version's SHIPPED bug: with the circle at (0, 0),
   "distance > r" is true for almost the whole box, which cuts a floating
   chevron out of the interior and leaves the true corner sharp -- exactly
   backwards, caught only by zooming into a rendered golden. On the inner
   corner the true corner is the point FARTHEST from the centre, so it is
   reliably cut first and the cut traces a proper quarter circle out to the
   tangent points at (r-1, 0) and (0, r-1). */
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

/* text_draw_centred centres on the whole panel; this centres on an arbitrary
   x -- the same arithmetic with that one substitution. */
static void text_draw_centred_at(uint8_t *fb, int stride, int W, int H,
                                 int cx, int y, const char *s, int px, uint8_t ink)
{
    text_draw(fb, stride, W, H, cx - text_measure(s, px) / 2, y, s, px, ink);
}

/* The d-pad cross, from a centre and an arm half-length. BOTH layouts call it:
   the DMG faceplate's cross and the strip's are the same object at different
   sizes. Extracted rather than copied -- a second copy is a second chance to
   get it wrong.

   Solid DPAD (near-black) arms, the darkest thing on the case. An earlier
   round filled these near-white, which read as a hole punched in the case
   rather than a raised control. DPAD sits clearly below DARK (the bezel tone)
   too, so the two darkest objects do not blur into one. */
static void draw_dpad(uint8_t *fb, int stride, int W, int H, int dcx, int dcy, int dr)
{
    int arm = dr / 3;
    box(fb, stride, W, H, dcx, dcy, arm, 2 * dr, DPAD);
    box(fb, stride, W, H, dcx, dcy, 2 * dr, arm, DPAD);
    frame(fb, stride, W, H, dcx - arm / 2, dcy - dr, arm, 2 * dr, 2, INK);
    frame(fb, stride, W, H, dcx - dr, dcy - arm / 2, 2 * dr, arm, 2, INK);

    /* Centre boss and ridges, matching the photo's moulded cross. Purely
       cosmetic pixels INSIDE the cross already filled -- dcx/dcy/dr/arm are
       untouched, so chrome_controls_top's d-pad terms and input.c's touch zone
       cannot drift from what is drawn.
       An earlier round's ridges were full-width ink lines spanning the WHOLE
       arm at even intervals, which subdivided each arm into visibly separate
       boxes. Fixed two ways: the ridge colour is DARK (a lighter tone scored
       onto the near-black fill -- a highlight, not a same-colour divider), and
       each ridge is SHORTER than the arm's width and clustered near the outer
       THIRD, three per tip, rather than evenly spaced hub-to-tip. */
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

/* No audio has shipped: this build is silent end to end, so the historic DMG
   strapline ("DOT MATRIX WITH STEREO SOUND") would be a FALSE CLAIM printed on
   a public faceplate. Swap it back only once audio is real. */
static const char STRAPLINE[] = "DOT MATRIX ON ELECTRONIC PAPER";

/* Fits a label to a box and draws it centred. Extracted because "the largest
   px that still clears the box" is the one sizing rule every label here
   follows, and nine copies is nine chances to overflow a box unnoticed.
   `max_px` caps a short word in a big box, keeping a two-character label from
   being drawn twice the height of a six-character one beside it. */
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

/* One pill: PILL fill, INK frame, label inside. Both faceplates draw pills
   this way, which is the point -- someone who has used one already knows what
   a pill with a word in it does. */
static void draw_pill(uint8_t *fb, int stride, int W, int H,
                      const koboy_rect *r, const char *label)
{
    fill_rect(fb, stride, W, H, r->x, r->y, r->x + r->w - 1, r->y + r->h - 1, PILL);
    frame(fb, stride, W, H, r->x, r->y, r->w, r->h, 2, INK);
    label_in_box(fb, stride, W, H, r->x, r->y, r->w, r->h, label, 4, INK);
}

/* One face-button disc with its name in it. The label goes INSIDE rather than
   below, unlike the DMG's A and B: those have a case band under them and these
   do not, and an unlabelled disc in a diamond of four is the "four
   indistinguishable grey shapes" text.c was added to stop.

   The name is the CALLER's (koboy_lcd_pad). The default is the RETROPAD's own
   X / Y / A / B, because the gw core's overlay speaks retropad ("NORTHEAST"
   sits over the SNES pad's TOP button, which is X) and a disagreeing label
   would send a user to the wrong button. A console overrides it -- a Mega
   Drive's overlay is the moulding on its own pad, and that calls this bit C. */
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

/* The LCD faceplate: a case, a recess around a full-panel-width game rect, and
   one bottom strip carrying a FULL RETROPAD.

   The strip's controls are the correction this layout's first version needed.
   That version drew none, on the theory that a Game & Watch title exposes its
   own on-artwork buttons to a pointer. MEASURED false: the shipped .mgw files
   route through gwlua's compat init, which has no pointer handling, so a
   pointer press anywhere on the artwork changes ZERO pixels while a joypad
   press changes 211k. These titles use per-title RETROPAD bindings -- Mickey
   Mouse uses up/down/x/b for four diagonals and l1/r1 for GAME A / GAME B,
   Donkey Kong the full cross plus b for JUMP -- and koboy cannot know which,
   so the strip exposes the WHOLE set rather than guessing.

   That same set is why SNES and Mega Drive live here too: it is exactly a SNES
   pad and a superset of a six-button Mega Drive's, and neither fits the DMG
   faceplate's two spare pockets (config_layout_for_rom). The GEOMETRY is
   identical for all of them; only the LABELS change, from `l->lcd`.

   NO STRAPLINE: in the DMG layout it sits in the top bezel band, whose height
   that layout guarantees, and here the game rect may reach y = 0.

   Same contract as chrome_render: NEVER write inside the game rect. */
/* An empty label means "the retropad's own name" (koboy_lcd_pad), which keeps
   a config predating that struct drawing exactly the strip it drew before. NOT
   a defensive clamp: this IS the encoding of the default, and the goldens
   depend on it. */
static const char *lcd_label(const char *set, const char *retropad)
{
    return (set && *set) ? set : retropad;
}

static void chrome_render_lcd(uint8_t *fb, int stride, const koboy_profile *p,
                              const koboy_layout *l)
{
    const int W = p->panel_w, H = p->panel_h;
    int gx0 = p->game_x, gy0 = p->game_y;
    int gx1 = p->game_x + p->game_w - 1, gy1 = p->game_y + p->game_h - 1;

    /* Case everywhere except the game rect, with chrome_bands' clamped widths
       for the same heap-overrun reason the DMG path has. */
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

    /* A recess band around the rect. SYMMETRIC here, unlike the DMG bezel's
       deliberate bottom-heavy asymmetry, which exists to make a rectangle read
       as a DMG handheld. fill_rect's clamp is what makes a full-width game
       rect (zero-width side bands, the Mickey Mouse case) draw correctly
       without its own branch. */
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

    /* One hairline separating the strip from the case: the strip carries live
       controls and the case does not, and on a colourless panel a tone change
       is the only way to say so. */
    hline(fb, stride, W, H, 0, W - 1, c.strip.y, CASE_LO);
    hline(fb, stride, W, H, 0, W - 1, c.strip.y + 1, CASE_HI);

    draw_dpad(fb, stride, W, H, c.dpad_cx, c.dpad_cy, c.dpad_r);

    /* The face buttons, in whichever arrangement this system uses.
       chrome_lcd_layout already decided; this reads its answer rather than
       asking the profile again.
       X and Y only exist in the arrangements that HAVE them: PAIR2 leaves
       their centres at zero, and a disc at (0,0) would be a quarter circle in
       the panel's top-left corner -- found by rendering the strip and looking
       at it. */
    if (c.face_n >= 4) {
        draw_face_button(fb, stride, W, H, c.x_cx, c.x_cy, c.face_r,
                         lcd_label(l->lcd.x, "X"));
        draw_face_button(fb, stride, W, H, c.y_cx, c.y_cy, c.face_r,
                         lcd_label(l->lcd.y, "Y"));
    }
    draw_face_button(fb, stride, W, H, c.a_cx, c.a_cy, c.face_r,
                     lcd_label(l->lcd.a, "A"));
    draw_face_button(fb, stride, W, H, c.b_cx, c.b_cy, c.face_r,
                     lcd_label(l->lcd.b, "B"));
    if (c.face_n == 6) {
        draw_face_button(fb, stride, W, H, c.l1_cx, c.l1_cy, c.face_r,
                         lcd_label(l->lcd.l1, "L1"));
        draw_face_button(fb, stride, W, H, c.r1_cx, c.r1_cy, c.face_r,
                         lcd_label(l->lcd.r1, "R1"));
    } else {
        draw_pill(fb, stride, W, H, &c.l1, lcd_label(l->lcd.l1, "L1"));
        draw_pill(fb, stride, W, H, &c.r1, lcd_label(l->lcd.r1, "R1"));
    }
    draw_pill(fb, stride, W, H, &c.select, lcd_label(l->lcd.select, "SELECT"));
    draw_pill(fb, stride, W, H, &c.start,  "START");
    draw_pill(fb, stride, W, H, &c.menu,   "MENU");

    /* Wordmark, in the centre column under MENU -- the only decoration the
       strip has room for, where the original console puts its logotype. Sized
       to the gap actually left, and SKIPPED rather than crushed when there is
       none. Decoration: derived FROM the controls' geometry, never feeding
       back into it. */
    int deco_y0 = c.menu.y + c.menu.h + KOBOY_CHROME_MARGIN;
    /* Anchored to START, NOT to L1: the two pills share a row, but L1 is
       ABSENT under ROWS6, so a zero-sized l1 made this y = -8 and the wordmark
       silently disappeared on every Mega Drive. Found by rendering the strip
       and looking -- every numeric check in the suite passed. */
    int deco_y1 = c.start.y - KOBOY_CHROME_MARGIN;
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
        /* `l` carries no permille GEOMETRY this branch can use, but it does
           carry `l->lcd` -- what the controls SAY on the loaded system, which
           is the whole of what the LCD faceplate reads from it. */
        chrome_render_lcd(fb, stride, p, l);
        return;
    }

    /* chrome_bands clamps the band widths into [0, W] BEFORE the cast to
       size_t, because an int cast to size_t wraps rather than saturating: a
       negative game_x, or a rect running past the right edge, would turn a
       width into a length near SIZE_MAX and memset the heap flat -- the same
       underflow that took four rounds and an ASan repro to get out of
       frame()/hline()/vline().
       config_resolve_profile keeps the invariant today, so the clamps LOOK
       dead. They are the local defence in the file that does the writing, so a
       change to the resolver elsewhere cannot reintroduce a heap overrun.
       tests/test_chrome.c asserts chrome_bands' clamped output directly, which
       is the ONLY way to cover the right-hand band: an unclamped `W - rx` hands
       memset a near-SIZE_MAX length and glibc on x86-64 writes inside the
       panel rather than into the guard band, so a sentinel test passes either
       way. DO NOT REMOVE AS "UNREACHABLE". */
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

    /* Asymmetric DARK bezel around the screen recess. A real DMG's bottom
       bezel is visibly taller than the other three sides, and that asymmetry
       more than anything else is what makes a rectangle read as a handheld
       rather than "a screen with a border". The STRAPLINE lives in the TOP
       band; the bottom stays plain and stays the taller (see top_t/bot_t).
       Four filled bands, entirely outside the game rect BY CONSTRUCTION -- the
       top stops at game_y - 1, the bottom starts at game_y + game_h, and the
       sides are cut to the rect's own y-span -- so this cannot intrude
       whatever side_t/top_t/bot_t are.
       side_t is kept modest: bot_t's mandatory game_h/12 already consumes most
       of the gap above chrome_controls_top on the narrowest panel (Clara), and
       a fatter side_t eats the band the wordmark needs below. 13 permille (up
       from 8) so the rounded corners read as rounded rather than notched;
       swept to confirm the wordmark band stays non-empty on Clara. */
    int side_t = perm(13, W);
    if (side_t < 5) side_t = 5;
    /* Grown enough to seat the strapline, but with a SMALLER bonus than
       bot_t's -- /24 against /12 -- so top_t < bot_t for every game_h > 0 and
       the bottom band stays the taller WITHOUT a runtime clamp. top_t never
       fights the resolver for headroom either: game_y is fixed at panel_h/20
       regardless of scale, comfortably larger than top_t on every supported
       panel (30+ px at scale 5), so by0 never goes negative. */
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

    /* The recess lip and the bezel's outer bevel: two cosmetic tones, spent
       here because chrome is drawn once. */
    frame(fb, stride, W, H, p->game_x - 1, p->game_y - 1, p->game_w + 2, p->game_h + 2, 2, CASE_LO);
    frame(fb, stride, W, H, bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1, 2, CASE_HI);

    /* Rounded bezel corners, larger at the bottom-right as the photo shows.
       EVERY RADIUS IS A FRACTION OF side_t, and that alone makes the bound
       safe: top_t and bot_t are both side_t PLUS a non-negative term, so
       side_t is the THINNER of the two bands meeting at every corner, and a
       radius no larger than it can only remove material that band already
       owned. Applied AFTER both frame() calls, not before, so the cut also
       removes the CASE_HI outer-bevel stroke -- a square frame() drawn
       afterwards would redraw the sharp corner it was meant to remove.
       NO ">0" guard: side_t floors at 5, so corner_small >= 2 and
       corner_big >= 5 for every panel. A guard that cannot fire is removed and
       replaced by the reason, per this file's convention. */
    int corner_small = side_t / 2;
    int corner_big   = side_t;         /* bottom-right: the photo's larger radius */
    round_out_corner(fb, stride, W, H, bx0, by0, corner_small, +1, +1, BG);
    round_out_corner(fb, stride, W, H, bx1, by0, corner_small, -1, +1, BG);
    round_out_corner(fb, stride, W, H, bx0, by1, corner_small, +1, -1, BG);
    round_out_corner(fb, stride, W, H, bx1, by1, corner_big, -1, -1, BG);

    /* Two short accent rules flank the strapline, echoing the real DMG's navy
       and maroon rules as two greys. THEIR SIZE IS FIXED BEFORE the
       strapline's px is chosen, and the ordering matters: reserving
       `2 * rule_reserve` in the text's width budget guarantees room BY
       CONSTRUCTION. Without it the text greedily claims the width first (the
       first pass gave 90% of game_w to the string) and the rules silently
       never draw on any panel -- which no test catches, because drawing
       nothing is not a crash. */
    int rule_gap = perm(6, W);  if (rule_gap < 2) rule_gap = 2;
    int rule_len = perm(20, W); if (rule_len < 6) rule_len = 6;
    int rule_margin = perm(10, W);
    int rule_reserve = rule_gap + rule_len + rule_margin;

    /* Strapline, centred in the TOP band -- it shipped in the bottom band and
       the real one sits above the screen. Sized to the widest px that clears
       the band's height, the rect's width with a margin, AND the two rules'
       reserved width, so one string fits every panel without a table.
       DO NOT "CORRECT" THE WORDING toward the photo -- see STRAPLINE. */
    int strap_px = 1;
    while (strap_px < 8 &&
          text_measure(STRAPLINE, strap_px + 1) <= p->game_w - perm(20, W) - 2 * rule_reserve &&
          TEXT_GLYPH_H * (strap_px + 1) <= top_t - 6)
        strap_px++;
    int strap_y = by0 + (top_t - TEXT_GLYPH_H * strap_px) / 2;
    text_draw_centred(fb, stride, W, H, strap_y, STRAPLINE, strap_px, BG);

    /* The rules sit LEVEL with the text rather than above/below it, so they
       cost no vertical room and top_t never has to account for them. The `if`
       guards STAY: belt and braces against the reservation above, and the only
       thing keeping this "skipped, not crushed" at scales where strap_px never
       leaves 1 and the reservation was never exercised. */
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

    /* Wordmark: in the free full-width band between the bezel's bottom edge
       and the topmost drawn control. Reusing chrome_controls_top() means it
       can never grow into a button however the bezel or layout change. It does
       NOT feed back into that function: it is decoration, not a control or a
       touch zone, and chrome.h's contract covers only those.
       Lower-left, like the original console's logotype, KOBOY_CHROME_MARGIN
       clear of the edge. On the tightest panel (Clara 1072x1448) this band is
       under twenty pixels tall at the shipped scale, so it sizes itself to the
       room actually there and is SKIPPED rather than crushed when there is
       none. */
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

    /* The cross, at the DMG layout's permille position. chrome_controls_top's
       d-pad terms come from these same three values, so the drawn control and
       the reserved band cannot drift. */
    int dcx = perm(l->dpad_cx, W), dcy = perm(l->dpad_cy, H), dr = perm(l->dpad_r, W);
    draw_dpad(fb, stride, W, H, dcx, dcy, dr);

    /* A and B: BUTTON, clearly darker than the case, matching the photo's
       magenta discs in greyscale. An earlier round shipped these near-white,
       the same "hole punched in the case" mistake as the d-pad. */
    int acx = perm(l->a_cx, W), acy = perm(l->a_cy, H), ar = perm(l->a_r, W);
    int bcx = perm(l->b_cx, W), bcy = perm(l->b_cy, H), br = perm(l->b_r, W);
    disc(fb, stride, W, H, acx, acy, ar, BUTTON);
    disc(fb, stride, W, H, bcx, bcy, br, BUTTON);

    /* The extra discs, for systems with controls this faceplate's original did
       not have (koboy.h). Drawn with draw_face_button rather than A and B's
       bare disc + below-label, because none has a case band under it: the
       Start/Select/MENU row begins 30 permille below the lowest of them. */
    for (int i = 0; i < KOBOY_MAX_EXTRA_BTNS; i++)
        if (l->extra[i].r > 0)
            draw_face_button(fb, stride, W, H,
                             perm(l->extra[i].cx, W), perm(l->extra[i].cy, H),
                             perm(l->extra[i].r, W), l->extra[i].label);

    /* A, B, Start and Select centred BELOW their control, where the real DMG
       puts them -- the four used to be indistinguishable grey shapes. ONE px
       size, picked so the longest ("SELECT") still clears its own pill with
       margin, keeps the row consistent. */
    int lbl_px = 1;
    while (lbl_px < 6 &&
          text_measure("SELECT", lbl_px + 1) <= perm(l->select_w, W) - perm(10, W) &&
          TEXT_GLYPH_H * (lbl_px + 1) <= perm(l->menu_h, H) - 6)
        lbl_px++;
    text_draw_centred_at(fb, stride, W, H, acx, acy + ar + perm(6, H), "A", lbl_px, INK);
    text_draw_centred_at(fb, stride, W, H, bcx, bcy + br + perm(6, H), "B", lbl_px, INK);

    /* Start and Select pills. The real ones sit at roughly 20 degrees; these
       stay AXIS-ALIGNED, a deliberate simplification -- angling them needs a
       sheared fill primitive this file lacks AND rotated text, which text.c
       cannot do. The tilted A/B pair already carries the diagonal feel. */
    int scx = perm(l->start_cx, W), scy = perm(l->start_cy, H);
    int sw = perm(l->start_w, W), sh = perm(l->start_h, H);
    int tcx = perm(l->select_cx, W), tcy = perm(l->select_cy, H);
    int tw = perm(l->select_w, W), th = perm(l->select_h, H);
    /* PILL: a little darker than the case, not "clearly" darker like DPAD or
       BUTTON -- the photo's pills sit much closer to the case tone than the
       magenta buttons. An earlier round had them LIGHTER than the case. */
    box(fb, stride, W, H, scx, scy, sw, sh, PILL);
    box(fb, stride, W, H, tcx, tcy, tw, th, PILL);
    text_draw_centred_at(fb, stride, W, H, scx, scy + sh / 2 + perm(6, H), "START", lbl_px, INK);
    text_draw_centred_at(fb, stride, W, H, tcx, tcy + th / 2 + perm(6, H), "SELECT", lbl_px, INK);

    /* MENU. DRAWN, not hidden behind a gesture: the drawn UI is what people
       trust, and v1 learned the input model has to match the drawing -- a
       relative thumb-pad under a drawn absolute cross was unusable. Power still
       means quit, so a menu that fails to draw cannot trap the user. */
    int mcx = perm(l->menu_cx, W), mcy = perm(l->menu_cy, H);
    int mw = perm(l->menu_w, W), mh = perm(l->menu_h, H);
    box(fb, stride, W, H, mcx, mcy, mw, mh, PILL);   /* same tone as Start/Select: a koboy-only control, styled like its neighbours */
    frame(fb, stride, W, H, mcx - mw / 2, mcy - mh / 2, mw, mh, 2, INK);
    /* Label INSIDE the box, where a real DMG puts equivalent markings, rather
       than below like the other four. */
    text_draw_centred_at(fb, stride, W, H, mcx, mcy - TEXT_GLYPH_H * lbl_px / 2, "MENU", lbl_px, INK);
}

/* One definition of the label and its scale, so the guard below cannot measure
   one string while text_draw_centred_at draws a different one. */
#define BATTERY_LABEL     "BATTERY"
#define BATTERY_LABEL_PX  1

/* The lamp itself, with no opinion about where it goes -- the two layouts put
   it in different places and this is what they share. Split out rather than
   copied, because the fill geometry below has already been wrong once on the
   device ("the battery fill is a rectangle"). */
static void battery_lamp(uint8_t *fb, int stride, int W, int H,
                         int cx, int cy, int r, int percent)
{
    disc(fb, stride, W, H, cx, cy, r, BG);
    ring(fb, stride, W, H, cx, cy, r, INK);
    if (percent >= 0) {
        /* THE FILL IS A CHORD OF THE LAMP, and the containment test below is
           what makes it one. The previous version ran hline from cx - r to
           cx + r -- the disc's BOUNDING BOX -- so the level painted full width
           and spilled outside the circle, with ring() merely outlining a circle
           on top. Reported from the device as "the battery fill is a
           rectangle". Same test as disc(), deliberately: one definition of what
           is inside this lamp. */
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
    /* LIVE GUARD, not dead code: percent comes from a backend reading a raw
       sysfs node, and while the defined values are 0..100 or -1 for "unknown",
       nothing stops a stray one. Below 0 collapses to "unknown"; above 100 is
       capped so the fill loop cannot paint past the lamp's circle. */
    if (percent > 100) percent = 100;
    if (percent < 0) percent = -1;

    const int W = p->panel_w, H = p->panel_h;

    /* THE USER'S EXPLICIT REQUEST: in the LCD layout the battery moves UNDER
       the screen. It has to -- the DMG position is the case band left of the
       rect, which here runs the full panel width, so the DMG guard below would
       silently draw nothing at all. */
    if (p->layout_mode == KOBOY_LAYOUT_LCD) {
        int lcx, lcy, lr;
        chrome_lcd_battery(p, &lcx, &lcy, &lr);
        /* Same contract as the DMG guard below, on the axis that can go wrong
           here: the lamp lives BELOW the game rect, so what must be proved is
           that its topmost row clears the rect's bottom. LIVE -- the resolver
           may hand this a panel short enough that the strip is most of it. */
        if (lcy - lr <= p->game_y + p->game_h - 1) return;
        battery_lamp(fb, stride, W, H, lcx, lcy, lr, percent);
        return;
    }

    /* Left of the screen, like the DMG's power LED. */
    int cx = p->game_x / 2;
    int cy = p->game_y + p->game_h / 2;
    int r  = W / 60;
    if (r < 4) r = 4;

    /* NEVER inside the game rect -- chrome_render's contract, and this is
       called from the same places.

       THE EXTENT TESTED IS THE LABEL'S, not just the disc's. "BATTERY" at
       px = 1 is ~42 px wide while r is W/60 (five pixels on a small panel), so
       a guard clearing only the disc let the text run into the rect: swept
       against the real resolver, 7,737 panel/scale combinations did. All four
       supported panels happened to be clear (Clara by 48 px), which is luck --
       nothing ties game_x to the width of a word.
       label_right is exactly the rightmost column text_draw_centred_at
       touches: it starts at cx - m/2 and writes m columns, so the last is
       cx + m - m/2 - 1 -- written that way rather than m/2 so the odd-width
       case is not one pixel short. */
    int lw = text_measure(BATTERY_LABEL, BATTERY_LABEL_PX);
    int label_right = lw > 0 ? lw - lw / 2 - 1 : 0;
    int extent = r > label_right ? r : label_right;
    if (cx + extent >= p->game_x) return;

    battery_lamp(fb, stride, W, H, cx, cy, r, percent);
}
