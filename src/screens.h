#ifndef KOBOY_SCREENS_H
#define KOBOY_SCREENS_H
/* The full-panel UI screens: one list widget driven six different ways.
 *
 * WHY THESE ARE NOT IN main.c, and it is not that main.c was long: main.c is
 * filtered out of $(SRC), so no test binary links it -- the highest-churn file
 * in the project (56 of the last 200 src commits) and the only one with zero
 * executed coverage. Everything here goes through the koboy_platform vtable
 * and names no platform_kobo_* / platform_sdl_* symbol, which is the one
 * condition for linking into every test binary.
 *
 * Each function's rationale stays with its body in screens.c; a header that
 * paraphrases it is a second copy that goes stale.
 */
#include "input.h"          /* must precede platform_if.h: declares struct koboy_input */
#include "platform_if.h"

#include "koboy.h"
#include "recent.h"
#include "ui.h"

#include <signal.h>
#include <stddef.h>

/* The process-wide "stop now" flag, set by main.c's SIGINT/SIGTERM handler
   and polled by every screen loop below alongside pf->should_quit().

   DEFINED IN screens.c AND NOT IN main.c: screens.c links into test binaries
   containing no main.c, so an extern defined there would not link. main.c's
   handler is still its only writer in the shipped binary. A test may set it to
   1 to prove a screen loop honours it and MUST set it back to 0 -- it is a
   global, and each test binary runs every case in one process. */
extern volatile sig_atomic_t koboy_stop;

/* THE ORDER IS NOT FREE: tests/smoke_host.sh drives this menu by hardcoded
   pixel coordinates derived from the row index, so a row inserted ABOVE an
   existing one silently strands every tap below it outside the row it names.
   Add at the END of the settings group. */
enum {
    MENU_SAVE = 0, MENU_LOAD, MENU_RESET, MENU_GRAY, MENU_FRAMES, MENU_MOTION,
    MENU_SHOT, MENU_CHOOSE_ROM, MENU_RESUME, MENU_QUIT,
    MENU_COUNT
};

enum { MAIN_RECENT = 0, MAIN_ALL_GAMES, MAIN_QUIT, MAIN_COUNT };

/* BROWSE_BACK is NOT BROWSE_NONE. Both mean "no rom", and they must not be
   confused: NONE is the run ending (a signal, or a script running out) and
   main.c exits on it, while BACK is the user asking for the MAIN MENU and
   main.c must go there. Collapsing them would turn the new escape row into a
   quit. */
enum { BROWSE_PICKED = 0, BROWSE_NONE, BROWSE_BACK, BROWSE_ERR_DIR,
       BROWSE_ERR_EMPTY };

/* Drives one list widget to a selection; the rationale for `script`,
   `script_i` and `disabled_index` is on the definition in screens.c. */
int screen_list(koboy_platform *pf, koboy_input *in, koboy_ui_list *u,
                uint8_t *panel, int stride, int pw, int ph,
                const koboy_input_state *script, int *script_i, int script_n,
                int disabled_index);

/* The in-game MENU. Returns a MENU_*; MENU_RESUME if the user backed out. */
int screen_menu(koboy_platform *pf, koboy_input *in, uint8_t *panel,
                int stride, int pw, int ph, bool has_states,
                koboy_gray_map map, int divisor,
                bool dither, koboy_wfm_policy wfm, int shot_next,
                const koboy_input_state *script, int *script_i, int script_n);

/* SAVE/LOAD STATE's slot list. Returns the slot (1-based), or 0 for BACK. */
int screen_slot_picker(koboy_platform *pf, koboy_input *in, uint8_t *panel,
                       int stride, int pw, int ph, const char *title,
                       const char *save_dir, const char *rom_path,
                       const koboy_input_state *script, int *script_i,
                       int script_n);

/* The startup MAIN MENU. Returns a MAIN_*, or -1 if the run was stopped. */
int screen_main_menu(koboy_platform *pf, koboy_input *in, uint8_t *panel,
                     int stride, int pw, int ph,
                     const koboy_input_state *script, int *script_i, int script_n);

/* The RECENT list. Returns an index into `rc`, or -1 for BACK/stopped. */
int screen_recent_picker(koboy_platform *pf, koboy_input *in, uint8_t *panel,
                         int stride, int pw, int ph, const koboy_recent *rc,
                         const koboy_input_state *script, int *script_i, int script_n);

/* The ROM browser. Returns a BROWSE_*; on BROWSE_PICKED it writes the ROM's
   full path into out_path. */
int screen_browser(koboy_platform *pf, koboy_input *in, uint8_t *panel,
                   int stride, int pw, int ph, const char *rom_dir,
                   char *out_path, size_t out_path_n,
                   const koboy_input_state *script, int *script_i,
                   int script_n);
#endif
