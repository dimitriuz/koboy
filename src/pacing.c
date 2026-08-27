#include "pacing.h"

/* The plausibility bound on a core's reported fps. See pacer_frame_us_from_fps
   in pacing.h for why these two numbers and not tighter or looser ones. */
#define KOBOY_FPS_MIN  10.0
#define KOBOY_FPS_MAX 300.0

/* Contract in pacing.h. The clamp is HERE and only here, so pacer_init and the
   in-game FRAMES entry cannot disagree about what a nonsense divisor means. */
void pacer_set_divisor(koboy_pacer *p, int divisor)
{
    p->divisor = divisor > 0 ? divisor : 1;
}

void pacer_init(koboy_pacer *p, uint64_t now_us, int divisor, uint32_t frame_us)
{
    p->start_us = now_us;
    p->frames = 0;
    pacer_set_divisor(p, divisor);
    p->frame_us = frame_us ? frame_us : (uint32_t)KOBOY_FRAME_US;
}

/* Contract in pacing.h. start_us is walked BACK by the frames already run, not
   simply set to now_us: pacer_delay_us measures from start_us + frames * frame
   time, so assigning now_us directly with a nonzero counter would ask the
   caller to sleep for the entire history of the run before the next frame.
   The clamp is for a synthetic clock that has not yet reached that offset --
   real callers pass a monotonic clock measured from boot, so it never binds
   on the device, but a test with now_us near zero would otherwise wrap. */
void pacer_rebase(koboy_pacer *p, uint64_t now_us)
{
    uint64_t elapsed = p->frames * (uint64_t)p->frame_us;
    p->start_us = now_us >= elapsed ? now_us - elapsed : 0;
}

/* Contract in pacing.h. */
void pacer_set_frame_us(koboy_pacer *p, uint64_t now_us, uint32_t frame_us)
{
    uint32_t fu = frame_us ? frame_us : (uint32_t)KOBOY_FRAME_US;
    if (fu == p->frame_us) return;
    p->frame_us = fu;
    pacer_rebase(p, now_us);
}

/* Contract in pacing.h. */
uint32_t pacer_frame_us_from_fps(double fps)
{
    if (!(fps >= KOBOY_FPS_MIN && fps <= KOBOY_FPS_MAX)) return (uint32_t)KOBOY_FRAME_US;
    /* TRUNCATED, not rounded, and that choice is the Game Boy's.

       gambatte reports 59.7275 fps and 1e6/59.7275 is 16742.73 -- so rounding
       gives 16743 and truncation gives 16742, which is exactly the
       KOBOY_FRAME_US this project has run on a real Libra 2 through a full
       playthrough. MEASURED, not assumed: the first version of this function
       rounded, and tests/test_pacing.c's Game Boy assertion caught it at
       16743. The constant was itself produced by truncating, so truncating is
       what reproduces it without a per-core special case.

       What truncation costs is at most one microsecond per frame -- 0.006% at
       60 Hz, sixty microseconds of drift per wall-clock second, which is four
       orders of magnitude below the panel refresh this pacer feeds. What
       rounding would cost is the one presentation anybody has verified on
       hardware. */
    return (uint32_t)(1000000.0 / fps);
}

/* Microseconds until the next core frame is due. The core is paced to the rate
   ITS OWN core reports regardless of how rarely we present, so games run at the
   correct speed on a panel that cannot match their frame rate. */
uint64_t pacer_delay_us(const koboy_pacer *p, uint64_t now_us)
{
    uint64_t due = p->start_us + p->frames * (uint64_t)p->frame_us;
    return now_us >= due ? 0 : due - now_us;
}

bool pacer_tick(koboy_pacer *p)
{
    bool present = (p->frames % (uint64_t)p->divisor) == 0;
    p->frames++;
    return present;
}
