#define _DEFAULT_SOURCE
#include "test.h"
#include "romlist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void touch_file(const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (f) { fputc('x', f); fclose(f); }
}

TEST_MAIN({
    /* Pure predicate first: extension matching is the whole filter. */
    CHECK_EQ_INT(romlist_is_rom("ZELDA.gb"), 1);
    CHECK_EQ_INT(romlist_is_rom("ZELDA.GB"), 1);
    CHECK_EQ_INT(romlist_is_rom("KIRBY.gbc"), 1);
    CHECK_EQ_INT(romlist_is_rom("KIRBY.GBC"), 1);
    CHECK_EQ_INT(romlist_is_rom("SAVE.srm"), 0);
    CHECK_EQ_INT(romlist_is_rom("NOTES.txt"), 0);
    CHECK_EQ_INT(romlist_is_rom("koboy.ini"), 0);
    /* A bare ".gb" has no stem; still a rom by extension, and the browser
       must not crash on it. */
    CHECK_EQ_INT(romlist_is_rom(".gb"), 1);
    /* Names shorter than the extension must not read before the string. */
    CHECK_EQ_INT(romlist_is_rom("g"), 0);
    CHECK_EQ_INT(romlist_is_rom(""), 0);
    /* A dotfile that merely CONTAINS gb is not a rom. */
    CHECK_EQ_INT(romlist_is_rom("gbfile"), 0);

    /* The short-name guard's failure mode, made deterministic instead of
       ASan-only: if ends_with_ci's `if (lx > ls) return false;` is removed,
       the backward read for suffix ".gb" walks off the front of "b" -- but
       here that lands on bytes WE chose, still inside this array, so the
       read is well-defined C rather than UB. Spelling those bytes ".gb"
       turns the missing guard into an observable false positive (name "b"
       reported as a ROM) rather than something only a sanitizer can see. */
    {
        char pad[4] = { '.', 'g', 'b', '\0' };
        CHECK_EQ_INT(romlist_is_rom(&pad[2]), 0); /* pad[2..] is just "b" */
    }

    /* A missing directory is reported, not treated as empty: "you have no
       ROMs" and "your rom_dir is wrong" are different diagnoses, and on a
       device with no terminal that distinction is the whole diagnostic. */
    koboy_romlist rl;
    CHECK_EQ_INT(romlist_scan(&rl, "/nonexistent/koboy/test/dir"), -1);

    /* Scan a real directory. */
    char dir[] = "/tmp/koboy_romlist_XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    touch_file(dir, "TETRIS.gb");
    touch_file(dir, "zelda.GB");
    touch_file(dir, "KIRBY 2.gbc");
    touch_file(dir, "notes.txt");
    touch_file(dir, "TETRIS.srm");

    int n = romlist_scan(&rl, dir);
    CHECK_EQ_INT(n, 3);

    /* Sorted case-insensitively, so the list reads the way a person expects
       rather than the way readdir happened to return it. */
    CHECK(strcmp(romlist_name(&rl, 0), "KIRBY 2.gbc") == 0);
    CHECK(strcmp(romlist_name(&rl, 1), "TETRIS.gb") == 0);
    CHECK(strcmp(romlist_name(&rl, 2), "zelda.GB") == 0);

    /* Full paths join the directory back on. */
    char path[512];
    romlist_path(&rl, 1, path, sizeof path);
    char want[512];
    snprintf(want, sizeof want, "%s/TETRIS.gb", dir);
    CHECK(strcmp(path, want) == 0);

    /* Out-of-range indices are safe, not undefined. The UI derives an index
       from a touch, so a stale page after a rescan is reachable. */
    CHECK(romlist_name(&rl, -1) != NULL);
    CHECK(romlist_name(&rl, 999) != NULL);
    path[0] = 'Z';
    romlist_path(&rl, 999, path, sizeof path);
    CHECK_EQ_INT(path[0], 0);

    /* items() hands ui_list_init an array it can index directly. */
    const char *const *items = romlist_items(&rl);
    CHECK(strcmp(items[0], "KIRBY 2.gbc") == 0);

    /* An empty directory scans to zero without failing. */
    char empty[] = "/tmp/koboy_romlist_e_XXXXXX";
    CHECK(mkdtemp(empty) != NULL);
    CHECK_EQ_INT(romlist_scan(&rl, empty), 0);

    char cmd[1024];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' '%s'", dir, empty);
    if (system(cmd) != 0) fprintf(stderr, "NOTE: cleanup failed\n");
})
