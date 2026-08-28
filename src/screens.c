/* The full-panel UI screens; screens.h says why they are not in main.c.
 * Everything below drives ui.c's one list widget through the koboy_platform
 * vtable. */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "screens.h"

#include "romlist.h"
#include "state.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Defined here rather than in main.c: see screens.h. */
volatile sig_atomic_t koboy_stop;

/* Drives one list widget to a selection. Returns the chosen index, or -1 if
   the user quit, the run was stopped, or a script ran out.

   `script`/`script_n` make the startup flow reachable in a bounded unattended
   run. Without them every automated test passes --rom and skips every list
   screen -- the blind spot that hid v1's first-run deadlock through twenty
   reviews. The `menu` verb extends that to screen_menu and everything CHOOSE
   ROM opens under it. screen_slot_picker is wired for a script but nothing
   walks into it yet.

   `script_i`, when not NULL, is a CURSOR shared across every screen one
   --ui-script run drives, so a script written as one flat sequence of taps
   walks through several screen_list calls, each picking up where the last
   consumed state left off. Every call still primes with one synthetic RELEASED
   state regardless of the cursor position (see `primed` below): each fresh
   koboy_ui_list demands its own release before its first tap.

   `disabled_index`, when not -1, is a row that SELECTS nothing -- the ROM
   browser's "+N MORE" row uses it so a tap cannot reach romlist_path as if it
   were a real ROM. The loop keeps polling, like any other no-op input. */
int screen_list(koboy_platform *pf, koboy_input *in, koboy_ui_list *u,
                uint8_t *panel, int stride, int pw, int ph,
                const koboy_input_state *script, int *script_i, int script_n,
                int disabled_index)
{
    int  chosen = -1;
    int  si = script_i ? *script_i : 0;
    bool need_draw = true;
    bool primed = false;

    while (!koboy_stop && !pf->should_quit(pf->ctx)) {
        if (need_draw) {
            need_draw = false;
            memset(panel, 0xFF, (size_t)stride * (size_t)ph);
            ui_list_render(u, panel, stride, pw, ph);
            pf->blit_gray8(pf->ctx, panel, pw, ph, stride, 0, 0);
            /* FULL, i.e. GC16: a list is about to sit still, and the game
               rect's four-level ceiling does not apply to it. */
            pf->refresh(pf->ctx, 0, 0, pw, ph, KOBOY_REFRESH_FULL);
        }

        const koboy_input_state *st;
        koboy_input_state synth;
        if (script) {
            /* One RELEASED state before the script's first entry, ALWAYS.
               ui_list_init sets prev_touch = true, so a script whose first
               verb is `tap` had its press swallowed and its release consumed
               as the priming edge -- selecting nothing and exiting 0, a green
               CI run that tested nothing. Confirmed on hardware with
               `printf 'tap 300 300\n'`. Primed HERE rather than documented in
               uiscript.h, because a note relies on every future author
               reading it. */
            if (!primed) {
                primed = true;
                memset(&synth, 0, sizeof synth);
                st = &synth;
            } else if (si >= script_n) {
                break;                      /* script exhausted: give up */
            } else {
                st = &script[si++];
            }
        } else {
            pf->poll_input(pf->ctx, in);
            /* NOT input_state(): the faceplate's A/B zones stay live under a
               full-panel list and ui_list_feed eats their synthesised bits as
               page-turns before any row hit-test. See input.h. */
            input_ui_state(in, &synth);
            st = &synth;
        }

        int idx = -1;
        ui_action a = ui_list_feed(u, st, &idx);
        if (a == UI_SELECT && idx != disabled_index) { chosen = idx; break; }
        if (a == UI_PAGE_NEXT || a == UI_PAGE_PREV || a == UI_JUMP) need_draw = true;

        if (!script) usleep(5000);
    }
    if (script_i) *script_i = si;
    return chosen;
}

/* Returns the chosen MENU_* action, or MENU_RESUME if the user backed out.
   `has_states` greys nothing out visually -- the LABEL says so, cheaper on a
   panel with no colour and no hover. `map` is likewise IN the row: with no
   hover and no second screen, a setting you cannot read without opening
   something is one nobody knows the value of. */
int screen_menu(koboy_platform *pf, koboy_input *in, uint8_t *panel,
                int stride, int pw, int ph, bool has_states,
                koboy_gray_map map, int divisor,
                bool dither, koboy_wfm_policy wfm, int shot_next,
                const koboy_input_state *script, int *script_i, int script_n)
{
    const char *items[MENU_COUNT];
    static char gray_label[48], divisor_label[48], motion_label[48], shot_label[48];
    ui_gray_label(gray_label, sizeof gray_label, map);
    ui_divisor_label(divisor_label, sizeof divisor_label, divisor);
    ui_motion_label(motion_label, sizeof motion_label, dither, wfm);
    ui_shot_label(shot_label, sizeof shot_label, shot_next);
    items[MENU_SAVE]        = has_states ? "SAVE STATE" : "SAVE STATE (UNSUPPORTED)";
    items[MENU_LOAD]        = has_states ? "LOAD STATE" : "LOAD STATE (UNSUPPORTED)";
    items[MENU_RESET]       = "RESET GAME";
    items[MENU_GRAY]        = gray_label;
    items[MENU_FRAMES]      = divisor_label;
    items[MENU_MOTION]      = motion_label;
    items[MENU_SHOT]        = shot_label;
    items[MENU_CHOOSE_ROM]  = "CHOOSE ROM";
    items[MENU_RESUME]      = "RESUME";
    items[MENU_QUIT]        = "QUIT";

    koboy_ui_list list;
    ui_list_init(&list, "MENU", items, MENU_COUNT,
                 KOBOY_CHROME_MARGIN, KOBOY_CHROME_MARGIN,
                 pw - 2 * KOBOY_CHROME_MARGIN, ph - 2 * KOBOY_CHROME_MARGIN);

    /* The cursor is shared with every other screen a --ui-script run drives:
       the `menu` verb opens this screen from inside the emulator loop, and
       whatever the script has left over goes on driving it. See screen_list's
       script_i comment. */
    int pick = screen_list(pf, in, &list, panel, stride, pw, ph, script, script_i, script_n, -1);
    if (pick < 0) return MENU_RESUME;
    if ((pick == MENU_SAVE || pick == MENU_LOAD) && !has_states) return MENU_RESUME;
    return pick;
}

/* Returns the chosen slot (1-based), or 0 if the user backed out. */
int screen_slot_picker(koboy_platform *pf, koboy_input *in, uint8_t *panel,
                       int stride, int pw, int ph, const char *title,
                       const char *save_dir, const char *rom_path,
                       const koboy_input_state *script, int *script_i,
                       int script_n)
{
    static char labels[KOBOY_STATE_SLOTS + 1][64];
    const char *items[KOBOY_STATE_SLOTS + 1];
    for (int s = 1; s <= KOBOY_STATE_SLOTS; s++) {
        state_slot_label(labels[s - 1], sizeof labels[s - 1], save_dir, rom_path, s);
        items[s - 1] = labels[s - 1];
    }
    snprintf(labels[KOBOY_STATE_SLOTS], sizeof labels[KOBOY_STATE_SLOTS], "BACK");
    items[KOBOY_STATE_SLOTS] = labels[KOBOY_STATE_SLOTS];

    koboy_ui_list list;
    ui_list_init(&list, title, items, KOBOY_STATE_SLOTS + 1,
                 KOBOY_CHROME_MARGIN, KOBOY_CHROME_MARGIN,
                 pw - 2 * KOBOY_CHROME_MARGIN, ph - 2 * KOBOY_CHROME_MARGIN);

    /* The same shared cursor screen_menu uses. Wired even though nothing
       scripts SAVE/LOAD, because the alternative is a TRAP: this screen is one
       tap past a row a script can reach, and an unscripted screen_list with no
       live input never exits -- a script tapping SAVE STATE would HANG rather
       than fail. */
    int pick = screen_list(pf, in, &list, panel, stride, pw, ph, script, script_i, script_n, -1);
    if (pick < 0 || pick >= KOBOY_STATE_SLOTS) return 0;
    return pick + 1;
}


/* Returns the chosen MAIN_* action, or -1 if the run was stopped or a script
   ran out. THIS screen IS scripted: it is the first screen of the startup
   flow, in front of both the ROM browser and the RECENT picker, so a
   --ui-script run must navigate it to reach either. */
int screen_main_menu(koboy_platform *pf, koboy_input *in, uint8_t *panel,
                     int stride, int pw, int ph,
                     const koboy_input_state *script, int *script_i, int script_n)
{
    static const char *const items[MAIN_COUNT] = { "RECENT", "ALL GAMES", "QUIT" };

    koboy_ui_list list;
    ui_list_init(&list, "KOBOY", items, MAIN_COUNT,
                 KOBOY_CHROME_MARGIN, KOBOY_CHROME_MARGIN,
                 pw - 2 * KOBOY_CHROME_MARGIN, ph - 2 * KOBOY_CHROME_MARGIN);

    return screen_list(pf, in, &list, panel, stride, pw, ph, script, script_i, script_n, -1);
}

/* Returns the chosen index into `rc`, or -1 if the user tapped BACK.

   BACK is a real trailing row, unlike the MAIN MENU and the ROM browser, which
   have no cancel gesture (they are reachable only by deliberate choice). A
   RECENT list can be genuinely EMPTY on a first run or after clearing history,
   and a screen with nothing to tap and no way back is a worse first experience
   than one more row. When `rc` is empty a disabled placeholder row explains
   why, using screen_list's disabled_index. */
int screen_recent_picker(koboy_platform *pf, koboy_input *in, uint8_t *panel,
                         int stride, int pw, int ph, const koboy_recent *rc,
                         const koboy_input_state *script, int *script_i, int script_n)
{
    enum { RECENT_UI_MAX = KOBOY_RECENT_MAX + 2 };   /* entries + placeholder + BACK */
    static char labels[RECENT_UI_MAX][KOBOY_RECENT_DISPLAY];
    const char *items[RECENT_UI_MAX];
    int n = 0, placeholder = -1;

    if (rc->count == 0) {
        snprintf(labels[n], sizeof labels[n], "NO RECENT GAMES YET");
        items[n] = labels[n];
        placeholder = n;
        n++;
    } else {
        for (int i = 0; i < rc->count; i++) {
            snprintf(labels[n], sizeof labels[n], "%s", recent_display(rc, i));
            items[n] = labels[n];
            n++;
        }
    }
    snprintf(labels[n], sizeof labels[n], "BACK");
    items[n] = labels[n];
    int back_index = n;
    n++;

    koboy_ui_list list;
    ui_list_init(&list, "RECENT", items, n,
                 KOBOY_CHROME_MARGIN, KOBOY_CHROME_MARGIN,
                 pw - 2 * KOBOY_CHROME_MARGIN, ph - 2 * KOBOY_CHROME_MARGIN);

    int pick = screen_list(pf, in, &list, panel, stride, pw, ph, script, script_i, script_n,
                        placeholder);
    if (pick < 0 || pick == back_index) return -1;
    return pick;    /* placeholder, when present, is index 0 and never reached here */
}

/* ------------------------------------------------------------- the browser
   ONE DIRECTORY AT A TIME, not the whole tree flattened. The flatten was fine
   for a hundred ROMs in one folder and unreadable for a real collection: 59
   Game & Watch titles produced 59 rows whose first 15 characters were
   identical, and ui_fit_label's middle ellipsis then spent the row's width on
   that shared prefix and ate the title.

   The header says where you are; ui_path_title builds it and owns the
   truncation rule, because how much a title row carries is the list widget's
   fact. "ALL GAMES" names the MAIN MENU row that got you here, and the
   breadcrumb below reads as a path. */
#define BROWSER_TITLE_HEAD "ALL GAMES"

/* Drives the ROM browser until the user picks a ROM, backs out of the root, or
   the run ends. Returns a BROWSE_*; on BROWSE_PICKED it writes the ROM's full
   path into out_path -- and NOT the row text as well, because recent.c derives
   that from the path (recent_name_from_path) and a second output could
   disagree with the first.

   Both entry points -- startup ALL GAMES and MENU -> CHOOSE ROM -- call
   this. */

int screen_browser(koboy_platform *pf, koboy_input *in, uint8_t *panel,
                   int stride, int pw, int ph, const char *rom_dir,
                   char *out_path, size_t out_path_n,
                   const koboy_input_state *script, int *script_i,
                   int script_n)
{
    /* memset, not `= {0}`: the Linaro 4.9 cross compiler warns
       -Wmissing-braces on `= {0}` for a struct whose first member is itself
       an array, and this project ships at zero warnings. */
    koboy_romlist rl;
    memset(&rl, 0, sizeof rl);

    int n = romlist_scan(&rl, rom_dir);
    if (n < 0) { romlist_free(&rl); return BROWSE_ERR_DIR; }
    /* rl.count, NOT n: n counts the synthetic overflow row too, and a rom_dir
       holding one oversized-name ROM (hidden > 0, count == 0) must report "no
       roms" rather than open a browser whose only row selects nothing. count
       rather than rl.roms because a root with no loose ROMs but a folder full
       of them is a good collection -- it just needs one tap first. */
    if (rl.count == 0) { romlist_free(&rl); return BROWSE_ERR_EMPTY; }

    int result = BROWSE_NONE;
    for (;;) {
        char title[UI_TITLE_CHARS + 8];
        ui_path_title(title, sizeof title, BROWSER_TITLE_HEAD,
                      romlist_subpath(&rl));

        /* Rebuilt after every navigation, never reused: romlist's arrays are
           reallocated wholesale by each rescan (see romlist.h), so a
           koboy_ui_list that outlived one would be holding freed pointers. */
        koboy_ui_list list;
        ui_list_init(&list, title, romlist_items(&rl), n,
                     KOBOY_CHROME_MARGIN, KOBOY_CHROME_MARGIN,
                     pw - 2 * KOBOY_CHROME_MARGIN, ph - 2 * KOBOY_CHROME_MARGIN);
        /* Letter index strip: only the ROM browser gets one, never MENU,
           MAIN MENU, RECENT or the slot picker, which are all short
           fixed-ish lists a strip would just clutter. */
        ui_list_enable_alpha_jump(&list, true);

        int pick = screen_list(pf, in, &list, panel, stride, pw, ph,
                            script, script_i, script_n,
                            rl.hidden > 0 ? rl.count : -1);
        if (pick < 0) {
            /* Stopped, or the script ran out. Leaves the BROWSER from whatever
               directory it is in rather than walking up one level per
               iteration, which would make a Ctrl-C in a nested folder take
               several passes to notice. The ".." row is what goes up. */
            result = BROWSE_NONE;
            break;
        }

        int kind = romlist_kind(&rl, pick);
        if (kind == ROMLIST_ROM) {
            romlist_path(&rl, pick, out_path, out_path_n);
            result = BROWSE_PICKED;
            break;
        }
        if (kind == ROMLIST_DIR)      n = romlist_enter(&rl, pick);
        else if (kind == ROMLIST_UP)  n = romlist_up(&rl);
        else                          continue;   /* the overflow row: not selectable */

        if (n < 0) {
            /* The directory we navigated to could not be listed at all, and
               romlist has already tried to fall back to where we were. There
               is nothing left to show. */
            result = BROWSE_ERR_DIR;
            break;
        }
    }

    romlist_free(&rl);
    return result;
}
