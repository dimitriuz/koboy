#ifndef KOBOY_SAFEFILE_H
#define KOBOY_SAFEFILE_H
#include <stdbool.h>
#include <stddef.h>

/* The two file operations koboy uses for user data, extracted from sram.c so
   save states inherit its discipline rather than reinventing a weaker version.
   Both save files and save states are the only data koboy owns, and e-readers
   get killed unceremoniously. */

/* Temp file, fsync, rename. A kill mid-write cannot corrupt an existing file. */
bool safefile_write(const char *path, const void *src, size_t len);

/* Fills dst only if the whole of it can be filled. NOTHING is written to dst
   otherwise -- see the long note in the implementation; this property is the
   entire point. A file LONGER than len is accepted, reading the first len
   bytes. */
bool safefile_read_exact(const char *path, void *dst, size_t len);
#endif
