#ifndef KOBOY_UISCRIPT_H
#define KOBOY_UISCRIPT_H
#include "koboy.h"

/* Replays synthetic input states into koboy's list screens, so a bounded,
   unattended run can reach them: the startup flow (MAIN MENU, then RECENT or
   ALL GAMES) and, since the `menu` verb, the IN-GAME MENU as well.

   The in-game menu needed a verb of its own because it is the one screen that
   is not entered by tapping a row. Every other screen follows from a
   selection on the one before it, so a flat sequence of taps walks the whole
   chain; MODE_MENU is entered by ASKING for it, from inside the emulator
   loop, which run_list never sees. `menu` is that ask.

   run_slot_picker (SAVE/LOAD STATE) is reachable from here in principle -- it
   is an ordinary run_list screen hanging off the menu -- but nothing scripts
   it yet, so no claim is made about it. MENU -> CHOOSE ROM, on the other
   hand, IS driven: the mid-session MAIN MENU, RECENT list and browser take
   the same cursor, which is what gives the mid-session ROM load its first
   automated coverage.

   This exists because of a recorded v1 failure: the first-run deadlock was
   invisible to twenty reviews because the scripted-run branch skipped
   calibration, so every automated test took the one path that could not reach
   the bug. Every existing smoke test passes --rom and would therefore skip the
   browser entirely. When a code path exists only for scripted runs, ask what
   it is hiding.

   Grammar, one verb per line; `#` starts a comment:
     tap X Y   press at panel (X, Y), then release   -> 2 states
     key a     press A (page previous), then release -> 2 states
     key b     press B (page next), then release     -> 2 states
     idle N    N states with nothing pressed         -> N states
     menu      open the in-game MENU                 -> 1 state

   `menu` is only honoured once the run is IN the emulator loop -- after a ROM
   has been chosen, or straight away under --rom. Placed earlier it is
   consumed by whichever list screen is running as an idle frame, which is
   deliberate: a cleared state is a harmless no-op, where a marker that
   survived to be re-read later would open the menu at a moment the script
   author did not write. */

#define UISCRIPT_MAX 256

/* True if `st` is a state with NOTHING pressed -- no button, no finger. That
   is what `idle` writes, what the release half of every `tap` and `key`
   writes, and what a `menu` marker's own (never-read) slot holds.

   It exists because the emulator loop is the one consumer that cannot use
   such a state for anything: it drives no list, so a cleared slot sitting at
   the cursor is not an idle frame there, it is a wall. Every list screen
   selects on touch-DOWN and leaves the matching release behind, so after a
   script walks MAIN MENU -> ALL GAMES -> a ROM, the cursor is parked on that
   release -- and a `menu` verb written after it could never fire, because
   the loop only ever advances the cursor past a marker. Skipping cleared
   states while LOOKING for a marker is what lets one script open the in-game
   MENU more than once, which is what a test that switches games twice needs.
   It cannot swallow a tap or a key: those states have something pressed. */
bool uiscript_state_is_idle(const koboy_input_state *st);

/* Returns the number of states written, or -1 on any error: a missing file, an
   unknown verb, a malformed argument, or a NULL array. An error must fail the
   run rather than silently pass a test that exercised nothing.

   `is_menu` is a PARALLEL array of the same `max` length: is_menu[i] is 1 iff
   state i came from a `menu` verb. Parallel rather than a field on
   koboy_input_state, because that struct is what the real device hands the
   emulator every frame: a marker living inside it would be a field the
   hardware path could in principle set, and "the touchscreen opened the menu
   by accident" is a bug nobody would look for. Kept outside, the marker
   exists only where the script does. Both pointers are required. */
int uiscript_load(const char *path, koboy_input_state *out,
                  unsigned char *is_menu, int max);
#endif
