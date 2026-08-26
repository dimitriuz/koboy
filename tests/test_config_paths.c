/* Regression test for the bug that cost a device round-trip: a core name with
 * no slash in it is unloadable, because dlopen() given a slashless name
 * searches the loader's paths and never the current directory. The fix
 * resolves such a name against the directory containing the executable.
 *
 * The important property this asserts is the one that was missing: a slashless
 * name must NOT be passed through unchanged. Note that a test which only
 * checked "the resolved path is non-empty", or which only used paths with
 * slashes in them, would pass identically with the fix reverted -- which is
 * exactly how this bug survived every test we had.
 */
#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include "config.h"
#include <limits.h>
#include <unistd.h>

TEST_MAIN({
    char out[512];

    /* --- config_join_sibling: the pure half, no /proc dependency --- */

    /* A slashless name is joined to the directory. This is the whole fix. */
    CHECK(config_join_sibling(out, sizeof out, "gambatte_libretro.so", "/mnt/onboard/.adds/koboy"));
    CHECK(!strcmp(out, "/mnt/onboard/.adds/koboy/gambatte_libretro.so"));
    /* Spelled out separately so a regression cannot hide behind the compare
       above being accidentally true: the result must differ from the input. */
    CHECK(strcmp(out, "gambatte_libretro.so") != 0);
    CHECK(strchr(out, '/') != NULL);

    /* Anything the caller spelled with a slash is theirs; pass it verbatim. */
    CHECK(config_join_sibling(out, sizeof out, "/abs/core.so", "/install"));
    CHECK(!strcmp(out, "/abs/core.so"));
    CHECK(config_join_sibling(out, sizeof out, "./core.so", "/install"));
    CHECK(!strcmp(out, "./core.so"));
    CHECK(config_join_sibling(out, sizeof out, "sub/core.so", "/install"));
    CHECK(!strcmp(out, "sub/core.so"));

    /* "." is the install directory itself, not "<dir>/." -- that trailing
       component would otherwise appear in every save path and error message. */
    CHECK(config_join_sibling(out, sizeof out, ".", "/install"));
    CHECK(!strcmp(out, "/install"));

    /* An executable living in "/" must not produce "//name". */
    CHECK(config_join_sibling(out, sizeof out, "core.so", "/"));
    CHECK(!strcmp(out, "/core.so"));

    /* Bad input and truncation report failure rather than emitting a
       half-formed path that would then be reported as "file not found". */
    CHECK(!config_join_sibling(out, sizeof out, "", "/install"));
    CHECK(!config_join_sibling(out, sizeof out, "core.so", ""));
    CHECK(!config_join_sibling(out, sizeof out, "core.so", NULL));
    CHECK(!config_join_sibling(NULL, sizeof out, "core.so", "/install"));
    char small[8];
    memcpy(small, "SENTINE", 8);           /* 7 chars + NUL, exactly fits */
    CHECK(!config_join_sibling(small, sizeof small, "core.so", "/a/long/enough/dir"));
    CHECK(!strcmp(small, "SENTINE"));      /* untouched by the failed call */

    /* --- config_exe_dir: the /proc half, checked against reality --- */

    char dir[PATH_MAX];
    CHECK(config_exe_dir(dir, sizeof dir));
    CHECK(dir[0] == '/');                             /* absolute */
    size_t dl = strlen(dir);
    CHECK(dl == 1 || dir[dl - 1] != '/');             /* no trailing slash */
    /* The directory really does contain this test binary: join our own name
       onto it and check the result is readable. */
    char self[PATH_MAX];
    CHECK(config_join_sibling(self, sizeof self, "test_config_paths", dir));
    CHECK(access(self, X_OK) == 0);

    /* --- config_resolve_paths: the four fields, end to end --- */

    koboy_config c;
    config_defaults(&c);
    /* The shipped defaults are exactly the broken shapes: a slashless core
       name, a save_dir of ".", and a slashless rom_dir. rom_dir was added
       after this function was already written and, the first time around,
       skipped this file entirely: a reviewer mutant deleted both its
       config_join_sibling call in config_resolve_paths (config.c:194-195) and
       its ini dispatch line (config.c:243), and the suite -- including
       smoke_host.sh, which only ever passes an absolute --rom-dir -- stayed
       green. The failure that hides is device-only and silent: a NickelMenu
       launch sets no cwd, so a shipped "rom_dir = roms" resolves against "/"
       and the browser reports "cannot read rom directory" on a device where
       that directory exists right next to the binary. */
    CHECK(strchr(c.core_path, '/') == NULL);
    CHECK(!strcmp(c.save_dir, "."));
    CHECK(strchr(c.rom_dir, '/') == NULL);
    snprintf(c.rom_path, sizeof c.rom_path, "tetris.gb");

    config_resolve_paths(&c);
    CHECK(strchr(c.core_path, '/') != NULL);
    CHECK(strcmp(c.core_path, "gambatte_libretro.so") != 0);
    CHECK(!strcmp(c.save_dir, dir));
    CHECK(strcmp(c.rom_path, "tetris.gb") != 0);
    CHECK(strchr(c.rom_path, '/') != NULL);
    CHECK(strchr(c.rom_dir, '/') != NULL);
    CHECK(strcmp(c.rom_dir, "roms") != 0);

    /* Idempotent: a second pass must not stack another directory on the
       front, because the paths now contain slashes. */
    char once[512], once_rom[512];
    snprintf(once, sizeof once, "%s", c.core_path);
    snprintf(once_rom, sizeof once_rom, "%s", c.rom_dir);
    config_resolve_paths(&c);
    CHECK(!strcmp(c.core_path, once));
    CHECK(!strcmp(c.rom_dir, once_rom));

    /* An explicit path survives resolution untouched, so --core /tmp/x.so
       still means /tmp/x.so, and --rom-dir /tmp/roms still means /tmp/roms. */
    config_defaults(&c);
    snprintf(c.core_path, sizeof c.core_path, "/tmp/explicit.so");
    snprintf(c.rom_dir, sizeof c.rom_dir, "/tmp/explicit_roms");
    config_resolve_paths(&c);
    CHECK(!strcmp(c.core_path, "/tmp/explicit.so"));
    CHECK(!strcmp(c.rom_dir, "/tmp/explicit_roms"));

    /* The ini dispatch line is the other half of the mutant that was deleted:
       config_load must actually populate rom_dir from a "rom_dir = " line.
       Checked before any resolution runs, so this cannot pass merely because
       the untouched default happens to look plausible. */
    FILE *rf = fopen("build/rom_dir.ini", "w");
    CHECK(rf);
    fprintf(rf, "rom_dir = mygames\n");
    fclose(rf);
    config_defaults(&c);
    CHECK(config_load(&c, "build/rom_dir.ini"));
    CHECK(!strcmp(c.rom_dir, "mygames"));
});
