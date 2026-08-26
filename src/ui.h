#ifndef KOBOY_UI_H
#define KOBOY_UI_H
#include "koboy.h"

/* One list widget, used for BOTH the ROM browser and the in-game menu.
   They are the same thing -- a titled list of strings you tap, with paging --
   and writing them separately would be duplication wearing a disguise.

   It consumes koboy_input_state rather than evdev events, which is what makes
   it a pure host unit test rather than device theatre, and it renders into the
   caller's panel buffer without ever touching the platform. */

typedef enum { UI_NONE = 0, UI_SELECT, UI_PAGE_NEXT, UI_PAGE_PREV } ui_action;

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
} koboy_ui_list;

void      ui_list_init(koboy_ui_list *u, const char *title,
                       const char *const *items, int count,
                       int x, int y, int w, int h);
int       ui_list_rows(const koboy_ui_list *u);
int       ui_list_pages(const koboy_ui_list *u);
void      ui_list_render(const koboy_ui_list *u, uint8_t *fb, int stride,
                         int W, int H);
ui_action ui_list_feed(koboy_ui_list *u, const koboy_input_state *st,
                       int *out_index);
#endif
