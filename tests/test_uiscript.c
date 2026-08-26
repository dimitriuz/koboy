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
    int n = uiscript_load(path, st, UISCRIPT_MAX);

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
    CHECK_EQ_INT(uiscript_load("/nonexistent/koboy/script", st, UISCRIPT_MAX), -1);

    /* An unknown verb is an error for the same reason. */
    write_script(path, "wiggle 1 2\n");
    CHECK_EQ_INT(uiscript_load(path, st, UISCRIPT_MAX), -1);

    /* A malformed tap is an error. */
    write_script(path, "tap 400\n");
    CHECK_EQ_INT(uiscript_load(path, st, UISCRIPT_MAX), -1);

    /* Overflow truncates rather than overruns. */
    {
        FILE *f = fopen(path, "wb");
        CHECK(f != NULL);
        for (int i = 0; i < UISCRIPT_MAX; i++) fputs("key a\n", f);
        fclose(f);
        int got = uiscript_load(path, st, UISCRIPT_MAX);
        CHECK(got > 0);
        CHECK(got <= UISCRIPT_MAX);
    }

    char cmd[1024];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) fprintf(stderr, "NOTE: cleanup failed\n");
})
