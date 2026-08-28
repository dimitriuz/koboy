#define _DEFAULT_SOURCE
#include "test.h"
#include "fakeplat.h"

#include "config.h"
#include "koboy.h"
#include "recent.h"
#include "romlist.h"
#include "screens.h"
#include "state.h"
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The six full-panel screens, driven directly.
 *
 * NONE OF THIS WAS POSSIBLE BEFORE src/screens.c EXISTED: these functions were
 * static inside main.c, which the Makefile filters out of $(SRC), so the only
 * instrument that could reach them was tests/smoke_host.sh reading an exit code
 * and stdout. That script drives the MENU by HARDCODED PANEL PIXELS -- a fifth
 * hand-derivation of geometry ui.c already computes -- so it can say a run
 * selected SOMETHING and cannot say which row, what was drawn, or with which
 * waveform.
 *
 * EVERY TAP COORDINATE HERE IS COMPUTED FROM THE WIDGET, never typed in: a
 * probe koboy_ui_list built with the same geometry every screen uses answers
 * "where is row k" from its own row_h/y. A changed row height moves these taps
 * with it instead of stranding them -- the exact failure the smoke script's
 * comment warns about and cannot prevent. */

#define PW 1264      /* the verified Libra 2 panel */
#define PH 1680

/* The region every screen hands ui_list_init, copied from screens.c so a probe
   list has identical geometry. Not shared through a header on purpose: if this
   ever drifts, the taps below miss and the tests go red, which is the whole
   point of deriving them rather than hardcoding them. */
static void probe_list(koboy_ui_list *u)
{
    static const char *const one[1] = { "PROBE" };
    ui_list_init(u, "PROBE", one, 1,
                 KOBOY_CHROME_MARGIN, KOBOY_CHROME_MARGIN,
                 PW - 2 * KOBOY_CHROME_MARGIN, PH - 2 * KOBOY_CHROME_MARGIN);
}

/* Panel y of the vertical centre of body row `r` (row 0 is the first item;
   the widget's row 0 is its title). */
static int row_cy(int r)
{
    koboy_ui_list p;
    probe_list(&p);
    return p.y + p.row_h + r * p.row_h + p.row_h / 2;
}

static int rows_per_page(void)
{
    koboy_ui_list p;
    probe_list(&p);
    return p.rows;
}

/* A synthetic finger down at a panel coordinate, and the release after it.
   These go into screen_list's SCRIPT path, which takes koboy_input_state
   directly -- the live path is driven through input.c further down, because a
   hand-built state skips the decode and that is what hid the faceplate bug. */
static koboy_input_state tap_at(int x, int y)
{
    koboy_input_state st;
    memset(&st, 0, sizeof st);
    st.touch[0].x = x; st.touch[0].y = y; st.touch[0].down = true;
    return st;
}

static koboy_input_state lift(void)
{
    koboy_input_state st;
    memset(&st, 0, sizeof st);
    return st;
}

/* One finger down at a panel coordinate through the REAL evdev decode, the way
   tests/test_ui.c and tests/test_chrome.c do it. */
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

/* The live-input case: nothing on the first poll, then tap (g_live_x, g_live_y),
   then release. A file-scope target because koboy_platform's poll_input takes
   no test-supplied payload beyond the fakeplat itself.

   THE EMPTY FIRST POLL IS NOT PADDING. ui_list_init sets prev_touch = true, so
   a fresh list refuses its first tap until it has seen a release -- the same
   rule the scripted path satisfies with its synthetic primer. On the device
   the finger IS up when a screen opens, so the first real poll supplies that
   release for free; here it has to be spelled out, and a down on poll 0 is
   swallowed exactly as it would be on hardware. */
static int g_live_x, g_live_y;
static void poll_tap_row(fakeplat *fp, koboy_input *in, int poll)
{
    (void)fp;
    if (poll == 1)      feed_down(in, g_live_x, g_live_y);
    else if (poll == 2) feed_up(in);
}

/* The browser's ".." row, and the same row when the tree has gone away
   underneath it. Driven live rather than by script because "the directory was
   removed between the scan and the navigation" is a real e-reader event -- a
   card pulled, or Nickel's own indexer moving a folder -- and it is the only
   way to reach romlist_up's failure return without chmod, which does nothing
   when the suite runs as root. */
static char g_vanish_root[256];
static int  g_dir_row, g_up_row;
static void vanish_tree(void);
static void poll_enter_then_up(fakeplat *fp, koboy_input *in, int poll)
{
    (void)fp;
    if (poll == 1)      feed_down(in, PW / 2, row_cy(g_dir_row));
    else if (poll == 2) feed_up(in);
    else if (poll == 3) { vanish_tree(); feed_down(in, PW / 2, row_cy(g_up_row)); }
}

static void unlink_in(const char *dir, const char *name)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    unlink(path);
}

static void touch_file(const char *dir, const char *name)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (f) { fputc('x', f); fclose(f); }
}

/* Removes everything poll_enter_then_up's fixture built, deepest first. */
static void vanish_tree(void)
{
    char sub[512];
    snprintf(sub, sizeof sub, "%.400s/GBC", g_vanish_root);
    unlink_in(sub, "KIRBY.gbc");
    rmdir(sub);
    unlink_in(g_vanish_root, "TETRIS.gb");
    rmdir(g_vanish_root);
}

/* The row index of the first entry of `kind` in a fresh listing of `dir`, or
   -1. Used to assert that a bare scan has no ".." row, so the "+1" the nested
   pick below relies on is a fact about romlist_enter and not an assumption. */
static int browser_row_of_kind(const char *dir, int kind)
{
    koboy_romlist rl;
    memset(&rl, 0, sizeof rl);
    int n = romlist_scan(&rl, dir);
    int found = -1;
    for (int i = 0; i < n && found < 0; i++)
        if (romlist_kind(&rl, i) == kind) found = i;
    romlist_free(&rl);
    return found;
}

/* The row index a name occupies in a fresh listing of `dir`, or -1. Asked of
   romlist itself rather than assumed, because the browser's sort order is
   romlist.c's business and a test that hardcoded row 0 would be asserting the
   sort as much as the browser. */
static int browser_row_of(const char *dir, const char *name)
{
    koboy_romlist rl;
    memset(&rl, 0, sizeof rl);
    int n = romlist_scan(&rl, dir);
    int found = -1;
    for (int i = 0; i < n && found < 0; i++)
        if (!strcmp(romlist_name(&rl, i), name)) found = i;
    romlist_free(&rl);
    return found;
}

TEST_MAIN({
    /* ------------------------------------------------ the fake itself, first
       A fake platform that lies is worse than none, so it gets a test before
       anything is asserted through it. MUTANT: `py = y + r + 1` in
       fakeplat_blit_cb -- both offset checks below go red. */
    {
        fakeplat fp;
        fakeplat_init(&fp, 64, 32);
        CHECK(fp.panel != NULL);
        CHECK(fp.stride > fp.pw);          /* a padded stride, so a row-index
                                              bug cannot hide behind w == stride */
        uint8_t src[4 * 2];
        for (int i = 0; i < 8; i++) src[i] = (uint8_t)(0x10 + i);
        fp.pf.blit_gray8(fp.pf.ctx, src, 4, 2, 4, 3, 5);
        CHECK_EQ_INT(fp.blits, 1);
        CHECK_EQ_INT(fp.panel[5 * fp.stride + 3], 0x10);
        CHECK_EQ_INT(fp.panel[6 * fp.stride + 6], 0x17);
        /* The pixel just outside the blit stays untouched. */
        CHECK_EQ_INT(fp.panel[5 * fp.stride + 2], 0x00);
        CHECK_EQ_INT(fp.panel[4 * fp.stride + 3], 0x00);

        fp.pf.refresh(fp.pf.ctx, 1, 2, 3, 4, KOBOY_REFRESH_FAST);
        CHECK_EQ_INT(fp.refreshes, 1);
        CHECK_EQ_INT(fp.refresh[0].x, 1);
        CHECK_EQ_INT(fp.refresh[0].y, 2);
        CHECK_EQ_INT(fp.refresh[0].w, 3);
        CHECK_EQ_INT(fp.refresh[0].h, 4);
        CHECK_EQ_INT(fp.refresh[0].mode, KOBOY_REFRESH_FAST);
        fakeplat_free(&fp);
    }

    /* The panel buffer every screen below draws into. One allocation, reused:
       these are full-panel screens and the content is overwritten each time. */
    koboy_ui_list geom;
    probe_list(&geom);
    CHECK(geom.row_h > 0);
    CHECK(geom.rows >= 10);            /* every fixed list below fits one page */

    /* -------------------------------------------------- screen_list, scripted
       A selection, and what reached the panel while it happened. The second
       half is the part smoke_host.sh cannot see at all. */
    {
        fakeplat fp;
        fakeplat_init(&fp, PW, PH);
        uint8_t *panel = (uint8_t *)malloc((size_t)fp.stride * PH);
        static const char *const items[4] = { "ONE", "TWO", "THREE", "FOUR" };
        koboy_ui_list u;
        ui_list_init(&u, "T", items, 4, KOBOY_CHROME_MARGIN, KOBOY_CHROME_MARGIN,
                     PW - 2 * KOBOY_CHROME_MARGIN, PH - 2 * KOBOY_CHROME_MARGIN);

        koboy_input_state script[1] = { tap_at(PW / 2, row_cy(2)) };
        int si = 0;
        int got = screen_list(&fp.pf, NULL, &u, panel, fp.stride, PW, PH,
                              script, &si, 1, -1);
        CHECK_EQ_INT(got, 2);
        CHECK_EQ_INT(si, 1);           /* the shared cursor advanced */

        /* Drawn exactly once, and refreshed FULL over the whole panel. FULL
           because a list is about to sit still and the game rect's four-level
           ceiling does not apply to it -- src/screens.c says so and nothing
           until now could check it.
           MUTANT: KOBOY_REFRESH_FAST in screen_list's refresh call. */
        CHECK_EQ_INT(fp.blits, 1);
        CHECK_EQ_INT(fp.refreshes, 1);
        CHECK_EQ_INT(fp.refresh[0].mode, KOBOY_REFRESH_FULL);
        CHECK_EQ_INT(fp.refresh[0].x, 0);
        CHECK_EQ_INT(fp.refresh[0].y, 0);
        CHECK_EQ_INT(fp.refresh[0].w, PW);
        CHECK_EQ_INT(fp.refresh[0].h, PH);
        /* Something was actually rendered: ui_list_render fills the region
           white and inks glyphs into it, so the panel is neither all-zero
           (the calloc) nor uniform. */
        int white = 0, ink = 0;
        for (int y = 0; y < PH; y++)
            for (int x = 0; x < PW; x++) {
                uint8_t v = fp.panel[(size_t)y * (size_t)fp.stride + (size_t)x];
                if (v == 0xFF) white++;
                else if (v == 0x00) ink++;
            }
        CHECK(white > PW * PH / 2);
        CHECK(ink > 0);

        free(panel);
        fakeplat_free(&fp);
    }

    /* ---------------------------------- the priming release, and why it exists
       ui_list_init sets prev_touch = true, so a script whose FIRST verb is a
       tap had its press swallowed unless screen_list feeds one synthetic
       release first. Without that, a scripted run selects nothing and exits 0
       -- a green CI run that tested nothing, confirmed on hardware.
       MUTANT: delete the `if (!primed)` block. This goes to -1. */
    {
        fakeplat fp;
        fakeplat_init(&fp, PW, PH);
        uint8_t *panel = (uint8_t *)malloc((size_t)fp.stride * PH);
        static const char *const items[3] = { "A", "B", "C" };
        koboy_ui_list u;
        ui_list_init(&u, "T", items, 3, KOBOY_CHROME_MARGIN, KOBOY_CHROME_MARGIN,
                     PW - 2 * KOBOY_CHROME_MARGIN, PH - 2 * KOBOY_CHROME_MARGIN);
        CHECK(u.prev_touch);          /* the precondition this is all about */

        koboy_input_state script[1] = { tap_at(PW / 2, row_cy(1)) };
        int si = 0;
        CHECK_EQ_INT(screen_list(&fp.pf, NULL, &u, panel, fp.stride, PW, PH,
                                 script, &si, 1, -1), 1);
        free(panel);
        fakeplat_free(&fp);
    }

    /* ------------------------------------------- the cursor spans two screens
       script_i is a pointer, not a local index, so one flat script walks
       through several screens in a row -- each picking up where the previous
       one stopped consuming. Nothing asserted this before; smoke_host.sh
       exercises it end to end but reports only the final exit code.
       MUTANT: `int si = 0;` instead of `script_i ? *script_i : 0` in
       screen_list. The second call replays the first tap and returns 1. */
    {
        fakeplat fp;
        fakeplat_init(&fp, PW, PH);
        uint8_t *panel = (uint8_t *)malloc((size_t)fp.stride * PH);
        static const char *const items[5] = { "A", "B", "C", "D", "E" };

        koboy_input_state script[4] = {
            tap_at(PW / 2, row_cy(1)), lift(),
            tap_at(PW / 2, row_cy(3)), lift(),
        };
        int si = 0;

        koboy_ui_list u1;
        ui_list_init(&u1, "T", items, 5, KOBOY_CHROME_MARGIN, KOBOY_CHROME_MARGIN,
                     PW - 2 * KOBOY_CHROME_MARGIN, PH - 2 * KOBOY_CHROME_MARGIN);
        CHECK_EQ_INT(screen_list(&fp.pf, NULL, &u1, panel, fp.stride, PW, PH,
                                 script, &si, 4, -1), 1);
        CHECK_EQ_INT(si, 1);

        koboy_ui_list u2;
        ui_list_init(&u2, "T", items, 5, KOBOY_CHROME_MARGIN, KOBOY_CHROME_MARGIN,
                     PW - 2 * KOBOY_CHROME_MARGIN, PH - 2 * KOBOY_CHROME_MARGIN);
        CHECK_EQ_INT(screen_list(&fp.pf, NULL, &u2, panel, fp.stride, PW, PH,
                                 script, &si, 4, -1), 3);
        CHECK_EQ_INT(si, 3);

        /* A script that runs out selects nothing, which is exit 4 territory
           for a --ui-script run rather than a silent success. */
        koboy_ui_list u3;
        ui_list_init(&u3, "T", items, 5, KOBOY_CHROME_MARGIN, KOBOY_CHROME_MARGIN,
                     PW - 2 * KOBOY_CHROME_MARGIN, PH - 2 * KOBOY_CHROME_MARGIN);
        CHECK_EQ_INT(screen_list(&fp.pf, NULL, &u3, panel, fp.stride, PW, PH,
                                 script, &si, 4, -1), -1);
        free(panel);
        fakeplat_free(&fp);
    }

    /* ------------------------------------------------------- disabled_index
       The row that selects nothing: the browser's "+N MORE ROMS NOT SHOWN"
       overflow row and the RECENT list's empty-state placeholder both use it,
       and handing either to romlist_path / recent_path as if it were real is
       the shape of a bug this project has already had.
       MUTANT: drop `&& idx != disabled_index`. The first tap returns 2. */
    {
        fakeplat fp;
        fakeplat_init(&fp, PW, PH);
        uint8_t *panel = (uint8_t *)malloc((size_t)fp.stride * PH);
        static const char *const items[4] = { "A", "B", "DEAD", "D" };
        koboy_ui_list u;
        ui_list_init(&u, "T", items, 4, KOBOY_CHROME_MARGIN, KOBOY_CHROME_MARGIN,
                     PW - 2 * KOBOY_CHROME_MARGIN, PH - 2 * KOBOY_CHROME_MARGIN);

        koboy_input_state script[3] = {
            tap_at(PW / 2, row_cy(2)), lift(), tap_at(PW / 2, row_cy(0)),
        };
        int si = 0;
        CHECK_EQ_INT(screen_list(&fp.pf, NULL, &u, panel, fp.stride, PW, PH,
                                 script, &si, 3, 2), 0);
        CHECK_EQ_INT(si, 3);           /* all three states were consumed */
        free(panel);
        fakeplat_free(&fp);
    }

    /* ------------------------------------------------------------ koboy_stop
       The signal flag every screen loop polls. It lives in screens.c precisely
       so this can be written; before the split it was a static in main.c.
       MUTANT: drop `!koboy_stop` from screen_list's while condition -- this
       returns 1 and blits once. */
    {
        fakeplat fp;
        fakeplat_init(&fp, PW, PH);
        uint8_t *panel = (uint8_t *)malloc((size_t)fp.stride * PH);
        static const char *const items[3] = { "A", "B", "C" };
        koboy_ui_list u;
        ui_list_init(&u, "T", items, 3, KOBOY_CHROME_MARGIN, KOBOY_CHROME_MARGIN,
                     PW - 2 * KOBOY_CHROME_MARGIN, PH - 2 * KOBOY_CHROME_MARGIN);
        koboy_input_state script[1] = { tap_at(PW / 2, row_cy(1)) };
        int si = 0;

        koboy_stop = 1;
        CHECK_EQ_INT(screen_list(&fp.pf, NULL, &u, panel, fp.stride, PW, PH,
                                 script, &si, 1, -1), -1);
        /* Not even a repaint: a stopped run must not leave a half-drawn list
           on a panel that holds its last image with the power off. */
        CHECK_EQ_INT(fp.blits, 0);
        CHECK_EQ_INT(fp.refreshes, 0);
        koboy_stop = 0;                /* a global, and the binary runs on */

        /* And with the flag clear the same call selects, so the check above
           is about koboy_stop and not about a script that never worked. */
        si = 0;
        CHECK_EQ_INT(screen_list(&fp.pf, NULL, &u, panel, fp.stride, PW, PH,
                                 script, &si, 1, -1), 1);
        free(panel);
        fakeplat_free(&fp);
    }

    /* --------------------------------------------- the live path: should_quit
       The unscripted branch, which no automated test has ever entered -- every
       --ui-script run takes the other one. That asymmetry is the shape of v1's
       first-run deadlock, which twenty reviews missed because every automated
       test took the one path that could not reach it.
       MUTANT: drop `!pf->should_quit(pf->ctx)` -- the loop never ends and the
       suite hangs, which is a failure a human notices immediately. */
    {
        fakeplat fp;
        fakeplat_init(&fp, PW, PH);
        fp.quit_after_polls = 3;
        uint8_t *panel = (uint8_t *)malloc((size_t)fp.stride * PH);
        static const char *const items[3] = { "A", "B", "C" };
        koboy_ui_list u;
        ui_list_init(&u, "T", items, 3, KOBOY_CHROME_MARGIN, KOBOY_CHROME_MARGIN,
                     PW - 2 * KOBOY_CHROME_MARGIN, PH - 2 * KOBOY_CHROME_MARGIN);

        koboy_config c; config_defaults(&c);
        koboy_profile prof;
        CHECK(config_resolve_profile(&prof, &c, PW, PH,
                                     KOBOY_GB_W, KOBOY_GB_H, KOBOY_GB_W, KOBOY_GB_H));
        koboy_input *in = input_create(&c, &prof);
        CHECK(in != NULL);
        /* Identity transform: raw_max == panel - 1 makes the scaling a no-op,
           so feed_down's arguments are panel coordinates. */
        input_set_touch_transform(in, PW - 1, PH - 1, false, false, false);

        CHECK_EQ_INT(screen_list(&fp.pf, in, &u, panel, fp.stride, PW, PH,
                                 NULL, NULL, 0, -1), -1);
        CHECK_EQ_INT(fp.polls, 3);
        /* One draw, not one per poll: need_draw is edge-triggered, and a list
           that repainted every 5 ms would flash an e-ink panel continuously. */
        CHECK_EQ_INT(fp.blits, 1);

        /* Now the same live path with a finger on it. This goes through
           input_feed -> input_ui_state -> ui_list_feed, i.e. the real decode,
           which a hand-built koboy_input_state skips.
           MUTANT: input_state() instead of input_ui_state() in screen_list --
           see below, where the tap lands on the faceplate's A disc. */
        fakeplat_reset_log(&fp);
        fp.quit_after_polls = 8;
        fp.on_poll = poll_tap_row;
        g_live_x = PW / 2; g_live_y = row_cy(1);
        koboy_ui_list u2;
        ui_list_init(&u2, "T", items, 3, KOBOY_CHROME_MARGIN, KOBOY_CHROME_MARGIN,
                     PW - 2 * KOBOY_CHROME_MARGIN, PH - 2 * KOBOY_CHROME_MARGIN);
        CHECK_EQ_INT(screen_list(&fp.pf, in, &u2, panel, fp.stride, PW, PH,
                                 NULL, NULL, 0, -1), 1);
        CHECK(fp.polls < 8);           /* it selected, it did not time out */

        input_destroy(in);
        free(panel);
        fakeplat_free(&fp);
    }

    /* -------------------- a live tap on a row the faceplate's A disc sits on
       The faceplate's touch zones do not stop being live under a full-panel
       list: input.c synthesises KOBOY_BTN_A from a tap inside the A disc, and
       ui_list_feed consumes a rising A as page-previous BEFORE any row
       hit-test. screen_list calls input_ui_state, which drops the synthesised
       bits and keeps the coordinates. tests/test_ui.c proves that about
       ui_list_feed; this proves the SCREEN wires it up, which is the link that
       was untestable.
       MUTANT: input_state() in place of input_ui_state() in screen_list -- the
       tap pages instead of selecting and this returns -1. */
    {
        fakeplat fp;
        fakeplat_init(&fp, PW, PH);
        fp.quit_after_polls = 8;
        fp.on_poll = poll_tap_row;
        uint8_t *panel = (uint8_t *)malloc((size_t)fp.stride * PH);

        koboy_config c; config_defaults(&c);
        koboy_profile prof;
        CHECK(config_resolve_profile(&prof, &c, PW, PH,
                                     KOBOY_GB_W, KOBOY_GB_H, KOBOY_GB_W, KOBOY_GB_H));
        koboy_input *in = input_create(&c, &prof);
        CHECK(in != NULL);
        input_set_touch_transform(in, PW - 1, PH - 1, false, false, false);

        /* The A disc's CENTRE, in panel pixels, and the row it falls on --
           both computed from the layout rather than assumed, and CHECKed,
           because a layout change that moved the disc off the list's rows
           would otherwise leave this block quietly asserting nothing. The tap
           below goes at the disc itself, not at the row's midline: a tap that
           merely shared the row's y would synthesise no A bit at all and the
           mutant would survive. It did, on the first version of this test. */
        int acx = c.layout.a_cx * PW / 1000;
        int acy = c.layout.a_cy * PH / 1000;
        int a_row = (acy - geom.y - geom.row_h) / geom.row_h;
        CHECK(a_row >= 0 && a_row < geom.rows);
        CHECK(acx >= geom.x && acx < geom.x + geom.w);

        int n = a_row + 4;                 /* enough rows to reach it */
        const char **items = (const char **)malloc(sizeof(char *) * (size_t)n);
        for (int i = 0; i < n; i++) items[i] = "ROM";
        koboy_ui_list u;
        ui_list_init(&u, "ALL GAMES", (const char *const *)items, n,
                     KOBOY_CHROME_MARGIN, KOBOY_CHROME_MARGIN,
                     PW - 2 * KOBOY_CHROME_MARGIN, PH - 2 * KOBOY_CHROME_MARGIN);
        g_live_x = acx; g_live_y = acy;
        CHECK_EQ_INT(screen_list(&fp.pf, in, &u, panel, fp.stride, PW, PH,
                                 NULL, NULL, 0, -1), a_row);

        free(items);
        input_destroy(in);
        free(panel);
        fakeplat_free(&fp);
    }

    /* ------------------------------------------------------------ screen_menu
       Every MENU_* row, by tapping the row its enum index names. This is the
       coupling src/screens.h's comment describes and tests/smoke_host.sh
       encodes as twenty-eight hardcoded pixel pairs: a row inserted above an
       existing one strands every tap below it. Here the widget supplies the
       coordinates, so an inserted row moves the taps instead of stranding
       them, and the loop below fails only if the ENUM and the RENDERED order
       actually disagree.
       MUTANT: swap MENU_MOTION and MENU_SHOT in the enum without swapping the
       items[] assignments -- the two rows go red. */
    {
        fakeplat fp;
        fakeplat_init(&fp, PW, PH);
        uint8_t *panel = (uint8_t *)malloc((size_t)fp.stride * PH);
        CHECK(MENU_COUNT <= rows_per_page());     /* one page, no paging taps */

        int checked = 0;
        for (int row = 0; row < MENU_COUNT; row++) {
            koboy_input_state script[1] = { tap_at(PW / 2, row_cy(row)) };
            int si = 0;
            int act = screen_menu(&fp.pf, NULL, panel, fp.stride, PW, PH,
                                  true, KOBOY_GRAY_BALANCED, 3, false,
                                  KOBOY_WFM_AUTO, 1, script, &si, 1);
            CHECK_EQ_INT(act, row);
            checked++;
        }
        /* The sweep must actually have swept, the guard tests/test_chrome.c
           and tests/test_video_pipeline.c already carry. */
        CHECK_EQ_INT(checked, MENU_COUNT);

        /* has_states == false: SAVE and LOAD are labelled UNSUPPORTED and must
           not be actionable, or the caller opens a slot picker for a core with
           no serialisation.
           MUTANT: delete the `(pick == MENU_SAVE || pick == MENU_LOAD) &&
           !has_states` line -- both of these return the row instead. */
        for (int row = MENU_SAVE; row <= MENU_LOAD; row++) {
            koboy_input_state script[1] = { tap_at(PW / 2, row_cy(row)) };
            int si = 0;
            CHECK_EQ_INT(screen_menu(&fp.pf, NULL, panel, fp.stride, PW, PH,
                                     false, KOBOY_GRAY_BALANCED, 3, false,
                                     KOBOY_WFM_AUTO, 1, script, &si, 1),
                         MENU_RESUME);
        }
        /* Every other row still acts when has_states is false: the two rows
           above are refused, not the whole menu. */
        {
            koboy_input_state script[1] = { tap_at(PW / 2, row_cy(MENU_RESET)) };
            int si = 0;
            CHECK_EQ_INT(screen_menu(&fp.pf, NULL, panel, fp.stride, PW, PH,
                                     false, KOBOY_GRAY_BALANCED, 3, false,
                                     KOBOY_WFM_AUTO, 1, script, &si, 1),
                         MENU_RESET);
        }

        /* Backing out -- an exhausted script, or a stopped run -- resumes the
           game rather than reporting a row. -1 leaking out of here would index
           items[] in the caller.
           MUTANT: `if (pick < 0) return pick;`. */
        {
            koboy_input_state script[1] = { lift() };
            int si = 0;
            CHECK_EQ_INT(screen_menu(&fp.pf, NULL, panel, fp.stride, PW, PH,
                                     true, KOBOY_GRAY_BALANCED, 3, false,
                                     KOBOY_WFM_AUTO, 1, script, &si, 1),
                         MENU_RESUME);
        }
        free(panel);
        fakeplat_free(&fp);
    }

    /* ----------------------------------------------------- screen_slot_picker
       "The one screen nothing drives yet", its own comment says -- wired for a
       script so that a script which tapped SAVE STATE would fail rather than
       hang, but never walked into. It is walked into here.
       MUTANT: `return pick;` instead of `return pick + 1` -- slot 1 becomes 0,
       which is the caller's "backed out" answer, so a SAVE would silently do
       nothing. Every slot check below goes red. */
    {
        fakeplat fp;
        fakeplat_init(&fp, PW, PH);
        uint8_t *panel = (uint8_t *)malloc((size_t)fp.stride * PH);
        char dir[] = "/tmp/koboy_screens_slots_XXXXXX";
        CHECK(mkdtemp(dir) != NULL);

        for (int s = 0; s < KOBOY_STATE_SLOTS; s++) {
            koboy_input_state script[1] = { tap_at(PW / 2, row_cy(s)) };
            int si = 0;
            CHECK_EQ_INT(screen_slot_picker(&fp.pf, NULL, panel, fp.stride, PW, PH,
                                            "SAVE STATE", dir, "/roms/T.gb",
                                            script, &si, 1), s + 1);
        }
        /* BACK is the trailing row and reports 0, the same "nothing chosen"
           answer an exhausted script gives.
           MUTANT: drop `pick >= KOBOY_STATE_SLOTS` -- BACK returns 4, and
           state_path would build a slot-4 file the labels never showed. */
        {
            koboy_input_state script[1] = { tap_at(PW / 2, row_cy(KOBOY_STATE_SLOTS)) };
            int si = 0;
            CHECK_EQ_INT(screen_slot_picker(&fp.pf, NULL, panel, fp.stride, PW, PH,
                                            "SAVE STATE", dir, "/roms/T.gb",
                                            script, &si, 1), 0);
        }
        {
            koboy_input_state script[1] = { lift() };
            int si = 0;
            CHECK_EQ_INT(screen_slot_picker(&fp.pf, NULL, panel, fp.stride, PW, PH,
                                            "LOAD STATE", dir, "/roms/T.gb",
                                            script, &si, 1), 0);
        }
        rmdir(dir);
        free(panel);
        fakeplat_free(&fp);
    }

    /* ------------------------------------------------------ screen_main_menu */
    {
        fakeplat fp;
        fakeplat_init(&fp, PW, PH);
        uint8_t *panel = (uint8_t *)malloc((size_t)fp.stride * PH);
        for (int row = 0; row < MAIN_COUNT; row++) {
            koboy_input_state script[1] = { tap_at(PW / 2, row_cy(row)) };
            int si = 0;
            CHECK_EQ_INT(screen_main_menu(&fp.pf, NULL, panel, fp.stride, PW, PH,
                                          script, &si, 1), row);
        }
        /* An exhausted script reports -1 and NOT MAIN_RECENT: this screen is
           the one --ui-script has to navigate before it can reach anything, so
           a default-to-the-first-row would make every script that failed to
           tap here look like it had chosen RECENT. */
        {
            koboy_input_state script[1] = { lift() };
            int si = 0;
            CHECK_EQ_INT(screen_main_menu(&fp.pf, NULL, panel, fp.stride, PW, PH,
                                          script, &si, 1), -1);
        }
        free(panel);
        fakeplat_free(&fp);
    }

    /* --------------------------------------------------- screen_recent_picker
       The empty list is the interesting one. Its placeholder row is a real,
       tappable row that must select NOTHING -- if it selected, index 0 would
       reach recent_path on a list with no entries. RECENT has already had one
       aliasing bug and this is the same family.
       MUTANT: pass -1 instead of `placeholder` to screen_list -- the tap on
       the placeholder returns 0 and this check goes red. */
    {
        fakeplat fp;
        fakeplat_init(&fp, PW, PH);
        uint8_t *panel = (uint8_t *)malloc((size_t)fp.stride * PH);

        koboy_recent rc;
        recent_init(&rc);
        CHECK_EQ_INT(rc.count, 0);
        {
            /* Tap the placeholder, release, tap it again: two attempts, so a
               single swallowed edge cannot be mistaken for the row being
               refused. Both must be ignored, and the script then runs out. */
            koboy_input_state script[3] = {
                tap_at(PW / 2, row_cy(0)), lift(), tap_at(PW / 2, row_cy(0)),
            };
            int si = 0;
            CHECK_EQ_INT(screen_recent_picker(&fp.pf, NULL, panel, fp.stride, PW, PH,
                                              &rc, script, &si, 3), -1);
            CHECK_EQ_INT(si, 3);
        }
        {
            /* BACK on the empty list is row 1, and reports -1 as well -- the
               same answer, reached the other way. */
            koboy_input_state script[1] = { tap_at(PW / 2, row_cy(1)) };
            int si = 0;
            CHECK_EQ_INT(screen_recent_picker(&fp.pf, NULL, panel, fp.stride, PW, PH,
                                              &rc, script, &si, 1), -1);
        }

        /* A populated list: rows are the entries, most recent first, and BACK
           is the row after them.
           MUTANT: `back_index = n` (before the increment moves on) or
           `return pick + 1` -- the index checks go red. */
        recent_touch(&rc, "/roms/AAA.gb");
        recent_touch(&rc, "/roms/BBB.gb");
        recent_touch(&rc, "/roms/CCC.gb");
        CHECK_EQ_INT(rc.count, 3);
        CHECK(strcmp(recent_path(&rc, 1), "/roms/BBB.gb") == 0);
        for (int row = 0; row < rc.count; row++) {
            koboy_input_state script[1] = { tap_at(PW / 2, row_cy(row)) };
            int si = 0;
            CHECK_EQ_INT(screen_recent_picker(&fp.pf, NULL, panel, fp.stride, PW, PH,
                                              &rc, script, &si, 1), row);
        }
        {
            koboy_input_state script[1] = { tap_at(PW / 2, row_cy(rc.count)) };
            int si = 0;
            CHECK_EQ_INT(screen_recent_picker(&fp.pf, NULL, panel, fp.stride, PW, PH,
                                              &rc, script, &si, 1), -1);
        }
        free(panel);
        fakeplat_free(&fp);
    }

    /* -------------------------------------------------------- screen_browser
       Against real directories, because the three failure answers it returns
       are all filesystem facts and a mock of the scan would be mocking the
       thing under test. */
    {
        fakeplat fp;
        fakeplat_init(&fp, PW, PH);
        uint8_t *panel = (uint8_t *)malloc((size_t)fp.stride * PH);
        char out[512];

        /* A rom_dir that is not there at all. On the device this is a card
           without .adds/koboy/roms, and it must not read the same as an empty
           one: the messages the two produce tell a user different things. */
        {
            int si = 0;
            koboy_input_state script[1] = { lift() };
            CHECK_EQ_INT(screen_browser(&fp.pf, NULL, panel, fp.stride, PW, PH,
                                        "/nonexistent/koboy/screens/dir",
                                        out, sizeof out, script, &si, 1),
                         BROWSE_ERR_DIR);
        }

        char dir[] = "/tmp/koboy_screens_browse_XXXXXX";
        CHECK(mkdtemp(dir) != NULL);

        /* A directory with files in it, none of them ROMs, is EMPTY as far as
           the browser is concerned -- and that is a different answer from the
           missing directory above, because the two produce different messages
           on a panel with no terminal.
           MUTANT: return BROWSE_ERR_DIR from the rl.count == 0 arm -- the two
           answers collapse into one and this check goes red. */
        touch_file(dir, "notes.txt");
        touch_file(dir, "koboy.ini");
        {
            int si = 0;
            koboy_input_state script[1] = { lift() };
            CHECK_EQ_INT(screen_browser(&fp.pf, NULL, panel, fp.stride, PW, PH,
                                        dir, out, sizeof out, script, &si, 1),
                         BROWSE_ERR_EMPTY);
        }

        /* Now put ROMs in it. The row indices come from romlist itself: the
           sort order is romlist.c's business, and a hardcoded row would be
           asserting the sort rather than the browser. */
        touch_file(dir, "TETRIS.gb");
        touch_file(dir, "ZELDA.gbc");
        {
            int row = browser_row_of(dir, "TETRIS.gb");
            CHECK(row >= 0);
            koboy_input_state script[1] = { tap_at(PW / 2, row_cy(row)) };
            int si = 0;
            memset(out, 0, sizeof out);
            CHECK_EQ_INT(screen_browser(&fp.pf, NULL, panel, fp.stride, PW, PH,
                                        dir, out, sizeof out, script, &si, 1),
                         BROWSE_PICKED);
            char want[512];
            snprintf(want, sizeof want, "%.400s/TETRIS.gb", dir);
            CHECK(strcmp(out, want) == 0);
        }

        /* A script that runs out leaves the browser without picking, and that
           is BROWSE_NONE and not BROWSE_PICKED with a stale out_path.
           MUTANT: `result = BROWSE_PICKED;` in the `pick < 0` arm. */
        {
            int si = 0;
            koboy_input_state script[1] = { lift() };
            CHECK_EQ_INT(screen_browser(&fp.pf, NULL, panel, fp.stride, PW, PH,
                                        dir, out, sizeof out, script, &si, 1),
                         BROWSE_NONE);
        }

        /* Descending into a subdirectory and picking inside it. Two taps in
           one flat script, which is also the second screen_list iteration of
           the browser's own loop -- the list is rebuilt after every
           navigation, and a koboy_ui_list that outlived a rescan would be
           holding freed pointers.
           MUTANT: hoist the ui_list_init above the `for (;;)` -- this reads
           freed memory, which valgrind or a fresh malloc will show. */
        {
            char sub[512];
            snprintf(sub, sizeof sub, "%.400s/GBC", dir);
            CHECK_EQ_INT(mkdir(sub, 0755), 0);
            touch_file(sub, "KIRBY.gbc");

            int drow = browser_row_of(dir, "GBC/");
            CHECK(drow >= 0);
            /* Inside the subdirectory there is a ".." row, so the ROM's index
               is asked of a listing of the subdirectory, not guessed. */
            int rrow = browser_row_of(sub, "KIRBY.gbc");
            CHECK(rrow >= 0);
            /* +1: romlist_enter prepends the ".." row that browser_row_of's
               root-level scan of `sub` does not have. Asserted rather than
               assumed, one line down. */
            koboy_input_state script[3] = {
                tap_at(PW / 2, row_cy(drow)), lift(),
                tap_at(PW / 2, row_cy(rrow + 1)),
            };
            int si = 0;
            memset(out, 0, sizeof out);
            CHECK_EQ_INT(screen_browser(&fp.pf, NULL, panel, fp.stride, PW, PH,
                                        dir, out, sizeof out, script, &si, 3),
                         BROWSE_PICKED);
            char want[700];
            snprintf(want, sizeof want, "%.400s/GBC/KIRBY.gbc", dir);
            CHECK(strcmp(out, want) == 0);

            /* Back UP one level, and then pick at the root. romlist_enter
               prepended a ".." row, so it is index 0 of the subdirectory's
               listing; asserted, not assumed.
               MUTANT: `else if (kind == ROMLIST_UP) continue;` -- the browser
               never leaves the subfolder and the pick below misses. */
            CHECK_EQ_INT(browser_row_of_kind(sub, ROMLIST_UP), -1);   /* a bare
                            scan of `sub` has no ".." -- only a descent adds one */
            int trow = browser_row_of(dir, "TETRIS.gb");
            CHECK(trow >= 0);
            koboy_input_state back[5] = {
                tap_at(PW / 2, row_cy(drow)), lift(),
                tap_at(PW / 2, row_cy(0)),            /* ".." */
                lift(),
                tap_at(PW / 2, row_cy(trow)),
            };
            si = 0;
            memset(out, 0, sizeof out);
            CHECK_EQ_INT(screen_browser(&fp.pf, NULL, panel, fp.stride, PW, PH,
                                        dir, out, sizeof out, back, &si, 5),
                         BROWSE_PICKED);
            snprintf(want, sizeof want, "%.400s/TETRIS.gb", dir);
            CHECK(strcmp(out, want) == 0);

            unlink_in(sub, "KIRBY.gbc");
            rmdir(sub);
        }

        /* The directory tree going away UNDER the browser, mid-navigation.
           romlist_up then cannot list the parent, and the browser's answer is
           BROWSE_ERR_DIR -- "there is nothing left to show" -- rather than an
           empty list the user cannot leave.
           MUTANT: `if (n < 0) continue;` in screen_browser's navigation arm --
           the loop spins on a listing that no longer exists. */
        {
            char root[] = "/tmp/koboy_screens_vanish_XXXXXX";
            CHECK(mkdtemp(root) != NULL);
            snprintf(g_vanish_root, sizeof g_vanish_root, "%s", root);
            char sub[512];
            snprintf(sub, sizeof sub, "%.400s/GBC", root);
            CHECK_EQ_INT(mkdir(sub, 0755), 0);
            touch_file(sub, "KIRBY.gbc");
            touch_file(root, "TETRIS.gb");

            g_dir_row = browser_row_of(root, "GBC/");
            CHECK(g_dir_row >= 0);
            g_up_row  = 0;                 /* ".." sorts first, always */

            koboy_config c; config_defaults(&c);
            koboy_profile prof;
            CHECK(config_resolve_profile(&prof, &c, PW, PH,
                                         KOBOY_GB_W, KOBOY_GB_H,
                                         KOBOY_GB_W, KOBOY_GB_H));
            koboy_input *in = input_create(&c, &prof);
            CHECK(in != NULL);
            input_set_touch_transform(in, PW - 1, PH - 1, false, false, false);

            fakeplat_reset_log(&fp);
            fp.quit_after_polls = 12;
            fp.on_poll = poll_enter_then_up;
            CHECK_EQ_INT(screen_browser(&fp.pf, in, panel, fp.stride, PW, PH,
                                        root, out, sizeof out, NULL, NULL, 0),
                         BROWSE_ERR_DIR);
            CHECK(fp.polls < 12);          /* it decided, it did not time out */
            fp.on_poll = NULL;
            input_destroy(in);
            vanish_tree();                 /* idempotent, in case it did not run */
        }

        unlink_in(dir, "TETRIS.gb");
        unlink_in(dir, "ZELDA.gbc");
        unlink_in(dir, "notes.txt");
        unlink_in(dir, "koboy.ini");
        rmdir(dir);
        free(panel);
        fakeplat_free(&fp);
    }
})
