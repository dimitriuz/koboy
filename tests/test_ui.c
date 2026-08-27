#include "test.h"
#include "chrome.h"
#include "config.h"
#include "input.h"
#include "text.h"
#include "ui.h"
#include "video.h"
#include <string.h>

/* ui.c's own UI_TEXT_PX is private to that file; the value here only needs
   to match it where a test cross-checks against a real rendered row (it
   currently does not), so any reasonable glyph scale works for exercising
   ui_fit_label as a standalone function. */
#define TEST_LABEL_PX 3

/* Press one finger at a panel coordinate through the REAL evdev decode path,
   the same way tests/test_chrome.c's zone sweep does. Nothing here builds a
   koboy_input_state by hand: a synthetic state is precisely what made the
   faceplate-versus-list bug invisible, because it skips input.c entirely and
   input.c is where the touch-to-joypad synthesis lives. */
static void feed_down(koboy_input *in, int x, int y)
{
    koboy_ev down[5] = {
        { KOBOY_EV_ABS, KOBOY_ABS_MT_SLOT,        0 },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, 1 },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_X,  x },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_Y,  y },
        { KOBOY_EV_SYN, 0, 0 },
    };
    input_feed(in, down, 5);
}

static void feed_up(koboy_input *in)
{
    koboy_ev up[2] = {
        { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, -1 },
        { KOBOY_EV_SYN, 0, 0 },
    };
    input_feed(in, up, 2);
}

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

/* A touch centred in letter-strip band `b` (0 = '#', 1..26 = A..Z),
   replicating ui.c's own band geometry from the widget's PUBLIC fields --
   there is no other state to compute this from, which is the point: if
   ui.c's internal band math ever drifts from this, a tap that should land
   on band `b` starts landing on its neighbour instead, and every alpha-jump
   test below would start failing at the boundary bands, not silently keep
   passing. */
/* released() returns by value, so `&released()` is not a valid C lvalue;
   this exists purely to spell "clear the touch/edge state" as one call at
   each of the many points below that need it. */
static void feed_release(koboy_ui_list *u, int *idx)
{
    koboy_input_state r = released();
    ui_list_feed(u, &r, idx);
}

static koboy_input_state strip_tap(const koboy_ui_list *u, int b)
{
    int body_top = u->y + u->row_h;
    int foot_top = u->y + u->h - u->row_h;
    int band_h = (foot_top - body_top) / 27;   /* '#' + A..Z */
    if (band_h < 1) band_h = 1;
    int ty = body_top + b * band_h + band_h / 2;
    int tx = u->x + u->w - u->row_h / 2;       /* well inside the strip column */
    return touch_at(tx, ty);
}

TEST_MAIN({
    static const char *const items[] = {
        "ZELDA.GB", "TETRIS.GB", "KIRBY 2.GBC", "DUCK.GB", "POKEMON.GBC",
        "SIX.GB", "SEVEN.GB", "EIGHT.GB", "NINE.GB", "TEN.GB",
        "ELEVEN.GB", "TWELVE.GB", "THIRTEEN.GB", "FOURTEEN.GB", "FIFTEEN.GB",
        "SIXTEEN.GB", "SEVENTEEN.GB", "EIGHTEEN.GB", "NINETEEN.GB", "TWENTY.GB",
        "TWENTYONE.GB", "TWENTYTWO.GB", "TWENTYTHREE.GB", "TWENTYFOUR.GB",
        "TWENTYFIVE.GB", "TWENTYSIX.GB", "TWENTYSEVEN.GB", "TWENTYEIGHT.GB",
        "TWENTYNINE.GB", "THIRTY.GB",
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

    /* Paging with the page-turn buttons, also edge-triggered.

       UNCONDITIONAL, and that matters more than it looks. Every assertion
       below used to sit inside `if (ui_list_pages(&u) > 1)` -- gated on the
       very function under test. Mutating ui_list_pages() to `return 1` left
       the whole 820-check suite green, including the CHECK_EQ_INT(idx, rows)
       below that exists to catch "loads the wrong ROM". So the geometry this
       list is built with is asserted OUTRIGHT first: 13 items at 10 rows a
       page is two pages, and if that ever stops being true the two lines
       below fail loudly instead of silently skipping the rest.

       24, not 10: UI_MAX_ROWS went from 10 to 24 so a 300-ROM collection
       does not need 23+ pages of prev/next taps to reach the end -- see
       src/ui.c. 30 items at 24 rows a page is two pages. */
    CHECK_EQ_INT(ui_list_rows(&u), 24);
    CHECK_EQ_INT(ui_list_pages(&u), 2);
    {
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

    /* ------------------------------------------------------------------
       REGRESSION, C1: the faceplate's A and B touch zones must not hijack a
       UI list.

       In every UI mode the drawn faceplate is still underneath the list, and
       input.c's recompute() hit-tests the A and B discs against the layout
       permille unconditionally -- it has no notion of a UI mode. ui_list_feed
       tests a rising A/B FIRST and returns early, so those synthesised bits
       were consumed as page-turns before any row hit-test ran. Measured at the
       shipped defaults on 1264x1680: a tap at (1049,1125), which is row 7 of
       the browser, produced buttons=0x0100 and UI_NONE; a tap at (834,1276),
       row 8, produced 0x0001 and UI_PAGE_NEXT. Rows 6, 7 and 8 were unusable
       over ~17% of the panel width each, on every panel, because the layout is
       permille.

       Driven through input_feed on purpose. tests/test_ui.c never touched
       input.c before, which is exactly why this shipped: a test that builds
       its own koboy_input_state reproduces the blind spot instead of closing
       it. */
    {
        const int W = 1264, H = 1680;
        koboy_config c; config_defaults(&c);
        koboy_profile prof;
        CHECK(config_resolve_profile(&prof, &c, W, H, KOBOY_GB_W, KOBOY_GB_H, KOBOY_GB_W, KOBOY_GB_H));

        koboy_input *in = input_create(&c, &prof);
        CHECK(in != NULL);
        /* Identity transform: raw_max == panel - 1 makes scale_axis a no-op,
           so the coordinates below are pixel-exact panel coordinates. */
        input_set_touch_transform(in, W - 1, H - 1, false, false, false);

        koboy_ui_list br;
        ui_list_init(&br, "CHOOSE A GAME", items, N,
                     KOBOY_CHROME_MARGIN, KOBOY_CHROME_MARGIN,
                     W - 2 * KOBOY_CHROME_MARGIN, H - 2 * KOBOY_CHROME_MARGIN);

        int acx = c.layout.a_cx * W / 1000, acy = c.layout.a_cy * H / 1000;
        int bcx = c.layout.b_cx * W / 1000, bcy = c.layout.b_cy * H / 1000;

        /* The rows those two discs sit on, computed from the widget's own
           geometry rather than hardcoded. CHECKed, not `if`-ed: if a layout
           change ever moved the discs off the list's rows this block would
           quietly stop testing anything, which is the failure mode this whole
           branch keeps finding. */
        int a_row = (acy - br.y - br.row_h) / br.row_h;
        int b_row = (bcy - br.y - br.row_h) / br.row_h;
        int foot_top = br.y + br.h - br.row_h;
        CHECK(acy >= br.y + br.row_h && acy < foot_top);
        CHECK(bcy >= br.y + br.row_h && bcy < foot_top);
        CHECK(a_row >= 0 && a_row < ui_list_rows(&br));
        CHECK(b_row >= 0 && b_row < ui_list_rows(&br));
        CHECK(a_row != b_row);

        koboy_input_state ui;

        /* Clear the fresh-list guard with a genuinely released poll. */
        input_ui_state(in, &ui);
        CHECK_EQ_INT(ui_list_feed(&br, &ui, &idx), UI_NONE);

        /* POSITIVE CONTROL. The raw game-facing state really does carry the A
           bit for this coordinate -- so when the assertion below passes it is
           because the UI projection dropped the bit, not because the hit-test
           moved or the probe silently pressed nothing. */
        feed_down(in, acx, acy);
        CHECK((input_state(in)->buttons & KOBOY_BTN_A) != 0);

        input_ui_state(in, &ui);
        CHECK_EQ_INT(ui.buttons, 0);
        idx = -1;
        CHECK_EQ_INT(ui_list_feed(&br, &ui, &idx), UI_SELECT);
        CHECK_EQ_INT(idx, a_row);
        CHECK_EQ_INT(br.page, 0);            /* it selected, it did not page */
        feed_up(in);
        input_ui_state(in, &ui);
        CHECK_EQ_INT(ui_list_feed(&br, &ui, &idx), UI_NONE);

        /* Same for B, whose disc paged the list forward instead. */
        feed_down(in, bcx, bcy);
        CHECK((input_state(in)->buttons & KOBOY_BTN_B) != 0);
        input_ui_state(in, &ui);
        CHECK_EQ_INT(ui.buttons, 0);
        idx = -1;
        CHECK_EQ_INT(ui_list_feed(&br, &ui, &idx), UI_SELECT);
        CHECK_EQ_INT(idx, b_row);
        CHECK_EQ_INT(br.page, 0);
        feed_up(in);
        input_ui_state(in, &ui);
        CHECK_EQ_INT(ui_list_feed(&br, &ui, &idx), UI_NONE);

        /* And the other half of the contract: REAL page-turn hardware must
           still page. Dropping A/B inside ui.c would have "fixed" the taps by
           breaking the buttons the list is named after. */
        CHECK(c.key_a != 0 && c.key_b != 0);
        input_feed_key(in, c.key_b, true);
        input_ui_state(in, &ui);
        CHECK_EQ_INT(ui.buttons, KOBOY_BTN_B);
        CHECK_EQ_INT(ui_list_feed(&br, &ui, &idx), UI_PAGE_NEXT);
        CHECK_EQ_INT(br.page, 1);
        input_feed_key(in, c.key_b, false);
        input_ui_state(in, &ui);
        CHECK_EQ_INT(ui_list_feed(&br, &ui, &idx), UI_NONE);

        input_feed_key(in, c.key_a, true);
        input_ui_state(in, &ui);
        CHECK_EQ_INT(ui.buttons, KOBOY_BTN_A);
        CHECK_EQ_INT(ui_list_feed(&br, &ui, &idx), UI_PAGE_PREV);
        CHECK_EQ_INT(br.page, 0);
        input_feed_key(in, c.key_a, false);

        input_destroy(in);
    }

    /* ------------------------------------------------------------------
       Alphabet jump: letter strip (touch) and the A+B combo (hardware).

       Deliberately sparse, with gaps -- '#'=0, A=1 (twice), B=2 MISSING,
       C=3, D..Y missing, Z=26 -- so "tap an empty letter" and "the next
       occupied letter" are both actually exercised rather than trivially
       true because every bucket happens to be full. Presorted, as
       romlist_scan's output always is: ui.c relies on that, it does not
       re-sort. */
    {
        static const char *const az[] = {
            "0START.GB", "APPLE.GB", "AVOCADO.GB", "CHERRY.GB", "ZEBRA.GB",
        };
        const int AZ_N = (int)(sizeof az / sizeof az[0]);

        koboy_ui_list j;
        ui_list_init(&j, "CHOOSE A GAME", az, AZ_N, 100, 200, 800, 900);

        /* OFF by default: a list that never opts in must behave exactly as
           it did before this feature existed. A tap inside what WOULD be
           the strip's column, on an ordinary row, must still select that
           row -- not be swallowed by strip logic that isn't there. */
        {
            koboy_input_state prime = released();
            ui_list_feed(&j, &prime, &idx);
            int tx = j.x + j.w - j.row_h / 2;         /* the strip's column */
            koboy_input_state d = touch_at(tx, row_y(&j, 0));
            CHECK_EQ_INT(ui_list_feed(&j, &d, &idx), UI_SELECT);
            CHECK_EQ_INT(idx, 0);
            feed_release(&j, &idx);

            /* And A+B together, with alpha_jump off, falls through to the
               ordinary A-alone handling (checked first) -- UI_JUMP must
               never appear for a list that did not ask for it. */
            koboy_input_state ab = button((uint16_t)(KOBOY_BTN_A | KOBOY_BTN_B));
            ui_action a_off = ui_list_feed(&j, &ab, &idx);
            CHECK(a_off != UI_JUMP);
            feed_release(&j, &idx);
        }

        ui_list_enable_alpha_jump(&j, true);
        feed_release(&j, &idx);   /* re-clear the guard */

        /* Tap directly on a PRESENT letter (Z, band 26) lands on its first
           entry. */
        {
            koboy_input_state d = strip_tap(&j, 26);
            CHECK_EQ_INT(ui_list_feed(&j, &d, &idx), UI_JUMP);
            CHECK_EQ_INT(idx, 4);              /* ZEBRA.GB */
            feed_release(&j, &idx);
        }

        /* Tap an EMPTY letter (B, band 2) degrades to the nearest occupied
           letter AFTER it (C), not a crash, not a no-op, not a silent
           landing on A. */
        {
            koboy_input_state d = strip_tap(&j, 2);
            CHECK_EQ_INT(ui_list_feed(&j, &d, &idx), UI_JUMP);
            CHECK_EQ_INT(idx, 3);              /* CHERRY.GB */
            feed_release(&j, &idx);
        }

        /* Tap an empty letter PAST the last occupied one wraps around to
           the FIRST occupied bucket rather than finding nothing. A
           separate, smaller list for this: `az` has Z occupied, which a tap
           anywhere below band 26 would reach WITHOUT crossing the wrap
           boundary, so it cannot tell "found going forward" apart from
           "found by wrapping". This one's highest occupied bucket is A (1),
           so a tap on 'T' (band 20) has nothing ahead of it at all until
           the search wraps past Z back to '#'. */
        {
            static const char *const az_wrap[] = { "0START.GB", "APPLE.GB" };
            koboy_ui_list w;
            ui_list_init(&w, "WRAP", az_wrap, 2, 100, 200, 800, 900);
            ui_list_enable_alpha_jump(&w, true);
            feed_release(&w, &idx);

            koboy_input_state d = strip_tap(&w, 20);
            CHECK_EQ_INT(ui_list_feed(&w, &d, &idx), UI_JUMP);
            CHECK_EQ_INT(idx, 0);              /* 0START.GB, only via wraparound */
        }

        /* The strip does not shadow the row hit-test: a tap in the ROW
           area (not the strip's column) still selects a row normally, even
           though alpha_jump is on. */
        {
            koboy_input_state d = touch_at(j.x + j.w / 2, row_y(&j, 1));
            CHECK_EQ_INT(ui_list_feed(&j, &d, &idx), UI_SELECT);
            CHECK_EQ_INT(idx, 1);              /* APPLE.GB */
            feed_release(&j, &idx);
        }

        /* Nor does it shadow the footer arrows: a tap whose X falls inside
           the strip's column but whose Y is in the FOOTER row (not the
           list body) is still a page arrow, because the strip check is
           bounded to body rows only. Right third -> next page (this list
           has one page, so page cannot move past 0, but the ACTION must
           still be recognised as a footer tap, not a jump). */
        {
            int tx = j.x + j.w - j.row_h / 2;          /* strip's column, x-wise */
            int ty = j.y + j.h - j.row_h / 2;          /* footer row, y-wise */
            koboy_input_state d = touch_at(tx, ty);
            ui_action a2 = ui_list_feed(&j, &d, &idx);
            CHECK(a2 == UI_PAGE_NEXT || a2 == UI_NONE);   /* footer logic, not UI_JUMP */
            CHECK(a2 != UI_JUMP);
            feed_release(&j, &idx);
        }

        /* Hardware: A alone and B alone are UNCHANGED by alpha_jump being
           on -- still plain paging, never a jump. */
        {
            koboy_input_state a_btn = button(KOBOY_BTN_A);
            CHECK(ui_list_feed(&j, &a_btn, &idx) != UI_JUMP);
            feed_release(&j, &idx);
            koboy_input_state b_btn = button(KOBOY_BTN_B);
            CHECK(ui_list_feed(&j, &b_btn, &idx) != UI_JUMP);
            feed_release(&j, &idx);
        }

        /* A+B TOGETHER jumps from the current top item's letter to the
           NEXT occupied one, wrapping. Page 0's top item is 0START.GB
           ('#', bucket 0); the next occupied bucket after '#' is A (band 1,
           APPLE.GB) -- landing on the FIRST 'A' entry, not AVOCADO. */
        {
            koboy_input_state ab = button((uint16_t)(KOBOY_BTN_A | KOBOY_BTN_B));
            CHECK_EQ_INT(ui_list_feed(&j, &ab, &idx), UI_JUMP);
            CHECK_EQ_INT(idx, 1);              /* APPLE.GB, not AVOCADO.GB */
            feed_release(&j, &idx);
        }

        /* A list where every entry shares one letter: the combo has
           nowhere DIFFERENT to go, and must degrade to UI_NONE rather than
           report a jump that changed nothing. */
        {
            static const char *const one_letter[] = {
                "APPLE.GB", "APRICOT.GB", "AVOCADO.GB",
            };
            koboy_ui_list ol;
            ui_list_init(&ol, "ONE LETTER", one_letter, 3, 100, 200, 800, 900);
            ui_list_enable_alpha_jump(&ol, true);
            feed_release(&ol, &idx);

            koboy_input_state ab = button((uint16_t)(KOBOY_BTN_A | KOBOY_BTN_B));
            CHECK_EQ_INT(ui_list_feed(&ol, &ab, &idx), UI_NONE);
            CHECK_EQ_INT(ol.page, 0);
        }
    }

    /* ------------------------------------------------------------------
       ui_fit_label: the row-label fitting that keeps a long ROM name off
       the letter strip -- task 5's "long names overflow" fix.

       Asserted directly against character counts and text_measure, not
       just inferred from a rendered image: the whole point is a class of
       bug (two names differing only in a trailing parenthetical rendering
       IDENTICALLY once truncated) that a golden image would only catch if
       a reviewer happened to look at exactly the right two rows. */
    {
        char out[256];

        /* A name that already fits is returned VERBATIM, extension and all.
           This asserted stripped labels until the device owner asked for the
           extensions back: with two systems in one tree, the extension is
           what says which system a row is, and the folder it sits in is not
           a reliable substitute -- nothing stops a .gb beside a .mgw. */
        ui_fit_label("TETRIS.GB", 100000, TEST_LABEL_PX, out, sizeof out);
        CHECK(strcmp(out, "TETRIS.GB") == 0);
        ui_fit_label("KIRBY 2.gbc", 100000, TEST_LABEL_PX, out, sizeof out);
        CHECK(strcmp(out, "KIRBY 2.gbc") == 0);
        ui_fit_label("Fire (Silver).mgw", 100000, TEST_LABEL_PX, out, sizeof out);
        CHECK(strcmp(out, "Fire (Silver).mgw") == 0);
        ui_fit_label("BALL.MGW", 100000, TEST_LABEL_PX, out, sizeof out);
        CHECK(strcmp(out, "BALL.MGW") == 0);
        /* Case is preserved too, both ways -- a label is not normalised on
           its way to the panel. */
        ui_fit_label("BALL.mgwx", 100000, TEST_LABEL_PX, out, sizeof out);
        CHECK(strcmp(out, "BALL.mgwx") == 0);
        /* Not a ROM extension: left alone. */
        ui_fit_label("SLOT 1 - SAVED", 100000, TEST_LABEL_PX, out, sizeof out);
        CHECK(strcmp(out, "SLOT 1 - SAVED") == 0);

        /* Whatever comes back never exceeds avail_px, at every width from
           "plenty of room" down to "barely any". */
        const char *long_name =
            "Legend of Zelda, The - Link's Awakening (USA, Europe) (Rev 2).gb";
        for (int avail = 400; avail <= 2000; avail += 137) {
            ui_fit_label(long_name, avail, TEST_LABEL_PX, out, sizeof out);
            CHECK(text_measure(out, TEST_LABEL_PX) <= avail);
        }

        /* THE acceptance case: two names identical except for the trailing
           parenthetical must still read as DIFFERENT once both are
           truncated to the same narrow width -- proving the tail, not just
           the head, survives elision. */
        const char *usa =
            "Some Very Long Game Title That Does Not Fit The Row (USA).gb";
        const char *eur =
            "Some Very Long Game Title That Does Not Fit The Row (Europe).gb";
        char out_usa[256], out_eur[256];
        int narrow = text_measure("Some Very Long Game", TEST_LABEL_PX);
        ui_fit_label(usa, narrow, TEST_LABEL_PX, out_usa, sizeof out_usa);
        ui_fit_label(eur, narrow, TEST_LABEL_PX, out_eur, sizeof out_eur);
        CHECK(strcmp(out_usa, out_eur) != 0);
        /* And both are visibly truncated (contain the ellipsis), not just
           silently cut -- a truncated row must look truncated. */
        CHECK(strstr(out_usa, "...") != NULL);
        CHECK(strstr(out_eur, "...") != NULL);

        /* A width too narrow even for the ellipsis marker degrades to a
           bare, unmarked partial head rather than crashing or overrunning
           `out`. */
        ui_fit_label(long_name, TEXT_ADVANCE * TEST_LABEL_PX, TEST_LABEL_PX,
                    out, sizeof out);
        CHECK_EQ_INT((int)strlen(out), 1);
        ui_fit_label(long_name, 0, TEST_LABEL_PX, out, sizeof out);
        CHECK_EQ_INT(out[0], 0);

        /* A destination buffer smaller than the source (even with unlimited
           avail_px, so the source-fits-avail branch is the one taken) is
           respected, not overrun. */
        char tiny[6];
        ui_fit_label("ABCDEFGHIJKLMNOP", 100000, TEST_LABEL_PX, tiny, sizeof tiny);
        CHECK_EQ_INT((int)strlen(tiny), (int)sizeof tiny - 1);
    }

    /* ------------------------------------------------------------------
       ui_path_title: the ROM browser's breadcrumb header. Asserted by
       character count rather than only through a render, for the same
       reason as ui_fit_label above -- the title row is the ONE string
       ui_list_render does not fit to the widget, so a title that overran
       would not ellipsise, it would run off the panel or under the letter
       strip, and a golden image only catches that if someone looks. */
    {
        char t[UI_TITLE_CHARS + 8];

        /* At the root there is no breadcrumb at all, not a trailing
           separator with nothing after it. */
        ui_path_title(t, sizeof t, "ALL GAMES", "");
        CHECK(strcmp(t, "ALL GAMES") == 0);
        ui_path_title(t, sizeof t, "ALL GAMES", NULL);
        CHECK(strcmp(t, "ALL GAMES") == 0);

        /* The case this exists for: the user's own folder. */
        ui_path_title(t, sizeof t, "ALL GAMES", "Game and Watch");
        CHECK(strcmp(t, "ALL GAMES / Game and Watch") == 0);
        CHECK((int)strlen(t) <= UI_TITLE_CHARS);

        /* Nested paths keep their separators. */
        ui_path_title(t, sizeof t, "ALL GAMES", "gbc/rpg");
        CHECK(strcmp(t, "ALL GAMES / gbc/rpg") == 0);

        /* An over-long path is clamped, marked as elided, and keeps its
           TAIL -- the deepest folder is the one the user is standing in.
           Both halves matter: a head-keeping truncation would fit just as
           well and tell them nothing. */
        ui_path_title(t, sizeof t, "ALL GAMES",
                      "Some Absurdly Long Folder Name/Game and Watch");
        CHECK((int)strlen(t) <= UI_TITLE_CHARS);
        CHECK(strstr(t, "...") != NULL);
        CHECK(strstr(t, "Game and Watch") != NULL);
        CHECK(strstr(t, "Some Absurdly") == NULL);

        /* Every length across the boundary stays inside the budget -- the
           off-by-one at exactly UI_TITLE_CHARS is the one a single example
           would miss. */
        for (int n = 1; n < 80; n++) {
            char sub[96];
            for (int i = 0; i < n; i++) sub[i] = 'X';
            sub[n] = 0;
            ui_path_title(t, sizeof t, "ALL GAMES", sub);
            CHECK((int)strlen(t) <= UI_TITLE_CHARS);
        }

        /* A head that leaves no room for a breadcrumb degrades to the head
           alone rather than to punctuation. */
        ui_path_title(t, sizeof t,
                      "A HEAD LONG ENOUGH TO FILL THE WHOLE TITLE ROW", "sub");
        CHECK(strcmp(t, "A HEAD LONG ENOUGH TO FILL THE WHOLE TITLE ROW") == 0);
    }

    /* Rendering is clipped and draws something. */
    enum { W = 1264, H = 1680 };
    static uint8_t fb[W * H];
    memset(fb, 0xFF, sizeof fb);
    ui_list_render(&u, fb, W, W, H);
    int painted = 0;
    for (size_t i = 0; i < sizeof fb; i++) if (fb[i] != 0xFF) painted++;
    CHECK(painted > 0);

    /* ---- the in-game MENU's GREYSCALE row ------------------------------- */
    /* The label must actually TRACK the mapping. A row that always read
       "GREYSCALE" would look right on the panel and tell the owner nothing,
       and this project has shipped exactly that kind of decorative control
       before -- so the assertion is that all five labels are DISTINCT and
       each names its own mapping, not merely that the function returns a
       string. */
    {
        char lab[KOBOY_GRAY_COUNT][48];
        for (int m = 0; m < KOBOY_GRAY_COUNT; m++) {
            ui_gray_label(lab[m], sizeof lab[m], (koboy_gray_map)m);
            CHECK(strncmp(lab[m], "GREYSCALE: ", 11) == 0);
            /* The suffix is the map's own name, uppercased -- so the ini and
               the panel spell the same setting the same way. */
            const char *n = video_gray_map_name((koboy_gray_map)m);
            CHECK_EQ_INT((int)strlen(lab[m]), 11 + (int)strlen(n));
            int same = 1;
            for (size_t j = 0; n[j]; j++) {
                char up = (n[j] >= 'a' && n[j] <= 'z') ? (char)(n[j] - 'a' + 'A') : n[j];
                if (lab[m][11 + j] != up) same = 0;
            }
            CHECK(same);
        }
        for (int a = 0; a < KOBOY_GRAY_COUNT; a++)
            for (int b = a + 1; b < KOBOY_GRAY_COUNT; b++)
                CHECK(strcmp(lab[a], lab[b]) != 0);

        /* It fits a MENU row on the narrowest supported panel, so the entry
           the owner is meant to read is not the one that ellipsises. */
        for (int m = 0; m < KOBOY_GRAY_COUNT; m++)
            CHECK((int)strlen(lab[m]) <= UI_TITLE_CHARS);

        /* Truncates rather than overruns, and always terminates. */
        char tiny[6];
        memset(tiny, 'Z', sizeof tiny);
        ui_gray_label(tiny, sizeof tiny, KOBOY_GRAY_DEFAULT);
        CHECK_EQ_INT((int)strlen(tiny), 5);
        CHECK(strcmp(tiny, "GREYS") == 0);

        /* A zero-size buffer writes nothing at all. */
        char guard[2] = { 'Q', 'Q' };
        ui_gray_label(guard, 0, KOBOY_GRAY_DEFAULT);
        CHECK_EQ_INT(guard[0], 'Q');
    }

    /* ------------------------------------------- the MENU's FRAMES row
       Every value the shipped ladder can put on the panel, spelled out in
       full rather than rebuilt from the same format string the implementation
       uses -- a check that formats its own expectation agrees with any
       format. The strings are the assertion. */
    {
        static const struct { int d; const char *want; } cases[] = {
            /* The whole ladder. */
            { 1, "FRAMES: EVERY FRAME" },
            { 2, "FRAMES: EVERY 2ND"   },
            { 3, "FRAMES: EVERY 3RD"   },
            { 4, "FRAMES: EVERY 4TH"   },
            { 6, "FRAMES: EVERY 6TH"   },
            { 8, "FRAMES: EVERY 8TH"   },
            /* In range but off-ladder: an ini can say either, so the row has
               to read correctly for them too. */
            { 5, "FRAMES: EVERY 5TH"   },
            { 7, "FRAMES: EVERY 7TH"   },
            /* Past the clamp. Unreachable from the menu today, and asserted
               anyway: the function is public, takes an int, and these are the
               cases the single-digit ordinal shortcut would have got wrong
               ("11ST") the day KOBOY_PRESENT_DIVISOR_MAX is raised. Cheap
               here, silent and wrong there. */
            { 11, "FRAMES: EVERY 11TH" },
            { 12, "FRAMES: EVERY 12TH" },
            { 13, "FRAMES: EVERY 13TH" },
            { 21, "FRAMES: EVERY 21ST" },
            { 22, "FRAMES: EVERY 22ND" },
            { 23, "FRAMES: EVERY 23RD" },
            { 111, "FRAMES: EVERY 111TH" },
            /* Nonsense in, English out: no "EVERY 0TH", no "EVERY -1ST". */
            { 0,  "FRAMES: EVERY FRAME" },
            { -3, "FRAMES: EVERY FRAME" },
        };
        char lab[64];
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            ui_divisor_label(lab, sizeof lab, cases[i].d);
            CHECK(strcmp(lab, cases[i].want) == 0);
            if (strcmp(lab, cases[i].want) != 0)
                fprintf(stderr, "  divisor %d gave \"%s\"\n", cases[i].d, lab);
        }

        /* Distinct rows for distinct values -- a label that dropped the number
           would look plausible on the panel and say nothing. */
        char a[64], b[64];
        ui_divisor_label(a, sizeof a, 3);
        ui_divisor_label(b, sizeof b, 6);
        CHECK(strcmp(a, b) != 0);

        /* Fits a MENU row on the narrowest supported panel. */
        for (int d = 1; d <= KOBOY_PRESENT_DIVISOR_MAX; d++) {
            ui_divisor_label(lab, sizeof lab, d);
            CHECK((int)strlen(lab) <= UI_TITLE_CHARS);
        }

        /* Truncates rather than overruns, and always terminates. */
        char tiny[6];
        memset(tiny, 'Z', sizeof tiny);
        ui_divisor_label(tiny, sizeof tiny, 3);
        CHECK_EQ_INT((int)strlen(tiny), 5);
        CHECK(strcmp(tiny, "FRAME") == 0);

        /* A zero-size buffer writes nothing at all. */
        char guard2[2] = { 'Q', 'Q' };
        ui_divisor_label(guard2, 0, 3);
        CHECK_EQ_INT(guard2[0], 'Q');
    }
})
