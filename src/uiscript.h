#ifndef KOBOY_UISCRIPT_H
#define KOBOY_UISCRIPT_H
#include "koboy.h"

/* Replays synthetic input states into the ROM browser, so a bounded,
   unattended run can reach MODE_BROWSE.

   MODE_MENU is NOT reachable this way, and saying otherwise here would
   overclaim the suite's coverage: no call site passes a script to run_menu or
   run_slot_picker (src/main.c), because both are only ever entered from
   inside the emulator loop and that loop has no --ui-script hook. Driving the
   in-game menu unattended is deferred, not done.

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
     idle N    N states with nothing pressed         -> N states */

#define UISCRIPT_MAX 256

/* Returns the number of states written, or -1 on any error: a missing file, an
   unknown verb, or a malformed argument. An error must fail the run rather
   than silently pass a test that exercised nothing. */
int uiscript_load(const char *path, koboy_input_state *out, int max);
#endif
