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

void recent_touch(koboy_recent *rc, const char *path, const char *display)
{
    if (!rc || !path || !*path) return;

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
    snprintf(moved.display, sizeof moved.display, "%s", display ? display : path);
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

        char display[KOBOY_RECENT_DISPLAY];
        snprintf(display, sizeof display, "%.*s",
                (int)sizeof buf[n].display - 1, buf[n].display);

        snprintf(rc->entries[n].path, sizeof rc->entries[n].path, "%s", path);
        snprintf(rc->entries[n].display, sizeof rc->entries[n].display, "%s", display);
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
