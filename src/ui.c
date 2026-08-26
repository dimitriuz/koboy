#include "ui.h"
#include "text.h"
#include <string.h>

#define UI_BG        0xFF
#define UI_INK       0x00
#define UI_RULE      0xAA
#define UI_MAX_ROWS  24
#define UI_TEXT_PX   3

/* UI_MAX_ROWS went from 10 to 24 (measured against the shipped browser
   geometry, 1264x1680 panel minus KOBOY_CHROME_MARGIN on every side, which
   is a 1664px-tall region): 10 rows meant 138px-tall rows carrying 21px
   text, and a 300-ROM collection needed 23+ pages reachable only by the
   footer arrows. 24 rows divides that region into exactly 64px rows -- still
   3x the 21px glyph height ui_list_init's own clamp requires -- and turns 23
   pages into 13. Verified by rendering: tests/golden/romlist_dense.pgm. */

/* ------------------------------------------------------- letter buckets
   Bucket 0 is '#' (anything that doesn't start with a letter -- a digit, a
   symbol, or an empty string), buckets 1..26 are A..Z. Kept as a small
   integer rather than the raw char so it doubles as a bit index into
   letter_present and an array index for iteration, and 27 fits comfortably
   in the uint32_t the bitmap uses. */
#define UI_BUCKETS 27

static int bucket_of(const char *s)
{
    char c = (s && *s) ? *s : 0;
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return 1 + (c - 'A');
    return 0;                              /* '#': digits, symbols, empty */
}

/* First index in u->items whose bucket is `b`, or -1. items[] is sorted by
   the caller (romlist_scan sorts before this widget ever sees the array),
   so the first match is also the page a jump to `b` should land on -- this
   does not re-derive that ordering, it relies on it. */
static int first_index_of_bucket(const koboy_ui_list *u, int b)
{
    for (int i = 0; i < u->count; i++)
        if (bucket_of(u->items[i]) == b) return i;
    return -1;
}

/* Nearest bucket that actually has an item, searching forward (wrapping
   after Z back to '#') from `from`. With allow_self, `from` itself counts if
   it is occupied -- the touch strip's "land on what I tapped, or the next
   thing after it" behaviour. Without allow_self, `from` is never returned --
   the hardware jump's "always move to a DIFFERENT letter" behaviour, so
   holding the combo on a list where every entry shares one starting letter
   is a no-op instead of reporting a jump that changed nothing.
   Returns -1 only if letter_present is entirely empty (an empty list). */
static int nearest_present_bucket(const koboy_ui_list *u, int from, bool allow_self)
{
    if (u->letter_present == 0) return -1;
    if (allow_self && (u->letter_present & (1u << from))) return from;
    for (int step = 1; step < UI_BUCKETS; step++) {
        int b = (from + step) % UI_BUCKETS;
        if (u->letter_present & (1u << b)) return b;
    }
    return -1;
}

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

    /* alpha_jump and letter_present are already zero from the memset above:
       off until ui_list_enable_alpha_jump says otherwise. */
}

void ui_list_enable_alpha_jump(koboy_ui_list *u, bool enabled)
{
    u->alpha_jump = enabled;
    u->letter_present = 0;
    if (!enabled) return;
    for (int i = 0; i < u->count; i++)
        u->letter_present |= (1u << bucket_of(u->items[i]));
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

/* Width, in panel pixels, of the letter strip. Tied to row_h (which already
   has a legibility-driven minimum) rather than a fixed constant, so it stays
   a comfortable touch target at any panel size instead of only the one this
   was measured on. */
static int strip_w(const koboy_ui_list *u) { return u->row_h; }

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

    /* Letter index strip, drawn LAST (after every row above) so its opaque
       background covers any row text that ran into its column -- text_draw
       clips only to the panel, not to a row's width, and a long filename
       already reaches the panel edge without this strip. Painting the strip
       on top is simpler and cheaper than measuring and truncating every row
       label to a narrower width.

       Its background fill goes to W, the true buffer edge, NOT to
       u->x + u->w like every other fill in this function: a row label can
       overrun the widget's own right edge (the same "clips to the panel,
       not the row" fact above) into whatever margin the caller left outside
       u->w, and that margin never gets background-cleared by anything else
       -- measured on tests/golden/romlist_dense.pgm, whose longest title
       left ink stray in exactly that margin before this widened to W. */
    if (u->alpha_jump) {
        int sw = strip_w(u);
        int sx = u->x + u->w - sw;
        int body_top = u->y + u->row_h;
        int foot_top = u->y + u->h - u->row_h;
        int body_h = foot_top - body_top;
        if (sx < u->x) sx = u->x;             /* degenerate/tiny region guard */

        for (int y = body_top; y < foot_top; y++) {
            if (y < 0 || y >= H) continue;
            int x0 = sx < 0 ? 0 : sx;
            if (x0 < W) memset(fb + (size_t)y * stride + x0, UI_BG, (size_t)(W - x0));
        }
        /* Divider between the strip and the row text it sits beside. */
        for (int y = body_top; y < foot_top; y++) {
            if (y < 0 || y >= H || sx < 0 || sx >= W) continue;
            fb[(size_t)y * stride + sx] = UI_RULE;
        }

        int band_h = body_h / UI_BUCKETS;
        if (band_h < 1) band_h = 1;
        for (int b = 0; b < UI_BUCKETS; b++) {
            char glyph[2] = { (char)(b == 0 ? '#' : 'A' + (b - 1)), 0 };
            /* Present letters draw in full ink; empty ones draw dim (reusing
               UI_RULE's tone rather than a new constant) so the strip still
               shows the whole alphabet for spatial consistency -- like an
               address book -- while making clear which taps land exactly
               where they say and which will fall through to the nearest
               occupied letter (see nearest_present_bucket). */
            uint8_t ink = (u->letter_present & (1u << b)) ? UI_INK : UI_RULE;
            int gy = body_top + b * band_h + (band_h - TEXT_GLYPH_H * UI_TEXT_PX) / 2;
            int gx = sx + (sw - TEXT_GLYPH_W * UI_TEXT_PX) / 2;
            text_draw(fb, stride, W, H, gx, gy, glyph, UI_TEXT_PX, ink);
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

/* Jumps to the first item of bucket `b`'s page. `b` must already be a
   PRESENT bucket (from nearest_present_bucket) -- this does not itself
   handle "no such bucket", callers do. Returns the index jumped to, or -1 if
   the bucket turned out to be empty after all (defensive; should not happen
   given the invariant above, but a jump that fails must still be UI_NONE
   rather than silently teleport the page). */
static int jump_to_bucket(koboy_ui_list *u, int b)
{
    int idx = first_index_of_bucket(u, b);
    if (idx < 0) return -1;
    u->page = idx / u->rows;
    return idx;
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

    /* A+B TOGETHER, checked before either alone: the hardware-only letter
       jump. Two physical page-turn buttons cannot spell a letter, but they
       can advance through the letters the list actually has, wrapping
       around -- which is real navigation once "the next occupied letter"
       instead of "the next page" is 300 ROMs away from where you are. Must
       be tested BEFORE the single-button checks below, or a rising A+B would
       always be consumed as PAGE_PREV first (checked here first is what
       makes the two distinguishable; see tests/test_ui.c). */
    if (u->alpha_jump && (rising & (KOBOY_BTN_A | KOBOY_BTN_B))
                       == (KOBOY_BTN_A | KOBOY_BTN_B)) {
        int top = u->count > 0 ? u->page * u->rows : -1;
        if (top >= u->count) top = u->count - 1;         /* defensive */
        if (top < 0) return UI_NONE;
        int cur_b = bucket_of(u->items[top]);
        int nb = nearest_present_bucket(u, cur_b, false);
        if (nb < 0) return UI_NONE;          /* only one letter in the list */
        int idx = jump_to_bucket(u, nb);
        if (idx < 0) return UI_NONE;
        if (out_index) *out_index = idx;
        return UI_JUMP;
    }
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

    int foot_top = u->y + u->h - u->row_h;
    int body_top = u->y + u->row_h;

    /* Letter strip: the narrow right-edge column, body rows only (never the
       title row above it or the footer row below it, which keeps this from
       ever competing with the footer arrows for the same pixels). Checked
       before both the footer-arrow test and the row hit-test below, so a tap
       inside the strip's column can never fall through to either -- it is
       always claimed here first. */
    if (u->alpha_jump && tx >= u->x + u->w - strip_w(u) &&
        ty >= body_top && ty < foot_top) {
        int band_h = (foot_top - body_top) / UI_BUCKETS;
        if (band_h < 1) band_h = 1;
        int b = (ty - body_top) / band_h;
        if (b >= UI_BUCKETS) b = UI_BUCKETS - 1;
        int nb = nearest_present_bucket(u, b, true);
        if (nb < 0) return UI_NONE;                       /* empty list */
        int idx = jump_to_bucket(u, nb);
        if (idx < 0) return UI_NONE;
        if (out_index) *out_index = idx;
        return UI_JUMP;
    }

    /* Footer arrows: left third is previous, right third is next. */
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
