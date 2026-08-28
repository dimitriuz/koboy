#ifndef KOBOY_RECENT_H
#define KOBOY_RECENT_H
#include <stdbool.h>
#include <stddef.h>

/* A small most-recently-played list. The mutating logic (recent_touch) is
   PURE -- no filesystem -- so it is fully exercised on the host, and the two
   entry points that do touch disk are thin wrappers over safefile.c's
   atomic-write / all-or-nothing-read discipline. Losing this list is a
   convenience regression; a torn read feeding a half-written entry into a live
   list would be worse. */

#define KOBOY_RECENT_MAX     10   /* "10 is plenty" -- more than anyone picks
                                      between in one sitting, and it keeps
                                      both the on-disk file and the on-panel
                                      list small enough to not need paging */
#define KOBOY_RECENT_PATH    512  /* matches koboy_config.rom_path */
#define KOBOY_RECENT_DISPLAY 160  /* headroom over ROMLIST_NAME (128): the
                                      display string is DERIVED from the path
                                      (recent_name_from_path), not copied from
                                      romlist.c, so it is sized independently
                                      rather than depending on romlist.h */

typedef struct {
    char path[KOBOY_RECENT_PATH];       /* what core_load_rom actually opens */
    /* A CACHE of recent_name_from_path(path), never an independent value.
       Still a STORED field for one reason: the on-disk record is fixed-size and
       safefile_read_exact recognises a foreign file by length alone, so
       dropping it would invalidate every recent.dat already on a device.
       recent_load and recent_touch both recompute it. */
    char display[KOBOY_RECENT_DISPLAY];
} koboy_recent_entry;

typedef struct {
    koboy_recent_entry entries[KOBOY_RECENT_MAX];
    int count;
} koboy_recent;

void recent_init(koboy_recent *rc);

/* The row text for `path`: its last component. Always a valid C string,
   reading at most n-1 bytes; a path with no separator, or whose last component
   is empty ("roms/"), yields the whole path rather than a blank row.

   Public because it is the RULE, not a helper: a recent row's name is a
   function of its path and nothing else, matching what the browser shows. */
void recent_name_from_path(char *out, size_t n, const char *path);

/* Records `path` as just-played. PURE: no filesystem, so every branch is
   exercised on the host.

   THE ROW'S NAME IS NOT A PARAMETER, which fixes a bug that reached a device:
   the caller used to pass recent_display(&rc, ri) -- a pointer INTO the array
   this function shifts -- and the moved entry took its neighbour's name, so the
   list showed one title twice and the second row started a different game.
   Snapshotting the argument stopped new rows going wrong but left the two
   fields independent by construction, and could not heal the row already
   written wrong on the owner's device. Deriving the name from the path makes
   the divergence unrepresentable and repairs a bad row on the next load.

   Most-recent first. A `path` already present (compared byte for byte --
   resolving two spellings of one file is not this module's job) moves to the
   front rather than duplicating; a new one is inserted at the front, aging out
   the OLDEST once the list is full. */
void recent_touch(koboy_recent *rc, const char *path);

/* Loads from `file`. Anything short of a complete, well-formed read leaves
   `rc` EMPTY and returns false: a recents file is a convenience, never user
   data worth blocking startup over.

   Every row's display is RE-DERIVED from its path and the file's bytes for it
   are not read at all, which is what repairs a recent.dat already carrying a
   wrong name (see recent_touch). */
bool recent_load(koboy_recent *rc, const char *file);

/* Writes `rc` to `file` atomically (safefile_write): temp file, fsync,
   rename, so a kill mid-write cannot corrupt an existing recents file. */
bool recent_save(const koboy_recent *rc, const char *file);

/* Drops every entry whose `path` no longer opens, compacting survivors
   forward in order. ROMs get deleted and renamed, and without this a recent
   entry becomes a dead row that fails to load when tapped, with nothing on a
   terminal-less device to explain why. Kept separate from recent_load so a
   caller that wants the raw recorded list can still get one. */
void recent_prune_missing(koboy_recent *rc);

/* Always valid C strings; an out-of-range index yields "". */
const char *recent_path(const koboy_recent *rc, int i);
const char *recent_display(const koboy_recent *rc, int i);
#endif
