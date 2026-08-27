#define _POSIX_C_SOURCE 200809L
#include "uiscript.h"
#include <stdio.h>
#include <string.h>

static int push(koboy_input_state *out, unsigned char *is_menu, int *n, int max,
                const koboy_input_state *st, int menu)
{
    if (*n >= max) return 0;                 /* truncate, never overrun */
    is_menu[*n] = (unsigned char)(menu ? 1 : 0);
    out[(*n)++] = *st;
    return 1;
}

static void clear(koboy_input_state *st) { memset(st, 0, sizeof *st); }

int uiscript_load(const char *path, koboy_input_state *out,
                  unsigned char *is_menu, int max)
{
    /* Both arrays are required, and a missing one is an ERROR rather than a
       tolerated NULL. A nullable is_menu would mean a `menu` verb silently
       doing nothing for one caller and opening the menu for another, which is
       precisely the sort of "the scripted path skipped the interesting
       branch" failure this file's header comment exists about. */
    if (!path || !out || !is_menu || max <= 0) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    int n = 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#');
        if (h) *h = 0;

        char verb[32];
        int a = 0, b = 0;
        int fields = sscanf(line, "%31s %d %d", verb, &a, &b);
        if (fields < 1) continue;            /* blank or comment-only */

        koboy_input_state st;
        if (!strcmp(verb, "tap")) {
            if (fields != 3) { fclose(f); return -1; }
            clear(&st);
            st.touch[0].x = a; st.touch[0].y = b; st.touch[0].down = true;
            if (!push(out, is_menu, &n, max, &st, 0)) break;
            clear(&st);
            if (!push(out, is_menu, &n, max, &st, 0)) break;
        } else if (!strcmp(verb, "key")) {
            /* sscanf put the key letter nowhere, so re-read it: "key a" has
               fields == 1 and the letter is still in the line. */
            char which = 0;
            if (sscanf(line, "%31s %c", verb, &which) != 2) { fclose(f); return -1; }
            uint16_t bit;
            if      (which == 'a' || which == 'A') bit = KOBOY_BTN_A;
            else if (which == 'b' || which == 'B') bit = KOBOY_BTN_B;
            else { fclose(f); return -1; }
            clear(&st);
            st.buttons = bit;
            if (!push(out, is_menu, &n, max, &st, 0)) break;
            clear(&st);
            if (!push(out, is_menu, &n, max, &st, 0)) break;
        } else if (!strcmp(verb, "idle")) {
            if (fields < 2 || a < 0) { fclose(f); return -1; }
            clear(&st);
            int pushed = 1;
            for (int i = 0; i < a && pushed; i++)
                pushed = push(out, is_menu, &n, max, &st, 0);
            if (!pushed) break;
        } else if (!strcmp(verb, "menu")) {
            /* ONE state, and its contents are never read: main.c's emulator
               loop consumes this slot itself (it is the only screen not
               driven by run_list, because the menu is not entered by tapping
               a row -- it is entered by asking for it) and then hands the
               REST of the script to run_menu. The state is cleared anyway, so
               that a `menu` verb misplaced inside a browser script degrades
               to an idle frame rather than to a stray tap at (0,0). */
            clear(&st);
            if (!push(out, is_menu, &n, max, &st, 1)) break;
        } else {
            fclose(f);
            return -1;                        /* unknown verb */
        }
    }
    fclose(f);
    return n;
}
