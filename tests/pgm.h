#ifndef KOBOY_PGM_H
#define KOBOY_PGM_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int pgm_write(const char *path, const uint8_t *px, int w, int h, int stride)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) fwrite(px + (size_t)y * stride, 1, (size_t)w, f);
    fclose(f);
    return 1;
}

static int pgm_read(const char *path, uint8_t **px, int *w, int *h)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int maxv = 0;
    if (fscanf(f, "P5 %d %d %d", w, h, &maxv) != 3) { fclose(f); return 0; }
    fgetc(f); /* single whitespace byte before the raster */
    size_t n = (size_t)*w * (size_t)*h;
    *px = malloc(n);
    size_t got = fread(*px, 1, n, f);
    fclose(f);
    if (got != n) { free(*px); return 0; }
    return 1;
}

/* Compares against tests/golden/<name>.pgm. If the golden file is absent and
   KOBOY_GOLDEN_UPDATE=1 is set, writes it and passes. Never auto-updates an
   existing golden: a changed image must be reviewed and deleted deliberately. */
static int pgm_compare_golden(const char *name, const uint8_t *px,
                              int w, int h, int stride)
{
    char path[512];
    snprintf(path, sizeof path, "tests/golden/%s.pgm", name);
    uint8_t *want = NULL; int ww = 0, wh = 0;
    if (!pgm_read(path, &want, &ww, &wh)) {
        if (getenv("KOBOY_GOLDEN_UPDATE")) {
            if (!pgm_write(path, px, w, h, stride)) return 0;
            fprintf(stderr, "NOTE wrote new golden %s\n", path);
            return 1;
        }
        fprintf(stderr, "FAIL missing golden %s (rerun with KOBOY_GOLDEN_UPDATE=1)\n", path);
        return 0;
    }
    int ok = (ww == w && wh == h);
    for (int y = 0; ok && y < h; y++)
        if (memcmp(want + (size_t)y * w, px + (size_t)y * stride, (size_t)w) != 0) ok = 0;
    free(want);
    return ok;
}
#endif
