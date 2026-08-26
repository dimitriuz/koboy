#ifndef KOBOY_ROMLIST_H
#define KOBOY_ROMLIST_H
#include <stdbool.h>
#include <stddef.h>

/* ROMLIST_NAME holds a path RELATIVE TO rom_dir, not a bare filename: a ROM
   under a subdirectory ("gbc/Shantae (USA).gbc") stores that whole relative
   path, so romlist_path's dir+"/"+name join still lands on the right file,
   and two ROMs with the same basename in different subfolders read as two
   distinct rows instead of colliding. A relative path that would not fit is
   never truncated and stored -- romlist_path would then reconstruct a path
   to a file that does not exist, and the ROM would look present in the
   browser but silently fail to load, which is worse than not listing it. It
   is skipped instead and folds into `hidden` below, same as a scan that hit
   the hard cap. */
#define ROMLIST_NAME 128

/* Not a visible cap any more (see romlist_scan) -- a last-resort safety
   ceiling on how many ROMs a scan will ever STORE, applied AFTER the full
   scan is sorted, never during the walk. 20000 is roughly 66x the collection
   that exposed the original bug (303 ROMs against a cap of 256), so a real
   user should never see it; it exists so a pathological rom_dir cannot make
   koboy allocate without bound. Overridable via KOBOY_ROMLIST_CAP_TEST so a
   test can exercise the truncation-is-visible path without creating 20000
   files. */
#define ROMLIST_HARD_CAP 20000

/* Total directory entries (files AND directories, matches or not) the whole
   recursive walk will visit before it gives up, independent of
   ROMLIST_HARD_CAP: it bounds the WORK a scan can do even inside a directory
   full of things that are not ROMs. Overridable via
   KOBOY_ROMLIST_VISIT_CAP_TEST for the same testing reason as the cap above. */
#define ROMLIST_VISIT_CAP 50000

/* Recursion depth into subdirectories. Generous -- a sorted collection nests
   one or two levels ("roms/gbc/") -- and mainly a guard against a
   pathologically deep tree; the symlink check in romlist.c is what actually
   stops a cycle, this just bounds anything that dodges it. */
#define ROMLIST_MAX_DEPTH 8

/* names[] and item_ptr[] are heap-allocated and sized to the scan that
   produced them, growing with the collection instead of the fixed-256 array
   this struct used to be. item_ptr[] is an array of pointers INTO names[]
   (plus, when `hidden` is nonzero, one final pointer to overflow_msg),
   because ui_list_init wants `const char *const *` and a 2-D array is not
   that.

   Both arrays are REBUILT WHOLESALE by every call to romlist_scan on this
   struct -- freed and reallocated, not resized in place -- so nothing may
   cache item_ptr, or any pointer taken out of it, across a call to
   romlist_scan on the same koboy_romlist: the array it pointed into may
   already be freed by the time the next scan returns. */
typedef struct {
    char         dir[512];
    char       (*names)[ROMLIST_NAME];   /* heap, `count` entries */
    const char **item_ptr;               /* heap, `count` + (hidden>0) entries */
    char         overflow_msg[96];       /* the "+N MORE ROMS" row's text */
    int          count;                  /* real, loadable ROMs */
    int          hidden;                 /* found but not stored; 0 if none */
} koboy_romlist;

/* True for a name ending .gb or .gbc, either case. Pure, so the filter is
   tested without a filesystem. */
bool romlist_is_rom(const char *name);

/* Recursively scans `dir` (and every subdirectory under it, symlinks not
   followed) for ROMs, sorts the WHOLE result case-insensitively, and only
   then applies the hard cap -- so which ROMs survive a truncation is
   alphabetical, never an artifact of readdir order. Returns the number of
   rows the UI should show, i.e. rl->count plus one more if rl->hidden > 0;
   check rl->count, not this return value, for "are there any ROMs at all".
   Returns -1 if `dir` cannot be opened, which is a different answer from 0
   and must stay that way: "you have no ROMs" and "your rom_dir is wrong" are
   different diagnoses to a user with no terminal.

   PRECONDITION: `*rl` must be zero-initialised (`koboy_romlist rl = {0};` or
   equivalent) before the FIRST call on it. romlist_scan frees whatever
   names/item_ptr already point to before it scans, so a rescan of the same
   struct (a second call on the same `rl`) does not leak -- but on a struct
   that was never zeroed, names/item_ptr are indeterminate stack contents and
   freeing them is undefined behaviour. This mirrors romlist_free's own
   precondition below, for the same reason. */
int  romlist_scan(koboy_romlist *rl, const char *dir);

/* Releases the heap arrays romlist_scan allocated. Safe to call on a
   zero-initialised or already-freed koboy_romlist. */
void romlist_free(koboy_romlist *rl);

/* Always returns a valid C string; an out-of-range index yields "". */
const char *romlist_name(const koboy_romlist *rl, int i);

/* Writes dir/name into out. An out-of-range index writes an empty string. */
void romlist_path(const koboy_romlist *rl, int i, char *out, size_t n);

const char *const *romlist_items(const koboy_romlist *rl);

/* True if index `i` names an actual ROM rather than the synthetic "+N MORE
   ROMS NOT SHOWN" row romlist_items appends when rl->hidden > 0. A caller
   that lets the user pick a row (the ROM browser) must check this before
   treating the pick as a ROM to load -- selecting the overflow row must not
   try to open a file that does not exist. */
bool romlist_is_real(const koboy_romlist *rl, int i);
#endif
