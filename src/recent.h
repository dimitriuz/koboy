#ifndef KOBOY_RECENT_H
#define KOBOY_RECENT_H
#include <stdbool.h>
#include <stddef.h>

/* A small most-recently-played list, in the same spirit as romlist.c: the
   mutating logic (recent_touch) is pure -- no filesystem -- so it is fully
   exercised on the host the way romlist_is_rom is, and the two entry points
   that DO touch disk (recent_load/recent_save) are thin wrappers over
   safefile.c, the same atomic-write/all-or-nothing-read discipline save
   states use. Losing this list is a convenience regression; a torn read
   feeding a half-written entry back into a live list would be worse, which
   is exactly what safefile.c exists to rule out. */

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
    /* What the picker draws for this row -- a CACHE of
       recent_name_from_path(path), never an independent value. It is still a
       stored field for one reason only: the on-disk record is fixed-size and
       safefile_read_exact recognises a foreign file by its length alone
       (recent_save's comment), so dropping the field would invalidate every
       recent.dat already on a device. recent_load and recent_touch both
       recompute it, so nothing downstream ever reads a byte of it that this
       build did not derive. */
    char display[KOBOY_RECENT_DISPLAY];
} koboy_recent_entry;

typedef struct {
    koboy_recent_entry entries[KOBOY_RECENT_MAX];
    int count;
} koboy_recent;

void recent_init(koboy_recent *rc);

/* The row text for `path`: its last component, i.e. the file's own name.
   Always writes a valid C string, and reads at most n-1 bytes of it; a path
   with no separator, or one whose last component is empty ("roms/"), yields
   the whole path rather than a blank row.

   Public because it is the RULE, not a helper: a recent row's name is a
   function of its path and of nothing else. It matches what the browser
   shows, which since the one-directory-at-a-time rewrite is a bare filename
   too (romlist.h's ROMLIST_NAME comment). */
void recent_name_from_path(char *out, size_t n, const char *path);

/* Records `path` as just-played. PURE: no filesystem, so every branch below
   is exercised directly on the host.

   THE ROW'S NAME IS NOT A PARAMETER, and that is the fix for a bug that
   reached a device: the caller used to pass one, main.c passed
   recent_display(&rc, ri) -- a pointer INTO the array this function shifts
   -- and the moved entry took its neighbour's name, so the list showed one
   title twice and the second row started a different game. Snapshotting the
   argument (the first fix) stopped new rows going wrong but left the two
   fields independent by construction, so nothing could stop them diverging
   again and the row already written wrong on the owner's device could never
   repair itself. Deriving the name from the path makes the divergence
   unrepresentable instead, and heals a bad row the next time it is loaded.

   Most-recent first. A `path` already present (compared byte-for-byte --
   resolving two differently-spelled paths to the same file is not this
   module's job) moves to the front instead of duplicating. A `path` not
   already present is inserted at the front, aging out the OLDEST entry once
   the list is at KOBOY_RECENT_MAX -- the newest entries are the entire point
   of a recent list, so the tail is what gives way. */
void recent_touch(koboy_recent *rc, const char *path);

/* Loads from `file`. On anything short of a complete, well-formed read --
   missing file, wrong size, a partial write -- `rc` comes back EMPTY and
   this returns false. There is no "failed to start" path here at all: a
   recents file is a convenience, never user data worth blocking startup
   over, so degrading to an empty list is the only failure mode.

   Every row's display is RE-DERIVED from its path here, and the bytes the
   file holds for it are not read at all. That is what repairs a recent.dat
   already carrying a wrong name (see recent_touch): the list is right the
   next time it is opened, without the user having to select the bad row to
   heal it. */
bool recent_load(koboy_recent *rc, const char *file);

/* Writes `rc` to `file` atomically (safefile_write): temp file, fsync,
   rename, so a kill mid-write cannot corrupt an existing recents file. */
bool recent_save(const koboy_recent *rc, const char *file);

/* Drops every entry whose `path` no longer opens, compacting survivors
   forward and preserving their relative order. ROMs get deleted and
   renamed; without this a recent entry becomes a dead row that fails to
   load when tapped, with nothing on a device with no terminal to explain
   why. The one entry point besides load/save that touches the filesystem,
   kept separate from recent_load on purpose: a caller that wants the raw
   recorded list (a settings/debug view, say) can still get one. */
void recent_prune_missing(koboy_recent *rc);

/* Always valid C strings; an out-of-range index yields "". */
const char *recent_path(const koboy_recent *rc, int i);
const char *recent_display(const koboy_recent *rc, int i);
#endif
