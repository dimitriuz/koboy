#define _DEFAULT_SOURCE
#include "test.h"
#include "uiscript.h"
#include <stdio.h>
#include <stdlib.h>

static void write_script(const char *path, const char *body)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fputs(body, f);
    fclose(f);
}

TEST_MAIN({
    char dir[] = "/tmp/koboy_uiscript_XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char path[512];
    snprintf(path, sizeof path, "%s/s.txt", dir);

    /* Each `tap` becomes TWO states -- press then release -- because
       ui_list_feed is edge triggered and a press with no release would latch
       the widget for the rest of the run. */
    write_script(path,
        "# a comment, and a blank line follow\n"
        "\n"
        "tap 400 900\n"
        "key b\n"
        "idle 3\n"
        "key a\n");

    static koboy_input_state st[UISCRIPT_MAX];
    static unsigned char mk[UISCRIPT_MAX];
    int n = uiscript_load(path, st, mk, UISCRIPT_MAX);

    /* tap = 2, key b = 2, idle 3 = 3, key a = 2  ->  9 */
    CHECK_EQ_INT(n, 9);

    CHECK_EQ_INT(st[0].touch[0].down, 1);
    CHECK_EQ_INT(st[0].touch[0].x, 400);
    CHECK_EQ_INT(st[0].touch[0].y, 900);
    CHECK_EQ_INT(st[1].touch[0].down, 0);

    CHECK_EQ_INT(st[2].buttons, KOBOY_BTN_B);
    CHECK_EQ_INT(st[3].buttons, 0);

    CHECK_EQ_INT(st[4].buttons, 0);
    CHECK_EQ_INT(st[4].touch[0].down, 0);
    CHECK_EQ_INT(st[6].buttons, 0);

    CHECK_EQ_INT(st[7].buttons, KOBOY_BTN_A);
    CHECK_EQ_INT(st[8].buttons, 0);

    /* A missing file is an error, not an empty script: a typo in --ui-script
       must fail the run rather than silently pass a test that exercised
       nothing. */
    CHECK_EQ_INT(uiscript_load("/nonexistent/koboy/script", st, mk, UISCRIPT_MAX), -1);

    /* An unknown verb is an error for the same reason. */
    write_script(path, "wiggle 1 2\n");
    CHECK_EQ_INT(uiscript_load(path, st, mk, UISCRIPT_MAX), -1);

    /* A malformed tap is an error. */
    write_script(path, "tap 400\n");
    CHECK_EQ_INT(uiscript_load(path, st, mk, UISCRIPT_MAX), -1);

    /* Overflow truncates rather than overruns. */
    {
        FILE *f = fopen(path, "wb");
        CHECK(f != NULL);
        for (int i = 0; i < UISCRIPT_MAX; i++) fputs("key a\n", f);
        fclose(f);
        int got = uiscript_load(path, st, mk, UISCRIPT_MAX);
        CHECK(got > 0);
        CHECK(got <= UISCRIPT_MAX);
    }

    /* ------------------------------------------------------- the menu verb
       One state, marked, and marked ONLY there. The negative half is the half
       that matters: a marker array that came back all-ones would open the
       menu on the first frame of every scripted run ever written, and every
       existing browser script in tests/smoke_host.sh would still pass, since
       they never reach the emulator loop with script left over. */
    write_script(path, "tap 10 20\nmenu\nidle 2\ntap 30 40\n");
    {
        memset(mk, 0xEE, sizeof mk);          /* poisoned: nothing may be left unwritten */
        int got = uiscript_load(path, st, mk, UISCRIPT_MAX);
        CHECK_EQ_INT(got, 2 + 1 + 2 + 2);     /* menu contributes exactly one state */
        CHECK_EQ_INT(mk[0], 0); CHECK_EQ_INT(mk[1], 0);     /* the tap */
        CHECK_EQ_INT(mk[2], 1);                             /* THE marker */
        CHECK_EQ_INT(mk[3], 0); CHECK_EQ_INT(mk[4], 0);     /* the idles */
        CHECK_EQ_INT(mk[5], 0); CHECK_EQ_INT(mk[6], 0);     /* the second tap */

        /* Its state is inert. A misplaced `menu` inside a browser script is
           consumed by run_list as an idle frame, and an inert state is what
           makes that harmless rather than a stray tap at (0,0). */
        CHECK_EQ_INT(st[2].buttons, 0);
        CHECK_EQ_INT(st[2].touch[0].down, 0);
        CHECK_EQ_INT(st[2].touch[0].x, 0);
        CHECK_EQ_INT(st[2].touch[0].y, 0);

        /* And the states either side of it are the ones the verbs before and
           after wrote -- i.e. the marker took a slot rather than displacing
           somebody's tap. */
        CHECK_EQ_INT(st[0].touch[0].x, 10);
        CHECK_EQ_INT(st[5].touch[0].x, 30);
        CHECK_EQ_INT(st[5].touch[0].down, 1);
    }

    /* `menu` takes no argument, so a stray one is ignored rather than being an
       error -- the same tolerance `key a` has for its own extra fields. This
       is asserted so a future tightening is a deliberate choice, not a
       surprise to whoever wrote "menu # open it". */
    write_script(path, "menu 7\n");
    CHECK_EQ_INT(uiscript_load(path, st, mk, UISCRIPT_MAX), 1);
    CHECK_EQ_INT(mk[0], 1);

    /* A NULL array is an error, not a segfault and not a silently disabled
       marker: a caller that cannot honour `menu` must not run a script that
       contains one and report success. */
    write_script(path, "menu\n");
    CHECK_EQ_INT(uiscript_load(path, st, NULL, UISCRIPT_MAX), -1);
    CHECK_EQ_INT(uiscript_load(path, NULL, mk, UISCRIPT_MAX), -1);
    CHECK_EQ_INT(uiscript_load(path, st, mk, 0), -1);

    char cmd[1024];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) fprintf(stderr, "NOTE: cleanup failed\n");
})
