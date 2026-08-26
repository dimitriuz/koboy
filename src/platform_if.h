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
} koboy_platform;
#endif
