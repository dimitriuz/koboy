#ifndef KOBOY_UISCRIPT_H
#define KOBOY_UISCRIPT_H
#include "koboy.h"

/* Replays synthetic input states into the UI modes, so a bounded, unattended
   run can reach MODE_BROWSE and MODE_MENU.

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
