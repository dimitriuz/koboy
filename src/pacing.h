#ifndef KOBOY_PACING_H
#define KOBOY_PACING_H
#include "koboy.h"

/* frame_us is per-PACER, not the KOBOY_FRAME_US constant it used to be, and
   the difference is a bug rather than a generalisation: 16742 us is the GAME
   BOY's frame time (59.7275 Hz) and koboy paced every one of eleven systems
   to it. Measured, from the cores themselves: PokeMini 72 fps, WonderSwan
   75.4717, PAL Atari 2600 49.92, NTSC 59.92, and across FinalBurn Neo's 227
   boards everything from 30 (Tapper, Popeye -- DOUBLE speed at 60) to 60.
   See docs/FOLLOWUPS.md #38 and #57. */
typedef struct { uint64_t start_us, frames; int divisor; uint32_t frame_us; } koboy_pacer;

/* frame_us of 0 means "the Game Boy's rate", which is what every caller got
   implicitly before this parameter existed. It is a clamp and not a
   convenience: pacer_delay_us divides nothing but MULTIPLIES by frame_us, and
   a zero would make every frame due at start_us -- an unpaced spin. */
void     pacer_init(koboy_pacer *p, uint64_t now_us, int divisor, uint32_t frame_us);

/* Change how many core frames pass per presented frame, on a RUNNING pacer.

   For the in-game FRAMES entry (src/main.c), which is a subjective judgement
   about how a reflective panel looks in motion and therefore has to be
   changeable while the game is on the panel. Deliberately NOT a plain
   assignment at the call site: `divisor > 0 ? divisor : 1` is the guard that
   keeps pacer_tick's `frames % divisor` from dividing by zero, and a second
   copy of it at a second call site is the way that guard eventually goes
   missing from one of them.

   Unlike pacer_set_frame_us this does NOT rebase, and must not: the pacer's
   wall clock is start_us + frames * frame_us, which the divisor does not
   appear in at all -- it selects which of those core frames reach the panel,
   not when any of them are due. What DOES change is the phase: the next
   presented frame is the next one whose index is a multiple of the new
   divisor, so raising it mid-run can delay presentation by up to
   divisor - 1 core frames (134 ms at the top of the shipped ladder, 60 Hz).
   That is invisible next to the panel's own latency, and main.c's
   return-from-menu path repaints unconditionally anyway. */
void     pacer_set_divisor(koboy_pacer *p, int divisor);

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

/* Change the frame time of a RUNNING pacer, for a core that re-announces its
   timing mid-session through SET_SYSTEM_AV_INFO (src/core.c).

   Rebasing is not optional here, which is why now_us is a parameter rather
   than this being a plain setter: the pacer's model is
   start_us + frames * frame_us, so changing frame_us with a frame counter in
   the thousands moves the due time of the NEXT frame by
   frames * (new - old) -- minutes, in either direction, for a run of any
   length. Rebasing pins the next frame to now and lets the new rate govern
   from there. A no-op when the rate has not actually changed, so a caller may
   poll this every frame; main.c calls it only from the geometry-changed
   branch, since every timing announcement sets that flag too. */
void     pacer_set_frame_us(koboy_pacer *p, uint64_t now_us, uint32_t frame_us);

/* The core's reported retro_system_timing.fps turned into microseconds per
   frame, or KOBOY_FRAME_US for a value that is not a frame rate.

   THE RANGE IS A PLAUSIBILITY BOUND, not a capability one. Everything koboy
   has ever measured sits between 30 fps (Tapper) and 75.4717 (WonderSwan);
   [10, 300] is a factor of three clear of both ends, and outside it the
   number is far more likely to be an uninitialised field, a units mistake or
   a sentinel than a rate any emulated machine runs at. A core reporting 0 --
   the case that matters, because it is what an av_info nobody filled in looks
   like -- would otherwise divide into a frame time of forever.

   Truncates rather than rounds; the reason is the Game Boy, and it is spelt
   out at the return statement in pacing.c.

   Written as a NEGATED range test on purpose: `!(fps >= lo && fps <= hi)`
   rejects NaN, which `fps < lo || fps > hi` would let through (every
   comparison against NaN is false) and which would then propagate through the
   division into an undefined cast. */
uint32_t pacer_frame_us_from_fps(double fps);

uint64_t pacer_delay_us(const koboy_pacer *p, uint64_t now_us);
bool     pacer_tick(koboy_pacer *p);
#endif
