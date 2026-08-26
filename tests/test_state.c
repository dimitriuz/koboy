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
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

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

    /* A buffer too small to hold the full path must not leave a truncated
       (and therefore WRONG) path in `out` -- same failure semantics as an
       out-of-range slot: writing "" makes the caller's fopen fail cleanly
       instead of silently saving under a shortened, surprising name. */
    char tiny[4];
    state_path(tiny, sizeof tiny, dir, rom, 1);
    CHECK_EQ_INT(tiny[0], 0);

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

    /* ATOMICITY, ASSERTED POSITIVELY.

       The check above is not enough on its own and used to be all there was:
       mutating safefile.c's "%s.tmp" to "%s" -- no temp file, no fsync, no
       rename, a plain truncating write straight onto the destination -- left
       the whole suite green, because "no .tmp left behind" is equally true of
       an implementation that never makes one.

       So make the temp path IMPOSSIBLE to open and watch what happens. A
       DIRECTORY at "<path>.tmp" cannot be fopen'd for writing, so an atomic
       implementation must fail the write and, crucially, leave the existing
       destination byte-for-byte intact -- which is the whole property. A
       non-atomic one opens the destination directly, succeeds, and destroys
       the previous contents. Both halves are asserted: the return value AND
       the surviving file. */
    {
        CHECK_EQ_INT(mkdir(tmp, 0700), 0);

        unsigned char before[64];
        CHECK_EQ_INT(safefile_read_exact(p1, before, sizeof before), 1);

        unsigned char other[64];
        for (int i = 0; i < 64; i++) other[i] = (unsigned char)(0xF0 ^ i);
        CHECK_EQ_INT(safefile_write(p1, other, sizeof other), 0);

        unsigned char after[64];
        CHECK_EQ_INT(safefile_read_exact(p1, after, sizeof after), 1);
        CHECK_EQ_INT(memcmp(before, after, sizeof before), 0);

        CHECK_EQ_INT(rmdir(tmp), 0);

        /* And with the obstruction gone the same write must now succeed and
           actually land, so the block above proves atomicity rather than
           merely proving that safefile_write can be made to fail. */
        CHECK_EQ_INT(safefile_write(p1, other, sizeof other), 1);
        CHECK_EQ_INT(safefile_read_exact(p1, after, sizeof after), 1);
        CHECK_EQ_INT(memcmp(other, after, sizeof other), 0);
        CHECK_EQ_INT(safefile_write(p1, blob, sizeof blob), 1);
    }

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
    /* SAVED and EMPTY must be DISTINGUISHABLE. The old assertion here was
       CHECK(strstr(label, "1")), which the slot number itself satisfies:
       hardcoding state.c's status to "EMPTY" left the suite green even though
       slot 1 had just been written. A label that cannot tell the user whether
       they are about to overwrite a save is the one thing it exists to do. */
    char label[64];
    CHECK_EQ_INT(state_exists(dir, rom, 1), 1);
    state_slot_label(label, sizeof label, dir, rom, 1);
    CHECK(strstr(label, "1") != NULL);
    CHECK(strstr(label, "SAVED") != NULL);
    CHECK(strstr(label, "EMPTY") == NULL);

    CHECK_EQ_INT(state_exists(dir, rom, 2), 0);
    state_slot_label(label, sizeof label, dir, rom, 2);
    CHECK(strstr(label, "2") != NULL);
    CHECK(strstr(label, "EMPTY") != NULL);
    CHECK(strstr(label, "SAVED") == NULL);

    char cmd[1024];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) fprintf(stderr, "NOTE: cleanup failed\n");
})
