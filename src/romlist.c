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
    /* One list, ten systems. .mgw is Game & Watch content for gw-libretro,
       .nes is a NES cartridge for fceumm, .min is a Pokemon Mini cartridge
       for PokeMini, .ws/.wsc are WonderSwan and WonderSwan Color for
       beetle-wswan, .ngp/.ngc are Neo Geo Pocket and Pocket Color for RACE,
       .a26 is an Atari 2600 cartridge for stella2014, .col is a ColecoVision
       one for Gearcoleco, .int an Intellivision one for FreeIntv, and
       .sms/.gg are Master System and Game Gear for Genesis Plus GX; none of
       them is a Game Boy ROM. The browser lists them all because the core is
       chosen from the extension at load time (config_core_for_rom, whose
       table this list must stay in step with), so a mixed roms/ directory is
       one list, not two.

       Case-insensitive, and not hypothetically: the device partition is
       FAT32, and the author's own Game Gear directory carries BOTH .gg and
       .GG -- 38 of one and 15 of the other, in one folder. Any of these can
       arrive in any case.

       What is NOT here matters as much as what is. A real NES collection
       carries .pal palette files beside the ROMs (262 of them in the
       author's, against 5263 .nes); a Pokemon Mini one carries boot.rom;
       BOTH a WonderSwan and a Neo Geo Pocket collection carry boot.rom and
       boot1.rom (five such files across the author's four directories); and
       an Intellivision one carries boot0.rom through boot3.rom, two of which
       ARE the BIOS this project asks the owner to install by hand. None is
       content, all would list as selectable "games", and all are excluded by
       this being an allowlist of extensions rather than a blocklist of the
       ones seen so far. */
    return ends_with_ci(name, ".gb") || ends_with_ci(name, ".gbc")
        || ends_with_ci(name, ".mgw")
        || ends_with_ci(name, ".nes")
        || ends_with_ci(name, ".min")
        || ends_with_ci(name, ".ws")  || ends_with_ci(name, ".wsc")
        || ends_with_ci(name, ".ngp") || ends_with_ci(name, ".ngc")
        || ends_with_ci(name, ".a26")
        || ends_with_ci(name, ".col")
        || ends_with_ci(name, ".int")
        || ends_with_ci(name, ".sms") || ends_with_ci(name, ".gg");
}

/* Row order, in one place: kind first (the enum's own order -- ".." above
   subdirectories above ROMs, see romlist.h), then case-insensitively by
   name. Sorting by kind is what puts folders at the top, and it is also what
   keeps the ".." row at index 0 without a special case anywhere else: it is
   the only ROMLIST_UP entry, and ROMLIST_UP is 0. */
static int entry_cmp(const void *a, const void *b)
{
    const koboy_romlist_entry *ea = (const koboy_romlist_entry *)a;
    const koboy_romlist_entry *eb = (const koboy_romlist_entry *)b;
    if (ea->kind != eb->kind) return ea->kind - eb->kind;
    return ci_cmp(ea->name, eb->name);
}

/* ---------------------------------------------------------- test overrides
   Both caps default to values no real ROM collection should reach (see
   romlist.h), which makes them impractical to exercise honestly in a test
   that has to actually create that many files. These let a test dial a cap
   down to something a tmpdir can hold in milliseconds while leaving the
   shipped defaults alone. Read once per scan, not cached: cheap, and it
   means a test can flip the env var between two scans. */
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

/* ------------------------------------------------------------- the listing
   Collects one directory's entries into a growable buffer. The cap is NOT
   applied here: applying it during the readdir loop is exactly the original
   bug (readdir order decides who gets dropped). It is applied once, in
   scan_at, after the whole listing is sorted. */
typedef struct {
    koboy_romlist_entry *ent;
    int   count;
    int   cap;          /* allocated capacity, not the display cap */
    int   visited;
    int   visit_limit;
    bool  oom;
    int   hidden;        /* oversized names / allocation failures, so far */
} scan_acc;

static void acc_push(scan_acc *acc, const char *name, int kind)
{
    if (acc->oom) { acc->hidden++; return; }
    if (strlen(name) >= ROMLIST_NAME) {
        /* Never truncate and store: a truncated name would round-trip
           through romlist_path (or romlist_enter) into something that does
           not exist, so the row would look present in the browser but fail
           to open -- worse than leaving it out. */
        acc->hidden++;
        fprintf(stderr, "koboy: name too long (>= %d bytes), skipping: %s\n",
                ROMLIST_NAME, name);
        return;
    }
    if (acc->count >= acc->cap) {
        int newcap = acc->cap ? acc->cap * 2 : 64;
        koboy_romlist_entry *grown =
            realloc(acc->ent, (size_t)newcap * sizeof *grown);
        if (!grown) {
            /* Keep what was already collected; stop growing rather than
               retrying a realloc that already failed once per remaining
               file. */
            acc->oom = true;
            acc->hidden++;
            return;
        }
        acc->ent = grown;
        acc->cap = newcap;
    }
    memcpy(acc->ent[acc->count].name, name, strlen(name) + 1);
    acc->ent[acc->count].kind = kind;
    acc->count++;
}

static void scan_one_dir(scan_acc *acc, const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (acc->visited++ >= acc->visit_limit) break;

        char full[4096];
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);

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
           need symlinks here. The one-directory-at-a-time listing does not
           itself recurse, but a browser that follows links can still be
           walked into a cycle one tap at a time, so this stays. */
        if (lstat(full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            /* Dot-directories are skipped, and this is new with folder rows:
               while the scan was recursive they contributed nothing visible
               (a .Trashes or .Spotlight-V100 holds no ROMs, so it produced no
               rows), but a directory is now a row of its own, and an SD card
               that has been in a Mac or a Windows box grows several of these.
               They are metadata, not collections, and putting them at the top
               of the list -- above every real folder, since dirs sort first --
               would be the first thing the user sees. Files are deliberately
               NOT filtered this way: that would change which ROMs are listed,
               which is the one thing this task must not do. */
            if (e->d_name[0] == '.') continue;
            /* A directory row carries a trailing '/' so it reads as a folder
               on the panel. It is a DISPLAY marker and not part of the path:
               romlist_enter strips it before joining, which is unambiguous
               because '/' cannot occur inside a directory entry's name. */
            /* 300, not ROMLIST_NAME + 2: a dirent name is at most 255 bytes,
               so this buffer cannot truncate, and acc_push is then the ONE
               place that decides an oversized name is skipped rather than
               stored short. Two places deciding that is how a half-truncated
               name reaches the list. */
            char label[300];
            snprintf(label, sizeof label, "%s/", e->d_name);
            acc_push(acc, label, ROMLIST_DIR);
        } else if (S_ISREG(st.st_mode) && romlist_is_rom(e->d_name)) {
            acc_push(acc, e->d_name, ROMLIST_ROM);
        }
    }
    closedir(d);
}

void romlist_free(koboy_romlist *rl)
{
    free(rl->ent);
    free(rl->item_ptr);
    rl->ent = NULL;
    rl->item_ptr = NULL;
    rl->count = 0;
    rl->roms = 0;
    rl->hidden = 0;
}

/* The one place a listing is produced. `root` and `sub` are copied to locals
   FIRST because they are usually rl->root and rl->sub themselves, and the
   memset below is about to erase them -- reading a source string out of the
   struct being cleared is the kind of aliasing bug that only shows up on the
   second navigation. */
static int scan_at(koboy_romlist *rl, const char *root, const char *sub)
{
    char r[512], s[512];
    snprintf(r, sizeof r, "%s", root);
    snprintf(s, sizeof s, "%s", sub ? sub : "");

    /* A rescan of the SAME struct must not leak the previous arrays -- see
       koboy_romlist's comment on why nothing may still hold item_ptr from
       before this call anyway, so freeing here is always safe. */
    romlist_free(rl);
    memset(rl, 0, sizeof *rl);
    snprintf(rl->root, sizeof rl->root, "%s", r);
    snprintf(rl->sub, sizeof rl->sub, "%s", s);
    if (s[0]) snprintf(rl->dir, sizeof rl->dir, "%s/%s", r, s);
    else      snprintf(rl->dir, sizeof rl->dir, "%s", r);

    /* Confirm the directory itself opens before spending any work listing it:
       distinguishes "rom_dir is wrong" (-1) from "rom_dir is empty" (0, from
       the listing below finding nothing), which is the only diagnostic a user
       with no terminal gets. */
    DIR *probe = opendir(rl->dir);
    if (!probe) return -1;
    closedir(probe);

    scan_acc acc;
    memset(&acc, 0, sizeof acc);
    acc.visit_limit = visit_cap();

    /* The ".." row exists everywhere but the root, and is pushed like any
       other entry rather than prepended afterwards: entry_cmp sorts
       ROMLIST_UP first, so it lands at index 0 by the same rule that puts
       folders above files, and the cap below cannot drop it. */
    if (s[0]) acc_push(&acc, "..", ROMLIST_UP);

    scan_one_dir(&acc, rl->dir);

    /* Sort BEFORE capping: the whole point. Whatever gets kept below is
       determined by kind and name, never by the order readdir happened to
       hand entries back in. */
    qsort(acc.ent, (size_t)acc.count, sizeof *acc.ent, entry_cmp);

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
           truncated listing is a row on the panel -- see the overflow
           row appended to item_ptr below. This stderr line is the extra
           detail a developer with SSH access still gets. */
        fprintf(stderr, "koboy: %d entr%s not shown (%s)\n",
                rl->hidden, rl->hidden == 1 ? "y" : "ies", rl->dir);

    rl->ent = acc.ent;
    rl->count = acc.count;
    rl->roms = 0;
    for (int i = 0; i < rl->count; i++)
        if (rl->ent[i].kind == ROMLIST_ROM) rl->roms++;

    int display = rl->count + (rl->hidden > 0 ? 1 : 0);
    rl->item_ptr = malloc(sizeof *rl->item_ptr * (size_t)(display > 0 ? display : 1));
    if (!rl->item_ptr) { romlist_free(rl); return -1; }
    for (int i = 0; i < rl->count; i++) rl->item_ptr[i] = rl->ent[i].name;
    if (rl->hidden > 0) {
        /* Still worded "ROMS": entries are dropped from the END of a sorted
           listing, and ROMs sort after every folder row, so a truncation eats
           ROMs long before it could reach a directory. */
        snprintf(rl->overflow_msg, sizeof rl->overflow_msg,
                "+ %d MORE ROM%s NOT SHOWN", rl->hidden, rl->hidden == 1 ? "" : "S");
        rl->item_ptr[rl->count] = rl->overflow_msg;
    }
    return display;
}

int romlist_scan(koboy_romlist *rl, const char *dir)
{
    return scan_at(rl, dir, "");
}

/* Rows the UI is currently showing, the same number a scan returned. Used by
   the navigation calls below for their "nothing changed" answer, so a refused
   descent is indistinguishable to the caller from never having asked. */
static int display_rows(const koboy_romlist *rl)
{
    return rl->count + (rl->hidden > 0 ? 1 : 0);
}

/* Components in `sub`: 0 at the root, 1 for "Game and Watch". */
static int sub_depth(const char *sub)
{
    if (!sub || !sub[0]) return 0;
    int d = 1;
    for (const char *p = sub; *p; p++) if (*p == '/') d++;
    return d;
}

int romlist_enter(koboy_romlist *rl, int i)
{
    if (i < 0 || i >= rl->count || rl->ent[i].kind != ROMLIST_DIR)
        return display_rows(rl);

    /* Copied out before scan_at clears the struct -- see its own comment. */
    char root[512], sub[512], name[ROMLIST_NAME];
    snprintf(root, sizeof root, "%s", rl->root);
    snprintf(sub,  sizeof sub,  "%s", rl->sub);
    snprintf(name, sizeof name, "%s", rl->ent[i].name);

    size_t nl = strlen(name);
    if (nl > 0 && name[nl - 1] == '/') name[nl - 1] = 0;   /* the display marker */

    if (sub_depth(sub) + 1 > ROMLIST_MAX_DEPTH) return display_rows(rl);

    /* Sized so the join can never itself truncate; whether the RESULT fits
       rl->sub is then a question this can answer honestly rather than
       discover after the fact. A sub that did not fit would list some other
       directory entirely, so refusing to descend is the only safe answer. */
    char newsub[sizeof sub + ROMLIST_NAME + 2];
    if (sub[0]) snprintf(newsub, sizeof newsub, "%s/%s", sub, name);
    else        snprintf(newsub, sizeof newsub, "%s", name);
    if (strlen(newsub) >= sizeof rl->sub) return display_rows(rl);

    int n = scan_at(rl, root, newsub);
    if (n < 0) {
        /* The folder could not be opened (removed, or not readable) after
           all. Go back to where the user actually was rather than leaving
           them looking at an empty listing of a directory that is gone. */
        n = scan_at(rl, root, sub);
    }
    return n;
}

int romlist_up(koboy_romlist *rl)
{
    if (!rl->sub[0]) return display_rows(rl);

    char root[512], sub[512];
    snprintf(root, sizeof root, "%s", rl->root);
    snprintf(sub,  sizeof sub,  "%s", rl->sub);

    char *slash = strrchr(sub, '/');
    if (slash) *slash = 0;
    else       sub[0] = 0;

    return scan_at(rl, root, sub);
}

const char *romlist_subpath(const koboy_romlist *rl)
{
    return rl->sub;
}

int romlist_kind(const koboy_romlist *rl, int i)
{
    if (i < 0 || i >= rl->count) return ROMLIST_OVERFLOW;
    return rl->ent[i].kind;
}

const char *romlist_name(const koboy_romlist *rl, int i)
{
    if (i < 0 || i >= rl->count) return "";
    return rl->ent[i].name;
}

void romlist_path(const koboy_romlist *rl, int i, char *out, size_t n)
{
    if (n == 0) return;
    if (i < 0 || i >= rl->count || rl->ent[i].kind != ROMLIST_ROM) { out[0] = 0; return; }
    snprintf(out, n, "%s/%s", rl->dir, rl->ent[i].name);
}

const char *const *romlist_items(const koboy_romlist *rl)
{
    return rl->item_ptr;
}

bool romlist_is_real(const koboy_romlist *rl, int i)
{
    return i >= 0 && i < rl->count;
}
