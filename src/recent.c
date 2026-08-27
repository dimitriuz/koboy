#define _POSIX_C_SOURCE 200809L
#include "recent.h"
#include "safefile.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void recent_init(koboy_recent *rc)
{
    memset(rc, 0, sizeof *rc);
}

void recent_name_from_path(char *out, size_t n, const char *path)
{
    if (!out || n == 0) return;
    out[0] = 0;
    if (!path) return;

    const char *slash = strrchr(path, '/');
    const char *name  = slash ? slash + 1 : path;
    /* A path ending in '/' has an empty last component. Falling back to the
       whole path keeps the row readable instead of blank -- a row you cannot
       read is a row you cannot avoid tapping. */
    if (!*name) name = path;
    /* Clipped by an EXPLICIT precision rather than by snprintf's own bound.
       Same result, but the destination (160 bytes) is smaller than the
       longest possible source (a 511-byte path with no separator), and this
       project ships at zero warnings -- a bare "%s" there is a
       -Wformat-truncation the compiler is right to raise and nobody can fix
       any other way. */
    snprintf(out, n, "%.*s", (int)n - 1, name);
}

void recent_touch(koboy_recent *rc, const char *path)
{
    if (!rc || !path || !*path) return;

    /* SNAPSHOT BEFORE TOUCHING rc, because a caller is allowed to pass a
       pointer INTO rc and one does: main.c re-touches a chosen recent entry
       with recent_path(&rc, ri), which points at rc->entries[ri].path, and
       the shifts below overwrite that slot with its neighbour's contents.
       Reading the argument afterwards would copy THE WRONG ROW -- which is
       exactly what happened to the display name this function no longer
       takes (recent.h). One buffer on the stack makes the aliasing question
       stop existing, which beats a comment telling every future caller not
       to do the obvious thing. */
    char path_copy[sizeof rc->entries[0].path];
    snprintf(path_copy, sizeof path_copy, "%s", path);
    path = path_copy;

    int found = -1;
    for (int i = 0; i < rc->count; i++)
        if (!strcmp(rc->entries[i].path, path)) { found = i; break; }

    koboy_recent_entry moved;
    if (found >= 0) {
        /* Already present: open a gap at slot 0 by shifting entries
           [0, found) down to [1, found] -- this is what "move found to the
           front" means without an actual remove-then-insert. */
        moved = rc->entries[found];
        for (int i = found; i > 0; i--) rc->entries[i] = rc->entries[i - 1];
    } else {
        memset(&moved, 0, sizeof moved);
        snprintf(moved.path, sizeof moved.path, "%s", path);
        /* New entry: shift everything down to make room at slot 0, dropping
           whatever falls off the end. `last` is the highest index that
           SURVIVES the shift -- either one past the current tail (the list
           grows by one) or the current last slot (the list is already full,
           so its current occupant -- the oldest entry -- is the one that
           gets overwritten and so ages out). */
        int last = rc->count < KOBOY_RECENT_MAX ? rc->count : KOBOY_RECENT_MAX - 1;
        for (int i = last; i > 0; i--) rc->entries[i] = rc->entries[i - 1];
        if (rc->count < KOBOY_RECENT_MAX) rc->count++;
    }
    /* Derived, never carried: see recent.h. Recomputed for an entry that was
       already present too, so a row loaded from an older, wrong recent.dat is
       corrected by playing it as well as by loading it. */
    recent_name_from_path(moved.display, sizeof moved.display, path);
    rc->entries[0] = moved;
}

bool recent_save(const koboy_recent *rc, const char *file)
{
    if (!rc || !file) return false;
    /* Always the full KOBOY_RECENT_MAX-entry blob, unused tail entries
       zeroed (their path[0] == 0 is what recent_load's loop stops at) --
       never a variable-length write. That is what lets recent_load ask
       safefile_read_exact for exactly this many bytes: a length mismatch
       alone is then enough to recognise a corrupt or foreign file, with no
       separate framing/version byte needed. */
    koboy_recent_entry buf[KOBOY_RECENT_MAX];
    memset(buf, 0, sizeof buf);
    int n = rc->count < KOBOY_RECENT_MAX ? rc->count : KOBOY_RECENT_MAX;
    for (int i = 0; i < n; i++) buf[i] = rc->entries[i];
    return safefile_write(file, buf, sizeof buf);
}

bool recent_load(koboy_recent *rc, const char *file)
{
    recent_init(rc);
    if (!rc || !file) return false;

    koboy_recent_entry buf[KOBOY_RECENT_MAX];
    /* All-or-nothing: a missing file, a short read (an old format, a
       mid-write kill despite safefile's rename discipline, anything) or an
       oversized one all fail here, and rc stays the empty list recent_init
       just set -- exactly the "corrupt or missing degrades to empty, never
       a failure to start" contract in recent.h. */
    if (!safefile_read_exact(file, buf, sizeof buf)) return false;

    int n = 0;
    for (; n < KOBOY_RECENT_MAX; n++) {
        /* Bounded by PRECISION, not by trusting a NUL terminator to exist in
           the file's bytes: "%.*s" is guaranteed by the C standard to read at
           most (sizeof field - 1) bytes from the source, null or no null, so
           a maximally corrupt recents file -- every byte of `buf` non-zero,
           no terminator anywhere in it -- still cannot walk this read past
           `buf`'s own end. A plain "%s"/strcpy on `buf[n].path` directly has
           no such guarantee and needs a NUL within the array to be
           well-defined AT ALL.
           This specific guard, like text_pixel_visible (text.h) and
           chrome_bands (chrome.c), cannot be proven by trying to observe
           what happens without it: `buf` is a local array, so an unbounded
           scan that finds a NUL anywhere else inside it (a neighbouring
           field, a later entry) stays in-bounds and reads WRONG data rather
           than crashing, and it only becomes true undefined behaviour for a
           file so corrupt that no zero byte survives anywhere in `buf` at
           all -- which a test cannot safely construct to prove the point,
           per this project's own "a guard that only fails via UB is not a
           guard" rule. tests/test_recent.c exercises this as a graceful-
           degradation check (a non-terminated field must not crash the load
           and must come back bounded), not as a test that distinguishes this
           line from its absence -- it cannot, for the reason above. */
        char path[KOBOY_RECENT_PATH];
        snprintf(path, sizeof path, "%.*s",
                (int)sizeof buf[n].path - 1, buf[n].path);
        if (!path[0]) break;              /* first empty path ends the list */

        snprintf(rc->entries[n].path, sizeof rc->entries[n].path, "%s", path);
        /* The file's own display bytes are NEVER read. They are a cache of
           this exact derivation (recent.h), and a recent.dat written before
           that was true can hold a name belonging to a different game -- one
           such row is on the author's device. Deriving here is what repairs
           it, and it is also why nothing needs a bounded read of
           buf[n].display: the field is not an input. */
        recent_name_from_path(rc->entries[n].display,
                              sizeof rc->entries[n].display, path);
    }
    rc->count = n;
    return true;
}

void recent_prune_missing(koboy_recent *rc)
{
    if (!rc) return;
    int w = 0;
    for (int i = 0; i < rc->count; i++) {
        /* F_OK, not R_OK: a file that exists but is unreadable is a
           different, rarer problem (permissions), and treating it the same
           as "gone" here would silently drop a row main.c's own ROM-open
           path is about to report a much clearer error for anyway. */
        if (access(rc->entries[i].path, F_OK) != 0) continue;
        if (w != i) rc->entries[w] = rc->entries[i];
        w++;
    }
    rc->count = w;
}

const char *recent_path(const koboy_recent *rc, int i)
{
    if (!rc || i < 0 || i >= rc->count) return "";
    return rc->entries[i].path;
}

const char *recent_display(const koboy_recent *rc, int i)
{
    if (!rc || i < 0 || i >= rc->count) return "";
    return rc->entries[i].display;
}
