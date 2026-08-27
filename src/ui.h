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

/* Builds the label ui_list_render actually draws for one row: `s` verbatim,
   extension and all -- and, if it does not fit
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

/* Characters a title row can carry. A count, not a pixel width, because the
   title is the one string ui_list_render does NOT put through ui_fit_label:
   it draws at UI_TEXT_PX + 1 (a 24px advance) starting one half-row in, which
   fits ~51 characters on the shipped 1264-wide panel and ~42 on the 1072-wide
   Clara. 40 stays inside both, and a title that overran would not ellipsise
   -- it would just run off the edge, or under the letter strip. */
/* The in-game MENU's GREYSCALE row, e.g. "GREYSCALE: BALANCED".

   Lives here rather than in main.c for one reason: main.c is not linked into
   the test binaries (see the Makefile's SRC filter), so a label built inline
   there could not be asserted against anything. A row that always read
   "GREYSCALE" would look identical on the panel and prove nothing -- which is
   exactly the failure this project keeps having to fix -- so the label has to
   be a function a test can call with each koboy_gray_map in turn.

   Uppercased because every other row is and the 5x7 font's lower case is not
   worth reading. Always NUL-terminated; truncates rather than overruns. */
void      ui_gray_label(char *out, size_t outsz, koboy_gray_map map);

/* The in-game MENU's FRAMES row, e.g. "FRAMES: EVERY 3RD" -- and, for a
   divisor of 1, "FRAMES: EVERY FRAME", because "EVERY 1ST" is not English.

   Here rather than in main.c for exactly the reason ui_gray_label is: main.c
   is not linked into the test binaries (the Makefile's SRC filter), so a
   label built inline there could be asserted against nothing, and a row that
   always read "FRAMES" would look right on the panel and prove nothing.

   The word is FRAMES and not `present_divisor` on purpose. The menu is read
   by someone holding the device, and "how many core frames pass per presented
   frame" is a sentence about the implementation; "every 3rd" is a sentence
   about what they are about to see.

   Accepts any int -- the ordinal rule is the general one and is tested past
   the range the clamp permits. Always NUL-terminated; truncates rather than
   overruns. */
void      ui_divisor_label(char *out, size_t outsz, int divisor);

#define UI_TITLE_CHARS 40

/* Builds a breadcrumb title -- `head` alone when `sub` is empty, otherwise
   "head / sub" -- clamped to UI_TITLE_CHARS. An over-long `sub` keeps its
   TAIL ("ALL GAMES / ...and Watch"), because the deepest folder is the one
   you are actually in: "roms/Collectio..." tells a lost user nothing. Lives
   here rather than in the ROM browser that uses it so the truncation branch
   can be asserted by character count instead of inferred from a render --
   the same reasoning as ui_fit_label above. */
void      ui_path_title(char *out, size_t outsz, const char *head,
                        const char *sub);
#endif
