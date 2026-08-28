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

    recent_touch(&rc, "/roms/AAA.gb");
    CHECK_EQ_INT(rc.count, 1);
    CHECK(strcmp(recent_path(&rc, 0), "/roms/AAA.gb") == 0);
    CHECK(strcmp(recent_display(&rc, 0), "AAA.gb") == 0);

    /* Most-recent first. */
    recent_touch(&rc, "/roms/BBB.gb");
    recent_touch(&rc, "/roms/CCC.gb");
    CHECK_EQ_INT(rc.count, 3);
    CHECK(strcmp(recent_path(&rc, 0), "/roms/CCC.gb") == 0);
    CHECK(strcmp(recent_path(&rc, 1), "/roms/BBB.gb") == 0);
    CHECK(strcmp(recent_path(&rc, 2), "/roms/AAA.gb") == 0);

    /* Playing an ALREADY-PRESENT rom moves it to the front instead of
       duplicating -- the count must not grow, and the other two entries
       keep their relative order. */
    recent_touch(&rc, "/roms/AAA.gb");
    CHECK_EQ_INT(rc.count, 3);
    CHECK(strcmp(recent_path(&rc, 0), "/roms/AAA.gb") == 0);
    CHECK(strcmp(recent_path(&rc, 1), "/roms/CCC.gb") == 0);
    CHECK(strcmp(recent_path(&rc, 2), "/roms/BBB.gb") == 0);

    /* ------------------------------------------- the name IS the path's tail
       Not a parameter any more, so this is the whole rule: whatever the path
       says, the row says. A ROM that moved into a subfolder has a different
       path and therefore a different row; nothing can label it with the name
       of a file it is not. */
    {
        char nm[KOBOY_RECENT_DISPLAY];
        recent_name_from_path(nm, sizeof nm, "/roms/GBA/Advance Wars 2.gba");
        CHECK(strcmp(nm, "Advance Wars 2.gba") == 0);
        /* No separator at all: the path is its own name. */
        recent_name_from_path(nm, sizeof nm, "BALL.mgw");
        CHECK(strcmp(nm, "BALL.mgw") == 0);
        /* A trailing separator leaves an empty tail. A blank row is a row the
           user cannot read and therefore cannot avoid tapping, so the whole
           path is shown instead. */
        recent_name_from_path(nm, sizeof nm, "/roms/GBA/");
        CHECK(strcmp(nm, "/roms/GBA/") == 0);
        /* Defensive, and live: NULL and a zero-length buffer are both
           reachable through recent_load's fixed-size record. */
        recent_name_from_path(nm, sizeof nm, NULL);
        CHECK_EQ_INT(nm[0], 0);
        recent_name_from_path(nm, 0, "/roms/X.gb");   /* must not write */

        /* A name longer than the row can hold is CLIPPED and terminated, not
           refused: the display field (160) is smaller than a path (512), so
           this is reachable with an ordinary long filename. Asserted on the
           length and the content, not with a sentinel band around the
           buffer -- a band could only turn red by observing undefined
           behaviour, which this project does not count as a test (CLAUDE.md,
           chrome_bands). */
        char longpath[KOBOY_RECENT_PATH];
        memset(longpath, 'P', sizeof longpath - 1);
        longpath[sizeof longpath - 1] = 0;
        recent_name_from_path(nm, sizeof nm, longpath);
        CHECK_EQ_INT((int)strlen(nm), KOBOY_RECENT_DISPLAY - 1);
        CHECK_EQ_INT((int)strspn(nm, "P"), KOBOY_RECENT_DISPLAY - 1);
    }

    /* Re-touching an already-present path re-derives its name, so a row that
       a previous build wrote with the WRONG name is corrected by playing it.
       Poisoned in place first, because there is no other way to construct a
       wrong row now that the name is not a parameter. */
    snprintf(rc.entries[0].display, sizeof rc.entries[0].display, "WRONG.gb");
    recent_touch(&rc, "/roms/AAA.gb");
    CHECK(strcmp(recent_display(&rc, 0), "AAA.gb") == 0);

    /* Cap eviction: past KOBOY_RECENT_MAX the OLDEST entry ages out, never
       the newest, and the count never exceeds the cap. */
    {
        koboy_recent cap;
        recent_init(&cap);
        char path[64];
        for (int i = 0; i < KOBOY_RECENT_MAX; i++) {
            snprintf(path, sizeof path, "/roms/G%02d.gb", i);
            recent_touch(&cap, path);
        }
        CHECK_EQ_INT(cap.count, KOBOY_RECENT_MAX);
        CHECK(strcmp(recent_path(&cap, 0), "/roms/G09.gb") == 0);   /* newest */
        CHECK(strcmp(recent_path(&cap, KOBOY_RECENT_MAX - 1),
                     "/roms/G00.gb") == 0);                         /* oldest, still present */

        /* One more push: G00 (the oldest) must be the one that disappears. */
        recent_touch(&cap, "/roms/G10.gb");
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
        recent_touch(&e, "");
        recent_touch(&e, NULL);
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
        /* NOT "TITLE", and this is the assertion that proves the file's own
           display bytes are not an input: this record carries a perfectly
           well-formed name that disagrees with its path, exactly like the
           corrupted row on the author's device, and the load must answer
           with the path's tail regardless. The path has no separator and is
           longer than the display field, so the tail is the path clipped. */
        char want_disp[KOBOY_RECENT_DISPLAY];
        memset(want_disp, 'P', sizeof want_disp - 1);
        want_disp[sizeof want_disp - 1] = 0;
        CHECK(strcmp(recent_display(&got, 0), want_disp) == 0);
    }

    /* ------------------- a recent.dat written by an older build, repaired
       THE OWNER'S FILE, in miniature: two GBA rows, and the second one's
       stored name is the first one's. Nothing in the record marks it as
       wrong -- path and display were independent fields, so the file is
       structurally perfect and semantically a lie. Loading it must produce
       two rows whose names match their own paths.

       This is the case that a save/load round trip cannot reach (a round
       trip only ever sees names this build derived) and that recent_touch's
       repair cannot reach either (that one needs the user to select the bad
       row first). */
    {
        koboy_recent_entry raw[KOBOY_RECENT_MAX];
        memset(raw, 0, sizeof raw);
        snprintf(raw[0].path, sizeof raw[0].path,
                 "/mnt/onboard/roms/GBA/Pokemon - Emerald Version (USA, Europe).gba");
        snprintf(raw[0].display, sizeof raw[0].display,
                 "Pokemon - Emerald Version (USA, Europe).gba");
        snprintf(raw[1].path, sizeof raw[1].path,
                 "/mnt/onboard/roms/GBA/Advance Wars 2.gba");
        snprintf(raw[1].display, sizeof raw[1].display,     /* THE WRONG NAME */
                 "Pokemon - Emerald Version (USA, Europe).gba");

        char stale[600];
        snprintf(stale, sizeof stale, "%s/stale.dat", dir);
        FILE *f = fopen(stale, "wb");
        CHECK(f != NULL);
        if (f) {
            CHECK(fwrite(raw, 1, sizeof raw, f) == sizeof raw);
            fclose(f);
        }

        koboy_recent got;
        CHECK(recent_load(&got, stale));
        CHECK_EQ_INT(got.count, 2);
        CHECK(strcmp(recent_display(&got, 1), "Advance Wars 2.gba") == 0);
        CHECK(strcmp(recent_display(&got, 0),
                     "Pokemon - Emerald Version (USA, Europe).gba") == 0);
        /* The shape the device showed: one title on two rows. */
        CHECK(strcmp(recent_display(&got, 0), recent_display(&got, 1)) != 0);
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
        recent_touch(&pr, gone);   /* index 1 after the next touch */
        recent_touch(&pr, keep);   /* index 0: most recent */
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

    /* ---- a caller may pass pointers INTO the list, and one does ---- */
    {
        koboy_recent rc;
        memset(&rc, 0, sizeof rc);
        recent_touch(&rc, "roms/GBA/Advance Wars 2.gba");
        recent_touch(&rc, "roms/GBA/Pokemon Emerald.gba");
        /* Now: [0] = Pokemon, [1] = Advance Wars. */
        CHECK(strcmp(recent_display(&rc, 0), "Pokemon Emerald.gba") == 0);
        CHECK(strcmp(recent_display(&rc, 1), "Advance Wars 2.gba") == 0);

        /* THE BUG, reported from the device: re-touching entry 1 the way main.c
           does -- with pointers INTO the list -- used to read them AFTER the
           shift had overwritten that slot, so the moved entry took its
           neighbour's name. The list showed "Pokemon Emerald" twice and the
           second row started Advance Wars.

           The display argument is gone (the name is derived), so this passes
           the one pointer that still aliases: recent_path returns
           &rc.entries[1].path, which the shift overwrites with entry 0's.
           Unsnapshotted, the moved entry comes back naming Pokemon Emerald.

           Asserted on the DISPLAY of the moved entry, not the count: the count
           was always right, which is why nothing caught this. */
        recent_touch(&rc, recent_path(&rc, 1));
        CHECK_EQ_INT(rc.count, 2);
        CHECK(strcmp(recent_path(&rc, 0), "roms/GBA/Advance Wars 2.gba") == 0);
        CHECK(strcmp(recent_display(&rc, 0), "Advance Wars 2.gba") == 0);
        /* And the displaced entry keeps its own name too. */
        CHECK(strcmp(recent_display(&rc, 1), "Pokemon Emerald.gba") == 0);
        /* No two rows may share a display while naming different paths --
           the exact shape the device showed. */
        CHECK(strcmp(recent_display(&rc, 0), recent_display(&rc, 1)) != 0);
    }
})
