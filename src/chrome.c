#include "chrome.h"
#include <string.h>

#define BG   0xFF
#define INK  0x00
#define MID  0xAA

static int perm(int v, int total) { return v * total / 1000; }

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

    /* background everywhere except the game rect */
    for (int y = 0; y < H; y++) {
        if (y >= p->game_y && y < p->game_y + p->game_h) {
            memset(fb + (size_t)y * stride, BG, (size_t)p->game_x);
            int rx = p->game_x + p->game_w;
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
