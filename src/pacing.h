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
typedef struct {
    uint64_t start_us, frames;
    int      divisor;
    uint32_t frame_us;
    /* Wall clock before which the panel is still working on the last update we
       sent it, so presenting again would photograph a transition rather than a
       picture. 0 means "nothing outstanding". See pacer_settle_us. */
    uint64_t hold_until_us;
    /* The earliest FRAME INDEX the divisor will allow a present at: set to
       frames + divisor on every present.

       This replaced `frames % divisor == 0`, and the reason is the settle
       hold. Under a modulo the eligible frames sit on a fixed lattice, so a
       hold that expires one frame after a lattice point costs the whole
       remaining stride -- a 150 ms hold against divisor 4 (66.97 ms) does not
       present at 150 ms, it presents at 200.9 ms, a third of the rate thrown
       away to phase alone. A minimum GAP has no lattice: the next frame at or
       after the hold expires presents, and the divisor still bounds the rate
       from above because that frame is by construction at least `divisor`
       frames after the last one.

       With no hold outstanding the two are indistinguishable -- presents land
       on 0, divisor, 2*divisor, ... exactly as before -- which is why the
       divisor's own assertions did not have to change. It is a FRAME count and
       not a wall clock deliberately: the clock is already jittery enough on a
       device this slow that a `now - last >= interval` test would slip a frame
       at random, and tests/smoke_host.sh asserts exact presented counts. */
    uint64_t next_frame;
    /* What the last pacer_presented() charged, kept only so main.c can print
       it and the owner can see the throttle working rather than infer it. */
    uint32_t last_settle_us;
    /* How many DIVISOR-ELIGIBLE frames the settle hold vetoed. Counted here
       rather than in main.c because main.c cannot tell a held frame from a
       divisor-skipped one without recomputing the modulo, and a second copy of
       that test is a second thing to get wrong. It is the run summary's only
       evidence that area pacing did anything, so it is not merely diagnostic:
       a build where the hold never binds and one where it is absent print the
       same presented= count and differ only here. */
    uint64_t held;
} koboy_pacer;

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

/* Advance the core-frame counter and say whether this frame may be presented.
   `now_us` is what makes this area-aware: the divisor sets the minimum GAP
   between presents, and the settle hold decides whether the panel is ready for
   the next one.

   THE FRAME COUNTER ADVANCES EITHER WAY. main.c's bounded-run test is
   `pace.frames >= frame_limit`, so a held frame that did not increment would
   make `--frames N` unbounded on exactly the content that holds most. */
bool     pacer_tick(koboy_pacer *p, uint64_t now_us);

/* How long the panel needs to settle an update covering `dirty_px` of a game
   rect of `whole_px`, in microseconds.

   THE MODEL IS AFFINE IN AREA, and both halves are measured rather than
   derived. `base_us` is the fixed cost every update pays whatever its size --
   the controller's own per-update overhead, which Appendix A's region sweep
   shows is roughly 40% of a full-rect DU4 refresh and does not shrink with the
   rectangle. `full_us` is what the area term ADDS at a rect covering the whole
   game area. So a whole-screen scroll is charged base + full, a tenth of the
   screen base + full/10, and a two-tile sprite move essentially base alone.

   Expressed against the GAME RECT rather than per pixel on purpose: koboy runs
   fourteen systems whose rects differ by more than 2x in area, and a per-pixel
   constant would silently mean something different on each of them. The owner
   tunes these in ms against "what a full screen costs", which is the thing
   they can see.

   Returns 0 when full_us and base_us are both 0, which is how the pacer is
   turned off; a caller that passes whole_px <= 0 gets base_us alone, since an
   unknown area cannot be charged for. Saturates at UINT32_MAX rather than
   wrapping -- config bounds the inputs long before that, and this is the
   arithmetic that would otherwise turn a fat-fingered ini into a freeze. */
uint32_t pacer_settle_us(uint32_t base_us, uint32_t full_us,
                         long dirty_px, long whole_px);

/* Record that a frame WAS presented, and charge the panel time it costs.

   Called after the refresh is submitted, with the total dirty area actually
   sent. Sets the hold pacer_tick tests. Passing settle_us == 0 clears the hold,
   which is what a build with the model disabled does every frame. */
void     pacer_presented(koboy_pacer *p, uint64_t now_us, uint32_t settle_us);
#endif
