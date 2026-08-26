#define _POSIX_C_SOURCE 200809L
#include "sram.h"
#include "safefile.h"
#include <stdio.h>
#include <string.h>

void sram_path_for_rom(char *out, size_t outlen, const char *save_dir,
                       const char *rom_path)
{
    const char *base = strrchr(rom_path, '/');
    base = base ? base + 1 : rom_path;
    char stem[256];
    snprintf(stem, sizeof stem, "%s", base);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = 0;
    /* Path truncation possible but safe for normal ROM filenames. rom_path ending in '/'
       (empty basename) yields "<dir>/.srm". No validation added — caller's responsibility. */
    snprintf(out, outlen, "%s/%s.srm", save_dir, stem);
}

/* Save/load are thin wrappers over safefile.c, which carries the atomic-write
   and all-or-nothing-read logic (and the comment explaining why it must be
   all-or-nothing) -- extracted there so save states share the same discipline
   instead of reinventing a weaker version. See safefile.c. */
bool sram_save(const char *path, const uint8_t *src, size_t len)
{ return safefile_write(path, src, len); }

bool sram_load(const char *path, uint8_t *dst, size_t len)
{ return safefile_read_exact(path, dst, len); }
