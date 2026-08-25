#define _POSIX_C_SOURCE 200809L
#include "romlist.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void rebuild_ptrs(koboy_romlist *rl)
{
    for (int i = 0; i < rl->count; i++) rl->item_ptr[i] = rl->names[i];
}

int romlist_scan(koboy_romlist *rl, const char *dir)
{
    memset(rl, 0, sizeof *rl);
    snprintf(rl->dir, sizeof rl->dir, "%s", dir);

    DIR *d = opendir(dir);
    if (!d) return -1;

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (rl->count >= ROMLIST_MAX) {
            /* Not silent. A browser that quietly shows 256 of 300 ROMs is a
               browser the user thinks is broken for one specific game. */
            fprintf(stderr, "koboy: more than %d roms in %s, listing the "
                            "first %d\n", ROMLIST_MAX, dir, ROMLIST_MAX);
            break;
        }
        if (!romlist_is_rom(e->d_name)) continue;
        snprintf(rl->names[rl->count], ROMLIST_NAME, "%s", e->d_name);
        rl->count++;
    }
    closedir(d);

    qsort(rl->names, (size_t)rl->count, ROMLIST_NAME, name_cmp);
    rebuild_ptrs(rl);
    return rl->count;
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
