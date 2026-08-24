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

    /* a short file does not overrun the destination */
    FILE *f = fopen("build/short.srm", "wb");
    fputc(0x41, f); fclose(f);
    memset(in, 0xEE, sizeof in);
    CHECK(!sram_load("build/short.srm", in, sizeof in));
    CHECK_EQ_INT(in[63], 0xEE);

    /* rename failure (destination is a directory) cleans up temp file */
    mkdir("build/is_a_dir", 0755);
    CHECK(!sram_save("build/is_a_dir", out, sizeof out));
    CHECK(access("build/is_a_dir.tmp", F_OK) != 0);  /* no temp file left */
    /* destination directory still exists and is untouched */
    CHECK(access("build/is_a_dir", F_OK) == 0);
    rmdir("build/is_a_dir");
})
