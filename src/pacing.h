#ifndef KOBOY_PACING_H
#define KOBOY_PACING_H
#include "koboy.h"

/* frame_us is per-PACER, and that fixed a BUG rather than generalising: 16742
   us is the GAME BOY's frame time (59.7275 Hz) and koboy paced every system to
   it. MEASURED from the cores: PokeMini 72 fps, WonderSwan 75.4717, PAL Atari
   2600 49.92, NTSC 59.92, and across FBNeo's 227 boards everything from 30
   (Tapper, Popeye -- DOUBLE speed at 60) to 60. docs/FOLLOWUPS.md #38, #57. */
typedef struct {
    uint64_t start_us, frames;
    int      divisor;
    uint32_t frame_us;
    /* Wall clock before which the panel is still working on the last update,
       so presenting again photographs a transition rather than a picture.
       0 = nothing outstanding. See pacer_settle_us. */
    uint64_t hold_until_us;
    /* The earliest FRAME INDEX the divisor allows a present at: frames +
       divisor on every present.

       This replaced `frames % divisor == 0` because of the settle hold. Under a
       modulo the eligible frames sit on a fixed LATTICE, so a hold expiring one
       frame after a lattice point costs the whole remaining stride -- a 150 ms
       hold against divisor 4 (66.97 ms) presents at 200.9 ms, not 150, a third
       of the rate thrown away to phase alone. A minimum GAP has no lattice, and
       the divisor still bounds the rate from above because the presenting
       frame is by construction at least `divisor` frames after the last.

       With no hold outstanding the two are indistinguishable. A FRAME count and
       not a wall clock, deliberately: the clock is jittery enough here that a
       `now - last >= interval` test would slip a frame at random, and
       tests/smoke_host.sh asserts exact presented counts. */
    uint64_t next_frame;
    /* What the last pacer_presented() charged, kept only so main.c can print
       it and the throttle can be seen working rather than inferred. */
    uint32_t last_settle_us;
    /* How many DIVISOR-ELIGIBLE frames the settle hold vetoed. Here rather
       than in main.c, which cannot tell a held frame from a divisor-skipped one
       without recomputing the test. NOT merely diagnostic: it is the run
       summary's only evidence that area pacing did anything -- a build where
       the hold never binds and one where it is absent print the same
       presented= count and differ only here. */
    uint64_t held;
} koboy_pacer;

/* frame_us of 0 means "the Game Boy's rate". A CLAMP, not a convenience:
   pacer_delay_us multiplies by frame_us, so a zero makes every frame due at
   start_us -- an unpaced spin. */
void     pacer_init(koboy_pacer *p, uint64_t now_us, int divisor, uint32_t frame_us);

/* Change how many core frames pass per presented frame, on a RUNNING pacer --
   the in-game FRAMES entry, which has to be changeable while the game is on
   the panel. NOT a plain assignment at the call site: `divisor > 0 ? divisor
   : 1` is the guard keeping pacer_tick from dividing by zero, and a second
   copy is how that guard eventually goes missing from one of them.

   Does NOT rebase, and must not: the pacer's clock is
   start_us + frames * frame_us, which the divisor does not appear in. The
   PHASE does change -- raising it mid-run can delay presentation by up to
   divisor - 1 core frames (134 ms at the top of the ladder), which is
   invisible next to the panel's own latency. */
void     pacer_set_divisor(koboy_pacer *p, int divisor);

/* Re-anchor the wall clock to `now_us` WITHOUT touching the frame counter,
   for coming back from a UI mode. Rebasing is necessary -- a menu open for
   thirty seconds leaves the pacer thirty seconds behind and pacer_delay_us
   returns 0 for the next eighteen hundred frames while the core sprints.
   Zeroing `frames` is NOT: main.c's bounded-run test is
   `pace.frames >= frame_limit`, so pacer_init on menu exit restarted a
   --frames N budget every time the menu closed. */
void     pacer_rebase(koboy_pacer *p, uint64_t now_us);

/* Change the frame time of a RUNNING pacer, for a core that re-announces its
   timing mid-session through SET_SYSTEM_AV_INFO (src/core.c).

   Rebasing is NOT optional here, which is why now_us is a parameter: the
   model is start_us + frames * frame_us, so changing frame_us with a counter
   in the thousands moves the next frame's due time by frames * (new - old) --
   minutes, either direction. A no-op when the rate has not changed, so a
   caller may poll it every frame. */
void     pacer_set_frame_us(koboy_pacer *p, uint64_t now_us, uint32_t frame_us);

/* The core's reported retro_system_timing.fps turned into microseconds per
   frame, or KOBOY_FRAME_US for a value that is not a frame rate.

   THE RANGE IS A PLAUSIBILITY BOUND, not a capability one. Everything measured
   sits between 30 fps (Tapper) and 75.4717 (WonderSwan); [10, 300] is a factor
   of three clear of both ends, and outside it the number is far likelier to be
   an uninitialised field or a sentinel than a real rate. A core reporting 0 --
   what an unfilled av_info looks like -- would divide into a frame time of
   forever.

   Truncates rather than rounds; pacing.c's return statement says why.

   A NEGATED range test on purpose: `!(fps >= lo && fps <= hi)` rejects NaN,
   which `fps < lo || fps > hi` would let through (every NaN comparison is
   false) and which then propagates into an undefined cast. */
uint32_t pacer_frame_us_from_fps(double fps);

uint64_t pacer_delay_us(const koboy_pacer *p, uint64_t now_us);

/* Advance the core-frame counter and say whether this frame may be presented.
   The divisor sets the minimum GAP; the settle hold decides whether the panel
   is ready.

   THE FRAME COUNTER ADVANCES EITHER WAY: main.c's bounded-run test is
   `pace.frames >= frame_limit`, so a held frame that did not increment would
   make `--frames N` unbounded on exactly the content that holds most. */
bool     pacer_tick(koboy_pacer *p, uint64_t now_us);

/* How long the panel needs to settle an update covering `dirty_px` of a game
   rect of `whole_px`, in microseconds.

   THE MODEL IS AFFINE IN AREA, both halves MEASURED. `base_us` is the fixed
   cost every update pays whatever its size -- roughly 40% of a full-rect DU4
   refresh in Appendix A's region sweep, and it does not shrink with the
   rectangle. `full_us` is what the area term ADDS at a whole-game-area rect.
   So a whole-screen scroll is charged base + full, a tenth of the screen
   base + full/10, a two-tile sprite move essentially base alone.

   Against the GAME RECT rather than per pixel on purpose: the shipped systems'
   rects differ by more than 2x in area, so a per-pixel constant would mean
   something different on each. The owner tunes these in ms against "what a
   full screen costs", which is what they can see.

   Both 0 returns 0, which is how the pacer is turned off; whole_px <= 0 gets
   base_us alone. SATURATES at UINT32_MAX rather than wrapping -- config bounds
   the inputs long before that, and this is the arithmetic that would otherwise
   turn a fat-fingered ini into a freeze. */
uint32_t pacer_settle_us(uint32_t base_us, uint32_t full_us,
                         long dirty_px, long whole_px);

/* Record that a frame WAS presented and charge the panel time it costs. Called
   after the refresh is submitted, with the total dirty area actually sent.
   settle_us == 0 clears the hold, which is what a disabled model does. */
void     pacer_presented(koboy_pacer *p, uint64_t now_us, uint32_t settle_us);
#endif
