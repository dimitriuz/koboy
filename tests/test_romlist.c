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

/* True if `name` is a row of the CURRENT listing. Used to assert "a ROM
   present on disk appears in the list" directly against the actual scan
   result rather than against a hardcoded index, since sort order across 300+
   files is not something a test should hardcode. */
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

    /* .nes and .min, same one-list reasoning as .mgw above. The negatives
       here are the point: a real NES collection ships .pal palette files
       beside the ROMs (262 of them against 5263 .nes in the author's), and a
       Pokemon Mini one ships boot.rom -- neither is content, and both would
       list as selectable "games" under a filter that matched loosely or that
       simply forgot them. */
    CHECK_EQ_INT(romlist_is_rom("Metroid (USA).nes"), 1);
    CHECK_EQ_INT(romlist_is_rom("METROID (USA).NES"), 1);
    CHECK_EQ_INT(romlist_is_rom("Metroid (USA).NeS"), 1);
    CHECK_EQ_INT(romlist_is_rom("Pokemon Tetris (Europe) (En,Ja,Fr).min"), 1);
    CHECK_EQ_INT(romlist_is_rom("POKEMON TETRIS.MIN"), 1);
    CHECK_EQ_INT(romlist_is_rom("Pokemon Tetris.MiN"), 1);
    /* A dumped BIOS is a .min and IS listed. Deliberate, and the choice is
       argued in the task report: this filter is an allowlist of EXTENSIONS
       and nothing else, so a name-prefix rule for "[BIOS] " would also hide
       a homebrew that happened to be named that way, and would be a second,
       invisible rule for a user to discover when their file vanished. The
       core needs no BIOS anyway (it links a free one), so the file is
       inert, not load-bearing. */
    CHECK_EQ_INT(romlist_is_rom("[BIOS] Nintendo Pokemon Mini (World).min"), 1);
    /* The files that must NOT be listed. */
    CHECK_EQ_INT(romlist_is_rom("NES-Classic.pal"), 0);
    CHECK_EQ_INT(romlist_is_rom("nes-classic.PAL"), 0);
    CHECK_EQ_INT(romlist_is_rom("boot.rom"), 0);
    CHECK_EQ_INT(romlist_is_rom("gamelist.xml"), 0);
    /* Superstring and prefix, the same trap the .mgw block above rules out.
       ".ne" matters more than it looks: a three-character compare against
       ".nes" would accept it. */
    CHECK_EQ_INT(romlist_is_rom("Metroid.nesx"), 0);
    CHECK_EQ_INT(romlist_is_rom("Metroid.ne"), 0);
    CHECK_EQ_INT(romlist_is_rom("Tetris.minx"), 0);
    CHECK_EQ_INT(romlist_is_rom("Tetris.mi"), 0);
    /* A Famicom Disk System image sits in the same directories and fceumm
       does accept .fds -- but koboy does not list it, on purpose: the FDS
       needs a BIOS koboy has no system directory to hand it, so listing it
       would offer a row that cannot load. */
    CHECK_EQ_INT(romlist_is_rom("Super Mario Bros. 2 (Japan) (En).fds"), 0);

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
       ent/item_ptr already point to before it scans, and on an
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

    /* At the ROOT there is no ".." row: it is the top, and a row that walked
       out of rom_dir would be a browser that can wander the device's
       filesystem. Asserted by kind, not by name, so a ".." that arrived with
       some other label would still fail. */
    CHECK(romlist_subpath(&rl)[0] == 0);
    for (int i = 0; i < rl.count; i++)
        CHECK_EQ_INT(romlist_kind(&rl, i), ROMLIST_ROM);
    CHECK_EQ_INT(rl.roms, 4);

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
       entries; which 256 was the bug).

       It now doubles as the navigation test, because the same fixture is
       exactly what navigation has to get right: the subdirectory is one row
       at the TOP of the root listing rather than a prefix on every file
       inside it, and the file inside it must still resolve to the SAME
       absolute path the flattened list produced -- which is what keeps an
       existing .srm, save state or RECENT entry pointing at the game it was
       written for. */
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
        /* 301 loose ROMs at the root, plus ONE row for gbc/ -- the two files
           inside it are no longer rows of this listing. The .txt is excluded
           at both levels. */
        CHECK_EQ_INT(big.count, GENERIC + 2 + 1);
        CHECK_EQ_INT(big.roms, GENERIC + 2);

        /* DIRECTORIES SORT FIRST. "gbc/" starts with 'g', so an ordering that
           merely sorted names would bury it between "GAME 299.gb" and
           "Legend of Zelda..."; it is at row 0 because kind outranks name. */
        CHECK_EQ_INT(romlist_kind(&big, 0), ROMLIST_DIR);
        CHECK(strcmp(romlist_name(&big, 0), "gbc/") == 0);
        CHECK_EQ_INT(romlist_kind(&big, 1), ROMLIST_ROM);

        /* The specific failure that was reported: present on disk, present
           in the list. */
        CHECK(has_name(&big, "Shantae (USA).gbc"));
        CHECK(has_name(&big, "Legend of Zelda, The - Link's Awakening "
                             "(USA, Europe) (Rev 2).gb"));

        /* The flattened form is GONE from the root listing -- that prefix on
           every row is the whole reason this changed. */
        CHECK(!has_name(&big, "gbc/Shantae (USA).gbc"));

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

        /* A directory row names no file. Handing its "path" to the core
           would be a load failure with nothing on the panel to explain it. */
        {
            char path[512];
            path[0] = 'Z';
            romlist_path(&big, 0, path, sizeof path);
            CHECK_EQ_INT(path[0], 0);
        }

        /* ---------------------------------------------------- descending */
        int sn = romlist_enter(&big, 0);
        CHECK(sn >= 0);
        CHECK(strcmp(romlist_subpath(&big), "gbc") == 0);
        /* ONLY that directory's contents: the 302 root ROMs are not here,
           and the .txt inside it is still filtered. Plus the ".." row. */
        CHECK_EQ_INT(big.count, 3);
        CHECK_EQ_INT(big.roms, 2);
        CHECK(has_name(&big, "Shantae (USA).gbc"));   /* the gbc/ copy IS here... */
        CHECK(!has_name(&big, "GAME 000.gb"));        /* ...but the root's ROMs are not */
        CHECK(!has_name(&big, "notes.txt"));

        /* ".." exists in a subdirectory, at row 0, and is the only ROMLIST_UP
           row there is. */
        CHECK_EQ_INT(romlist_kind(&big, 0), ROMLIST_UP);
        CHECK(strcmp(romlist_name(&big, 0), "..") == 0);
        CHECK_EQ_INT(romlist_kind(&big, 1), ROMLIST_ROM);
        CHECK_EQ_INT(romlist_kind(&big, 2), ROMLIST_ROM);

        /* THE COMPATIBILITY ASSERTION, and the reason the rest of this task
           is constrained the way it is. A ROM inside a subdirectory must
           resolve to the byte-identical path the FLATTENED list produced --
           "<root>/gbc/Shantae (USA).gbc" -- because that string is what
           sram.c and state.c derive a .srm and a save state from, and what
           recent.dat stores. If this ever changes, every existing save on the
           device is silently orphaned: the game still loads, with no
           progress, and nothing says why. `want` is spelled out here the way
           the OLD code built it (dir + "/" + the relative name it stored),
           not the way the new code does, so the two constructions are
           actually compared rather than one being restated. */
        for (int i = 0; i < big.count; i++) {
            if (strcmp(romlist_name(&big, i), "Shantae (USA).gbc") != 0) continue;
            char path[512], want[512];
            romlist_path(&big, i, path, sizeof path);
            snprintf(want, sizeof want, "%s/%s", root, "gbc/Shantae (USA).gbc");
            CHECK(strcmp(path, want) == 0);
            FILE *f = fopen(path, "rb");
            CHECK(f != NULL);                     /* and it really opens */
            if (f) fclose(f);
            break;
        }

        /* ------------------------------------------------------ going up */
        int un = romlist_up(&big);
        CHECK(un >= 0);
        CHECK(romlist_subpath(&big)[0] == 0);
        CHECK_EQ_INT(big.count, GENERIC + 2 + 1);      /* the root listing, again */
        CHECK(has_name(&big, "GAME 000.gb"));

        /* Up from the ROOT is a no-op, not an escape into the parent
           directory: rom_dir is the top of the world the browser can see. */
        int rn = romlist_up(&big);
        CHECK_EQ_INT(rn, un);
        CHECK(romlist_subpath(&big)[0] == 0);
        CHECK(has_name(&big, "GAME 000.gb"));

        /* Entering something that is not a directory changes nothing --
           the caller loads a ROM row, it does not descend into one. */
        int roms_row = -1;
        for (int i = 0; i < big.count; i++)
            if (romlist_kind(&big, i) == ROMLIST_ROM) { roms_row = i; break; }
        CHECK(roms_row >= 0);
        CHECK_EQ_INT(romlist_enter(&big, roms_row), rn);
        CHECK(romlist_subpath(&big)[0] == 0);
        CHECK_EQ_INT(romlist_enter(&big, 99999), rn);   /* nor does a bogus index */

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
       A symlinked directory must not be LISTED -- that is exactly how a
       "sorted collection" of subfolders could loop back on itself, and with
       folder rows the loop is now one the user walks by tapping rather than
       one the scan walks by recursing. lstat's whole job here is that a
       symlink is neither S_ISDIR nor S_ISREG, so it never becomes a row at
       all: no row, no tap, no cycle.

       The assertion is on the ROW, not on the scan finishing: a
       one-directory listing would terminate promptly even if it did follow
       the link, so "it did not hang" no longer discriminates. */
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
        CHECK_EQ_INT(lp.count, 1);             /* real.gb only; "self" is not a row */
        CHECK_EQ_INT(lp.hidden, 0);
        CHECK(!has_name(&lp, "self/"));
        CHECK(!has_name(&lp, "self"));
        CHECK_EQ_INT(romlist_kind(&lp, 0), ROMLIST_ROM);

        romlist_free(&lp);
        char cmd[1024];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
        if (system(cmd) != 0) fprintf(stderr, "NOTE: cleanup failed\n");
    }

    /* ------------------------------------------------------------------
       Directories sort ahead of files, so a truncated listing drops FILES,
       and it drops the alphabetically last ones -- the cap is still applied
       after the sort, which is the original bug's fix. The cap block above
       proves that for a flat directory; this proves the folder rows did not
       quietly become exempt from the ordering (or, worse, first in line to
       be dropped). */
    {
        char dir[] = "/tmp/koboy_romlist_capdir_XXXXXX";
        CHECK(mkdtemp(dir) != NULL);
        touch_file(dir, "AA.gb");
        touch_file(dir, "BB.gb");
        touch_file(dir, "CC.gb");
        /* Named to sort LAST among the four if kind were ignored -- so a
           listing that merely sorted by name would drop it, and the
           assertions below would fail. */
        char zz[80];
        snprintf(zz, sizeof zz, "%s/zz sub", dir);
        CHECK(mkdir(zz, 0755) == 0);

        CHECK(setenv("KOBOY_ROMLIST_CAP_TEST", "2", 1) == 0);
        koboy_romlist cd;
        memset(&cd, 0, sizeof cd);
        int cdn = romlist_scan(&cd, dir);
        CHECK(unsetenv("KOBOY_ROMLIST_CAP_TEST") == 0);

        CHECK_EQ_INT(cd.count, 2);
        CHECK_EQ_INT(cdn, 3);                  /* 2 rows + the overflow row */
        CHECK_EQ_INT(cd.hidden, 2);            /* CC.gb and BB.gb, in that order */
        CHECK(strcmp(romlist_name(&cd, 0), "zz sub/") == 0);
        CHECK_EQ_INT(romlist_kind(&cd, 0), ROMLIST_DIR);
        CHECK(strcmp(romlist_name(&cd, 1), "AA.gb") == 0);
        CHECK(!has_name(&cd, "CC.gb"));

        romlist_free(&cd);
        char cmd[1024];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
        if (system(cmd) != 0) fprintf(stderr, "NOTE: cleanup failed\n");
    }

    /* ------------------------------------------------------------------
       Dot-directories are not rows. New with folder rows: while the scan was
       recursive a .Trashes or .Spotlight-V100 (an SD card that has been in a
       Mac grows several) contributed nothing visible because it held no
       ROMs. A directory is now a row of its own, and dirs sort first, so
       without this filter the FIRST thing a user sees is their card's
       metadata. Files are deliberately not filtered this way -- that would
       change which ROMs are listed. */
    {
        char dir[] = "/tmp/koboy_romlist_dot_XXXXXX";
        CHECK(mkdtemp(dir) != NULL);
        touch_file(dir, "AA.gb");

        char dot[80], vis[80];
        snprintf(dot, sizeof dot, "%s/.Trashes", dir);
        snprintf(vis, sizeof vis, "%s/Game and Watch", dir);
        CHECK(mkdir(dot, 0755) == 0);
        CHECK(mkdir(vis, 0755) == 0);

        koboy_romlist dt;
        memset(&dt, 0, sizeof dt);
        int dtn = romlist_scan(&dt, dir);
        CHECK_EQ_INT(dtn, 2);                        /* the folder and the ROM */
        CHECK(has_name(&dt, "Game and Watch/"));     /* the control: real folders DO show */
        CHECK(!has_name(&dt, ".Trashes/"));
        CHECK(!has_name(&dt, ".Trashes"));

        romlist_free(&dt);
        char cmd[1024];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
        if (system(cmd) != 0) fprintf(stderr, "NOTE: cleanup failed\n");
    }

    /* ------------------------------------------------------------------
       ROMLIST_MAX_DEPTH still bounds how deep the browser can go -- it used
       to bound the recursive walk, and now bounds navigation, which is the
       same ceiling reached one tap at a time. Builds one directory per level
       past the limit and taps down through them: the descent must stop, and
       stop where the user still sees a real listing rather than an error. */
    {
        char dir[] = "/tmp/koboy_romlist_deep_XXXXXX";
        CHECK(mkdtemp(dir) != NULL);
        /* Appended in place rather than snprintf'd through a second buffer:
           gcc's -Wformat-truncation cannot see that these paths are short,
           and this project ships at zero warnings. */
        char cur[256];
        int len = snprintf(cur, sizeof cur, "%s", dir);
        for (int d = 0; d < ROMLIST_MAX_DEPTH + 2; d++) {
            CHECK(len + 2 < (int)sizeof cur);
            cur[len++] = '/';
            cur[len++] = 'd';
            cur[len]   = 0;
            CHECK(mkdir(cur, 0755) == 0);
            touch_file(cur, "DEEP.gb");
        }

        koboy_romlist dp;
        memset(&dp, 0, sizeof dp);
        CHECK(romlist_scan(&dp, dir) >= 0);
        int reached = 0;
        for (int step = 0; step < ROMLIST_MAX_DEPTH + 4; step++) {
            int row = -1;
            for (int i = 0; i < dp.count; i++)
                if (romlist_kind(&dp, i) == ROMLIST_DIR) { row = i; break; }
            if (row < 0) break;
            char before[512];
            snprintf(before, sizeof before, "%s", romlist_subpath(&dp));
            CHECK(romlist_enter(&dp, row) >= 0);
            if (!strcmp(before, romlist_subpath(&dp))) break;   /* refused: at the limit */
            reached++;
        }
        CHECK_EQ_INT(reached, ROMLIST_MAX_DEPTH);
        /* And it is still a usable listing at the bottom, not an error
           state: the ROM that lives there is right where it should be. */
        CHECK(has_name(&dp, "DEEP.gb"));

        /* Up from depth N lands on depth N-1, not back at the root. Asserted
           from a NESTED directory on purpose: at depth 1 those two answers
           are the same string, so a "go up" that always jumped to the root
           would pass every other check in this file. */
        char deepest[512];
        snprintf(deepest, sizeof deepest, "%s", romlist_subpath(&dp));
        CHECK(romlist_up(&dp) >= 0);
        {
            const char *now = romlist_subpath(&dp);
            CHECK((int)strlen(now) == (int)strlen(deepest) - 2);   /* one "/d" less */
            CHECK(now[0] != 0);                                    /* not the root */
            CHECK(strncmp(now, deepest, strlen(now)) == 0);
            CHECK(has_name(&dp, "DEEP.gb"));       /* every level holds one */
            CHECK(has_name(&dp, "d/"));            /* and the child we came from */
        }

        romlist_free(&dp);
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
