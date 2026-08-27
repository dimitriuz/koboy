#ifndef KOBOY_PLATFORM_IF_H
#define KOBOY_PLATFORM_IF_H
#include "koboy.h"
#include "config.h"

typedef struct koboy_platform {
    void *ctx;
    bool     (*init)(void *ctx, const koboy_config *c);
    void     (*shutdown)(void *ctx);
    void     (*screen_info)(void *ctx, int *w, int *h);
    /* Blit a gray8 rectangle to panel coordinates (x, y). */
    bool     (*blit_gray8)(void *ctx, const uint8_t *px, int w, int h,
                           int stride, int x, int y);
    /* Request a panel refresh. Must not block waiting for completion. */
    bool     (*refresh)(void *ctx, int x, int y, int w, int h,
                        koboy_refresh_mode mode);
    /* Non-blocking: drain pending input into the emulator's input state. */
    bool     (*poll_input)(void *ctx, struct koboy_input *in);
    uint64_t (*now_us)(void *ctx);
    bool     (*should_quit)(void *ctx);
    /* Device battery, 0..100, or -1 when unknown. Optional: the SDL backend
       has no battery, and an unseen Kobo may not expose one either. Read only
       when the whole panel is already being repainted, so the faceplate keeps
       its zero-per-frame-cost property and needs no timer. */
    int      (*battery_percent)(void *ctx);
    /* Re-pick the waveform KOBOY_REFRESH_FAST asks for, on a running session.
       Optional -- callers must null-check -- but implemented by BOTH backends,
       because "the menu row reached the platform" is the one link of this
       chain a host test can observe.

       It exists because the in-game MOTION entry is a judgement about a
       reflective panel in motion, which can only be made while looking at the
       game: a policy that took effect on the next LAUNCH would be a policy
       nobody ever compares. The caller is responsible for putting a whole
       frame back afterwards (main.c's return-from-menu path already does
       redraw_chrome + video_invalidate), because a waveform change says
       nothing about what the panel is currently holding. */
    void     (*set_wfm_policy)(void *ctx, koboy_wfm_policy policy);
    /* The name of the waveform FAST refreshes are ASKING FOR right now, e.g.
       "AUTO", "DU4", "DU" -- never NULL. Read back off the platform rather
       than off koboy_config for the same reason main.c reads the divisor off
       the live pacer: a line reporting what config.c parsed proves config.c,
       while this one fails if the policy never reached the backend.

       On the Kobo it is the REAL mapped waveform, which is strictly more than
       an echo -- a DU4 request on a panel without the eclipse quirk maps to
       A2, and this is what says so. On SDL it is the policy it was handed,
       because a monitor has no waveforms at all and inventing one would be
       device theatre. */
    const char *(*wfm_fast_name)(void *ctx);
} koboy_platform;
#endif
