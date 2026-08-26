#include "chrome.h"
#include "text.h"
#include <string.h>

/* The faceplate is drawn with KOBOY_REFRESH_FULL -- GC16, sixteen levels. The
   four-level ceiling is a constraint on the GAME RECT only, and this file used
   three values out of sixteen. Depth is free here: chrome is drawn once, so
   elaborateness is an authoring question and not a performance one. */
#define BG      0xFF   /* case */
#define CASE_HI 0xEE   /* raised edge */
#define CASE_LO 0xD0   /* recess */
#define MID     0xAA   /* button face */
#define DARK    0x66   /* button shadow, bezel inner */
#define INK     0x00

static int perm(int v, int total) { return v * total / 1000; }

static int min2(int a, int b) { return a < b ? a : b; }

/* Contract and rationale in chrome.h. Every term below is the exact expression
   the corresponding draw call in chrome_render() uses for its top edge, so the
   two cannot drift: box() spans cy - h/2, disc() spans cy - r, and frame() with
   thickness t reaches t-1 rows above its y. */
int chrome_controls_top(const koboy_layout *l, int panel_w, int panel_h)
{
    const int W = panel_w, H = panel_h;
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

void chrome_render(uint8_t *fb, int stride, const koboy_profile *p,
                   const koboy_layout *l)
{
    const int W = p->panel_w, H = p->panel_h;

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
       than the other three sides -- it is where the strapline lives -- and
       that asymmetry, more than any other single choice, is what makes a
       rectangle read as a handheld console rather than "a screen with a
       border".
       Four filled bands, entirely outside the game rect by construction (the
       top band stops at game_y - 1, the bottom band starts at
       game_y + game_h, and the side bands are cut to exactly the rect's own
       y-span), so this can never intrude on it regardless of side_t/bot_t. */
    int side_t = perm(18, W);
    if (side_t < 4) side_t = 4;
    int bot_t = side_t + p->game_h / 12;      /* the asymmetry */

    int bx0 = p->game_x - side_t, bx1 = p->game_x + p->game_w - 1 + side_t;
    int by0 = p->game_y - side_t, by1 = p->game_y + p->game_h - 1 + bot_t;

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

    /* Strapline, centred in the tall lower band. Sized to the widest px that
       still clears both the band's own height and the game rect's width (with
       a margin), so the one string fits every supported panel without a
       per-device table. */
    int strap_px = 1;
    while (strap_px < 8 &&
          text_measure(STRAPLINE, strap_px + 1) <= p->game_w - perm(20, W) &&
          TEXT_GLYPH_H * (strap_px + 1) <= bot_t - 6)
        strap_px++;
    int strap_y = p->game_y + p->game_h + (bot_t - TEXT_GLYPH_H * strap_px) / 2;
    text_draw_centred(fb, stride, W, H, strap_y, STRAPLINE, strap_px, BG);

    /* d-pad cross */
    int dcx = perm(l->dpad_cx, W), dcy = perm(l->dpad_cy, H), dr = perm(l->dpad_r, W);
    int arm = dr / 3;
    box(fb, stride, W, H, dcx, dcy, arm, 2 * dr, MID);
    box(fb, stride, W, H, dcx, dcy, 2 * dr, arm, MID);
    frame(fb, stride, W, H, dcx - arm / 2, dcy - dr, arm, 2 * dr, 2, INK);
    frame(fb, stride, W, H, dcx - dr, dcy - arm / 2, 2 * dr, arm, 2, INK);

    /* koboy wordmark, where the original console's own logotype sits: to the
       left of the d-pad, at the d-pad's own height. That spot -- not the open
       band below the screen -- is deliberate: on the narrowest supported panel
       (Clara, 1072x1448) the gap between the screen's bottom bezel and
       chrome_controls_top is under ten pixels at the shipped scale, nowhere
       near enough room for a wordmark, while the margin to the left of the
       d-pad is comfortable on every panel because the d-pad's own permille
       position scales with it. Sized to the widest px that clears that margin,
       the same technique as the strapline above. */
    int word_avail = dcx - dr - perm(10, W);
    if (word_avail < 0) word_avail = 0;
    int word_px = 1;
    while (word_px < 6 && text_measure("koboy", word_px + 1) <= word_avail)
        word_px++;
    int word_x = perm(10, W);
    int word_y = dcy - TEXT_GLYPH_H * word_px / 2;
    text_draw(fb, stride, W, H, word_x, word_y, "koboy", word_px, DARK);

    /* A and B */
    int acx = perm(l->a_cx, W), acy = perm(l->a_cy, H), ar = perm(l->a_r, W);
    int bcx = perm(l->b_cx, W), bcy = perm(l->b_cy, H), br = perm(l->b_r, W);
    disc(fb, stride, W, H, acx, acy, ar, MID);
    disc(fb, stride, W, H, bcx, bcy, br, MID);

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

    /* Speaker grille: six parallel diagonal slashes in the lower-right case
       area, beside the A button -- there is no line primitive in this file
       and six short diagonals do not justify adding one, so each is stepped a
       column at a time with hline, the same primitive frame() and fill_rect
       already use. Skipped rather than crushed if the panel leaves no room:
       every supported panel does, but a hypothetical narrow one should not
       draw garbage instead of nothing. */
    {
        int gx0 = acx + ar + perm(15, W);
        int gx1 = W - perm(20, W);
        int gcy = bcy;
        int gh2 = ar;
        int gy0 = gcy - gh2, gy1 = gcy + gh2;
        if (gx1 - gx0 > 12 && gy1 - gy0 > 12) {
            int gw = gx1 - gx0, gh = gy1 - gy0;
            int n = 6;
            for (int i = 1; i <= n; i++) {
                int lx0 = gx0 + (i * gw) / (n + 1);
                int len = gh;
                if (lx0 + len > gx1) len = gx1 - lx0;
                for (int s = 0; s < len; s++)
                    hline(fb, stride, W, H, lx0 + s, lx0 + s + 1, gy0 + s, DARK);
            }
        }
    }

    /* Start and Select pills */
    int scx = perm(l->start_cx, W), scy = perm(l->start_cy, H);
    int sw = perm(l->start_w, W), sh = perm(l->start_h, H);
    int tcx = perm(l->select_cx, W), tcy = perm(l->select_cy, H);
    int tw = perm(l->select_w, W), th = perm(l->select_h, H);
    box(fb, stride, W, H, scx, scy, sw, sh, MID);
    box(fb, stride, W, H, tcx, tcy, tw, th, MID);
    text_draw_centred_at(fb, stride, W, H, scx, scy + sh / 2 + perm(6, H), "START", lbl_px, INK);
    text_draw_centred_at(fb, stride, W, H, tcx, tcy + th / 2 + perm(6, H), "SELECT", lbl_px, INK);

    /* MENU. Drawn, not hidden behind a gesture: the drawn UI is the part
       people trust, and v1 already learned that the input model has to match
       the drawing -- a relative thumb-pad under a drawn absolute cross was
       unusable. Power still means quit, so a menu that fails to draw can never
       trap the user on a device where a stuck app looks like a brick. */
    int mcx = perm(l->menu_cx, W), mcy = perm(l->menu_cy, H);
    int mw = perm(l->menu_w, W), mh = perm(l->menu_h, H);
    box(fb, stride, W, H, mcx, mcy, mw, mh, MID);
    frame(fb, stride, W, H, mcx - mw / 2, mcy - mh / 2, mw, mh, 2, INK);
    /* Label INSIDE the box -- exactly where a real DMG puts MENU-equivalent
       markings on its own controls -- rather than below it like the other
       four, which is the one placement rule this element does not share with
       them. */
    text_draw_centred_at(fb, stride, W, H, mcx, mcy - TEXT_GLYPH_H * lbl_px / 2, "MENU", lbl_px, INK);
}

void chrome_render_battery(uint8_t *fb, int stride, const koboy_profile *p,
                           const koboy_layout *l, int percent)
{
    (void)l;
    const int W = p->panel_w, H = p->panel_h;
    /* Left of the screen, like the DMG's power LED. */
    int cx = p->game_x / 2;
    int cy = p->game_y + p->game_h / 2;
    int r  = W / 60;
    if (r < 4) r = 4;

    /* Never inside the game rect: the contract chrome_render lives under, and
       this function is called from the same places. */
    if (cx + r >= p->game_x) return;

    disc(fb, stride, W, H, cx, cy, r, BG);
    ring(fb, stride, W, H, cx, cy, r, INK);
    if (percent >= 0) {
        /* Fill proportionally, so the lamp says something rather than merely
           existing. */
        int fill = r * 2 * percent / 100;
        for (int y = cy + r - fill; y <= cy + r; y++)
            hline(fb, stride, W, H, cx - r, cx + r, y, DARK);
        ring(fb, stride, W, H, cx, cy, r, INK);
    }
    text_draw_centred_at(fb, stride, W, H, cx, cy + r + r / 2, "BATTERY", 1, INK);
}
