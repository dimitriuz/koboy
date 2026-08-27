/* mkdtemp() is not declared under -std=c11 without this -- matches
   tests/test_state.c and the others that make a scratch directory. */
#define _DEFAULT_SOURCE
#include "test.h"
#include "png.h"
#include "shot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void touch(const char *dir, const char *name)
{
    char p[600];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "wb");
    if (f) { fputc('x', f); fclose(f); }
}

TEST_MAIN({
    char stem[96];

    /* --------------------------------------------------- 1. the filename */
    shot_stem_for_rom(stem, sizeof stem, "/mnt/onboard/roms/TETRIS.gb");
    CHECK(!strcmp(stem, "TETRIS"));

    /* The library this runs against is a No-Intro set: spaces, commas and
       parentheses in nearly every name. They are folded, and RUNS of them
       collapse to one separator -- "Zelda__The__USA_" is what the naive
       version produces and it is ugly in a README, which is where these
       filenames end up. */
    shot_stem_for_rom(stem, sizeof stem, "/roms/Legend of Zelda, The (USA).gb");
    CHECK(!strcmp(stem, "Legend_of_Zelda_The_USA"));

    /* No trailing separator, from a name that ends in folded characters. */
    shot_stem_for_rom(stem, sizeof stem, "/roms/Sonic (USA) .gb");
    CHECK(!strcmp(stem, "Sonic_USA"));

    /* Only the LAST extension goes, and a dot-leading name keeps its dot. */
    shot_stem_for_rom(stem, sizeof stem, "/roms/game.v2.nes");
    CHECK(!strcmp(stem, "game.v2"));
    shot_stem_for_rom(stem, sizeof stem, "/roms/NOEXT");
    CHECK(!strcmp(stem, "NOEXT"));

    /* Degenerate inputs land somewhere predictable instead of producing a
       file called ".png". */
    shot_stem_for_rom(stem, sizeof stem, NULL);
    CHECK(!strcmp(stem, "game"));
    shot_stem_for_rom(stem, sizeof stem, "");
    CHECK(!strcmp(stem, "game"));
    shot_stem_for_rom(stem, sizeof stem, "/roms/!!! ???.gb");
    CHECK(!strcmp(stem, "game"));

    /* Truncation never overruns and always terminates. */
    {
        char small[8];
        shot_stem_for_rom(small, sizeof small, "/roms/ABCDEFGHIJKLMNOP.gb");
        CHECK_EQ_INT(strlen(small), 7);
        CHECK(!strncmp(small, "ABCDEFG", 7));
    }

    /* ------------------------------------------------------- 2. the path */
    {
        char p[512];
        CHECK(shot_path(p, sizeof p, "/tmp/shots", "TETRIS", 7));
        CHECK(!strcmp(p, "/tmp/shots/TETRIS-007.png"));
        /* Zero-padded to three, so a directory listing sorts in the order the
           shots were taken. */
        CHECK(shot_path(p, sizeof p, "/tmp/shots", "TETRIS", 123));
        CHECK(!strcmp(p, "/tmp/shots/TETRIS-123.png"));

        /* Out of range writes "" rather than a surprising name -- the same
           rule state_path follows, for the same reason: this path is about to
           be written to. */
        CHECK(!shot_path(p, sizeof p, "/tmp/shots", "TETRIS", 0));
        CHECK_EQ_INT(p[0], 0);
        CHECK(!shot_path(p, sizeof p, "/tmp/shots", "TETRIS", KOBOY_SHOT_SEQ_MAX + 1));
        CHECK_EQ_INT(p[0], 0);
        char tiny[10];
        CHECK(!shot_path(tiny, sizeof tiny, "/tmp/shots", "TETRIS", 1));
        CHECK_EQ_INT(tiny[0], 0);
    }

    /* ------------------------------------------- 3. the counter is SCANNED */
    {
        char dir[] = "/tmp/koboy_shot_XXXXXX";
        CHECK(mkdtemp(dir) != NULL);

        /* Nothing there yet, and a directory that does not exist at all. */
        CHECK_EQ_INT(shot_last_seq(dir, "TETRIS"), 0);
        CHECK_EQ_INT(shot_last_seq("/tmp/koboy_no_such_dir_at_all", "TETRIS"), 0);

        touch(dir, "TETRIS-001.png");
        touch(dir, "TETRIS-004.png");
        touch(dir, "TETRIS-002.png");
        /* THE POINT OF SCANNING: the highest, not the count and not the last
           readdir returned. readdir order is not sorted on any filesystem
           this runs on, so "004 was created second" is exactly the case a
           count-based or last-seen implementation gets wrong. */
        CHECK_EQ_INT(shot_last_seq(dir, "TETRIS"), 4);

        /* Another game's shots, and files that merely look similar, must not
           move this game's counter -- otherwise the numbering of every game
           in the directory drifts with every other game's. */
        touch(dir, "ZELDA-009.png");
        touch(dir, "TETRIS-9.png");          /* too few digits */
        touch(dir, "TETRIS-007.txt");        /* not a png */
        touch(dir, "TETRIS-007.png.bak");    /* not a png either */
        touch(dir, "TETRIS007.png");         /* no separator */
        touch(dir, "TETRIS-EXTRA-008.png");  /* a different stem */
        CHECK_EQ_INT(shot_last_seq(dir, "TETRIS"), 4);
        CHECK_EQ_INT(shot_last_seq(dir, "ZELDA"), 9);

        char rm[600];
        const char *junk[] = { "TETRIS-001.png", "TETRIS-004.png", "TETRIS-002.png",
                               "ZELDA-009.png", "TETRIS-9.png", "TETRIS-007.txt",
                               "TETRIS-007.png.bak", "TETRIS007.png",
                               "TETRIS-EXTRA-008.png" };
        for (size_t i = 0; i < sizeof junk / sizeof junk[0]; i++) {
            snprintf(rm, sizeof rm, "%s/%s", dir, junk[i]);
            remove(rm);
        }
        rmdir(dir);
    }

    /* --------------------------------------------------- 4. compositing */
    /* The panel buffer holds the FACEPLATE and video.c's buffer holds the
       GAME; nothing in koboy ever holds both, which is why this function
       exists at all. So the assertion is that both halves survive, in the
       right places. */
    {
        enum { PW = 20, PH = 16, PSTRIDE = 24, GX = 4, GY = 3, GW = 8, GH = 6,
               GSTRIDE = 10 };
        uint8_t panel[PSTRIDE * PH], game[GSTRIDE * GH], dst[PW * PH];
        memset(panel, 0x40, sizeof panel);          /* the faceplate */
        memset(game,  0x11, sizeof game);           /* padding in the stride */
        for (int y = 0; y < GH; y++)
            for (int x = 0; x < GW; x++) game[y * GSTRIDE + x] = (uint8_t)(0x80 + y * GW + x);

        koboy_rect r = { GX, GY, GW, GH };
        memset(dst, 0, sizeof dst);
        CHECK(shot_compose(dst, PW, panel, PSTRIDE, PW, PH, game, GSTRIDE, &r));

        int chrome_ok = 1, game_ok = 1, padding_leaked = 0;
        for (int y = 0; y < PH; y++)
            for (int x = 0; x < PW; x++) {
                bool inside = (x >= GX && x < GX + GW && y >= GY && y < GY + GH);
                uint8_t v = dst[y * PW + x];
                if (!inside && v != 0x40) chrome_ok = 0;
                if (inside && v != game[(y - GY) * GSTRIDE + (x - GX)]) game_ok = 0;
                if (v == 0x11) padding_leaked = 1;
            }
        CHECK(chrome_ok);
        CHECK(game_ok);
        /* The game buffer's stride is wider than the rect (video.c's is), so
           an implementation that copied stride-wide rows would smear the
           padding across the picture. */
        CHECK(!padding_leaked);

        /* A rect that does not fit is REFUSED, not clipped: it means the
           caller's geometry and video's disagree, and half a picture written
           as though it were whole is worse than no file. */
        koboy_rect bad = { GX, GY, PW, GH };
        CHECK(!shot_compose(dst, PW, panel, PSTRIDE, PW, PH, game, GSTRIDE, &bad));
        bad = (koboy_rect){ -1, GY, GW, GH };
        CHECK(!shot_compose(dst, PW, panel, PSTRIDE, PW, PH, game, GSTRIDE, &bad));
        bad = (koboy_rect){ GX, PH - 1, GW, GH };
        CHECK(!shot_compose(dst, PW, panel, PSTRIDE, PW, PH, game, GSTRIDE, &bad));
        bad = (koboy_rect){ GX, GY, 0, GH };
        CHECK(!shot_compose(dst, PW, panel, PSTRIDE, PW, PH, game, GSTRIDE, &bad));
        /* A game stride narrower than the rect would read past each row. */
        r = (koboy_rect){ GX, GY, GW, GH };
        CHECK(!shot_compose(dst, PW, panel, PSTRIDE, PW, PH, game, GW - 1, &r));
        /* And a dst/panel stride narrower than the panel. */
        CHECK(!shot_compose(dst, PW - 1, panel, PSTRIDE, PW, PH, game, GSTRIDE, &r));
        CHECK(!shot_compose(dst, PW, panel, PW - 1, PW, PH, game, GSTRIDE, &r));
    }

    /* ----------------------------------------------- 5. capture, end to end */
    {
        char base[] = "/tmp/koboy_cap_XXXXXX";
        CHECK(mkdtemp(base) != NULL);
        char dir[300];
        snprintf(dir, sizeof dir, "%s/screenshots", base);   /* does NOT exist yet */

        enum { PW = 40, PH = 30, GX = 5, GY = 4, GW = 16, GH = 12 };
        uint8_t panel[PW * PH], game[GW * GH];
        memset(panel, 0xFF, sizeof panel);
        memset(game, 0x00, sizeof game);
        koboy_rect r = { GX, GY, GW, GH };

        char path[512];
        int  seq = -1;
        CHECK(shot_capture(dir, "/roms/AAA TEST.gb", panel, PW, PW, PH,
                           game, GW, &r, path, sizeof path, &seq));
        CHECK_EQ_INT(seq, 1);
        CHECK(strstr(path, "AAA_TEST-001.png") != NULL);

        /* It is a real PNG of the whole PANEL -- not of the game rect, and
           not a zero-byte file, which is what a test that only checked
           existence would have accepted. */
        FILE *f = fopen(path, "rb");
        CHECK(f != NULL);
        if (f) {
            uint8_t head[8];
            CHECK_EQ_INT(fread(head, 1, 8, f), 8);
            static const uint8_t SIG[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
            CHECK(!memcmp(head, SIG, 8));
            fseek(f, 0, SEEK_END);
            CHECK_EQ_INT(ftell(f), (long)png_gray8_size(PW, PH));
            fclose(f);
        }

        /* A second shot of the same game does not overwrite the first. */
        CHECK(shot_capture(dir, "/roms/AAA TEST.gb", panel, PW, PW, PH,
                           game, GW, &r, path, sizeof path, &seq));
        CHECK_EQ_INT(seq, 2);
        CHECK(strstr(path, "AAA_TEST-002.png") != NULL);

        /* AND NEITHER DOES A RELAUNCH. This is the whole reason the counter
           is scanned rather than kept in memory: a fresh process (no state
           carried over -- this call knows nothing of the two above beyond
           what is on disk) must continue the numbering, not restart it. The
           file planted here stands in for a previous session's best shot. */
        {
            char planted[400];
            snprintf(planted, sizeof planted, "%s/AAA_TEST-009.png", dir);
            FILE *pf = fopen(planted, "wb");
            CHECK(pf != NULL);
            if (pf) { fputc('x', pf); fclose(pf); }
            CHECK(shot_capture(dir, "/roms/AAA TEST.gb", panel, PW, PW, PH,
                               game, GW, &r, path, sizeof path, &seq));
            CHECK_EQ_INT(seq, 10);
            /* and the planted file is untouched */
            struct { long n; } sz = { 0 };
            FILE *chk = fopen(planted, "rb");
            if (chk) { fseek(chk, 0, SEEK_END); sz.n = ftell(chk); fclose(chk); }
            CHECK_EQ_INT(sz.n, 1);
        }

        /* Refusals: a rect that does not fit the panel writes nothing at all
           (and does not consume a number, because nothing was written). */
        koboy_rect huge = { 0, 0, PW + 1, GH };
        CHECK(!shot_capture(dir, "/roms/AAA TEST.gb", panel, PW, PW, PH,
                            game, GW, &huge, path, sizeof path, &seq));
        CHECK_EQ_INT(seq, 0);
        CHECK_EQ_INT(path[0], 0);
        CHECK_EQ_INT(shot_last_seq(dir, "AAA_TEST"), 10);

        /* The ceiling: past KOBOY_SHOT_SEQ_MAX it refuses rather than
           wrapping onto a file that already exists. */
        {
            char planted[400];
            snprintf(planted, sizeof planted, "%s/AAA_TEST-%03d.png", dir,
                     KOBOY_SHOT_SEQ_MAX);
            FILE *pf = fopen(planted, "wb");
            if (pf) { fputc('x', pf); fclose(pf); }
            CHECK(!shot_capture(dir, "/roms/AAA TEST.gb", panel, PW, PW, PH,
                                game, GW, &r, path, sizeof path, &seq));
            remove(planted);
        }

        char rm[400];
        for (int i = 1; i <= 10; i++) {
            snprintf(rm, sizeof rm, "%s/AAA_TEST-%03d.png", dir, i);
            remove(rm);
        }
        rmdir(dir);
        rmdir(base);
    }
})
