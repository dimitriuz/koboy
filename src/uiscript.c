#define _POSIX_C_SOURCE 200809L
#include "uiscript.h"
#include <stdio.h>
#include <string.h>

static int push(koboy_input_state *out, int *n, int max,
                const koboy_input_state *st)
{
    if (*n >= max) return 0;                 /* truncate, never overrun */
    out[(*n)++] = *st;
    return 1;
}

static void clear(koboy_input_state *st) { memset(st, 0, sizeof *st); }

int uiscript_load(const char *path, koboy_input_state *out, int max)
{
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
            if (!push(out, &n, max, &st)) break;
            clear(&st);
            if (!push(out, &n, max, &st)) break;
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
            if (!push(out, &n, max, &st)) break;
            clear(&st);
            if (!push(out, &n, max, &st)) break;
        } else if (!strcmp(verb, "idle")) {
            if (fields < 2 || a < 0) { fclose(f); return -1; }
            clear(&st);
            int pushed = 1;
            for (int i = 0; i < a && pushed; i++)
                pushed = push(out, &n, max, &st);
            if (!pushed) break;
        } else {
            fclose(f);
            return -1;                        /* unknown verb */
        }
    }
    fclose(f);
    return n;
}
