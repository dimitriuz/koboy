#ifndef KOBOY_UI_H
#define KOBOY_UI_H
#include "koboy.h"

/* One list widget, used for BOTH the ROM browser and the in-game menu: they
   are the same thing, a titled list of strings you tap, with paging.

   It consumes koboy_input_state rather than evdev events, which is what makes
   it a pure host unit test rather than device theatre, and it renders into the
   caller's panel buffer without touching the platform. */

typedef enum { UI_NONE = 0, UI_SELECT, UI_PAGE_NEXT, UI_PAGE_PREV, UI_JUMP } ui_action;

typedef struct {
    const char        *title;
    const char *const *items;
    int                count;
    int                page;
    int                x, y, w, h;    /* panel region */
    int                row_h;         /* derived; row 0 is the title */
    int                rows;          /* items per page */

    /* Edge state. A tap is accepted on touch-DOWN and not again until release:
       level-triggered would fire ~60 times a second. */
    bool               prev_touch;
    uint16_t           prev_buttons;

    /* Letter index strip (ui_list_enable_alpha_jump), OFF by default -- only
       the ROM browser turns it on. letter_present is a 27-bit map: bit 0 for
       the "not a letter" bucket ('#'), bits 1..26 for A..Z, built ONCE at
       enable time, because rebuilding per render would be an O(count) scan
       every frame for a list that never changes after ui_list_init. */
    bool               alpha_jump;
    uint32_t           letter_present;
} koboy_ui_list;

void      ui_list_init(koboy_ui_list *u, const char *title,
                       const char *const *items, int count,
                       int x, int y, int w, int h);

/* Turns the right-edge letter index strip on or off (default off). Scans
   `items` once, now, for which of the 27 buckets have an entry. Call AFTER
   ui_list_init, since it reads u->items/u->count. */
void      ui_list_enable_alpha_jump(koboy_ui_list *u, bool enabled);

int       ui_list_rows(const koboy_ui_list *u);
int       ui_list_pages(const koboy_ui_list *u);
void      ui_list_render(const koboy_ui_list *u, uint8_t *fb, int stride,
                         int W, int H);
ui_action ui_list_feed(koboy_ui_list *u, const koboy_input_state *st,
                       int *out_index);

/* The label ui_list_render draws for one row: `s` verbatim, extension and all,
   MIDDLE-ellipsised when it does not fit `avail_px` at scale `px` so BOTH head
   and tail survive. That matters for a No-Intro collection: two ROMs differing
   only in a trailing "(USA)" vs "(Europe)" need the tail to stay
   distinguishable, which tail-only truncation would discard. Always
   NUL-terminated. Exposed (not static) so the ellipsis logic can be asserted
   by character count rather than inferred from a render. */
void      ui_fit_label(const char *s, int avail_px, int px,
                       char *out, size_t outsz);

/* The four MENU row labels below live HERE rather than in main.c for one
   reason: main.c is not linked into the test binaries (the Makefile's SRC
   filter), so a label built inline there could be asserted against nothing --
   a row that always read "GREYSCALE" would look right on the panel and prove
   nothing. All four uppercase (the 5x7 font's lower case is not worth
   reading), always NUL-terminated, and truncate rather than overrun. */

/* The GREYSCALE row, e.g. "GREYSCALE: BALANCED". */
void      ui_gray_label(char *out, size_t outsz, koboy_gray_map map);

/* The FRAMES row, e.g. "FRAMES: EVERY 3RD" -- and "FRAMES: EVERY FRAME" at a
   divisor of 1, because "EVERY 1ST" is not English.

   The word is FRAMES and not `present_divisor` on purpose: "how many core
   frames pass per presented frame" is a sentence about the implementation,
   "every 3rd" is one about what the reader is about to see.

   Accepts any int -- the ordinal rule is general and is tested past the range
   the clamp permits. */
void      ui_divisor_label(char *out, size_t outsz, int divisor);

/* The MOTION row, e.g. "MOTION: 1-BIT / DU".

   BOTH HALVES ARE ALWAYS NAMED, and that is the point of the row rather than
   a formatting choice: the hypothesis this setting tests is the PAIR, and a
   label showing only the rung's name would leave the owner unable to say which
   combination they were looking at when they report back. COMPOSABLE, not a
   table of the ladder's three rungs -- a hand-edited ini can put the pair in
   any of the six legal combinations and every one must render something
   true. */
void      ui_motion_label(char *out, size_t outsz, bool dither,
                          koboy_wfm_policy wfm);

/* The SCREENSHOT row, e.g. "SCREENSHOT 004 (AFTER THIS MENU)".

   BOTH HALVES ARE LOAD-BEARING. The NUMBER is the file about to be written, so
   the owner knows which shot is which without leaving the game. "AFTER THIS
   MENU" is the row saying what it does: the MENU is drawn OVER the game, so a
   capture taken here would photograph the menu, and selecting this row ARMS
   one for when the game is back. The alternative is an owner who takes six
   shots of the menu before working it out.

   `next_seq` is shot_last_seq + 1. Past KOBOY_SHOT_SEQ_MAX the row says the
   directory is full rather than naming a file shot_path will refuse to
   build. */
void      ui_shot_label(char *out, size_t outsz, int next_seq);

/* Characters a title row can carry. A COUNT, not a pixel width, because the
   title is the one string ui_list_render does NOT put through ui_fit_label: it
   draws at UI_TEXT_PX + 1 (a 24 px advance) starting one half-row in, which
   fits ~51 characters on the 1264-wide panel and ~42 on the 1072-wide Clara.
   40 stays inside both, and a title that overran would not ellipsise -- it
   would run off the edge, or under the letter strip. */
#define UI_TITLE_CHARS 40

/* A breadcrumb title -- `head` alone when `sub` is empty, otherwise
   "head / sub" -- clamped to UI_TITLE_CHARS. An over-long `sub` keeps its
   TAIL ("ALL GAMES / ...and Watch"), because the deepest folder is the one you
   are in: "roms/Collectio..." tells a lost user nothing. Here rather than in
   the browser so the truncation branch can be asserted by character count. */
void      ui_path_title(char *out, size_t outsz, const char *head,
                        const char *sub);
#endif
