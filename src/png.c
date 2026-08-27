#define _POSIX_C_SOURCE 200809L
#include "png.h"
#include "safefile.h"
#include <stdlib.h>
#include <string.h>

/* Contract, and the reason there is no zlib here: png.h. */

/* The largest edge this encoder accepts. PNG itself allows 2^31-1, but every
   image this writes is a panel capture, and a bound this small makes the size
   arithmetic below easy to reason about. The overflow check in
   png_gray8_size is still a real check and not a formality: the device is
   32-bit ARM, where 65535 rows of 65536 bytes does not fit in a size_t. */
#define PNG_MAX_EDGE 65535

/* One stored DEFLATE block cannot carry more than 65535 bytes: LEN is a
   16-bit field. Everything about the block count follows from this. */
#define PNG_STORED_MAX 65535u

static uint32_t crc_tbl[256];
static int      crc_ready;

static void crc_init(void)
{
    for (int n = 0; n < 256; n++) {
        uint32_t c = (uint32_t)n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_tbl[n] = c;
    }
    crc_ready = 1;
}

/* The PNG spec's CRC-32, run over the chunk TYPE and the chunk DATA together
   -- not over the data alone. That is the mistake this comment exists to
   prevent: it produces a file some viewers open anyway and libpng rejects,
   so it fails late and selectively, which is the worst way for a file format
   bug to present. */
static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    if (!crc_ready) crc_init();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = crc_tbl[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* Appends `n` bytes and folds them into the running Adler32 in one pass.
   ONE function for both jobs, because the checksum is over exactly the bytes
   that reach the file: computing it from a separate walk of the source is
   how an encoder ends up with a checksum for data it did not actually emit.
   5552 is the largest run that cannot overflow the 32-bit accumulators, and
   is the same constant zlib uses. */
static void emit(uint8_t **p, uint32_t *a, uint32_t *b,
                 const uint8_t *src, size_t n)
{
    memcpy(*p, src, n);
    *p += n;
    uint32_t A = *a, B = *b;
    while (n) {
        size_t k = n < 5552 ? n : 5552;
        for (size_t i = 0; i < k; i++) { A += src[i]; B += A; }
        A %= 65521; B %= 65521;
        src += k; n -= k;
    }
    *a = A; *b = B;
}

/* Number of stored blocks a `raw`-byte zlib stream needs. Zero bytes still
   needs ONE, because the final-block flag has to live somewhere: a stream
   with no block at all is truncated, not empty. png_gray8_size refuses a
   zero-pixel image anyway; this keeps the arithmetic total rather than
   depending on that. */
static size_t stored_blocks(size_t raw)
{
    return raw ? (raw + PNG_STORED_MAX - 1) / PNG_STORED_MAX : 1;
}

size_t png_gray8_size(int w, int h)
{
    if (w <= 0 || h <= 0 || w > PNG_MAX_EDGE || h > PNG_MAX_EDGE) return 0;

    /* One filter byte per row -- filter type 0, "none". PNG's per-row filters
       exist to make the data compress; with stored blocks there is nothing to
       help, so the cheapest correct choice is the right one. */
    size_t rowbytes = (size_t)w + 1;
    /* Done as a division, not as a product: on the shipped 32-bit device
       rowbytes * h really can wrap, and a wrapped size here would size the
       caller's buffer smaller than the encoder then writes. */
    if (rowbytes > (size_t)-1 / (size_t)h) return 0;
    size_t raw = rowbytes * (size_t)h;

    size_t nblk = stored_blocks(raw);
    /* 2 bytes of zlib header, 5 of block header each, the data, and the
       4-byte Adler32 trailer. */
    if (nblk > ((size_t)-1 - raw - 6) / 5) return 0;
    size_t zlen = 2 + nblk * 5 + raw + 4;

    /* 8 signature + IHDR (12 framing + 13 data) + IDAT (12 framing + zlen)
       + IEND (12 framing, no data). */
    if (zlen > (size_t)-1 - 57) return 0;
    return 8 + 25 + 12 + zlen + 12;
}

size_t png_encode_gray8(uint8_t *out, size_t outsz, const uint8_t *pix,
                        int stride, int w, int h)
{
    size_t need = png_gray8_size(w, h);
    if (!out || !pix || !need || outsz < need) return 0;
    /* A stride shorter than a row would pull the next row's pixels into this
       one; refusing is the only honest answer, because there is no correct
       image to produce from it. */
    if (stride < w) return 0;

    uint8_t *p = out;

    static const uint8_t SIG[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    memcpy(p, SIG, 8); p += 8;

    /* ---- IHDR. Bit depth 8, colour type 0 (greyscale), no interlace. */
    put32(p, 13); p += 4;
    uint8_t *ihdr = p;
    memcpy(p, "IHDR", 4); p += 4;
    put32(p, (uint32_t)w); p += 4;
    put32(p, (uint32_t)h); p += 4;
    *p++ = 8;      /* bit depth */
    *p++ = 0;      /* colour type: greyscale */
    *p++ = 0;      /* compression method: deflate, the only one defined */
    *p++ = 0;      /* filter method 0, the only one defined */
    *p++ = 0;      /* no interlace */
    put32(p, crc32_of(ihdr, 17)); p += 4;

    /* ---- IDAT. The length is written now because it is known now
       (png_gray8_size computed it), which keeps this a single forward pass:
       nothing seeks back to patch a length, so the length and the bytes that
       follow it cannot disagree. */
    size_t rowbytes = (size_t)w + 1;
    size_t raw  = rowbytes * (size_t)h;
    size_t nblk = stored_blocks(raw);
    size_t zlen = 2 + nblk * 5 + raw + 4;

    put32(p, (uint32_t)zlen); p += 4;
    uint8_t *idat = p;
    memcpy(p, "IDAT", 4); p += 4;

    /* zlib header: CM=8 (deflate), CINFO=7 (32K window), FDICT=0, FLEVEL=0,
       and FCHECK chosen so the two bytes read as a big-endian multiple of 31.
       0x78 0x01 is that pair -- 0x7801 = 31 * 991 -- and it is what every
       uncompressed zlib stream in the world starts with. Written directly
       rather than through emit(): the zlib header is NOT part of the data the
       Adler32 covers. */
    *p++ = 0x78; *p++ = 0x01;

    uint32_t adler_a = 1, adler_b = 0;
    static const uint8_t FILTER_NONE = 0;

    /* The scanlines, in stored blocks. Block boundaries deliberately ignore
       row boundaries: a block is a transport unit and the decoder simply
       concatenates them, so aligning the two would cost bytes and buy
       nothing. The consequence is that a row can straddle a block, which is
       why this walks the raw stream by OFFSET rather than by row. */
    size_t done = 0;
    while (done < raw) {
        size_t left = raw - done;
        size_t n = left > PNG_STORED_MAX ? PNG_STORED_MAX : left;
        /* BFINAL on the last block only, BTYPE 00 = stored. A stream whose
           last block is not final is truncated as far as a decoder is
           concerned; one that marks an early block final silently loses every
           row after it. */
        *p++ = (uint8_t)((done + n >= raw) ? 1 : 0);
        *p++ = (uint8_t)(n & 0xFF);
        *p++ = (uint8_t)((n >> 8) & 0xFF);
        /* NLEN is LEN's ones' complement, and decoders do check it. */
        *p++ = (uint8_t)(~n & 0xFF);
        *p++ = (uint8_t)((~n >> 8) & 0xFF);

        size_t emitted = 0;
        while (emitted < n) {
            size_t off   = done + emitted;      /* offset into the raw stream */
            size_t row   = off / rowbytes;
            size_t col   = off % rowbytes;      /* 0 == this row's filter byte */
            if (col == 0) {
                emit(&p, &adler_a, &adler_b, &FILTER_NONE, 1);
                emitted++;
                continue;
            }
            size_t chunk = n - emitted;
            if (chunk > rowbytes - col) chunk = rowbytes - col;
            /* A row at a time, not a byte at a time: 1264 memcpys per panel
               instead of 2.1 million loop iterations, on a 1 GHz ARM. */
            emit(&p, &adler_a, &adler_b,
                 pix + row * (size_t)stride + (col - 1), chunk);
            emitted += chunk;
        }
        done += n;
    }

    put32(p, (adler_b << 16) | adler_a); p += 4;
    put32(p, crc32_of(idat, (size_t)(p - idat))); p += 4;

    /* ---- IEND */
    put32(p, 0); p += 4;
    uint8_t *iend = p;
    memcpy(p, "IEND", 4); p += 4;
    put32(p, crc32_of(iend, 4)); p += 4;

    /* LIVE CHECK, not an assertion that compiles away. A mismatch means
       png_gray8_size and this function disagree, which is the one bug a
       caller that sized its buffer from the first cannot survive -- and by
       the time it is observable, the overrun has already happened. Returning
       0 makes the caller discard the buffer instead of writing a file whose
       length nobody can account for. */
    if ((size_t)(p - out) != need) return 0;
    return need;
}

bool png_write_gray8(const char *path, const uint8_t *pix,
                     int stride, int w, int h)
{
    size_t need = png_gray8_size(w, h);
    if (!path || !need) return false;
    uint8_t *buf = malloc(need);
    if (!buf) return false;
    size_t n = png_encode_gray8(buf, need, pix, stride, w, h);
    bool ok = (n == need) && safefile_write(path, buf, n);
    free(buf);
    return ok;
}
