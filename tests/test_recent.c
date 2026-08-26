#define _DEFAULT_SOURCE
#include "test.h"
#include "recent.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

TEST_MAIN({
    /* ---------------------------------------------------- recent_touch: pure */
    koboy_recent rc;
    recent_init(&rc);
    CHECK_EQ_INT(rc.count, 0);

    recent_touch(&rc, "/roms/AAA.gb", "AAA.gb");
    CHECK_EQ_INT(rc.count, 1);
    CHECK(strcmp(recent_path(&rc, 0), "/roms/AAA.gb") == 0);
    CHECK(strcmp(recent_display(&rc, 0), "AAA.gb") == 0);

    /* Most-recent first. */
    recent_touch(&rc, "/roms/BBB.gb", "BBB.gb");
    recent_touch(&rc, "/roms/CCC.gb", "CCC.gb");
    CHECK_EQ_INT(rc.count, 3);
    CHECK(strcmp(recent_path(&rc, 0), "/roms/CCC.gb") == 0);
    CHECK(strcmp(recent_path(&rc, 1), "/roms/BBB.gb") == 0);
    CHECK(strcmp(recent_path(&rc, 2), "/roms/AAA.gb") == 0);

    /* Playing an ALREADY-PRESENT rom moves it to the front instead of
       duplicating -- the count must not grow, and the other two entries
       keep their relative order. */
    recent_touch(&rc, "/roms/AAA.gb", "AAA.gb");
    CHECK_EQ_INT(rc.count, 3);
    CHECK(strcmp(recent_path(&rc, 0), "/roms/AAA.gb") == 0);
    CHECK(strcmp(recent_path(&rc, 1), "/roms/CCC.gb") == 0);
    CHECK(strcmp(recent_path(&rc, 2), "/roms/BBB.gb") == 0);

    /* The display label refreshes even for an already-present path -- a ROM
       that moved into a subfolder since it was last recorded should show
       its CURRENT relative name, not a stale one. */
    recent_touch(&rc, "/roms/AAA.gb", "sub/AAA.gb");
    CHECK(strcmp(recent_display(&rc, 0), "sub/AAA.gb") == 0);

    /* Cap eviction: past KOBOY_RECENT_MAX the OLDEST entry ages out, never
       the newest, and the count never exceeds the cap. */
    {
        koboy_recent cap;
        recent_init(&cap);
        char path[64], disp[64];
        for (int i = 0; i < KOBOY_RECENT_MAX; i++) {
            snprintf(path, sizeof path, "/roms/G%02d.gb", i);
            snprintf(disp, sizeof disp, "G%02d.gb", i);
            recent_touch(&cap, path, disp);
        }
        CHECK_EQ_INT(cap.count, KOBOY_RECENT_MAX);
        CHECK(strcmp(recent_path(&cap, 0), "/roms/G09.gb") == 0);   /* newest */
        CHECK(strcmp(recent_path(&cap, KOBOY_RECENT_MAX - 1),
                     "/roms/G00.gb") == 0);                         /* oldest, still present */

        /* One more push: G00 (the oldest) must be the one that disappears. */
        recent_touch(&cap, "/roms/G10.gb", "G10.gb");
        CHECK_EQ_INT(cap.count, KOBOY_RECENT_MAX);
        CHECK(strcmp(recent_path(&cap, 0), "/roms/G10.gb") == 0);
        bool has_g00 = false, has_g01 = false;
        for (int i = 0; i < cap.count; i++) {
            if (!strcmp(recent_path(&cap, i), "/roms/G00.gb")) has_g00 = true;
            if (!strcmp(recent_path(&cap, i), "/roms/G01.gb")) has_g01 = true;
        }
        CHECK(!has_g00);
        CHECK(has_g01);
    }

    /* Out-of-range indices are safe, not undefined -- selection in the
       picker derives an index from a touch, same reasoning as romlist. */
    CHECK(recent_path(&rc, -1) != NULL);
    CHECK(recent_path(&rc, 999) != NULL);
    CHECK_EQ_INT(recent_path(&rc, 999)[0], 0);
    CHECK_EQ_INT(recent_display(&rc, -1)[0], 0);

    /* An empty/garbage `path` is not recorded at all. */
    {
        koboy_recent e; recent_init(&e);
        recent_touch(&e, "", "nope");
        recent_touch(&e, NULL, "nope");
        CHECK_EQ_INT(e.count, 0);
    }

    /* ---------------------------------------------------- load/save round trip */
    char dir[] = "/tmp/koboy_recent_XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char file[600];
    snprintf(file, sizeof file, "%s/recent.dat", dir);

    CHECK(recent_save(&rc, file));
    koboy_recent loaded;
    CHECK(recent_load(&loaded, file));
    CHECK_EQ_INT(loaded.count, rc.count);
    for (int i = 0; i < rc.count; i++) {
        CHECK(strcmp(recent_path(&loaded, i), recent_path(&rc, i)) == 0);
        CHECK(strcmp(recent_display(&loaded, i), recent_display(&rc, i)) == 0);
    }

    /* -------------------------------------------- missing/corrupt degrades */
    {
        koboy_recent miss;
        /* Poisoned first, not zero-initialised: recent_load must OVERWRITE
           whatever was there, not merely fail to fill it in -- a test that
           starts from a zeroed struct cannot tell "load cleared it" from
           "load never touched it, and it happened to already be zero". */
        memset(&miss, 0x5A, sizeof miss);
        CHECK(!recent_load(&miss, "/nonexistent/koboy/recent.dat"));
        CHECK_EQ_INT(miss.count, 0);

        char bad[600];
        snprintf(bad, sizeof bad, "%s/short.dat", dir);
        FILE *f = fopen(bad, "wb");
        CHECK(f != NULL);
        if (f) { fputc('x', f); fclose(f); }   /* far short of the real size */

        koboy_recent bad_rc;
        memset(&bad_rc, 0x5A, sizeof bad_rc);
        CHECK(!recent_load(&bad_rc, bad));
        CHECK_EQ_INT(bad_rc.count, 0);
    }

    /* A path field with NO NUL byte anywhere in it must not crash the load
       and must come back as a bounded, valid C string -- a graceful-
       degradation check, not a mutant-catching one: see recent_load's
       "%.*s" comment for why a test cannot cleanly distinguish this line
       from a plain "%s" here (both end up producing the same clamped
       output for data shaped like this, since the destination is exactly
       as wide as the source field) without constructing a file corrupt
       enough that the UNGUARDED version would need real undefined
       behaviour to fail -- which this suite does not do. */
    {
        koboy_recent_entry raw[KOBOY_RECENT_MAX];
        memset(raw, 0, sizeof raw);
        memset(raw[0].path, 'P', sizeof raw[0].path);       /* no NUL at all */
        snprintf(raw[0].display, sizeof raw[0].display, "TITLE");

        char nonul[600];
        snprintf(nonul, sizeof nonul, "%s/nonul.dat", dir);
        FILE *f = fopen(nonul, "wb");
        CHECK(f != NULL);
        if (f) {
            CHECK(fwrite(raw, 1, sizeof raw, f) == sizeof raw);
            fclose(f);
        }

        koboy_recent got;
        CHECK(recent_load(&got, nonul));
        CHECK_EQ_INT(got.count, 1);
        CHECK_EQ_INT((int)strlen(recent_path(&got, 0)), KOBOY_RECENT_PATH - 1);
        char want[KOBOY_RECENT_PATH];
        memset(want, 'P', sizeof want - 1);
        want[sizeof want - 1] = 0;
        CHECK(strcmp(recent_path(&got, 0), want) == 0);
        CHECK(strcmp(recent_display(&got, 0), "TITLE") == 0);
    }

    /* ---------------------------------------------------- recent_prune_missing */
    {
        char romdir[] = "/tmp/koboy_recent_roms_XXXXXX";
        CHECK(mkdtemp(romdir) != NULL);
        char keep[600], gone[600];
        snprintf(keep, sizeof keep, "%s/KEEP.gb", romdir);
        snprintf(gone, sizeof gone, "%s/GONE.gb", romdir);
        FILE *kf = fopen(keep, "wb");
        CHECK(kf != NULL);
        if (kf) fclose(kf);
        /* `gone` is deliberately never created. */

        koboy_recent pr;
        recent_init(&pr);
        recent_touch(&pr, gone, "GONE.gb");   /* index 1 after the next touch */
        recent_touch(&pr, keep, "KEEP.gb");   /* index 0: most recent */
        CHECK_EQ_INT(pr.count, 2);

        recent_prune_missing(&pr);
        CHECK_EQ_INT(pr.count, 1);
        CHECK(strcmp(recent_path(&pr, 0), keep) == 0);

        char cmd[1024];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", romdir);
        if (system(cmd) != 0) fprintf(stderr, "NOTE: cleanup failed\n");
    }

    char cmd[1024];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) fprintf(stderr, "NOTE: cleanup failed\n");
})
