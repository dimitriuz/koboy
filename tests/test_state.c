/* mkdtemp() and system() are not declared under -std=c11 without this --
   matches src/sram.c, tests/test_romlist.c, tests/test_uiscript.c and
   tests/test_config.c. */
#define _DEFAULT_SOURCE
#include "test.h"
#include "safefile.h"
#include "state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TEST_MAIN({
    char dir[] = "/tmp/koboy_state_XXXXXX";
    CHECK(mkdtemp(dir) != NULL);

    char rom[512];
    snprintf(rom, sizeof rom, "%s/ZELDA.gb", dir);

    /* Slot paths are derived from the ROM stem, like .srm, so two games never
       share a slot. */
    char p1[512], p2[512];
    state_path(p1, sizeof p1, dir, rom, 1);
    state_path(p2, sizeof p2, dir, rom, 2);
    CHECK(strcmp(p1, p2) != 0);
    CHECK(strstr(p1, "ZELDA") != NULL);
    CHECK(strstr(p1, ".st1") != NULL);

    /* Out-of-range slots write an empty path rather than a surprising file. */
    char bad[512];
    state_path(bad, sizeof bad, dir, rom, 0);
    CHECK_EQ_INT(bad[0], 0);
    state_path(bad, sizeof bad, dir, rom, KOBOY_STATE_SLOTS + 1);
    CHECK_EQ_INT(bad[0], 0);

    CHECK_EQ_INT(state_exists(dir, rom, 1), 0);

    /* safefile_write is atomic: temp file plus rename, so a kill mid-write
       cannot corrupt an existing file. */
    unsigned char blob[64];
    for (int i = 0; i < 64; i++) blob[i] = (unsigned char)i;
    CHECK_EQ_INT(safefile_write(p1, blob, sizeof blob), 1);
    CHECK_EQ_INT(state_exists(dir, rom, 1), 1);

    /* No .tmp is left behind. */
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", p1);
    FILE *leftover = fopen(tmp, "rb");
    CHECK(leftover == NULL);
    if (leftover) fclose(leftover);

    unsigned char back[64];
    memset(back, 0xAA, sizeof back);
    CHECK_EQ_INT(safefile_read_exact(p1, back, sizeof back), 1);
    CHECK_EQ_INT(memcmp(back, blob, sizeof blob), 0);

    /* ALL OR NOTHING. A file shorter than the buffer must leave the buffer
       UNTOUCHED, because in real use that buffer is the core's live state.
       sram.c's comment records what happens otherwise: the previous version
       read straight into live memory and only then reported failure, so
       loading a truncated save destroyed it. A truncated state does the same
       to a running game. */
    {
        char shortp[512];
        snprintf(shortp, sizeof shortp, "%s/short.bin", dir);
        FILE *f = fopen(shortp, "wb");
        CHECK(f != NULL);
        fwrite(blob, 1, 10, f);
        fclose(f);

        unsigned char guard[64];
        memset(guard, 0x5A, sizeof guard);
        CHECK_EQ_INT(safefile_read_exact(shortp, guard, sizeof guard), 0);
        int untouched = 1;
        for (int i = 0; i < 64; i++) if (guard[i] != 0x5A) untouched = 0;
        CHECK_EQ_INT(untouched, 1);
    }

    /* A LONGER file is accepted, reading the first len bytes -- matching what
       sram_load does, because a mismatch there means a different cartridge or
       trailing data (RTC state, say), and refusing would be a new failure mode
       rather than a fix for this one. */
    {
        char longp[512];
        snprintf(longp, sizeof longp, "%s/long.bin", dir);
        FILE *f = fopen(longp, "wb");
        CHECK(f != NULL);
        fwrite(blob, 1, sizeof blob, f);
        fwrite(blob, 1, sizeof blob, f);
        fclose(f);
        unsigned char got[64];
        CHECK_EQ_INT(safefile_read_exact(longp, got, sizeof got), 1);
        CHECK_EQ_INT(memcmp(got, blob, sizeof blob), 0);
    }

    /* A missing file is a clean false, not a crash. */
    CHECK_EQ_INT(safefile_read_exact("/nonexistent/koboy/x", back, sizeof back), 0);

    /* Labels tell the user which slot they are about to overwrite, which is
       most of the value of having slots at all. */
    char label[64];
    state_slot_label(label, sizeof label, dir, rom, 1);
    CHECK(strstr(label, "1") != NULL);
    state_slot_label(label, sizeof label, dir, rom, 2);
    CHECK(strstr(label, "EMPTY") != NULL);

    char cmd[1024];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) fprintf(stderr, "NOTE: cleanup failed\n");
})
