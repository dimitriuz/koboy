#ifndef KOBOY_PNG_H
#define KOBOY_PNG_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A PNG writer for one thing only: an 8-bit greyscale panel capture.
 *
 * NOT A LIBRARY, because the dependency ceiling is libc/libm/libdl
 * (scripts/verify-core.sh enforces it), so there is no zlib on the device.
 * A screenshot still has to be a PNG rather than the .pgm the goldens use:
 * these files go into README.md and onto the web.
 *
 * WHY THAT IS CHEAP: DEFLATE has a STORED block type (BTYPE 00, "here are N
 * literal bytes"), and a zlib stream made entirely of stored blocks is legal
 * and universally accepted. The whole compressor is a length, its complement
 * and a memcpy; the only arithmetic is CRC32 and Adler32.
 * NO COMPRESSOR IS INVENTED HERE, and none should be: a home-grown Huffman
 * coder is a decoder-compatibility bug waiting to happen, and it would buy
 * disk space on a 24 GB device.
 *
 * COST: a 1264x1680 panel is 2,125,200 bytes plus ~200 of framing.
 */

/* Exact size png_encode_gray8 will emit for `w` x `h`, or 0 if the geometry
   is refused. Exposed so a caller can size its buffer without guessing, and
   so the size arithmetic (which is where an encoder like this overflows) can
   be asserted on its own. */
size_t png_gray8_size(int w, int h);

/* Encodes `w` x `h` gray8 pixels read at `stride` bytes per row into `out`.
   Returns the number of bytes written, or 0 if anything was refused -- a
   NULL argument, a non-positive or absurd dimension, or an `outsz` smaller
   than png_gray8_size says. On 0, `out` may have been partially written and
   means nothing; the caller must not keep it. */
size_t png_encode_gray8(uint8_t *out, size_t outsz, const uint8_t *pix,
                        int stride, int w, int h);

/* Encode, then write through safefile_write -- temp file, fsync, rename.
   The atomic write is not ceremony here: an e-reader is killed
   unceremoniously and loses power, and a half-written screenshot that still
   has a .png name is worse than no screenshot, because it looks like one. */
bool png_write_gray8(const char *path, const uint8_t *pix,
                     int stride, int w, int h);
#endif
