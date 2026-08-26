#define _POSIX_C_SOURCE 200809L
#include "safefile.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Temp file plus rename, so a kill mid-write cannot corrupt an existing save. */
bool safefile_write(const char *path, const void *src, size_t len)
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

/* ALL OR NOTHING, and this is the whole point of the temporary buffer: `dst` is
   the core's LIVE save RAM. The previous version fread() straight into it and
   only then reported failure, so a truncated save file left SRAM as a mix of
   partial file and core initial state -- and the periodic flush ten seconds
   later wrote that hybrid back over the user's only save. Loading the save
   destroyed it. Nothing may touch `dst` unless the whole of it can be filled.
   A file LONGER than SRAM is still accepted, reading the first `len` bytes, which
   is what the old code did: a mismatch there means a different cartridge or a
   format with trailing data (RTC state, for instance), and refusing to load would
   be a new failure mode rather than a fix for this one. */
bool safefile_read_exact(const char *path, void *dst, size_t len)
{
    if (!dst || !len) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t *tmp = malloc(len);
    if (!tmp) { fclose(f); return false; }
    size_t got = fread(tmp, 1, len, f);
    fclose(f);
    bool ok = (got == len);
    if (ok) memcpy(dst, tmp, len);
    free(tmp);
    return ok;
}
