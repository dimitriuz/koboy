#include "test.h"
#include "ui.h"
#include <string.h>

static koboy_input_state touch_at(int x, int y)
{
    koboy_input_state st;
    memset(&st, 0, sizeof st);
    st.touch[0].x = x; st.touch[0].y = y; st.touch[0].down = true;
    return st;
}

static koboy_input_state released(void)
{
    koboy_input_state st;
    memset(&st, 0, sizeof st);
    return st;
}

static koboy_input_state button(uint16_t bits)
{
    koboy_input_state st;
    memset(&st, 0, sizeof st);
    st.buttons = bits;
    return st;
}

/* Row centre in panel coordinates for row `r` of the current page. */
static int row_y(const koboy_ui_list *u, int r)
{
    return u->y + u->row_h + u->row_h * r + u->row_h / 2;
}

TEST_MAIN({
    static const char *const items[] = {
        "ZELDA.GB", "TETRIS.GB", "KIRBY 2.GBC", "DUCK.GB", "POKEMON.GBC",
        "SIX.GB", "SEVEN.GB", "EIGHT.GB", "NINE.GB", "TEN.GB",
        "ELEVEN.GB", "TWELVE.GB", "THIRTEEN.GB",
    };
    const int N = (int)(sizeof items / sizeof items[0]);

    koboy_ui_list u;
    ui_list_init(&u, "CHOOSE A GAME", items, N, 100, 200, 800, 900);

    CHECK(ui_list_rows(&u) > 0);
    CHECK(ui_list_pages(&u) >= 1);

    int idx = -1;

    /* A freshly created list requires a release before it accepts its first
       tap -- see ui_list_init's prev_touch=true comment. A touch that is
       already down on the very first poll must not select: that is exactly
       the bug this guard exists to close (a still-down finger carrying a
       selection through from the PREVIOUS screen into this new one). */
    koboy_input_state down = touch_at(u.x + u.w / 2, row_y(&u, 0));
    CHECK_EQ_INT(ui_list_feed(&u, &down, &idx), UI_NONE);
    koboy_input_state up0 = released();
    CHECK_EQ_INT(ui_list_feed(&u, &up0, &idx), UI_NONE);

    /* NOW a touch that is merely HELD produces one action, not one per poll.
       Level-triggered would select an item sixty times a second. */
    CHECK_EQ_INT(ui_list_feed(&u, &down, &idx), UI_SELECT);
    CHECK_EQ_INT(idx, 0);
    CHECK_EQ_INT(ui_list_feed(&u, &down, &idx), UI_NONE);
    CHECK_EQ_INT(ui_list_feed(&u, &down, &idx), UI_NONE);

    /* Release, then a second tap on row 2 selects index 2. */
    koboy_input_state up = released();
    CHECK_EQ_INT(ui_list_feed(&u, &up, &idx), UI_NONE);
    koboy_input_state down2 = touch_at(u.x + u.w / 2, row_y(&u, 2));
    CHECK_EQ_INT(ui_list_feed(&u, &down2, &idx), UI_SELECT);
    CHECK_EQ_INT(idx, 2);
    CHECK_EQ_INT(ui_list_feed(&u, &up, &idx), UI_NONE);

    /* Paging with the page-turn buttons, also edge-triggered. */
    if (ui_list_pages(&u) > 1) {
        koboy_input_state b = button(KOBOY_BTN_B);
        CHECK_EQ_INT(ui_list_feed(&u, &b, &idx), UI_PAGE_NEXT);
        CHECK_EQ_INT(u.page, 1);
        CHECK_EQ_INT(ui_list_feed(&u, &b, &idx), UI_NONE);   /* held, not repeated */
        koboy_input_state none = released();
        CHECK_EQ_INT(ui_list_feed(&u, &none, &idx), UI_NONE);

        /* A selection on page 1 must index into the SECOND page, not the
           first. Getting this wrong loads the wrong ROM, which is the whole
           point of the widget. */
        int rows = ui_list_rows(&u);
        koboy_input_state d = touch_at(u.x + u.w / 2, row_y(&u, 0));
        CHECK_EQ_INT(ui_list_feed(&u, &d, &idx), UI_SELECT);
        CHECK_EQ_INT(idx, rows);
        CHECK_EQ_INT(ui_list_feed(&u, &up, &idx), UI_NONE);

        koboy_input_state a = button(KOBOY_BTN_A);
        CHECK_EQ_INT(ui_list_feed(&u, &a, &idx), UI_PAGE_PREV);
        CHECK_EQ_INT(u.page, 0);
    }

    /* Paging never runs off either end. */
    koboy_ui_list s;
    static const char *const one[] = { "ONLY.GB" };
    ui_list_init(&s, "ONE", one, 1, 0, 0, 400, 400);
    koboy_input_state nb = released(), bb = button(KOBOY_BTN_B);
    CHECK_EQ_INT(ui_list_feed(&s, &bb, &idx), UI_NONE);
    CHECK_EQ_INT(s.page, 0);
    CHECK_EQ_INT(ui_list_feed(&s, &nb, &idx), UI_NONE);
    koboy_input_state ab = button(KOBOY_BTN_A);
    CHECK_EQ_INT(ui_list_feed(&s, &ab, &idx), UI_NONE);
    CHECK_EQ_INT(s.page, 0);

    /* A tap on the last page must never select past the end of the list.
       The last page is short, so the rows below the final item are dead. */
    koboy_ui_list t;
    ui_list_init(&t, "SHORT", items, N, 0, 0, 400, 400);
    /* Clear the fresh-list guard unconditionally before the loop below, so
       row 0 is exercised the same way regardless of whether this list has
       one page or several (a bare touch_at() as the very first feed would
       otherwise be swallowed by the guard on a single-page list, silently
       skipping the row-0 case rather than failing). */
    koboy_input_state prime = released();
    ui_list_feed(&t, &prime, &idx);
    while (t.page + 1 < ui_list_pages(&t)) {
        koboy_input_state b = button(KOBOY_BTN_B), r = released();
        ui_list_feed(&t, &b, &idx);
        ui_list_feed(&t, &r, &idx);
    }
    int last_rows = ui_list_rows(&t);
    for (int r = 0; r < last_rows; r++) {
        koboy_input_state d = touch_at(t.x + t.w / 2, row_y(&t, r));
        int got = -1;
        ui_action a2 = ui_list_feed(&t, &d, &got);
        koboy_input_state rel = released();
        ui_list_feed(&t, &rel, &got);
        if (a2 == UI_SELECT) CHECK(got >= 0 && got < N);
    }

    /* A touch outside the list region selects nothing. Primed with a
       release first so this genuinely exercises the out-of-bounds check,
       rather than trivially passing because the fresh-list guard alone would
       already return UI_NONE for ANY position. */
    koboy_ui_list o;
    ui_list_init(&o, "OUT", items, N, 100, 200, 800, 900);
    koboy_input_state prime_o = released();
    ui_list_feed(&o, &prime_o, &idx);
    koboy_input_state far = touch_at(10, 10);
    CHECK_EQ_INT(ui_list_feed(&o, &far, &idx), UI_NONE);

    /* Regression: a still-down finger must not carry a selection into the
       NEXT screen. Reproduces the menu-chaining bug directly against ui.c,
       the way the reviewer who found it did, rather than through main()'s
       plumbing: build one list, select a row with a HELD touch, then build a
       SECOND list sharing the same geometry and feed it that SAME still-down
       state. Before ui_list_init's prev_touch=true fix, the second list had
       no memory of the first and saw the touch as a fresh edge -- selecting
       immediately. That is exactly how SAVE STATE silently overwrote slot 1
       with no picker ever shown, and how CHOOSE ROM loaded the 4th ROM after
       the current game had already been unloaded. */
    koboy_ui_list first, second;
    ui_list_init(&first, "MENU", items, N, 100, 200, 800, 900);
    koboy_input_state rel_first = released();
    ui_list_feed(&first, &rel_first, &idx);          /* clear its own guard */
    koboy_input_state held = touch_at(first.x + first.w / 2, row_y(&first, 0));
    CHECK_EQ_INT(ui_list_feed(&first, &held, &idx), UI_SELECT);
    CHECK_EQ_INT(idx, 0);

    ui_list_init(&second, "SLOT", items, N, 100, 200, 800, 900);
    idx = -1;
    CHECK_EQ_INT(ui_list_feed(&second, &held, &idx), UI_NONE);   /* still down */
    koboy_input_state rel_second = released();
    CHECK_EQ_INT(ui_list_feed(&second, &rel_second, &idx), UI_NONE);
    CHECK_EQ_INT(ui_list_feed(&second, &held, &idx), UI_SELECT); /* fresh down selects */
    CHECK_EQ_INT(idx, 0);

    /* Rendering is clipped and draws something. */
    enum { W = 1264, H = 1680 };
    static uint8_t fb[W * H];
    memset(fb, 0xFF, sizeof fb);
    ui_list_render(&u, fb, W, W, H);
    int painted = 0;
    for (size_t i = 0; i < sizeof fb; i++) if (fb[i] != 0xFF) painted++;
    CHECK(painted > 0);
})
