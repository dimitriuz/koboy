#include "ui.h"
#include "text.h"
#include <string.h>

#define UI_BG        0xFF
#define UI_INK       0x00
#define UI_RULE      0xAA
#define UI_MAX_ROWS  10
#define UI_TEXT_PX   3

void ui_list_init(koboy_ui_list *u, const char *title,
                  const char *const *items, int count,
                  int x, int y, int w, int h)
{
    memset(u, 0, sizeof *u);
    u->title = title;
    u->items = items;
    u->count = count < 0 ? 0 : count;
    u->x = x; u->y = y; u->w = w; u->h = h;

    /* A freshly created list assumes a finger may ALREADY be down, and so
       requires a release before it will accept its first tap.

       Without this, chaining screens breaks: selection happens on touch-down,
       a real tap lasts ~100ms, and building the next screen takes far less
       than that -- so the same still-down finger selects the same row index on
       the new list. Measured consequence: SAVE STATE overwrote slot 1 with no
       picker ever shown, and CHOOSE ROM loaded the 4th ROM after unloading the
       current game.

       Costs one poll cycle when no finger is down, which is imperceptible. */
    u->prev_touch = true;

    /* One title row plus one footer row, so the divisor is rows + 2. Clamped
       to at least one row so a tiny region still produces usable geometry
       instead of a division that yields zero and a widget nothing can hit. */
    int slots = UI_MAX_ROWS + 2;
    u->row_h = h / slots;
    if (u->row_h < TEXT_GLYPH_H * UI_TEXT_PX) u->row_h = TEXT_GLYPH_H * UI_TEXT_PX;
    u->rows = (h / u->row_h) - 2;
    if (u->rows < 1) u->rows = 1;
    if (u->rows > UI_MAX_ROWS) u->rows = UI_MAX_ROWS;
}

int ui_list_rows(const koboy_ui_list *u) { return u->rows; }

int ui_list_pages(const koboy_ui_list *u)
{
    if (u->count <= 0) return 1;
    return (u->count + u->rows - 1) / u->rows;
}

/* Index of the item drawn on row `r` of the current page, or -1 if that row is
   past the end of the list. The last page is short, and a tap on one of its
   dead rows must select nothing rather than an item that does not exist. */
static int item_at_row(const koboy_ui_list *u, int r)
{
    if (r < 0 || r >= u->rows) return -1;
    int i = u->page * u->rows + r;
    return (i >= 0 && i < u->count) ? i : -1;
}

void ui_list_render(const koboy_ui_list *u, uint8_t *fb, int stride,
                    int W, int H)
{
    /* Clear the region, clamped: the caller passes panel geometry, and a
       region running past an edge would otherwise memset past the buffer. */
    for (int y = u->y; y < u->y + u->h; y++) {
        if (y < 0 || y >= H) continue;
        int x0 = u->x < 0 ? 0 : u->x;
        int x1 = u->x + u->w;
        if (x1 > W) x1 = W;
        if (x0 < x1) memset(fb + (size_t)y * stride + x0, UI_BG, (size_t)(x1 - x0));
    }

    text_draw(fb, stride, W, H, u->x + u->row_h / 2, u->y + u->row_h / 4,
              u->title, UI_TEXT_PX + 1, UI_INK);

    for (int r = 0; r < u->rows; r++) {
        int i = item_at_row(u, r);
        if (i < 0) break;
        int ry = u->y + u->row_h + r * u->row_h;
        text_draw(fb, stride, W, H, u->x + u->row_h, ry + u->row_h / 4,
                  u->items[i], UI_TEXT_PX, UI_INK);
        /* Row rule, so a finger can tell where one entry ends. */
        int ly = ry + u->row_h - 1;
        if (ly >= 0 && ly < H) {
            int x0 = u->x < 0 ? 0 : u->x;
            int x1 = u->x + u->w; if (x1 > W) x1 = W;
            if (x0 < x1) memset(fb + (size_t)ly * stride + x0, UI_RULE, (size_t)(x1 - x0));
        }
    }

    /* Footer: page indicator plus the arrows a touch-only Kobo taps. */
    char foot[64];
    int page = u->page + 1, pages = ui_list_pages(u);
    foot[0] = 0;
    {
        const char *d = "0123456789";
        char tmp[64]; int n = 0;
        tmp[n++] = '<'; tmp[n++] = ' ';
        if (page >= 10) tmp[n++] = d[(page / 10) % 10];
        tmp[n++] = d[page % 10];
        tmp[n++] = '/';
        if (pages >= 10) tmp[n++] = d[(pages / 10) % 10];
        tmp[n++] = d[pages % 10];
        tmp[n++] = ' '; tmp[n++] = '>';
        tmp[n] = 0;
        memcpy(foot, tmp, (size_t)n + 1);
    }
    text_draw_centred(fb, stride, W, H, u->y + u->h - u->row_h + u->row_h / 4,
                      foot, UI_TEXT_PX, UI_INK);
}

/* First touch slot that is down, or -1. */
static int first_down(const koboy_input_state *st)
{
    for (int s = 0; s < KOBOY_MAX_TOUCH; s++)
        if (st->touch[s].down) return s;
    return -1;
}

static void page_by(koboy_ui_list *u, int delta)
{
    int p = u->page + delta;
    int max = ui_list_pages(u) - 1;
    if (p < 0) p = 0;
    if (p > max) p = max;
    u->page = p;
}

ui_action ui_list_feed(koboy_ui_list *u, const koboy_input_state *st,
                       int *out_index)
{
    ui_action act = UI_NONE;

    /* Buttons first, on the rising edge only. A and B are the calibrated
       page-turn keys; in a UI mode they mean previous and next page, which is
       what the hardware is named after. */
    uint16_t rising = (uint16_t)(st->buttons & ~u->prev_buttons);
    u->prev_buttons = st->buttons;

    int slot = first_down(st);
    bool touching = (slot >= 0);
    bool tap = touching && !u->prev_touch;
    u->prev_touch = touching;

    if (rising & KOBOY_BTN_A) {
        int before = u->page;
        page_by(u, -1);
        if (u->page != before) return UI_PAGE_PREV;
        return UI_NONE;
    }
    if (rising & KOBOY_BTN_B) {
        int before = u->page;
        page_by(u, +1);
        if (u->page != before) return UI_PAGE_NEXT;
        return UI_NONE;
    }

    if (!tap) return UI_NONE;

    int tx = st->touch[slot].x, ty = st->touch[slot].y;
    if (tx < u->x || tx >= u->x + u->w || ty < u->y || ty >= u->y + u->h)
        return UI_NONE;

    /* Footer arrows: left third is previous, right third is next. */
    int foot_top = u->y + u->h - u->row_h;
    if (ty >= foot_top) {
        int before = u->page;
        if (tx < u->x + u->w / 3)            page_by(u, -1);
        else if (tx > u->x + 2 * u->w / 3)   page_by(u, +1);
        else                                 return UI_NONE;
        if (u->page == before) return UI_NONE;
        return (u->page > before) ? UI_PAGE_NEXT : UI_PAGE_PREV;
    }

    int r = (ty - u->y - u->row_h) / u->row_h;
    if (ty < u->y + u->row_h) return UI_NONE;      /* the title is not a row */
    int i = item_at_row(u, r);
    if (i < 0) return UI_NONE;
    if (out_index) *out_index = i;
    act = UI_SELECT;
    return act;
}
