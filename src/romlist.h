#ifndef KOBOY_ROMLIST_H
#define KOBOY_ROMLIST_H
#include <stdbool.h>
#include <stddef.h>

/* ROMLIST_NAME holds ONE directory entry's own name -- "Tetris (World).gb",
   or "Game and Watch/" for a subdirectory row -- not a path. It used to hold
   a path relative to rom_dir, because the scan was recursive and flattened
   every subdirectory into one list; the browser now shows one directory at a
   time (see romlist_scan), so a row's text is a single name again and the
   path is reconstructed from rl->dir, which tracks where the user has
   navigated to.

   A name that would not fit is never truncated and stored -- romlist_path
   would then reconstruct a path to a file that does not exist, and the ROM
   would look present in the browser but silently fail to load, which is
   worse than not listing it. It is skipped instead and folds into `hidden`
   below, same as a scan that hit the hard cap. 128 was generous for a
   relative path and is now enormous for a bare name; it stays because
   nothing gains from shrinking it. */
#define ROMLIST_NAME 128

/* Not a visible cap any more (see romlist_scan) -- a last-resort safety
   ceiling on how many entries a scan will ever STORE, applied AFTER the full
   listing is sorted, never during the walk. 20000 is roughly 66x the
   collection that exposed the original bug (303 ROMs against a cap of 256),
   so a real user should never see it; it exists so a pathological rom_dir
   cannot make koboy allocate without bound. Overridable via
   KOBOY_ROMLIST_CAP_TEST so a test can exercise the truncation-is-visible
   path without creating 20000 files. */
#define ROMLIST_HARD_CAP 20000

/* Total directory entries (files AND directories, matches or not) one
   listing will visit before it gives up, independent of ROMLIST_HARD_CAP: it
   bounds the WORK a scan can do even inside a directory full of things that
   are not ROMs. Overridable via KOBOY_ROMLIST_VISIT_CAP_TEST for the same
   testing reason as the cap above. */
#define ROMLIST_VISIT_CAP 50000

/* How deep romlist_enter will let the user descend below rom_dir. Generous --
   a sorted collection nests one or two levels ("roms/Game and Watch/") -- and
   mainly a guard against a pathologically deep tree. It bounded the recursive
   walk when the scan was recursive; with one-directory-at-a-time listing it
   bounds navigation instead, which is the same ceiling reached one tap at a
   time. The symlink check in romlist.c is still what actually stops a cycle. */
#define ROMLIST_MAX_DEPTH 8

/* What a row IS. The numeric order is load-bearing: entry_cmp in romlist.c
   sorts by kind first, so this enum's order IS the row order -- ".." at the
   top, then subdirectories, then ROMs, each group alphabetical. Reordering
   these values reorders the browser. */
typedef enum {
    ROMLIST_UP = 0,    /* the ".." row; present in every directory but the root */
    ROMLIST_DIR,       /* a subdirectory: selecting it descends (romlist_enter) */
    ROMLIST_ROM,       /* a loadable file: selecting it loads (romlist_path) */
    ROMLIST_OVERFLOW   /* the synthetic "+N MORE" row; not a file, not selectable */
} koboy_romlist_kind;

typedef struct {
    char name[ROMLIST_NAME];  /* the row's text; a directory keeps a trailing '/' */
    int  kind;                /* koboy_romlist_kind */
} koboy_romlist_entry;

/* ent[] and item_ptr[] are heap-allocated and sized to the listing that
   produced them. item_ptr[] is an array of pointers INTO ent[] (plus, when
   `hidden` is nonzero, one final pointer to overflow_msg), because
   ui_list_init wants `const char *const *` and an array of structs is not
   that.

   Both arrays are REBUILT WHOLESALE by every call that rescans -- romlist_scan,
   romlist_enter, romlist_up -- freed and reallocated, not resized in place --
   so nothing may cache item_ptr, or any pointer taken out of it, across such a
   call on the same koboy_romlist: the array it pointed into may already be
   freed by the time the call returns. The ROM browser rebuilds its
   koboy_ui_list after every navigation for exactly this reason. */
typedef struct {
    char                 root[512];  /* rom_dir; fixed for the life of a browse */
    char                 sub[512];   /* where we are, relative to root; "" at the root */
    char                 dir[1100];  /* root + "/" + sub: what romlist_path joins */
    koboy_romlist_entry *ent;        /* heap, `count` entries */
    const char         **item_ptr;   /* heap, `count` + (hidden>0) entries */
    char                 overflow_msg[96];  /* the "+N MORE ROMS" row's text */
    int                  count;      /* rows excluding the overflow row */
    int                  roms;       /* of those, how many are loadable ROMs */
    int                  hidden;     /* found but not stored; 0 if none */
} koboy_romlist;

/* True for a name ending .gb, .gbc or .mgw, any case. Pure, so the filter is
   tested without a filesystem. */
bool romlist_is_rom(const char *name);

/* Lists ONE directory -- `dir` itself, the root of a browse -- and does not
   recurse. Subdirectories become rows of their own (koboy_romlist_kind), so
   59 Game & Watch titles read as their own names under one "Game and Watch/"
   row instead of 59 rows sharing an identical 15-character prefix that the
   list widget's middle ellipsis then ate the actual title to make room for.

   Sorts the WHOLE listing (kind first, then case-insensitively by name) and
   only then applies the hard cap -- so which entries survive a truncation is
   alphabetical, never an artifact of readdir order. That ordering property is
   the original bug's fix and must survive any change here.

   Returns the number of rows the UI should show, i.e. rl->count plus one more
   if rl->hidden > 0; check rl->count (any rows at all) or rl->roms (loadable
   ones) rather than this return value. Returns -1 if `dir` cannot be opened,
   which is a different answer from 0 and must stay that way: "you have no
   ROMs" and "your rom_dir is wrong" are different diagnoses to a user with no
   terminal.

   PRECONDITION: `*rl` must be zero-initialised (`koboy_romlist rl = {0};` or
   equivalent) before the FIRST call on it. Every rescan frees whatever
   ent/item_ptr already point to before it scans, so a second call on the same
   `rl` does not leak -- but on a struct that was never zeroed, those pointers
   are indeterminate stack contents and freeing them is undefined behaviour.
   This mirrors romlist_free's own precondition below, for the same reason. */
int  romlist_scan(koboy_romlist *rl, const char *dir);

/* Descends into row `i` (which must be a ROMLIST_DIR row) and lists it,
   returning the new row count exactly as romlist_scan does. A row that is not
   a directory, a descent past ROMLIST_MAX_DEPTH, or a subdirectory path too
   long to store all leave the listing where it is and return its unchanged
   row count -- staying put is the only answer that keeps the browser usable,
   since the alternative is a screen showing a directory nobody navigated to.
   Returns -1 only if the directory could not be re-listed at all. */
int  romlist_enter(koboy_romlist *rl, int i);

/* Goes up one level and lists the parent, returning the new row count. At the
   root it is a no-op returning the current count: the browser's own caller
   decides what backing out of the root means (today: back to the MAIN MENU),
   and romlist has no business making that decision for it. */
int  romlist_up(koboy_romlist *rl);

/* Where the listing currently is, relative to the root: "" at the root,
   "Game and Watch" one level down. The browser's header is built from this. */
const char *romlist_subpath(const koboy_romlist *rl);

/* What row `i` is -- see koboy_romlist_kind. An out-of-range index reports
   ROMLIST_OVERFLOW, i.e. "not something you can act on", which is the safe
   answer for the one caller that derives an index from a touch. */
int  romlist_kind(const koboy_romlist *rl, int i);

/* Releases the heap arrays a scan allocated. Safe to call on a
   zero-initialised or already-freed koboy_romlist. */
void romlist_free(koboy_romlist *rl);

/* Always returns a valid C string; an out-of-range index yields "". */
const char *romlist_name(const koboy_romlist *rl, int i);

/* Writes the full path of ROM row `i` into out. An out-of-range index, or a
   row that is not a ROM, writes an empty string -- a "Game and Watch/" row
   names no file, and handing its path to the core would be a load failure
   with no explanation. */
void romlist_path(const koboy_romlist *rl, int i, char *out, size_t n);

const char *const *romlist_items(const koboy_romlist *rl);

/* True if index `i` names an actual row rather than the synthetic "+N MORE
   ROMS NOT SHOWN" row romlist_items appends when rl->hidden > 0. A caller
   that lets the user pick a row (the ROM browser) must check this, or use
   romlist_kind, before acting on the pick -- selecting the overflow row must
   not try to open a file that does not exist. */
bool romlist_is_real(const koboy_romlist *rl, int i);
#endif
