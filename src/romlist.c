#define _POSIX_C_SOURCE 200809L
#include "romlist.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Own case-insensitive compare rather than strcasecmp: this project keeps its
   portable code free of anything the host might not have, and the comparison
   is four lines. */
static int ci_cmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int ca = (*a >= 'a' && *a <= 'z') ? *a - 32 : *a;
        int cb = (*b >= 'a' && *b <= 'z') ? *b - 32 : *b;
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
    }
}

static bool ends_with_ci(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    /* Guard is live: readdir returns names shorter than the suffix (".", ".."),
       and an unguarded s + ls - lx would read before the string. */
    if (lx > ls) return false;
    return ci_cmp(s + ls - lx, suffix) == 0;
}

bool romlist_is_rom(const char *name)
{
    if (!name || !*name) return false;
    return ends_with_ci(name, ".gb") || ends_with_ci(name, ".gbc");
}

static int name_cmp(const void *a, const void *b)
{
    return ci_cmp((const char *)a, (const char *)b);
}

/* ---------------------------------------------------------- test overrides
   Both caps default to values no real ROM collection should reach (see
   romlist.h), which makes them impractical to exercise honestly in a test
   that has to actually create that many files. These let a test dial a cap
   down to something a tmpdir can hold in milliseconds while leaving the
   shipped defaults alone. Read once per scan, not cached: cheap, and it
   means a test can flip the env var between two romlist_scan calls. */
static int env_or_default(const char *var, int fallback)
{
    const char *s = getenv(var);
    if (!s) return fallback;
    int v = atoi(s);
    return v > 0 ? v : fallback;
}

static int hard_cap(void)
{
    return env_or_default("KOBOY_ROMLIST_CAP_TEST", ROMLIST_HARD_CAP);
}

static int visit_cap(void)
{
    return env_or_default("KOBOY_ROMLIST_VISIT_CAP_TEST", ROMLIST_VISIT_CAP);
}

/* ------------------------------------------------------------- the walk
   Collects every ROM under `dir`, recursively, into a growable buffer. The
   cap is NOT applied here: applying it during the walk is exactly the
   original bug (readdir order decides who gets dropped). It is applied once,
   in romlist_scan, after the whole collection is sorted. */
typedef struct {
    char (*names)[ROMLIST_NAME];
    int   count;
    int   cap;          /* allocated capacity, not the display cap */
    int   visited;
    int   visit_limit;
    bool  oom;
    int   hidden;        /* oversized names / allocation failures, so far */
} scan_acc;

static void acc_push(scan_acc *acc, const char *relpath)
{
    if (acc->oom) { acc->hidden++; return; }
    if (strlen(relpath) >= ROMLIST_NAME) {
        /* Never truncate and store: a truncated relative path would
           round-trip through romlist_path into a file that does not exist,
           so the ROM would look present in the browser but fail to load --
           worse than leaving it out. */
        acc->hidden++;
        fprintf(stderr, "koboy: rom path too long (>= %d bytes), skipping: %s\n",
                ROMLIST_NAME, relpath);
        return;
    }
    if (acc->count >= acc->cap) {
        int newcap = acc->cap ? acc->cap * 2 : 64;
        char (*grown)[ROMLIST_NAME] =
            realloc(acc->names, (size_t)newcap * sizeof *grown);
        if (!grown) {
            /* Keep what was already collected; stop growing rather than
               retrying a realloc that already failed once per remaining
               file. */
            acc->oom = true;
            acc->hidden++;
            return;
        }
        acc->names = grown;
        acc->cap = newcap;
    }
    memcpy(acc->names[acc->count], relpath, strlen(relpath) + 1);
    acc->count++;
}

static void scan_walk(scan_acc *acc, const char *base_dir,
                      const char *rel_prefix, int depth)
{
    /* Checked before opendir, not just inside the loop below: once the visit
       budget is spent, every recursive call still pending on the stack
       returns immediately instead of opening one more directory each. */
    if (acc->visited >= acc->visit_limit) return;
    if (depth > ROMLIST_MAX_DEPTH) return;

    DIR *d = opendir(base_dir);
    if (!d) return;   /* an unreadable subdirectory just contributes nothing */

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (acc->visited++ >= acc->visit_limit) break;

        char full[4096];
        snprintf(full, sizeof full, "%s/%s", base_dir, e->d_name);

        struct stat st;
        /* lstat, never stat: a symlinked directory is exactly how a
           collection can loop back on itself. lstat reports the LINK's own
           type, never resolving it, so a symlink -- to a file or a
           directory -- is never S_ISDIR or S_ISREG below and so is silently
           skipped by the dispatch itself; there is deliberately no separate
           `if (S_ISLNK(...)) continue`, because with stat swapped in for
           lstat such a line would look like it was still doing its job while
           actually doing nothing (stat's result is never S_ISLNK either --
           it also resolves through the link). Real ROM collections do not
           need symlinks here. */
        if (lstat(full, &st) != 0) continue;

        char rel[4096];
        if (rel_prefix && *rel_prefix)
            snprintf(rel, sizeof rel, "%s/%s", rel_prefix, e->d_name);
        else
            snprintf(rel, sizeof rel, "%s", e->d_name);

        if (S_ISDIR(st.st_mode))
            scan_walk(acc, full, rel, depth + 1);
        else if (S_ISREG(st.st_mode) && romlist_is_rom(e->d_name))
            acc_push(acc, rel);
    }
    closedir(d);
}

void romlist_free(koboy_romlist *rl)
{
    free(rl->names);
    free(rl->item_ptr);
    rl->names = NULL;
    rl->item_ptr = NULL;
    rl->count = 0;
    rl->hidden = 0;
}

int romlist_scan(koboy_romlist *rl, const char *dir)
{
    /* A rescan of the SAME struct must not leak the previous arrays -- see
       koboy_romlist's comment on why nothing may still hold item_ptr from
       before this call anyway, so freeing here is always safe. */
    romlist_free(rl);
    memset(rl, 0, sizeof *rl);
    snprintf(rl->dir, sizeof rl->dir, "%s", dir);

    /* Confirm the directory itself opens before spending any work recursing:
       distinguishes "rom_dir is wrong" (-1) from "rom_dir is empty" (0, from
       the walk below finding nothing), which is the only diagnostic a user
       with no terminal gets. */
    DIR *probe = opendir(dir);
    if (!probe) return -1;
    closedir(probe);

    scan_acc acc;
    memset(&acc, 0, sizeof acc);
    acc.visit_limit = visit_cap();
    scan_walk(&acc, dir, "", 0);

    /* Sort BEFORE capping: the whole point. Whatever gets kept below is
       determined by name, never by which directory the walk happened to
       visit first. */
    qsort(acc.names, (size_t)acc.count, sizeof *acc.names, name_cmp);

    int cap = hard_cap();
    int extra_hidden = 0;
    if (acc.count > cap) {
        extra_hidden = acc.count - cap;
        acc.count = cap;
    }
    rl->hidden = acc.hidden + extra_hidden;
    if (rl->hidden > 0)
        /* Not just to the log: a user on a device with no terminal will
           never see koboy.log, so the ONLY diagnostic they get for a
           truncated collection is a row on the panel -- see the overflow
           row appended to item_ptr below. This stderr line is the extra
           detail a developer with SSH access still gets. */
        fprintf(stderr, "koboy: %d rom(s) not shown (%s)\n", rl->hidden, dir);

    rl->names = acc.names;
    rl->count = acc.count;

    int display = rl->count + (rl->hidden > 0 ? 1 : 0);
    rl->item_ptr = malloc(sizeof *rl->item_ptr * (size_t)(display > 0 ? display : 1));
    if (!rl->item_ptr) { romlist_free(rl); return -1; }
    for (int i = 0; i < rl->count; i++) rl->item_ptr[i] = rl->names[i];
    if (rl->hidden > 0) {
        snprintf(rl->overflow_msg, sizeof rl->overflow_msg,
                "+ %d MORE ROM%s NOT SHOWN", rl->hidden, rl->hidden == 1 ? "" : "S");
        rl->item_ptr[rl->count] = rl->overflow_msg;
    }
    return display;
}

const char *romlist_name(const koboy_romlist *rl, int i)
{
    if (i < 0 || i >= rl->count) return "";
    return rl->names[i];
}

void romlist_path(const koboy_romlist *rl, int i, char *out, size_t n)
{
    if (n == 0) return;
    if (i < 0 || i >= rl->count) { out[0] = 0; return; }
    snprintf(out, n, "%s/%s", rl->dir, rl->names[i]);
}

const char *const *romlist_items(const koboy_romlist *rl)
{
    return rl->item_ptr;
}

bool romlist_is_real(const koboy_romlist *rl, int i)
{
    return i >= 0 && i < rl->count;
}
