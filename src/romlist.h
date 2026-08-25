#ifndef KOBOY_ROMLIST_H
#define KOBOY_ROMLIST_H
#include <stdbool.h>
#include <stddef.h>

#define ROMLIST_MAX  256
#define ROMLIST_NAME 128

/* names[] is an array of fixed-width buffers, and item_ptr[] is an array of
   pointers INTO it, because ui_list_init wants `const char *const *` and a
   2-D char array is not that. Kept together so the two cannot drift. */
typedef struct {
    char        dir[512];
    char        names[ROMLIST_MAX][ROMLIST_NAME];
    const char *item_ptr[ROMLIST_MAX];
    int         count;
} koboy_romlist;

/* True for a name ending .gb or .gbc, either case. Pure, so the filter is
   tested without a filesystem. */
bool romlist_is_rom(const char *name);

/* Scans `dir` for ROMs, sorted case-insensitively. Returns the count, or -1 if
   the directory cannot be opened -- which is a different answer from 0 and must
   stay that way: "you have no ROMs" and "your rom_dir is wrong" are different
   diagnoses to a user with no terminal. */
int  romlist_scan(koboy_romlist *rl, const char *dir);

/* Always returns a valid C string; an out-of-range index yields "". */
const char *romlist_name(const koboy_romlist *rl, int i);

/* Writes dir/name into out. An out-of-range index writes an empty string. */
void romlist_path(const koboy_romlist *rl, int i, char *out, size_t n);

const char *const *romlist_items(const koboy_romlist *rl);
#endif
