#ifndef KOBOY_PACING_H
#define KOBOY_PACING_H
#include "koboy.h"

typedef struct { uint64_t start_us, frames; int divisor; } koboy_pacer;

void     pacer_init(koboy_pacer *p, uint64_t now_us, int divisor);

/* Re-anchor the wall clock to `now_us` WITHOUT touching the frame counter.

   For coming back from a UI mode. Rebasing the clock is necessary -- a menu
   that was open for thirty seconds leaves the pacer thirty seconds behind, and
   pacer_delay_us would then return 0 for the next eighteen hundred frames
   while the core sprints to catch up. Zeroing `frames` is NOT: main.c's
   bounded-run test is `pace.frames >= frame_limit`, so pacer_init on menu exit
   restarted a --frames N budget every single time the menu closed, and an
   unattended run could never terminate as long as the menu kept being opened.
   Keeping the count also keeps the present-divisor phase continuous. */
void     pacer_rebase(koboy_pacer *p, uint64_t now_us);
uint64_t pacer_delay_us(const koboy_pacer *p, uint64_t now_us);
bool     pacer_tick(koboy_pacer *p);
#endif
