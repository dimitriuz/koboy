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
    p->hold_until_us = 0;
    p->last_settle_us = 0;
    p->held = 0;
    /* 0, so the first frame of a session presents: it is certainly full-dirty
       (video_create seeds prev to force it) and the panel is certainly idle. */
    p->next_frame = 0;
}

/* Contract in pacing.h. start_us is walked BACK by the frames already run
   rather than set to now_us: pacer_delay_us measures from
   start_us + frames * frame_us, so assigning directly would ask the caller to
   sleep for the run's entire history. The clamp is for a synthetic clock that
   has not reached that offset -- it never binds on a monotonic device clock,
   but a test with now_us near zero would wrap. */
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
    /* TRUNCATED, not rounded, and the choice is the Game Boy's: gambatte
       reports 59.7275 fps, 1e6/59.7275 is 16742.73, so rounding gives 16743
       and truncation gives 16742 -- exactly the KOBOY_FRAME_US that ran a full
       playthrough on a real Libra 2. The first version rounded and
       tests/test_pacing.c caught it at 16743.
       Truncation costs at most one microsecond per frame (0.006% at 60 Hz,
       four orders of magnitude below the panel refresh this feeds). Rounding
       costs the one presentation anybody has verified on hardware. */
    return (uint32_t)(1000000.0 / fps);
}

/* Microseconds until the next core frame is due. The core is paced to ITS OWN
   reported rate regardless of how rarely we present, so games run at the
   correct speed on a panel that cannot match their frame rate. */
uint64_t pacer_delay_us(const koboy_pacer *p, uint64_t now_us)
{
    uint64_t due = p->start_us + p->frames * (uint64_t)p->frame_us;
    return now_us >= due ? 0 : due - now_us;
}

/* Contract in pacing.h. Integer throughout, MULTIPLY BEFORE DIVIDE: the
   product fits a uint64_t with forty bits to spare, while dividing first would
   quantise every update smaller than the game rect to zero. */
uint32_t pacer_settle_us(uint32_t base_us, uint32_t full_us,
                         long dirty_px, long whole_px)
{
    uint64_t area_us = 0;
    if (full_us && whole_px > 0 && dirty_px > 0) {
        /* LIVE CLAMP: video_split_dirty may emit overlapping rects, so a
           summed dirty area CAN exceed the game rect and would otherwise
           charge more than a full screen for a screen. */
        uint64_t d = (uint64_t)dirty_px, w = (uint64_t)whole_px;
        if (d > w) d = w;
        area_us = ((uint64_t)full_us * d) / w;
    }
    uint64_t total = (uint64_t)base_us + area_us;
    return total > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)total;
}

/* Contract in pacing.h. */
void pacer_presented(koboy_pacer *p, uint64_t now_us, uint32_t settle_us)
{
    p->last_settle_us = settle_us;
    p->hold_until_us = settle_us ? now_us + settle_us : 0;
}

/* Contract in pacing.h.

   THE TWO GATES ARE AND-ED, NOT MAX-ED, which keeps present_divisor a CEILING
   rather than a target: the divisor sets the minimum gap, the hold vetoes a
   frame whose panel is still busy, and the next frame after gets through. The
   delivered rate is never faster than the divisor allows nor than the panel
   can finish -- what the owner produced by hand at divisor 8, except that here
   it lasts only as long as the large updates do. */
bool pacer_tick(koboy_pacer *p, uint64_t now_us)
{
    bool present = p->frames >= p->next_frame;
    if (present && p->hold_until_us && now_us < p->hold_until_us) {
        present = false;
        p->held++;
    }
    /* Advanced HERE and NOT in pacer_presented, and the difference is a real
       bug: main.c does not call pacer_presented for every frame this returns
       true for -- an unchanged frame produces zero rects and exits early. If
       the gate advanced on presentation, a static screen would leave
       next_frame behind forever and video_submit_rects (the measured 17 ms
       bottleneck) would run on EVERY core frame. The gate is about how often
       we LOOK, and looking is what costs; the hold is about what the panel is
       doing. */
    if (present) p->next_frame = p->frames + (uint64_t)p->divisor;
    p->frames++;
    return present;
}
