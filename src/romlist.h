#ifndef KOBOY_ROMLIST_H
#define KOBOY_ROMLIST_H
#include <stdbool.h>
#include <stddef.h>

/* ROMLIST_NAME holds ONE directory entry's own name -- "Tetris (World).gb",
   or "Game and Watch/" for a subdirectory row -- not a path. The path is
   reconstructed from rl->dir, which tracks where the user navigated.

   A name that would not fit is NEVER truncated and stored: romlist_path would
   reconstruct a path to a file that does not exist, so the ROM would look
   present and silently fail to load, which is worse than not listing it. It is
   skipped and folds into `hidden`, like a scan that hit the hard cap. */
#define ROMLIST_NAME 128

/* A last-resort ceiling on entries a scan will ever STORE, applied AFTER the
   full listing is sorted, NEVER during the walk. 20000 is ~66x the collection
   that exposed the original bug (303 ROMs against a cap of 256), so a real
   user should never see it -- it exists so a pathological rom_dir cannot make
   koboy allocate without bound. KOBOY_ROMLIST_CAP_TEST overrides it so a test
   can exercise the truncation path without creating 20000 files. */
#define ROMLIST_HARD_CAP 20000

/* Directory entries (matches or not) one listing visits before giving up,
   independent of ROMLIST_HARD_CAP: it bounds the WORK a scan can do inside a
   directory full of things that are not ROMs.
   KOBOY_ROMLIST_VISIT_CAP_TEST overrides it. */
#define ROMLIST_VISIT_CAP 50000

/* How deep romlist_enter lets the user descend below rom_dir. Generous -- a
   sorted collection nests one or two levels -- and mainly a guard against a
   pathologically deep tree. THE SYMLINK CHECK IN romlist.c is what actually
   stops a cycle. */
#define ROMLIST_MAX_DEPTH 8

/* What a row IS. THE NUMERIC ORDER IS LOAD-BEARING: entry_cmp sorts by kind
   first, so this enum's order IS the row order -- ".." on top, then
   subdirectories, then ROMs, each group alphabetical. */
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
   produced them. item_ptr[] holds pointers INTO ent[] (plus, when `hidden` is
   nonzero, one to overflow_msg), because ui_list_init wants
   `const char *const *`.

   BOTH ARE REBUILT WHOLESALE by every rescan -- freed and reallocated, not
   resized -- so NOTHING may cache item_ptr or a pointer out of it across
   romlist_scan/enter/up: the array may already be freed when the call returns.
   The ROM browser rebuilds its koboy_ui_list after every navigation for this
   reason. */
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

/* True for a name ending in one of the shipped ROM extensions, any case (the
   allowlist is in romlist.c and must stay in step with
   config_core_for_rom's table). Pure, so the filter is tested without a
   filesystem. */
bool romlist_is_rom(const char *name);

/* Lists ONE directory and does NOT recurse. Subdirectories become rows of
   their own, so 59 Game & Watch titles read as their own names under one
   "Game and Watch/" row instead of 59 rows sharing a 15-character prefix that
   the list widget's middle ellipsis then ate the title to make room for.

   SORTS THE WHOLE LISTING (kind first, then case-insensitively by name) and
   only THEN applies the hard cap, so which entries survive a truncation is
   alphabetical, never an artifact of readdir order. That ordering is the
   original bug's fix and must survive any change here.

   Returns rows the UI should show (rl->count, plus one if rl->hidden > 0);
   check rl->count or rl->roms rather than this. Returns -1 if `dir` cannot be
   opened, which MUST stay distinct from 0: "you have no ROMs" and "your
   rom_dir is wrong" are different diagnoses to a user with no terminal.

   PRECONDITION: `*rl` must be zero-initialised before the FIRST call. Every
   rescan frees whatever ent/item_ptr point to, so on a struct that was never
   zeroed those are indeterminate stack contents and freeing them is UB. */
int  romlist_scan(koboy_romlist *rl, const char *dir);

/* Descends into ROMLIST_DIR row `i` and lists it, returning the new row count
   as romlist_scan does. A non-directory row, a descent past
   ROMLIST_MAX_DEPTH, or an over-long subdirectory path all STAY PUT and return
   the unchanged count -- the alternative is a screen showing a directory
   nobody navigated to. Returns -1 only if the directory could not be
   re-listed. */
int  romlist_enter(koboy_romlist *rl, int i);

/* Goes up one level and lists the parent, returning the new row count. At the
   root, a no-op returning the current count: the caller decides what backing
   out of the root means. */
int  romlist_up(koboy_romlist *rl);

/* Where the listing currently is, relative to the root: "" at the root,
   "Game and Watch" one level down. The browser's header is built from this. */
const char *romlist_subpath(const koboy_romlist *rl);

/* What row `i` is. An out-of-range index reports ROMLIST_OVERFLOW -- "not
   something you can act on" -- the safe answer for a caller deriving an index
   from a touch. */
int  romlist_kind(const koboy_romlist *rl, int i);

/* Releases the heap arrays a scan allocated. Safe to call on a
   zero-initialised or already-freed koboy_romlist. */
void romlist_free(koboy_romlist *rl);

/* Always returns a valid C string; an out-of-range index yields "". */
const char *romlist_name(const koboy_romlist *rl, int i);

/* Writes the full path of ROM row `i`. An out-of-range index or a non-ROM row
   writes "" -- a directory row names no file, and handing its path to the core
   would be a load failure with no explanation. */
void romlist_path(const koboy_romlist *rl, int i, char *out, size_t n);

const char *const *romlist_items(const koboy_romlist *rl);

/* True if `i` names an actual row rather than the synthetic "+N MORE" row
   romlist_items appends when rl->hidden > 0. A caller that lets the user pick
   MUST check this (or romlist_kind) before acting: selecting the overflow row
   must not try to open a file that does not exist. */
bool romlist_is_real(const koboy_romlist *rl, int i);
#endif
