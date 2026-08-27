/* mkdtemp() is not declared under -std=c11 without this -- matches
   tests/test_state.c and the others that make a scratch directory. */
#define _DEFAULT_SOURCE
#include "test.h"
#include "png.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------------
   AN INDEPENDENT DECODER, and independence is the whole point of this file.
   Every routine below is written from the PNG/zlib specifications rather
   than from src/png.c: the CRC is computed BITWISE where the encoder uses a
   table, and the Adler32 is a straight two-accumulator loop where the
   encoder folds it into its emit path. A test that reused the encoder's own
   helpers would agree with it about a wrong answer, which is the exact shape
   of "a test that passes whether or not the code it guards is correct" this
   project keeps having to fix.

   tests/smoke_host.sh puts the same bytes through a REAL zlib (python3) end
   to end. This decoder is here because it runs everywhere `make test` runs,
   and because it can point at which field is wrong instead of just saying
   "not a PNG".
   ------------------------------------------------------------------------ */

static uint32_t bitwise_crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    return c ^ 0xFFFFFFFFu;
}

static uint32_t plain_adler32(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) { a = (a + p[i]) % 65521; b = (b + a) % 65521; }
    return (b << 16) | a;
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

typedef struct {
    int      w, h, depth, colour, interlace;
    uint8_t *raw;              /* the inflated scanline stream */
    size_t   raw_len;
    int      blocks;           /* stored blocks seen */
    int      final_flags;      /* how many carried BFINAL */
    int      bad;              /* first failure, as a small code; 0 = fine */
} decoded;

/* Codes, so a failure names its own field instead of "decode failed". */
enum { D_SIG = 1, D_CHUNK, D_CRC, D_IHDR, D_ZHDR, D_BTYPE, D_NLEN, D_ADLER,
       D_IEND, D_TRAILING, D_MEM };

static void decode(const uint8_t *f, size_t n, decoded *d)
{
    memset(d, 0, sizeof *d);
    static const uint8_t SIG[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    if (n < 8 || memcmp(f, SIG, 8) != 0) { d->bad = D_SIG; return; }

    size_t   i = 8;
    uint8_t *z = NULL;              /* the concatenated IDAT payloads */
    size_t   zn = 0;
    int      saw_iend = 0;

    while (i + 12 <= n) {
        uint32_t len = be32(f + i);
        if (len > n || i + 12 + len > n) { d->bad = D_CHUNK; free(z); return; }
        const uint8_t *type = f + i + 4;
        const uint8_t *data = type + 4;
        uint32_t crc = be32(data + len);
        /* Over TYPE and DATA together -- the field src/png.c's comment warns
           about getting wrong. */
        if (bitwise_crc32(type, 4 + len) != crc) { d->bad = D_CRC; free(z); return; }

        if (!memcmp(type, "IHDR", 4)) {
            if (len != 13) { d->bad = D_IHDR; free(z); return; }
            d->w = (int)be32(data);
            d->h = (int)be32(data + 4);
            d->depth = data[8]; d->colour = data[9];
            if (data[10] != 0 || data[11] != 0) { d->bad = D_IHDR; free(z); return; }
            d->interlace = data[12];
        } else if (!memcmp(type, "IDAT", 4)) {
            uint8_t *nz = realloc(z, zn + len);
            if (!nz) { d->bad = D_MEM; free(z); return; }
            z = nz; memcpy(z + zn, data, len); zn += len;
        } else if (!memcmp(type, "IEND", 4)) {
            saw_iend = 1;
            i += 12 + len;
            break;
        }
        i += 12 + len;
    }
    if (!saw_iend) { d->bad = D_IEND; free(z); return; }
    if (i != n)    { d->bad = D_TRAILING; free(z); return; }

    /* ---- the zlib stream. Header: CM must be 8, and the two bytes must read
       as a big-endian multiple of 31, which is the check every real decoder
       makes and the one a hand-written header most easily fails. */
    if (zn < 6 || (z[0] & 0x0F) != 8 || ((z[0] << 8) | z[1]) % 31 != 0 ||
        (z[1] & 0x20)) { d->bad = D_ZHDR; free(z); return; }

    uint8_t *raw = NULL;
    size_t   rn = 0;
    size_t   p = 2;
    int      done = 0;
    while (!done && p + 5 <= zn - 4) {
        uint8_t hdr = z[p];
        if ((hdr >> 1) & 3) { d->bad = D_BTYPE; free(z); free(raw); return; }
        if (hdr & 1) { done = 1; d->final_flags++; }
        size_t blen  = (size_t)z[p+1] | ((size_t)z[p+2] << 8);
        size_t nlen  = (size_t)z[p+3] | ((size_t)z[p+4] << 8);
        if ((blen ^ 0xFFFF) != nlen) { d->bad = D_NLEN; free(z); free(raw); return; }
        p += 5;
        if (p + blen > zn - 4) { d->bad = D_CHUNK; free(z); free(raw); return; }
        uint8_t *nr = realloc(raw, rn + blen);
        if (!nr) { d->bad = D_MEM; free(z); free(raw); return; }
        raw = nr; memcpy(raw + rn, z + p, blen); rn += blen;
        p += blen;
        d->blocks++;
    }
    if (!done || p != zn - 4) { d->bad = D_CHUNK; free(z); free(raw); return; }
    if (plain_adler32(raw, rn) != be32(z + zn - 4)) {
        d->bad = D_ADLER; free(z); free(raw); return;
    }
    free(z);
    d->raw = raw; d->raw_len = rn;
}

/* Pixel (x,y) out of the inflated stream, given the filter byte per row. */
static int px(const decoded *d, int x, int y)
{
    size_t row = (size_t)y * ((size_t)d->w + 1);
    return d->raw[row + 1 + (size_t)x];
}

TEST_MAIN({
    /* ---------------------------------------------------------- 1. sizing */
    /* A refused geometry returns 0 rather than a size a caller would then
       malloc and hand to the encoder. */
    CHECK_EQ_INT(png_gray8_size(0, 10), 0);
    CHECK_EQ_INT(png_gray8_size(10, 0), 0);
    CHECK_EQ_INT(png_gray8_size(-1, 10), 0);
    CHECK_EQ_INT(png_gray8_size(70000, 10), 0);

    /* The exact arithmetic, spelled out here so a change to the framing has
       to be deliberate: 8 signature + 25 IHDR + 12 IDAT framing + zlib
       (2 header + 5 per stored block + raw + 4 adler) + 12 IEND. */
    {
        size_t raw = (size_t)(4 + 1) * 3;
        CHECK_EQ_INT(png_gray8_size(4, 3), 8 + 25 + 12 + (2 + 5 + raw + 4) + 12);
    }

    /* ------------------------------------------- 2. a small image decodes */
    {
        enum { W = 7, H = 5 };
        uint8_t pix[W * H];
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) pix[y * W + x] = (uint8_t)(x * 30 + y);

        size_t need = png_gray8_size(W, H);
        uint8_t *buf = malloc(need);
        CHECK(buf != NULL);
        CHECK_EQ_INT(png_encode_gray8(buf, need, pix, W, W, H), need);

        decoded d;
        decode(buf, need, &d);
        CHECK_EQ_INT(d.bad, 0);
        CHECK_EQ_INT(d.w, W);
        CHECK_EQ_INT(d.h, H);
        CHECK_EQ_INT(d.depth, 8);
        CHECK_EQ_INT(d.colour, 0);          /* greyscale */
        CHECK_EQ_INT(d.interlace, 0);
        CHECK_EQ_INT(d.raw_len, (size_t)(W + 1) * H);
        CHECK_EQ_INT(d.final_flags, 1);     /* exactly one block is final */

        /* Every filter byte is 0 ("none"). A stream whose filter bytes were
           anything else would decode to different pixels through a real
           decoder while these raw offsets still looked right. */
        int filters_ok = 1;
        for (int y = 0; y < H; y++)
            if (d.raw[(size_t)y * (W + 1)] != 0) filters_ok = 0;
        CHECK(filters_ok);

        int pixels_ok = 1;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if (px(&d, x, y) != pix[y * W + x]) pixels_ok = 0;
        CHECK(pixels_ok);
        free(d.raw);
        free(buf);
    }

    /* -------------------------------- 3. a stride wider than the image */
    /* The capture path hands the encoder a buffer whose stride may exceed
       its width (the panel's), so the row pitch has to be honoured -- an
       encoder that ignored it would emit the padding as pixels and shear
       the picture one column further every row. */
    {
        enum { W = 5, H = 4, STRIDE = 9 };
        uint8_t pix[STRIDE * H];
        memset(pix, 0xEE, sizeof pix);                 /* the padding */
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) pix[y * STRIDE + x] = (uint8_t)(10 + y * W + x);

        size_t need = png_gray8_size(W, H);
        uint8_t *buf = malloc(need);
        CHECK_EQ_INT(png_encode_gray8(buf, need, pix, STRIDE, W, H), need);
        decoded d;
        decode(buf, need, &d);
        CHECK_EQ_INT(d.bad, 0);
        int ok = 1, saw_padding = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                if (px(&d, x, y) != pix[y * STRIDE + x]) ok = 0;
                if (px(&d, x, y) == 0xEE) saw_padding = 1;
            }
        CHECK(ok);
        CHECK(!saw_padding);
        free(d.raw);
        free(buf);
    }

    /* --------------------- 4. MORE THAN ONE STORED BLOCK, the real panel */
    /* 65535 bytes is a stored block's ceiling, so anything panel-sized needs
       dozens of them -- and the block-splitting loop is the part of the
       encoder with somewhere to be wrong. This is the shipped panel's exact
       geometry, which is 33 blocks and a row that STRADDLES a boundary
       (1265 does not divide 65535). */
    {
        const int W = 1264, H = 1680;
        uint8_t *pix = malloc((size_t)W * H);
        CHECK(pix != NULL);
        for (size_t i = 0; i < (size_t)W * H; i++) pix[i] = (uint8_t)(i * 7 + (i >> 11));

        size_t need = png_gray8_size(W, H);
        uint8_t *buf = malloc(need);
        CHECK(buf != NULL);
        CHECK_EQ_INT(png_encode_gray8(buf, need, pix, W, W, H), need);

        decoded d;
        decode(buf, need, &d);
        CHECK_EQ_INT(d.bad, 0);
        CHECK_EQ_INT(d.w, W);
        CHECK_EQ_INT(d.h, H);
        CHECK(d.blocks > 1);
        CHECK_EQ_INT(d.final_flags, 1);     /* and only the LAST one */
        CHECK_EQ_INT(d.raw_len, (size_t)(W + 1) * H);

        int ok = 1;
        for (int y = 0; y < H && ok; y++)
            for (int x = 0; x < W; x++)
                if (px(&d, x, y) != pix[(size_t)y * W + x]) { ok = 0; break; }
        CHECK(ok);
        free(d.raw);
        free(buf);
        free(pix);
    }

    /* ------------------------------------------------ 5. refusals */
    {
        uint8_t pix[16];
        uint8_t out[4096];
        memset(pix, 1, sizeof pix);
        /* A stride narrower than the width would read the next row's pixels
           into this one; there is no correct image to emit, so it refuses. */
        CHECK_EQ_INT(png_encode_gray8(out, sizeof out, pix, 3, 4, 4), 0);
        /* A buffer one byte short of what png_gray8_size demands. */
        size_t need = png_gray8_size(4, 4);
        CHECK(need > 0 && need < sizeof out);
        CHECK_EQ_INT(png_encode_gray8(out, need - 1, pix, 4, 4, 4), 0);
        CHECK_EQ_INT(png_encode_gray8(NULL, need, pix, 4, 4, 4), 0);
        CHECK_EQ_INT(png_encode_gray8(out, need, NULL, 4, 4, 4), 0);
    }

    /* ------------------------------------- 6. png_write_gray8 hits a disk */
    {
        char dir[] = "/tmp/koboy_png_XXXXXX";
        CHECK(mkdtemp(dir) != NULL);
        char path[512];
        snprintf(path, sizeof path, "%s/out.png", dir);

        enum { W = 11, H = 9 };
        uint8_t pix[W * H];
        for (int i = 0; i < W * H; i++) pix[i] = (uint8_t)(255 - i);
        CHECK(png_write_gray8(path, pix, W, W, H));

        FILE *f = fopen(path, "rb");
        CHECK(f != NULL);
        if (f) {
            uint8_t got[4096];
            size_t n = fread(got, 1, sizeof got, f);
            fclose(f);
            CHECK_EQ_INT(n, png_gray8_size(W, H));
            decoded d;
            decode(got, n, &d);
            CHECK_EQ_INT(d.bad, 0);
            CHECK_EQ_INT(d.w, W);
            CHECK_EQ_INT(d.h, H);
            int ok = 1;
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++)
                    if (px(&d, x, y) != pix[y * W + x]) ok = 0;
            CHECK(ok);
            free(d.raw);
        }
        /* And the temp file safefile_write used is gone, not left beside it. */
        char tmp[600];
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        CHECK(access(tmp, F_OK) != 0);
        remove(path);
        rmdir(dir);
    }
})
