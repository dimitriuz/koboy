#define _POSIX_C_SOURCE 200809L
#include "state.h"
#include "sram.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void state_path(char *out, size_t n, const char *save_dir,
                const char *rom_path, int slot)
{
    if (!out || n == 0) return;
    /* Live bound: the slot index comes from a touch on a menu row, so a stale
       page after a ROM switch can reach this with a nonsense value. Writing ""
       makes every caller's fopen fail cleanly instead of creating a file with a
       surprising name. */
    if (slot < 1 || slot > KOBOY_STATE_SLOTS) { out[0] = 0; return; }

    /* Reuse the .srm stem logic rather than duplicating it, so the two kinds of
       save file can never disagree about what a ROM is called. */
    char srm[512];
    sram_path_for_rom(srm, sizeof srm, save_dir, rom_path);
    char *dot = strrchr(srm, '.');
    if (dot) *dot = 0;
    int len = snprintf(out, n, "%s.st%d", srm, slot);
    /* Truncation writes "" for the same reason an out-of-range slot does: a
       truncated path is a WRONG path, and this one names a save-state file.
       Silently writing user data to a shortened filename is a data bug, not a
       cosmetic one. Also keeps the build warning-free -- state.c links into
       every test binary, so one warning here is 21 lines of noise that would
       hide the next real one. */
    if (len < 0 || (size_t)len >= n) out[0] = 0;
}

bool state_exists(const char *save_dir, const char *rom_path, int slot)
{
    char p[512];
    state_path(p, sizeof p, save_dir, rom_path, slot);
    if (!p[0]) return false;
    return access(p, R_OK) == 0;
}

void state_slot_label(char *out, size_t n, const char *save_dir,
                      const char *rom_path, int slot)
{
    if (!out || n == 0) return;
    snprintf(out, n, "SLOT %d - %s", slot,
             state_exists(save_dir, rom_path, slot) ? "SAVED" : "EMPTY");
}
