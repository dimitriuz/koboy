#define _DEFAULT_SOURCE
#include "test.h"
#include "romlist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void touch_file(const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (f) { fputc('x', f); fclose(f); }
}

/* True if `name` (a rl->count-relative path, e.g. "gbc/Shantae (USA).gbc")
   is present among rl's REAL entries. Used to assert "a ROM present on disk
   appears in the list" directly against the actual scan result rather than
   against a hardcoded index, since sort order across 300+ files is not
   something a test should hardcode. */
static bool has_name(const koboy_romlist *rl, const char *name)
{
    for (int i = 0; i < rl->count; i++)
        if (!strcmp(romlist_name(rl, i), name)) return true;
    return false;
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

    /* .mgw is Game & Watch content for gw-libretro, listed in the SAME
       browser as Game Boy ROMs because the core is chosen from the extension
       at load time (config_core_for_rom), not from a per-system list. */
    CHECK_EQ_INT(romlist_is_rom("BALL.mgw"), 1);
    CHECK_EQ_INT(romlist_is_rom("BALL.MGW"), 1);
    CHECK_EQ_INT(romlist_is_rom("BALL.Mgw"), 1);
    CHECK_EQ_INT(romlist_is_rom(".mgw"), 1);
    /* Neither a superstring nor a prefix of the extension counts. A filter
       written with strstr, or with a 3-character compare, would accept both
       of these -- and the browser would then hand gw-libretro a file it
       cannot parse. */
    CHECK_EQ_INT(romlist_is_rom("BALL.mgwx"), 0);
    CHECK_EQ_INT(romlist_is_rom("BALL.mg"), 0);
    CHECK_EQ_INT(romlist_is_rom("BALL.gw"), 0);

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
    /* Zero-initialised, per romlist_scan's precondition: it frees whatever
       names/item_ptr already point to before it scans, and on an
       uninitialised struct those are indeterminate stack contents. */
    koboy_romlist rl = {0};
    CHECK_EQ_INT(romlist_scan(&rl, "/nonexistent/koboy/test/dir"), -1);

    /* Scan a real directory. */
    char dir[] = "/tmp/koboy_romlist_XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    touch_file(dir, "TETRIS.gb");
    touch_file(dir, "zelda.GB");
    touch_file(dir, "KIRBY 2.gbc");
    touch_file(dir, "notes.txt");
    touch_file(dir, "TETRIS.srm");
    /* Sorts LAST of the four ("zz" > "ze"), deliberately: the index-based
       assertions below were written against a three-entry list, and a name
       that landed anywhere else would make this addition look like it broke
       them. */
    touch_file(dir, "zz BALL.mgw");
    /* Rejected by the same filter, in a real scan rather than only in the
       predicate calls above. */
    touch_file(dir, "zz BALL.mgwx");

    int n = romlist_scan(&rl, dir);
    CHECK_EQ_INT(n, 4);
    CHECK(strcmp(romlist_name(&rl, 3), "zz BALL.mgw") == 0);

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

    /* ------------------------------------------------------------------
       REGRESSION: the reported bug. A user copied 303 ROMs into roms/;
       ROMLIST_MAX was 256, applied inside the readdir loop before the
       qsort, so 47 ROMs vanished -- chosen by filesystem order, not
       alphabetically. Shantae (USA).gbc was one of the ones that
       disappeared. This builds a collection past the OLD cap (300+ files),
       including that exact name, a long real-world name, and a
       subdirectory (roms/gbc/ exists on the user's device) holding a file
       with the SAME basename as one at the top level -- and asserts every
       one of them is not just counted but present, loadable, and
       distinguishable. A test that only checked count == 3, or count ==
       303, would not have caught the original bug: the count was never
       wrong in a way "== 303" would show (256 were still counted as valid
       entries; which 256 was the bug). */
    {
        char root[] = "/tmp/koboy_romlist_big_XXXXXX";
        CHECK(mkdtemp(root) != NULL);

        char gbc[64];   /* small and fixed, not 600: a generously-sized buffer
                           here makes gcc's interprocedural format-truncation
                           check flag EVERY touch_file call site once it
                           inlines touch_file and sees this call's worst case */
        snprintf(gbc, sizeof gbc, "%s/gbc", root);
        CHECK(mkdir(gbc, 0755) == 0);

        enum { GENERIC = 300 };
        for (int i = 0; i < GENERIC; i++) {
            char name[64];
            snprintf(name, sizeof name, "GAME %03d.gb", i);
            touch_file(root, name);
        }
        touch_file(root, "Shantae (USA).gbc");
        touch_file(root, "Legend of Zelda, The - Link's Awakening (USA, "
                          "Europe) (Rev 2).gb");
        touch_file(gbc, "Shantae (USA).gbc");        /* same basename, diff folder */
        touch_file(gbc, "Pokemon Crystal Version (USA, Europe).gbc");
        touch_file(gbc, "notes.txt");                 /* not a rom, must not count */

        koboy_romlist big;
        memset(&big, 0, sizeof big);
        int bn = romlist_scan(&big, root);
        CHECK(bn >= 0);
        CHECK_EQ_INT(big.hidden, 0);                  /* nowhere near the cap */
        CHECK_EQ_INT(big.count, GENERIC + 2 + 2);      /* the .txt is excluded */

        /* The specific failure that was reported: present on disk, present
           in the list. */
        CHECK(has_name(&big, "Shantae (USA).gbc"));
        CHECK(has_name(&big, "Legend of Zelda, The - Link's Awakening "
                             "(USA, Europe) (Rev 2).gb"));

        /* The subdirectory copy is a DIFFERENT entry with a DIFFERENT
           display name -- not the same row, not missing, not colliding. */
        CHECK(has_name(&big, "gbc/Shantae (USA).gbc"));
        CHECK(has_name(&big, "gbc/Pokemon Crystal Version (USA, Europe).gbc"));

        /* Loadable, not just listed: romlist_path on the root-level Shantae
           entry must reconstruct a path that actually opens -- this is what
           "present in the list" needs to mean for a user who taps it. */
        for (int i = 0; i < big.count; i++) {
            if (strcmp(romlist_name(&big, i), "Shantae (USA).gbc") != 0) continue;
            char path[512];
            romlist_path(&big, i, path, sizeof path);
            FILE *f = fopen(path, "rb");
            CHECK(f != NULL);
            if (f) fclose(f);
            break;
        }

        romlist_free(&big);
        char cmd[1024];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
        if (system(cmd) != 0) fprintf(stderr, "NOTE: cleanup failed\n");
    }

    /* ------------------------------------------------------------------
       Sort BEFORE cap, not after: with the cap dialled down to something a
       test can actually reach, the entries that SURVIVE truncation must be
       the alphabetically first ones, regardless of the order the
       filesystem happened to hand them back in. The original bug applied
       the cap inside the readdir loop, before the sort -- this asserts the
       fix by checking WHICH names remain, not just how many. */
    {
        char dir[] = "/tmp/koboy_romlist_cap_XXXXXX";
        CHECK(mkdtemp(dir) != NULL);
        static const char *const names[] = {
            "MM.gb", "AA.gb", "ZZ.gb", "CC.gb", "QQ.gb", "BB.gb", "YY.gb",
            "DD.gb", "XX.gb", "EE.gb",
        };
        const int NAMES_N = (int)(sizeof names / sizeof names[0]);
        for (int i = 0; i < NAMES_N; i++) touch_file(dir, names[i]);

        CHECK(setenv("KOBOY_ROMLIST_CAP_TEST", "4", 1) == 0);
        koboy_romlist capped;
        memset(&capped, 0, sizeof capped);
        int cn = romlist_scan(&capped, dir);
        CHECK(unsetenv("KOBOY_ROMLIST_CAP_TEST") == 0);

        CHECK_EQ_INT(capped.count, 4);
        CHECK_EQ_INT(capped.hidden, NAMES_N - 4);
        CHECK_EQ_INT(cn, 5);   /* 4 real rows + the overflow row */

        /* The 4 kept are the 4 lexicographically SMALLEST of the 10 -- not
           whatever readdir happened to visit first. */
        CHECK(strcmp(romlist_name(&capped, 0), "AA.gb") == 0);
        CHECK(strcmp(romlist_name(&capped, 1), "BB.gb") == 0);
        CHECK(strcmp(romlist_name(&capped, 2), "CC.gb") == 0);
        CHECK(strcmp(romlist_name(&capped, 3), "DD.gb") == 0);
        CHECK(!has_name(&capped, "ZZ.gb"));
        CHECK(!has_name(&capped, "EE.gb"));

        /* The overflow row is visible ON THE PANEL (it is item_ptr[count],
           which ui_list_init will render as a row), not only in a log a
           device with no terminal will never show. It must also never be
           mistaken for a real, loadable ROM. */
        const char *const *its = romlist_items(&capped);
        CHECK(its[4] != NULL);
        CHECK(strstr(its[4], "6") != NULL);     /* 10 - 4 = 6 hidden */
        CHECK(romlist_is_real(&capped, 0));
        CHECK(romlist_is_real(&capped, 3));
        CHECK(!romlist_is_real(&capped, 4));    /* the overflow row itself */
        CHECK(!romlist_is_real(&capped, -1));

        romlist_free(&capped);
        char cmd[1024];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
        if (system(cmd) != 0) fprintf(stderr, "NOTE: cleanup failed\n");
    }

    /* ------------------------------------------------------------------
       A relative path that would not fit in ROMLIST_NAME is skipped, never
       truncated and stored: a truncated name would round-trip through
       romlist_path into a file that does not exist, so the ROM would look
       present in the browser but silently fail to load -- worse than
       leaving it out entirely. */
    {
        char dir[] = "/tmp/koboy_romlist_long_XXXXXX";
        CHECK(mkdtemp(dir) != NULL);

        char longname[200];
        int i = 0;
        for (; i < 190; i++) longname[i] = 'A' + (i % 26);
        snprintf(longname + i, sizeof longname - (size_t)i, ".gb");
        touch_file(dir, longname);
        CHECK(strlen(longname) >= ROMLIST_NAME);   /* the setup must actually be oversized */
        touch_file(dir, "short.gb");                /* a normal neighbour is unaffected */

        koboy_romlist ln;
        memset(&ln, 0, sizeof ln);
        int lnn = romlist_scan(&ln, dir);
        CHECK_EQ_INT(ln.count, 1);            /* only "short.gb" */
        CHECK_EQ_INT(ln.hidden, 1);           /* the oversized one, accounted for */
        CHECK_EQ_INT(lnn, 2);                 /* 1 real row + the overflow row */
        CHECK(strcmp(romlist_name(&ln, 0), "short.gb") == 0);

        romlist_free(&ln);
        char cmd[1024];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
        if (system(cmd) != 0) fprintf(stderr, "NOTE: cleanup failed\n");
    }

    /* ------------------------------------------------------------------
       A symlinked directory must not be followed -- that is exactly how a
       "sorted collection" of subfolders could loop back on itself. A
       self-referential symlink here would hang (or exhaust the stack) if
       the guard in scan_walk were ever removed; reaching CHECK at all,
       promptly, is the assertion. */
    {
        char dir[] = "/tmp/koboy_romlist_loop_XXXXXX";
        CHECK(mkdtemp(dir) != NULL);
        touch_file(dir, "real.gb");

        char linkpath[64];
        snprintf(linkpath, sizeof linkpath, "%s/self", dir);
        CHECK(symlink(dir, linkpath) == 0);    /* points right back at `dir` */

        koboy_romlist lp;
        memset(&lp, 0, sizeof lp);
        int lpn = romlist_scan(&lp, dir);
        CHECK(lpn >= 0);
        CHECK_EQ_INT(lp.count, 1);             /* real.gb only; "self" not followed */
        CHECK_EQ_INT(lp.hidden, 0);

        romlist_free(&lp);
        char cmd[1024];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
        if (system(cmd) != 0) fprintf(stderr, "NOTE: cleanup failed\n");
    }

    /* An empty directory scans to zero without failing. */
    char empty[] = "/tmp/koboy_romlist_e_XXXXXX";
    CHECK(mkdtemp(empty) != NULL);
    CHECK_EQ_INT(romlist_scan(&rl, empty), 0);

    char cmd[1024];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' '%s'", dir, empty);
    if (system(cmd) != 0) fprintf(stderr, "NOTE: cleanup failed\n");
})
