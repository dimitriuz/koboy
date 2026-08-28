#ifndef KOBOY_TESTS_FAKEPLAT_H
#define KOBOY_TESTS_FAKEPLAT_H
/* A koboy_platform a test can assert against.
 *
 * WHY IT EXISTS: until screens.c was split out, everything taking a
 * koboy_platform * lived in the one file no test binary links, so the only
 * instrument that could reach a screen was tests/smoke_host.sh reading an exit
 * code and a log line. That cannot say what was DRAWN, or that the panel was
 * refreshed FULL rather than FAST, or how many times.
 *
 * A HEADER OF static FUNCTIONS AND NOT A .c FILE, forced rather than chosen:
 * the Makefile's test rule compiles `$< $(SRC)`, so a shared .c under tests/
 * is never compiled into anything. tests/test.h and tests/pgm.h are the same
 * shape for the same reason.
 *
 * DELIBERATELY DUMB. A fake platform that grows behaviour becomes a second
 * implementation nobody maintains, and one that LIES is worse than none -- so
 * it records and bounds and decides nothing. Every field below is read by a
 * live assertion in tests/test_screens.c.
 *
 * THE BOUND IS THE POINT OF should_quit: screen_list's unscripted branch polls
 * until something is selected or the platform says stop, so a test that forgot
 * to arm `quit_after_polls` would not fail, it would HANG. fakeplat_init arms
 * it by default and a test has to go out of its way to unarm it. */
#include "input.h"          /* must precede platform_if.h: declares struct koboy_input */
#include "platform_if.h"

#include <stdlib.h>
#include <string.h>

#define FAKEPLAT_MAX_REFRESH 128

typedef struct { int x, y, w, h; koboy_refresh_mode mode; } fakeplat_refresh;

typedef struct fakeplat {
    koboy_platform pf;              /* the vtable; pass &fp->pf to a screen */

    int      pw, ph, stride;
    uint8_t *panel;                 /* what blit_gray8 was handed, composited */

    int      blits;
    fakeplat_refresh refresh[FAKEPLAT_MAX_REFRESH];
    int      refreshes;             /* may exceed FAKEPLAT_MAX_REFRESH; the
                                       array stops recording, the count does
                                       not, so an overflow is visible */
    int      polls;
    int      quit_after_polls;      /* should_quit() once polls >= this */
    uint64_t clock_us;

    /* Called on each poll_input with the poll's 0-based index, so a test can
       drive the REAL input.c decode (input_feed) rather than hand-building a
       koboy_input_state -- which is exactly what hid the faceplate-versus-list
       bug tests/test_ui.c was later written for. NULL feeds nothing. */
    void (*on_poll)(struct fakeplat *fp, koboy_input *in, int poll);
    void  *ud;
} fakeplat;

static bool fakeplat_init_cb(void *ctx, const koboy_config *c)
{ (void)ctx; (void)c; return true; }

static void fakeplat_shutdown_cb(void *ctx) { (void)ctx; }

static void fakeplat_screen_info_cb(void *ctx, int *w, int *h)
{
    fakeplat *fp = (fakeplat *)ctx;
    if (w) *w = fp->pw;
    if (h) *h = fp->ph;
}

static bool fakeplat_blit_cb(void *ctx, const uint8_t *px, int w, int h,
                             int stride, int x, int y)
{
    fakeplat *fp = (fakeplat *)ctx;
    fp->blits++;
    /* Copied row by row at the offset given rather than memcpy'd wholesale,
       because a fake that ignored x/y would report a correct panel for a blit
       that landed in the wrong place -- and the mutant for this header is
       precisely an off-by-one here. */
    for (int r = 0; r < h; r++) {
        int py = y + r;
        if (py < 0 || py >= fp->ph) continue;
        for (int c = 0; c < w; c++) {
            int pxx = x + c;
            if (pxx < 0 || pxx >= fp->pw) continue;
            fp->panel[(size_t)py * (size_t)fp->stride + (size_t)pxx] =
                px[(size_t)r * (size_t)stride + (size_t)c];
        }
    }
    return true;
}

static bool fakeplat_refresh_cb(void *ctx, int x, int y, int w, int h,
                                koboy_refresh_mode mode)
{
    fakeplat *fp = (fakeplat *)ctx;
    if (fp->refreshes < FAKEPLAT_MAX_REFRESH) {
        fakeplat_refresh *r = &fp->refresh[fp->refreshes];
        r->x = x; r->y = y; r->w = w; r->h = h; r->mode = mode;
    }
    fp->refreshes++;
    return true;
}

static bool fakeplat_poll_cb(void *ctx, struct koboy_input *in)
{
    fakeplat *fp = (fakeplat *)ctx;
    int n = fp->polls++;
    if (fp->on_poll) fp->on_poll(fp, in, n);
    return true;
}

static uint64_t fakeplat_now_cb(void *ctx)
{
    fakeplat *fp = (fakeplat *)ctx;
    fp->clock_us += 1000;      /* a settable clock that always advances */
    return fp->clock_us;
}

static bool fakeplat_should_quit_cb(void *ctx)
{
    fakeplat *fp = (fakeplat *)ctx;
    return fp->quit_after_polls > 0 && fp->polls >= fp->quit_after_polls;
}

/* Allocates the panel buffer; call fakeplat_free when done. `stride` is the
   panel width plus a deliberate margin, because a stride equal to the width
   makes a row-indexing bug invisible. */
static void fakeplat_init(fakeplat *fp, int pw, int ph)
{
    memset(fp, 0, sizeof *fp);
    fp->pw = pw;
    fp->ph = ph;
    fp->stride = pw + 16;
    fp->panel = (uint8_t *)calloc((size_t)fp->stride * (size_t)ph, 1);
    fp->quit_after_polls = 64;      /* armed by default: see the header note */

    fp->pf.ctx             = fp;
    fp->pf.init            = fakeplat_init_cb;
    fp->pf.shutdown        = fakeplat_shutdown_cb;
    fp->pf.screen_info     = fakeplat_screen_info_cb;
    fp->pf.blit_gray8      = fakeplat_blit_cb;
    fp->pf.refresh         = fakeplat_refresh_cb;
    fp->pf.poll_input      = fakeplat_poll_cb;
    fp->pf.now_us          = fakeplat_now_cb;
    fp->pf.should_quit     = fakeplat_should_quit_cb;
    /* The three optional entries stay NULL, which is the SDL backend's own
       answer for battery_percent and is what every caller must tolerate. */
}

static void fakeplat_free(fakeplat *fp) { free(fp->panel); fp->panel = NULL; }

/* Forget what was recorded without rebuilding the panel, for a test that
   drives several screens in a row. */
static void fakeplat_reset_log(fakeplat *fp)
{ fp->blits = 0; fp->refreshes = 0; fp->polls = 0; }

#endif
