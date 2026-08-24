#define _POSIX_C_SOURCE 200809L
#include "sram.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

/* Temp file plus rename, so a kill mid-write cannot corrupt an existing save. */
bool sram_save(const char *path, const uint8_t *src, size_t len)
{
    if (!len) return true;
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(src, 1, len, f) == len;
    if (ok) ok = (fflush(f) == 0);
    if (ok) ok = (fsync(fileno(f)) == 0);
    if (fclose(f) != 0) ok = false;
    if (!ok) { remove(tmp); return false; }  /* best-effort cleanup: ignore remove() result */
    if (rename(tmp, path) != 0) { remove(tmp); return false; }
    return true;
}

bool sram_load(const char *path, uint8_t *dst, size_t len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t got = fread(dst, 1, len, f);
    fclose(f);
    return got == len;
}
