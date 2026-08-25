#include "test.h"
#include "sram.h"
#include <sys/stat.h>
#include <unistd.h>

TEST_MAIN({
    char path[512];
    sram_path_for_rom(path, sizeof path, "/saves", "/roms/Tetris (World).gb");
    CHECK(strcmp(path, "/saves/Tetris (World).srm") == 0);
    sram_path_for_rom(path, sizeof path, "/saves", "noext");
    CHECK(strcmp(path, "/saves/noext.srm") == 0);

    uint8_t out[64], in[64];
    for (int i = 0; i < 64; i++) out[i] = (uint8_t)(i * 3);
    CHECK(sram_save("build/t.srm", out, sizeof out));
    memset(in, 0, sizeof in);
    CHECK(sram_load("build/t.srm", in, sizeof in));
    CHECK(memcmp(in, out, sizeof out) == 0);

    /* ATOMICITY: no temp file may survive a successful save */
    CHECK(access("build/t.srm.tmp", F_OK) != 0);

    /* a failed save (fopen failure) must leave the previous good file intact
       and clean up any temp file */
    CHECK(!sram_save("build/no_such_dir/x.srm", out, sizeof out));
    CHECK(access("build/no_such_dir/x.srm.tmp", F_OK) != 0);  /* no temp file left */
    memset(in, 0, sizeof in);
    CHECK(sram_load("build/t.srm", in, sizeof in));
    CHECK(memcmp(in, out, sizeof out) == 0);

    /* a missing file is a benign miss, not a crash */
    CHECK(!sram_load("build/absent.srm", in, sizeof in));

    /* A SHORT FILE MUST NOT TOUCH THE DESTINATION AT ALL, byte for byte, and
       the whole buffer is checked rather than just its tail. `dst` is the core's
       LIVE save RAM: the old sram_load fread() straight into it and only then
       returned false, so a truncated save left SRAM as a mix of file bytes and
       core initial state, and the periodic flush ten seconds later wrote that
       hybrid back over the user's only save file. Loading the save destroyed it.
       Checking in[63] alone passed that version, because the damage was at the
       FRONT of the buffer. */
    FILE *f = fopen("build/short.srm", "wb");
    fputc(0x41, f); fclose(f);
    uint8_t sentinel[64];
    memset(sentinel, 0xEE, sizeof sentinel);
    memset(in, 0xEE, sizeof in);
    CHECK(!sram_load("build/short.srm", in, sizeof in));
    CHECK(memcmp(in, sentinel, sizeof in) == 0);
    CHECK_EQ_INT(in[0], 0xEE);          /* where the partial read landed */
    CHECK_EQ_INT(in[63], 0xEE);

    /* Same for a file one byte short of the whole thing: the failure boundary is
       "all of it or none of it", not "most of it". */
    f = fopen("build/nearly.srm", "wb");
    CHECK(fwrite(out, 1, sizeof out - 1, f) == sizeof out - 1);
    fclose(f);
    memset(in, 0xEE, sizeof in);
    CHECK(!sram_load("build/nearly.srm", in, sizeof in));
    CHECK(memcmp(in, sentinel, sizeof in) == 0);

    /* An empty file is short too, and must be just as harmless. */
    f = fopen("build/empty.srm", "wb"); fclose(f);
    memset(in, 0xEE, sizeof in);
    CHECK(!sram_load("build/empty.srm", in, sizeof in));
    CHECK(memcmp(in, sentinel, sizeof in) == 0);

    /* A missing file leaves the destination alone as well: this is the ordinary
       first-run path, and the core's freshly initialised SRAM is what should
       survive it. */
    memset(in, 0xEE, sizeof in);
    CHECK(!sram_load("build/absent.srm", in, sizeof in));
    CHECK(memcmp(in, sentinel, sizeof in) == 0);

    /* and the exact-length success case still copies the whole file through */
    memset(in, 0xEE, sizeof in);
    CHECK(sram_load("build/t.srm", in, sizeof in));
    CHECK(memcmp(in, out, sizeof out) == 0);

    /* a zero-length request is refused rather than reading anything */
    CHECK(!sram_load("build/t.srm", in, 0));
    CHECK(!sram_load("build/t.srm", NULL, sizeof in));

    /* rename failure (destination is a directory) cleans up temp file */
    mkdir("build/is_a_dir", 0755);
    CHECK(!sram_save("build/is_a_dir", out, sizeof out));
    CHECK(access("build/is_a_dir.tmp", F_OK) != 0);  /* no temp file left */
    /* destination directory still exists and is untouched */
    CHECK(access("build/is_a_dir", F_OK) == 0);
    rmdir("build/is_a_dir");
})
