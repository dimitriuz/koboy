/* Desktop backend for the platform seam.
 *
 * The point of this file is not to be a nice desktop port -- it is to drive
 * exactly the same code the Kobo backend will drive, so that everything above
 * the seam is debugged here before any device work happens. In particular
 * poll_input never computes a button itself: keyboard keys go through
 * input_feed_key() and the mouse is turned into a synthetic multitouch
 * protocol-B event stream fed through input_feed(), so the thumb-pad origin
 * latching, the deadzone/hysteresis logic and the A/B/Start/Select zone
 * hit-testing in input.c are the ones being exercised.
 */
#define _POSIX_C_SOURCE 200809L
#include "input.h"          /* must precede platform_if.h: declares struct koboy_input */
#include "platform_if.h"

#include <SDL.h>
#include <stdlib.h>
#include <string.h>

/* Default synthetic panel: the Clara-family geometry the chrome golden image
   was generated against. Overridable with --panel. */
#define SDL_PANEL_W 1072
#define SDL_PANEL_H 1448

#define RAWKEY_RING 32

/* Virtual touch slots. Slot 0 is the mouse (a real finger on the device);
   the rest are keyboard conveniences that still travel the touch path. */
#define SLOT_MOUSE  0
#define SLOT_DPAD   1
#define SLOT_START  2
#define SLOT_SELECT 3

typedef struct {
    koboy_config cfg;
    int          panel_w, panel_h;

    SDL_Window   *win;
    SDL_Renderer *ren;
    SDL_Texture  *tex;
    uint32_t     *argb;          /* full-panel ARGB8888 shadow of the panel */

    uint64_t perf_freq;
    bool     quit;

    bool mouse_down;

    /* keyboard direction state -> synthetic thumb-pad drag */
    bool kb_up, kb_down, kb_left, kb_right;
    bool kb_pad_down;
    bool kb_start_down, kb_select_down;

    /* raw key-down codes, consumed by first-run calibration only */
    uint16_t raw[RAWKEY_RING];
    int      raw_head, raw_tail;

    /* The fast-refresh waveform policy this backend has been HANDED. A monitor
       has no waveforms, so nothing here ever reads it to draw with -- it is
       recorded so that main.c can print it back and a host test can tell
       "the MOTION row reached the platform" from "the MOTION row updated a
       struct in main". That is the only link of the waveform chain the host
       can observe at all; the rest is panel-side and needs the device. */
    koboy_wfm_policy wfm_policy;
} sdl_ctx;

/* ------------------------------------------------------------------ keys */

/* SDL scancodes are translated to Linux evdev keycodes rather than used raw,
   so a koboy.ini calibrated here means the same thing as one calibrated on
   the device and calib.c's power-button rejection keeps working. */
static uint16_t sdl_to_evdev(SDL_Scancode sc)
{
    switch (sc) {
    case SDL_SCANCODE_A: return 30;  case SDL_SCANCODE_B: return 48;
    case SDL_SCANCODE_C: return 46;  case SDL_SCANCODE_D: return 32;
    case SDL_SCANCODE_E: return 18;  case SDL_SCANCODE_F: return 33;
    case SDL_SCANCODE_G: return 34;  case SDL_SCANCODE_H: return 35;
    case SDL_SCANCODE_I: return 23;  case SDL_SCANCODE_J: return 36;
    case SDL_SCANCODE_K: return 37;  case SDL_SCANCODE_L: return 38;
    case SDL_SCANCODE_M: return 50;  case SDL_SCANCODE_N: return 49;
    case SDL_SCANCODE_O: return 24;  case SDL_SCANCODE_P: return 25;
    case SDL_SCANCODE_Q: return 16;  case SDL_SCANCODE_R: return 19;
    case SDL_SCANCODE_S: return 31;  case SDL_SCANCODE_T: return 20;
    case SDL_SCANCODE_U: return 22;  case SDL_SCANCODE_V: return 47;
    case SDL_SCANCODE_W: return 17;  case SDL_SCANCODE_X: return 45;
    case SDL_SCANCODE_Y: return 21;  case SDL_SCANCODE_Z: return 44;
    case SDL_SCANCODE_1: return 2;   case SDL_SCANCODE_2: return 3;
    case SDL_SCANCODE_3: return 4;   case SDL_SCANCODE_4: return 5;
    case SDL_SCANCODE_5: return 6;   case SDL_SCANCODE_6: return 7;
    case SDL_SCANCODE_7: return 8;   case SDL_SCANCODE_8: return 9;
    case SDL_SCANCODE_9: return 10;  case SDL_SCANCODE_0: return 11;
    case SDL_SCANCODE_RETURN:    return 28;
    case SDL_SCANCODE_ESCAPE:    return 1;
    case SDL_SCANCODE_BACKSPACE: return 14;
    case SDL_SCANCODE_TAB:       return 15;
    case SDL_SCANCODE_SPACE:     return 57;
    case SDL_SCANCODE_LSHIFT:    return 42;
    case SDL_SCANCODE_RSHIFT:    return 54;
    case SDL_SCANCODE_LCTRL:     return 29;
    case SDL_SCANCODE_LALT:      return 56;
    case SDL_SCANCODE_UP:        return 103;
    case SDL_SCANCODE_LEFT:      return 105;
    case SDL_SCANCODE_RIGHT:     return 106;
    case SDL_SCANCODE_DOWN:      return 108;
    case SDL_SCANCODE_PAGEUP:    return 104;
    case SDL_SCANCODE_PAGEDOWN:  return 109;
    case SDL_SCANCODE_HOME:      return 102;
    case SDL_SCANCODE_END:       return 107;
    default:                     return 0;
    }
}

static void raw_push(sdl_ctx *s, uint16_t code)
{
    int nxt = (s->raw_head + 1) % RAWKEY_RING;
    if (nxt == s->raw_tail) return;       /* ring full: drop, calibration is slow */
    s->raw[s->raw_head] = code;
    s->raw_head = nxt;
}

/* --------------------------------------------------- synthetic touch feed */

static int perm(int v, int total) { return v * total / 1000; }

static void touch_down(koboy_input *in, int slot, int x, int y)
{
    koboy_ev ev[5] = {
        { KOBOY_EV_ABS, KOBOY_ABS_MT_SLOT,        slot },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, slot + 1 },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_X,  x },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_Y,  y },
        { KOBOY_EV_SYN, 0, 0 },
    };
    input_feed(in, ev, 5);
}

static void touch_move(koboy_input *in, int slot, int x, int y)
{
    koboy_ev ev[4] = {
        { KOBOY_EV_ABS, KOBOY_ABS_MT_SLOT,       slot },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_X, x },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_Y, y },
        { KOBOY_EV_SYN, 0, 0 },
    };
    input_feed(in, ev, 4);
}

static void touch_up(koboy_input *in, int slot)
{
    koboy_ev ev[3] = {
        { KOBOY_EV_ABS, KOBOY_ABS_MT_SLOT,        slot },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, -1 },
        { KOBOY_EV_SYN, 0, 0 },
    };
    input_feed(in, ev, 3);
}

/* Arrow keys are not a separate input path: they place a virtual finger at the
   centre of the thumb-pad and drag it past the deadzone, so the same origin
   latching and hysteresis run as for a real thumb. */
static void kb_pad_update(sdl_ctx *s, koboy_input *in)
{
    const koboy_layout *l = &s->cfg.layout;
    int dcx = perm(l->dpad_cx, s->panel_w);
    int dcy = perm(l->dpad_cy, s->panel_h);
    int dx = (s->kb_right ? 1 : 0) - (s->kb_left ? 1 : 0);
    int dy = (s->kb_down  ? 1 : 0) - (s->kb_up   ? 1 : 0);
    int off = s->cfg.dpad_deadzone + s->cfg.dpad_hysteresis + 8;

    if (!dx && !dy) {
        if (s->kb_pad_down) { touch_up(in, SLOT_DPAD); s->kb_pad_down = false; }
        return;
    }
    if (!s->kb_pad_down) {
        /* Land on the centre first so input.c latches the origin there. */
        touch_down(in, SLOT_DPAD, dcx, dcy);
        s->kb_pad_down = true;
    }
    touch_move(in, SLOT_DPAD, dcx + dx * off, dcy + dy * off);
}

static void kb_zone_update(sdl_ctx *s, koboy_input *in, int slot, bool want,
                           bool *have, int cx_permille, int cy_permille)
{
    if (want == *have) return;
    if (want) touch_down(in, slot, perm(cx_permille, s->panel_w),
                                   perm(cy_permille, s->panel_h));
    else      touch_up(in, slot);
    *have = want;
}

/* ------------------------------------------------------------- pump/vtable */

static void pump(sdl_ctx *s, koboy_input *in)
{
    SDL_Event e;

    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            s->quit = true;
            break;

        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            bool down = (e.type == SDL_KEYDOWN);
            if (down && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) s->quit = true;

            uint16_t code = sdl_to_evdev(e.key.keysym.scancode);
            if (!code) break;
            /* Only the first press enters the calibration queue; repeats would
               fill it with duplicates of a key still being held. */
            if (down && !e.key.repeat) raw_push(s, code);
            if (!in) break;

            /* Auto-repeat is deliberately NOT filtered out of the button path.
               X11 auto-repeat is a stream of release/press pairs, so dropping
               the repeat press while honouring the release would turn a held
               key into a single ~30ms tap. Every handler below is idempotent,
               so replaying the press is the safe direction to err in. */

            switch (e.key.keysym.scancode) {
            /* Applied per event rather than once after the drain: a batch
               containing both the press and the release of the same arrow must
               not collapse into a single no-op packet. */
            case SDL_SCANCODE_UP:    s->kb_up    = down; kb_pad_update(s, in); break;
            case SDL_SCANCODE_DOWN:  s->kb_down  = down; kb_pad_update(s, in); break;
            case SDL_SCANCODE_LEFT:  s->kb_left  = down; kb_pad_update(s, in); break;
            case SDL_SCANCODE_RIGHT: s->kb_right = down; kb_pad_update(s, in); break;
            case SDL_SCANCODE_RETURN:
                kb_zone_update(s, in, SLOT_START, down, &s->kb_start_down,
                               s->cfg.layout.start_cx, s->cfg.layout.start_cy);
                break;
            case SDL_SCANCODE_BACKSPACE:
                kb_zone_update(s, in, SLOT_SELECT, down, &s->kb_select_down,
                               s->cfg.layout.select_cx, s->cfg.layout.select_cy);
                break;
            default:
                /* A and B travel the hardware-key path, exactly as on the
                   device, using whatever calibration wrote into koboy.ini. */
                input_feed_key(in, code, down);
                break;
            }
            break;
        }

        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
        case SDL_MOUSEMOTION: {
            if (!in) break;
            int wx, wy;
            if (e.type == SDL_MOUSEMOTION) { wx = e.motion.x; wy = e.motion.y; }
            else                           { wx = e.button.x; wy = e.button.y; }
            float lx = (float)wx, ly = (float)wy;
            SDL_RenderWindowToLogical(s->ren, wx, wy, &lx, &ly);
            int px = (int)lx, py = (int)ly;
            if (px < 0) px = 0;
            if (px >= s->panel_w) px = s->panel_w - 1;
            if (py < 0) py = 0;
            if (py >= s->panel_h) py = s->panel_h - 1;

            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                touch_down(in, SLOT_MOUSE, px, py);
                s->mouse_down = true;
            } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
                if (s->mouse_down) { touch_up(in, SLOT_MOUSE); s->mouse_down = false; }
            } else if (s->mouse_down) {
                touch_move(in, SLOT_MOUSE, px, py);
            }
            break;
        }

        default:
            break;
        }
    }
}

/* Destroys whatever has been created so far, in reverse order, leaving the
   context reusable. Shared by shutdown and by every failure branch of init, so
   a half-built backend never leaks a window or an open video subsystem. */
static void sdl_teardown(sdl_ctx *s)
{
    free(s->argb);
    s->argb = NULL;
    if (s->tex) { SDL_DestroyTexture(s->tex);  s->tex = NULL; }
    if (s->ren) { SDL_DestroyRenderer(s->ren); s->ren = NULL; }
    if (s->win) { SDL_DestroyWindow(s->win);   s->win = NULL; }
    SDL_Quit();
}

static bool sdl_init(void *ctx, const koboy_config *c)
{
    sdl_ctx *s = ctx;
    s->cfg = *c;
    /* Same starting point the Kobo backend takes from the same key, so the
       name main.c prints at startup is the configured policy on both sides
       rather than "AUTO" here and the truth there. */
    s->wfm_policy = (koboy_wfm_policy)c->wfm_fast_policy;
    if (s->panel_w <= 0) s->panel_w = SDL_PANEL_W;
    if (s->panel_h <= 0) s->panel_h = SDL_PANEL_H;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        goto fail;          /* SDL_Init can fail with subsystems already up */
    }
    s->perf_freq = SDL_GetPerformanceFrequency();
    if (!s->perf_freq) s->perf_freq = 1;

    /* The window may be smaller than the panel so a 1448px-tall e-reader
       geometry still fits a laptop screen; logical size keeps every
       coordinate above the seam in panel space. */
    int win_w = s->panel_w, win_h = s->panel_h;
    SDL_Rect usable;
    if (SDL_GetDisplayUsableBounds(0, &usable) == 0 && usable.h > 0 && usable.w > 0) {
        double fx = (double)usable.w * 0.95 / (double)s->panel_w;
        double fy = (double)usable.h * 0.95 / (double)s->panel_h;
        double f = fx < fy ? fx : fy;
        if (f < 1.0) { win_w = (int)(s->panel_w * f); win_h = (int)(s->panel_h * f); }
    }

    s->win = SDL_CreateWindow("koboy", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              win_w, win_h, SDL_WINDOW_SHOWN);
    if (!s->win) { SDL_Log("SDL_CreateWindow: %s", SDL_GetError()); goto fail; }

    s->ren = SDL_CreateRenderer(s->win, -1, 0);
    if (!s->ren) { SDL_Log("SDL_CreateRenderer: %s", SDL_GetError()); goto fail; }
    SDL_RenderSetLogicalSize(s->ren, s->panel_w, s->panel_h);

    s->tex = SDL_CreateTexture(s->ren, SDL_PIXELFORMAT_ARGB8888,
                               SDL_TEXTUREACCESS_STREAMING, s->panel_w, s->panel_h);
    if (!s->tex) { SDL_Log("SDL_CreateTexture: %s", SDL_GetError()); goto fail; }

    s->argb = calloc((size_t)s->panel_w * (size_t)s->panel_h, sizeof *s->argb);
    if (!s->argb) { SDL_Log("out of memory allocating panel shadow"); goto fail; }
    for (size_t i = 0; i < (size_t)s->panel_w * (size_t)s->panel_h; i++)
        s->argb[i] = 0xFFFFFFFFu;
    return true;

fail:
    sdl_teardown(s);
    return false;
}

static void sdl_shutdown(void *ctx)
{
    sdl_ctx *s = ctx;
    sdl_teardown(s);
    free(s);
}

static void sdl_screen_info(void *ctx, int *w, int *h)
{
    sdl_ctx *s = ctx;
    if (w) *w = s->panel_w;
    if (h) *h = s->panel_h;
}

static bool sdl_blit_gray8(void *ctx, const uint8_t *px, int w, int h,
                           int stride, int x, int y)
{
    sdl_ctx *s = ctx;
    if (!px || w <= 0 || h <= 0) return false;
    if (x < 0 || y < 0 || x + w > s->panel_w || y + h > s->panel_h) return false;

    for (int row = 0; row < h; row++) {
        const uint8_t *sp = px + (size_t)row * stride;
        uint32_t *dp = s->argb + (size_t)(y + row) * s->panel_w + x;
        for (int col = 0; col < w; col++) {
            uint32_t g = sp[col];
            dp[col] = 0xFF000000u | (g << 16) | (g << 8) | g;
        }
    }
    return true;
}

static bool sdl_refresh(void *ctx, int x, int y, int w, int h, koboy_refresh_mode mode)
{
    sdl_ctx *s = ctx;
    (void)mode;                       /* waveforms are meaningless on a monitor */
    if (w <= 0 || h <= 0) return false;
    if (x < 0 || y < 0 || x + w > s->panel_w || y + h > s->panel_h) return false;

    SDL_Rect r = { x, y, w, h };
    SDL_UpdateTexture(s->tex, &r, s->argb + (size_t)y * s->panel_w + x,
                      s->panel_w * (int)sizeof *s->argb);
    SDL_RenderClear(s->ren);
    SDL_RenderCopy(s->ren, s->tex, NULL, NULL);
    SDL_RenderPresent(s->ren);
    return true;
}

/* Contract in platform_if.h. Recorded, not applied: sdl_refresh ignores the
   waveform mode entirely and always will. */
static void sdl_set_wfm_policy(void *ctx, koboy_wfm_policy policy)
{
    sdl_ctx *s = ctx;
    if (s) s->wfm_policy = policy;
}

/* The POLICY, uppercased, and not a waveform: on a monitor there is no
   waveform to report, and inventing a plausible one would be exactly the
   device theatre this seam exists to avoid. On the Kobo the same call returns
   the real mapped waveform, which is a strictly stronger answer -- the two
   agree except where a device downgrades a request, and no monitor does. */
static const char *sdl_wfm_fast_name(void *ctx)
{
    sdl_ctx *s = ctx;
    switch (s ? s->wfm_policy : KOBOY_WFM_AUTO) {
        case KOBOY_WFM_DU4: return "DU4";
        case KOBOY_WFM_DU:  return "DU";
        default:            return "AUTO";
    }
}

static bool sdl_poll_input(void *ctx, struct koboy_input *in)
{
    pump(ctx, in);
    return true;
}

static uint64_t sdl_now_us(void *ctx)
{
    sdl_ctx *s = ctx;
    return (uint64_t)((double)SDL_GetPerformanceCounter() * 1000000.0 / (double)s->perf_freq);
}

static bool sdl_should_quit(void *ctx) { return ((sdl_ctx *)ctx)->quit; }

/* The desktop has no battery to report; -1 is a valid, expected answer, not a
   stub that needs revisiting. */
static int sdl_battery_percent(void *ctx) { (void)ctx; return -1; }

/* ------------------------------------------------------------------ ctor */

koboy_platform *platform_sdl_create(void);
void            platform_sdl_set_panel(koboy_platform *pf, int w, int h);
bool            platform_poll_raw_key(koboy_platform *pf, uint16_t *code);

koboy_platform *platform_sdl_create(void)
{
    koboy_platform *pf = calloc(1, sizeof *pf);
    if (!pf) return NULL;
    sdl_ctx *s = calloc(1, sizeof *s);
    if (!s) { free(pf); return NULL; }

    pf->ctx         = s;
    pf->init        = sdl_init;
    pf->shutdown    = sdl_shutdown;
    pf->screen_info = sdl_screen_info;
    pf->blit_gray8  = sdl_blit_gray8;
    pf->refresh     = sdl_refresh;
    pf->poll_input  = sdl_poll_input;
    pf->now_us      = sdl_now_us;
    pf->should_quit = sdl_should_quit;
    pf->battery_percent = sdl_battery_percent;
    pf->set_wfm_policy  = sdl_set_wfm_policy;
    pf->wfm_fast_name   = sdl_wfm_fast_name;
    return pf;
}

void platform_sdl_set_panel(koboy_platform *pf, int w, int h)
{
    sdl_ctx *s = pf->ctx;
    s->panel_w = w;
    s->panel_h = h;
}

/* First-run calibration is the one place that needs a raw key code rather
   than a normalised button, so it lives beside the vtable rather than in it:
   putting it in koboy_platform would make every backend carry a hook the
   steady-state loop never calls. */
bool platform_poll_raw_key(koboy_platform *pf, uint16_t *code)
{
    sdl_ctx *s = pf->ctx;
    pump(s, NULL);
    if (s->raw_tail == s->raw_head) return false;
    if (code) *code = s->raw[s->raw_tail];
    s->raw_tail = (s->raw_tail + 1) % RAWKEY_RING;
    return true;
}
