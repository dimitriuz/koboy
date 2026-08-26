#ifndef KOBOY_UI_H
#define KOBOY_UI_H
#include "koboy.h"

/* One list widget, used for BOTH the ROM browser and the in-game menu.
   They are the same thing -- a titled list of strings you tap, with paging --
   and writing them separately would be duplication wearing a disguise.

   It consumes koboy_input_state rather than evdev events, which is what makes
   it a pure host unit test rather than device theatre, and it renders into the
   caller's panel buffer without ever touching the platform. */

typedef enum { UI_NONE = 0, UI_SELECT, UI_PAGE_NEXT, UI_PAGE_PREV, UI_JUMP } ui_action;

typedef struct {
    const char        *title;
    const char *const *items;
    int                count;
    int                page;
    int                x, y, w, h;    /* panel region */
    int                row_h;         /* derived; row 0 is the title */
    int                rows;          /* items per page */

    /* Edge state. A tap is accepted on touch-down and not accepted again
       until release: level-triggered would fire ~60 times a second. This is
       the d-pad's hysteresis lesson in a different costume. */
    bool               prev_touch;
    uint16_t           prev_buttons;

    /* Letter index strip -- see ui_list_enable_alpha_jump. OFF by default (a
       6-item MENU or slot picker has no use for one; only the ROM browser
       turns it on), so every existing caller is unaffected until it opts in.
       letter_present is a 27-bit map, bit 0 for the "not a letter" bucket
       ('#') and bits 1..26 for A..Z, built once at enable time from `items`
       -- rebuilding it per render would be an O(count) scan on every frame
       for a list that never changes after ui_list_init. */
    bool               alpha_jump;
    uint32_t           letter_present;
} koboy_ui_list;

void      ui_list_init(koboy_ui_list *u, const char *title,
                       const char *const *items, int count,
                       int x, int y, int w, int h);

/* Turns the right-edge letter index strip on or off for this list (default
   off). Scans `items` once, right now, to record which of the 27 buckets
   ('#', A-Z) have at least one entry -- see koboy_ui_list.letter_present.
   Call AFTER ui_list_init, since it reads u->items/u->count. */
void      ui_list_enable_alpha_jump(koboy_ui_list *u, bool enabled);

int       ui_list_rows(const koboy_ui_list *u);
int       ui_list_pages(const koboy_ui_list *u);
void      ui_list_render(const koboy_ui_list *u, uint8_t *fb, int stride,
                         int W, int H);
ui_action ui_list_feed(koboy_ui_list *u, const koboy_input_state *st,
                       int *out_index);

/* Builds the label ui_list_render actually draws for one row: `s` with any
   known ROM extension (.gb/.gbc, either case) stripped -- display only, the
   string a caller loads a ROM from is built separately from the untouched
   item text and never passes through here -- and, if it still does not fit
   `avail_px` at glyph scale `px`, middle-ellipsised so BOTH the head and the
   tail survive. That matters specifically for a No-Intro collection: two
   ROMs that differ only in a trailing "(USA)" vs "(Europe)" need that tail
   visible to stay distinguishable, and a naive tail-only truncation would
   have discarded exactly that. Writes into `out` (size `outsz`), always
   NUL-terminated. Exposed (not static) so the fit/ellipsis logic can be
   asserted directly by index/character-count rather than only inferred from
   a rendered image -- the same reasoning as text_pixel_visible in text.h. */
void      ui_fit_label(const char *s, int avail_px, int px,
                       char *out, size_t outsz);
#endif
