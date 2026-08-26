#include "pacing.h"

void pacer_init(koboy_pacer *p, uint64_t now_us, int divisor)
{
    p->start_us = now_us;
    p->frames = 0;
    p->divisor = divisor > 0 ? divisor : 1;
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
    uint64_t elapsed = p->frames * (uint64_t)KOBOY_FRAME_US;
    p->start_us = now_us >= elapsed ? now_us - elapsed : 0;
}

/* Microseconds until the next core frame is due. The core is paced to true
   60Hz wall-clock regardless of how rarely we present, so games run at the
   correct speed on a panel that cannot match their frame rate. */
uint64_t pacer_delay_us(const koboy_pacer *p, uint64_t now_us)
{
    uint64_t due = p->start_us + p->frames * (uint64_t)KOBOY_FRAME_US;
    return now_us >= due ? 0 : due - now_us;
}

bool pacer_tick(koboy_pacer *p)
{
    bool present = (p->frames % (uint64_t)p->divisor) == 0;
    p->frames++;
    return present;
}
