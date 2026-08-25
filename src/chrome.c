#include "chrome.h"
#include <string.h>

#define BG   0xFF
#define INK  0x00
#define MID  0xAA

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

void chrome_render(uint8_t *fb, int stride, const koboy_profile *p,
                   const koboy_layout *l)
{
    const int W = p->panel_w, H = p->panel_h;

    /* The left/right band widths below are clamped into [0, W] before the cast
       to size_t, exactly as hline/vline clamp, and for the same reason: a plain
       int cast to size_t does not saturate, it wraps. A negative game_x or a
       game rect running past the right edge would turn a band width into a
       length near SIZE_MAX and memset the heap flat -- the same underflow
       mechanism that took four rounds and an ASan repro to get out of
       frame()/hline()/vline() in this file.
       config_resolve_profile does keep the invariant for every caller today, so
       these look dead. They are not: they are the local defence, in the file
       that does the writing, so that a change to the resolver in another file
       can never reintroduce a heap overrun here. tests/test_chrome.c drives
       chrome_render with a deliberately invariant-violating profile (negative
       game_x, and a rect wider than the panel) to keep this path exercised. Do
       not remove them as "unreachable". */
    int lx = p->game_x;                     /* width of the left band */
    if (lx < 0) lx = 0;
    if (lx > W) lx = W;
    int rx = p->game_x + p->game_w;         /* first column right of the rect */
    if (rx < 0) rx = 0;
    if (rx > W) rx = W;

    /* background everywhere except the game rect */
    for (int y = 0; y < H; y++) {
        if (y >= p->game_y && y < p->game_y + p->game_h) {
            memset(fb + (size_t)y * stride, BG, (size_t)lx);
            memset(fb + (size_t)y * stride + rx, BG, (size_t)(W - rx));
        } else {
            memset(fb + (size_t)y * stride, BG, (size_t)W);
        }
    }

    /* bezel around the screen, drawn entirely outside the game rect */
    frame(fb, stride, W, H, p->game_x - 1, p->game_y - 1, p->game_w + 2, p->game_h + 2, 6, INK);

    /* d-pad cross */
    int dcx = perm(l->dpad_cx, W), dcy = perm(l->dpad_cy, H), dr = perm(l->dpad_r, W);
    int arm = dr / 3;
    box(fb, stride, W, H, dcx, dcy, arm, 2 * dr, MID);
    box(fb, stride, W, H, dcx, dcy, 2 * dr, arm, MID);
    frame(fb, stride, W, H, dcx - arm / 2, dcy - dr, arm, 2 * dr, 2, INK);
    frame(fb, stride, W, H, dcx - dr, dcy - arm / 2, 2 * dr, arm, 2, INK);

    /* A and B */
    disc(fb, stride, W, H, perm(l->a_cx, W), perm(l->a_cy, H), perm(l->a_r, W), MID);
    disc(fb, stride, W, H, perm(l->b_cx, W), perm(l->b_cy, H), perm(l->b_r, W), MID);

    /* Start and Select pills */
    box(fb, stride, W, H, perm(l->start_cx, W), perm(l->start_cy, H),
        perm(l->start_w, W), perm(l->start_h, H), MID);
    box(fb, stride, W, H, perm(l->select_cx, W), perm(l->select_cy, H),
        perm(l->select_w, W), perm(l->select_h, H), MID);
}
