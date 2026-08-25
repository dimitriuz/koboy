/* Kobo backend for the platform seam: FBInk for the panel, raw evdev for input.
 *
 * This is the counterpart of platform_sdl.c and drives exactly the same vtable,
 * so everything above the seam -- chrome, video pipeline, thumb-pad hit-testing,
 * pacing -- is code already debugged on the desktop. What is genuinely new here
 * is only what a monitor cannot model: e-ink waveform modes, a framebuffer whose
 * scanline is wider than the visible panel, and an input layer that another
 * process may be holding exclusively.
 *
 * Three things about this file are load-bearing and easy to get wrong:
 *
 *  1. Refreshes never wait for completion. fbink_wait_for_complete() is
 *     deliberately never called; the emulator must not stall for the panel.
 *
 *  2. Pixels are written straight into the mmap'ed framebuffer, honouring
 *     fInfo.line_length rather than assuming stride == width, and offset by the
 *     framebuffer's viewport origin. See kobo_blit_gray8 and the long note
 *     above ORIGIN, which is where FBInk's reporting is easy to misread.
 *
 *  3. Nickel holds EVIOCGRAB on the input nodes. While Nickel runs, *no* other
 *     process can read input at all, so an empty input stream here means the
 *     launcher failed to stop it, not that the hardware is broken.
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "input.h"          /* must precede platform_if.h: declares struct koboy_input */
#include "platform_if.h"

#include <fbink.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <linux/input.h>

#define RAWKEY_RING   32
#define MAX_KEY_NODES 4
#define EV_BATCH      64

typedef struct {
    koboy_config cfg;

    int         fbfd;
    FBInkConfig fb;                 /* seed config: quiet, no implicit refresh */

    /* Geometry, all read from fbink_get_state() -- never assumed. */
    int      view_w, view_h;        /* visible panel, in pixels */
    int      origin_x, origin_y;    /* framebuffer viewport origin, in pixels */
    uint32_t stride;                /* fInfo.line_length, in BYTES */
    uint32_t bpp;
    bool     inverted_gray;         /* 8bpp GRAYSCALE_8BIT_INVERTED */

    unsigned char *fbmem;
    size_t         fbmem_len;

    /* Waveform mapping, indexed by koboy_refresh_mode. */
    uint8_t     wfm[3];
    bool        flash[3];
    const char *wfm_name[3];

    /* Input nodes. */
    int  touch_fd;
    bool touch_grabbed;
    int  key_fd[MAX_KEY_NODES];
    bool key_grabbed[MAX_KEY_NODES];
    bool key_has_power[MAX_KEY_NODES];
    int  n_key;

    int  raw_max_x, raw_max_y;
    bool transpose, flip_x, flip_y;

    bool quit;
    bool trace;                     /* KOBOY_TRACE_REFRESH in the environment */

    uint16_t raw[RAWKEY_RING];
    int      raw_head, raw_tail;
} kobo_ctx;

/* ------------------------------------------------------------------ helpers */

static void kobo_say(const kobo_ctx *k, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void kobo_say(const kobo_ctx *k, const char *fmt, ...)
{
    (void)k;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------ waveform map */

/* Kobo's devicePlatform is "Mark <n>" for every mxcfb-era device; anything else
   (Kindle/PocketBook/reMarkable codenames, or Kobo's own "Mark ?" placeholder)
   yields 0, which lands on the conservative fallback. */
static int platform_mark(const char *plat)
{
    int n = 0;
    if (!plat) return 0;
    if (sscanf(plat, "Mark %d", &n) == 1) return n;
    return 0;
}

/* MEASURED on this project's reference device (Libra 2, Mark 9, 1264x1680),
   timing a 5x game rect (800x720) with CLOCK_MONOTONIC around fbink_refresh:

       DU4  non-blocking    15.0 ms
       DU4  blocking        39.2 ms
       A2   blocking       135.8 ms
       GL16 blocking       310.8 ms

   So FAST maps to DU4, never A2. A2 is ~3.5x slower here *and* its cost is
   nearly flat with area, which means dirty rectangles cannot claw the
   difference back -- shrinking the region does not shrink an A2 refresh.
   Both are 4-level-capable, and src/video.c already quantises to
   KOBOY_DU4_LEVELS, so DU4 costs nothing in fidelity either.

   The gate is not the Mark number alone. On Kobo's mxcfb path FBInk maps
   WFM_DU4 to the real WAVEFORM_MODE_DU4 only when deviceQuirks.hasEclipseWfm
   is set, and *silently downgrades it to GC4 otherwise* -- so asking for DU4
   on a device without it would leave us reporting DU4 while the panel ran
   something else. We therefore require the quirk as well, and exclude sunxi,
   whose waveform table is a different enum entirely. Platforms that fail the
   gate fall back to A2 for FAST and GL16 for GRAY, which is the classic
   pre-Mk.9 pairing. FULL is GC16, flashing, everywhere: it is the
   ghost-clearing refresh, and a non-flashing GC16 does not clear ghosting. */
static void map_waveforms(kobo_ctx *k, const FBInkState *st)
{
    int  mark    = platform_mark(st->device_platform);
    bool has_du4 = !st->is_sunxi && st->has_eclipse_wfm && mark >= 9;

    if (has_du4) {
        k->wfm[KOBOY_REFRESH_FAST] = WFM_DU4; k->wfm_name[KOBOY_REFRESH_FAST] = "DU4";
        k->wfm[KOBOY_REFRESH_GRAY] = WFM_DU4; k->wfm_name[KOBOY_REFRESH_GRAY] = "DU4";
    } else {
        k->wfm[KOBOY_REFRESH_FAST] = WFM_A2;   k->wfm_name[KOBOY_REFRESH_FAST] = "A2";
        k->wfm[KOBOY_REFRESH_GRAY] = WFM_GL16; k->wfm_name[KOBOY_REFRESH_GRAY] = "GL16";
    }
    k->wfm[KOBOY_REFRESH_FULL]   = WFM_GC16;
    k->wfm_name[KOBOY_REFRESH_FULL] = "GC16";

    k->flash[KOBOY_REFRESH_FAST] = false;
    k->flash[KOBOY_REFRESH_GRAY] = false;
    k->flash[KOBOY_REFRESH_FULL] = true;
}

/* ------------------------------------------------------- grabs and teardown */

/* Closing the fd is itself enough to drop an EVIOCGRAB -- the kernel releases
   it when the file description goes away, including on an abnormal exit. The
   explicit ungrab is here so that the release is observable and ordered, and so
   the emergency handler below does something meaningful rather than relying on
   process teardown it cannot verify. */
static void release_input(kobo_ctx *k)
{
    if (k->touch_fd >= 0) {
        if (k->touch_grabbed) { ioctl(k->touch_fd, EVIOCGRAB, 0); k->touch_grabbed = false; }
        close(k->touch_fd);
        k->touch_fd = -1;
    }
    for (int i = 0; i < k->n_key; i++) {
        if (k->key_fd[i] < 0) continue;
        if (k->key_grabbed[i]) { ioctl(k->key_fd[i], EVIOCGRAB, 0); k->key_grabbed[i] = false; }
        close(k->key_fd[i]);
        k->key_fd[i] = -1;
    }
    k->n_key = 0;
}

static kobo_ctx *g_emergency;

/* Only for the signals that mean "this process is about to die without running
   any cleanup". SIGTERM/SIGINT are deliberately NOT handled here: main.c
   installs its own handlers for those (after platform init, on purpose), turns
   them into a clean loop exit, and reaches shutdown() normally.

   ioctl() and close() are both async-signal-safe, so this is legal in a
   handler; nothing else is attempted. */
static void emergency(int sig)
{
    if (g_emergency) release_input(g_emergency);
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_emergency(kobo_ctx *k)
{
    static const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT,
                                SIGHUP, SIGQUIT };
    g_emergency = k;
    for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++)
        signal(sigs[i], emergency);
}

/* ------------------------------------------------------------- input set-up */

static int abs_max(int fd, unsigned int axis, int fallback)
{
    struct input_absinfo ai;
    memset(&ai, 0, sizeof ai);
    if (ioctl(fd, EVIOCGABS(axis), &ai) < 0) return fallback;
    if (ai.maximum <= ai.minimum) return fallback;
    return ai.maximum;
}

static bool node_has_key(int fd, unsigned int code)
{
    unsigned char bits[(KEY_MAX + 7) / 8];
    memset(bits, 0, sizeof bits);
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) < 0) return false;
    return (bits[code / 8] & (1u << (code % 8))) != 0;
}

/* Classification comes from fbink_input_scan rather than from hardcoded
   event<N> numbers: the numbering is not stable across firmwares, and on the
   reference device the touchscreen happens to be event1 while the keys are
   event0 and the accelerometer -- which we want nothing to do with -- is
   event2. SCAN_ONLY means FBInk closes every fd it opened; we reopen the two
   we care about ourselves, so the open flags and the grabs are ours. */
static void open_input(kobo_ctx *k)
{
    size_t            n   = 0;
    FBInkInputDevice *dev = fbink_input_scan(INPUT_TOUCHSCREEN | INPUT_KEY, 0,
                                             SCAN_ONLY | NO_RECAP, &n);
    if (!dev) {
        kobo_say(k, "koboy: fbink_input_scan found no input devices\n");
        return;
    }

    for (size_t i = 0; i < n; i++) {
        const FBInkInputDevice *d = &dev[i];

        /* The accelerometer is on its own node and reports a continuous stream
           of nonsense as far as we are concerned. Never open it. */
        if (d->type & INPUT_ACCELEROMETER) continue;

        if ((d->type & INPUT_TOUCHSCREEN) && k->touch_fd < 0) {
            int fd = open(d->path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) {
                kobo_say(k, "koboy: cannot open touchscreen %s: %s\n",
                         d->path, strerror(errno));
                continue;
            }
            k->touch_fd = fd;

            /* Protocol B first, protocol A as the fallback. The transposition
               is DETECTED, not tabulated: on the reference device ABS_X tops
               out at 1680 and ABS_Y at 1264, on a panel 1264 wide and 1680
               tall, i.e. the touch layer is mounted rotated. Comparing the two
               maxima catches that on any device without a per-model table. */
            int mx = abs_max(fd, ABS_MT_POSITION_X, 0);
            int my = abs_max(fd, ABS_MT_POSITION_Y, 0);
            if (mx <= 0 || my <= 0) {
                mx = abs_max(fd, ABS_X, k->view_w - 1);
                my = abs_max(fd, ABS_Y, k->view_h - 1);
            }
            k->raw_max_x = mx;
            k->raw_max_y = my;
            k->transpose = (mx > my);
            kobo_say(k, "koboy: touch %s (%s) raw %dx%d transpose=%d\n",
                     d->path, d->name, mx, my, k->transpose ? 1 : 0);
            continue;
        }

        /* A key node. Touchscreens also advertise EV_KEY (BTN_TOUCH), so skip
           anything already claimed as the touchscreen. */
        if ((d->type & INPUT_KEY) && !(d->type & INPUT_TOUCHSCREEN) &&
            k->n_key < MAX_KEY_NODES) {
            int fd = open(d->path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) {
                kobo_say(k, "koboy: cannot open key node %s: %s\n",
                         d->path, strerror(errno));
                continue;
            }
            int idx = k->n_key++;
            k->key_fd[idx]        = fd;
            k->key_has_power[idx] = (d->type & INPUT_POWER_BUTTON) ||
                                    node_has_key(fd, KEY_POWER);
            kobo_say(k, "koboy: keys %s (%s)%s\n", d->path, d->name,
                     k->key_has_power[idx] ? " [carries KEY_POWER]" : "");
        }
    }
    free(dev);

    /* Grabbing keeps a stray Nickel restart, or anything else that wakes up
       mid-game, from stealing or double-handling our input. But never grab a
       node that carries KEY_POWER: on this hardware the page-turn buttons and
       the power button are the same gpio-keys node, and swallowing power
       presses on a device whose only other way out is a paperclip reset is not
       a trade worth making. We still *read* that node, so KEY_POWER quitting
       and the page-turn buttons both work; we just do not take it exclusively. */
    if (k->cfg.grab_input) {
        if (k->touch_fd >= 0 && ioctl(k->touch_fd, EVIOCGRAB, 1) == 0)
            k->touch_grabbed = true;
        for (int i = 0; i < k->n_key; i++) {
            if (k->key_has_power[i]) continue;
            if (ioctl(k->key_fd[i], EVIOCGRAB, 1) == 0) k->key_grabbed[i] = true;
        }
    }

    if (k->touch_fd < 0 && k->n_key == 0)
        kobo_say(k, "koboy: no usable input node -- is Nickel still running? "
                    "it holds EVIOCGRAB on every event node\n");
}

/* ------------------------------------------------------------------ vtable */

static void raw_push(kobo_ctx *k, uint16_t code)
{
    int nxt = (k->raw_head + 1) % RAWKEY_RING;
    if (nxt == k->raw_tail) return;
    k->raw[k->raw_head] = code;
    k->raw_head = nxt;
}

static void kobo_teardown(kobo_ctx *k)
{
    release_input(k);
    g_emergency = NULL;
    k->fbmem     = NULL;
    k->fbmem_len = 0;
    if (k->fbfd >= 0) { fbink_close(k->fbfd); k->fbfd = -1; }
}

static bool kobo_init(void *ctx, const koboy_config *c)
{
    kobo_ctx *k = ctx;
    k->cfg   = *c;
    k->trace = (getenv("KOBOY_TRACE_REFRESH") != NULL);

    k->fbfd = fbink_open();
    if (k->fbfd < 0) {
        kobo_say(k, "koboy: fbink_open failed: %s\n", strerror(errno));
        return false;
    }
    if (fbink_init(k->fbfd, &k->fb) < 0) {
        kobo_say(k, "koboy: fbink_init failed\n");
        goto fail;
    }

    FBInkState st;
    memset(&st, 0, sizeof st);
    fbink_get_state(&k->fb, &st);

    k->view_w = (int)st.view_width;
    k->view_h = (int)st.view_height;
    k->stride = st.scanline_stride;
    k->bpp    = st.bpp;
    k->inverted_gray = st.inverted_grayscale;

    /* ORIGIN. FBInkState.view_vert_origin is *not* the framebuffer viewport
       origin: FBInk documents it as "viewport + viewVertOffset", and
       viewVertOffset is a text-layout shift used to vertically balance whole
       character rows inside the viewport (fbink.c: `viewVertOrigin =
       viewVertOrigin + viewVertOffset`, after the viewport block).
       The reference device's own quirk table sets no koboVertOffset at all
       (fbink_device_id.c, DEVICE_KOBO_LIBRA_2), so its real framebuffer
       viewport origin is 0 and the reported viewVertOrigin of 8 is purely
       FBInk's font-row centring. Adding it to a pixel blit would push the
       whole image down by 8px and clip the bottom 8 rows.
       The framebuffer viewport is therefore origin = origin - offset, which is
       0 here and non-zero only on the handful of Kobos that really do hide
       rows behind the bezel (koboVertOffset != 0). */
    k->origin_x = (int)st.view_hori_origin;
    k->origin_y = (int)st.view_vert_origin - (int)st.view_vert_offset;
    if (k->origin_x < 0) k->origin_x = 0;
    if (k->origin_y < 0) k->origin_y = 0;

    if (k->view_w <= 0 || k->view_h <= 0 || k->stride == 0) {
        kobo_say(k, "koboy: implausible framebuffer geometry %dx%d stride=%u\n",
                 k->view_w, k->view_h, k->stride);
        goto fail;
    }
    if (k->bpp != 8 && k->bpp != 16 && k->bpp != 32) {
        kobo_say(k, "koboy: unsupported framebuffer depth %ubpp\n", k->bpp);
        goto fail;
    }

    k->fbmem = fbink_get_fb_pointer(k->fbfd, &k->fbmem_len);
    if (!k->fbmem) {
        kobo_say(k, "koboy: fbink_get_fb_pointer failed\n");
        goto fail;
    }
    /* The blit trusts stride arithmetic; make sure the mapping is actually big
       enough for the last visible row before writing a single byte into it. */
    size_t need = (size_t)k->stride * (size_t)(k->origin_y + k->view_h);
    if (k->fbmem_len < need) {
        kobo_say(k, "koboy: framebuffer mapping is %zu bytes, need %zu\n",
                 k->fbmem_len, need);
        goto fail;
    }

    map_waveforms(k, &st);

    kobo_say(k, "koboy: %s (id %u, %s, %s), %dx%d @ %ubpp, stride %u bytes "
                "(%u px), origin (%d,%d), fast=%s\n",
             st.device_name, (unsigned)st.device_id, st.device_codename,
             st.device_platform, k->view_w, k->view_h, k->bpp, k->stride,
             k->stride / (k->bpp / 8), k->origin_x, k->origin_y,
             k->wfm_name[KOBOY_REFRESH_FAST]);

    /* FBInk's own view of the touch layer, kept only as a cross-check against
       the axis maxima we measure ourselves. */
    k->flip_x = st.touch_mirror_x;
    k->flip_y = st.touch_mirror_y;

    open_input(k);

    /* input.c applies transpose first and the mirrors afterwards, which is the
       same order FBInk defines its quirks in, so the mirror flags carry over
       directly -- but only if the two agree about the swap. If they disagree,
       the mirrors were derived under a different convention and applying them
       would be worse than not: trust the measured maxima for the swap and drop
       the mirrors. */
    if (k->touch_fd >= 0 && st.touch_swap_axes != k->transpose) {
        kobo_say(k, "koboy: FBInk reports touchSwapAxes=%d but the axis maxima "
                    "say %d; ignoring its mirror quirks\n",
                 st.touch_swap_axes ? 1 : 0, k->transpose ? 1 : 0);
        k->flip_x = k->flip_y = false;
    }

    install_emergency(k);
    return true;

fail:
    kobo_teardown(k);
    return false;
}

static void kobo_shutdown(void *ctx)
{
    kobo_ctx *k = ctx;
    kobo_teardown(k);
    free(k);
}

static void kobo_screen_info(void *ctx, int *w, int *h)
{
    kobo_ctx *k = ctx;
    if (w) *w = k->view_w;
    if (h) *h = k->view_h;
}

/* Writes gray8 rows straight into the mmap'ed framebuffer.
 *
 * Deliberately not fbink_print_raw_data: that call would have to be told the
 * offsets anyway, and it copies the whole input buffer once to normalise the
 * pixel format. Here the destination pixel format is known from
 * fbink_get_state, the source is already the exact gray8 the panel wants, and
 * the only thing that must not be got wrong is the stride -- which is why it
 * is spelled out on every row rather than folded into a library call.
 *
 * All three plausible depths are handled, so the launch script's `fbdepth -d 8`
 * is an optimisation (a straight memcpy per row, and a quarter of the memory
 * bandwidth) rather than a precondition. Running at the native 32bpp works;
 * it just copies four bytes per pixel instead of one. */
static bool kobo_blit_gray8(void *ctx, const uint8_t *px, int w, int h,
                            int stride, int x, int y)
{
    kobo_ctx *k = ctx;
    if (!px || w <= 0 || h <= 0 || stride < w || !k->fbmem) return false;
    if (x < 0 || y < 0 || x + w > k->view_w || y + h > k->view_h) return false;

    const uint32_t bytes = k->bpp / 8;
    unsigned char *base  = k->fbmem
                         + (size_t)(y + k->origin_y) * k->stride
                         + (size_t)(x + k->origin_x) * bytes;

    for (int row = 0; row < h; row++) {
        const uint8_t *sp = px   + (size_t)row * stride;
        unsigned char *dp = base + (size_t)row * k->stride;

        switch (k->bpp) {
        case 8:
            if (k->inverted_gray)
                for (int col = 0; col < w; col++) dp[col] = (uint8_t)(0xFFu - sp[col]);
            else
                memcpy(dp, sp, (size_t)w);
            break;
        case 16:
            /* RGB565. Gray means r == g == b, so component order is moot. */
            for (int col = 0; col < w; col++) {
                unsigned g = sp[col];
                uint16_t v = (uint16_t)(((g & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (g >> 3));
                memcpy(dp + (size_t)col * 2, &v, sizeof v);
            }
            break;
        default: /* 32bpp: BGRA or RGBA, identical for a gray value. */
            for (int col = 0; col < w; col++) {
                unsigned char *p = dp + (size_t)col * 4;
                p[0] = p[1] = p[2] = sp[col];
                p[3] = 0xFF;
            }
            break;
        }
    }
    return true;
}

/* Submit and return. fbink_wait_for_complete() is NOT called, here or anywhere:
   at the shipped 5x game rect the same refresh measured 15.0 ms submitted
   versus 39.2 ms waited, and the emulator has 16.7 ms of wall clock per core
   frame to spend. The EPDC queues and merges updates on its own; blocking would
   buy nothing but a stall. (The reference device is also flagged
   unreliableWaitFor by FBInk, so the wait can outright time out.) */
static bool kobo_refresh(void *ctx, int x, int y, int w, int h,
                         koboy_refresh_mode mode)
{
    kobo_ctx *k = ctx;
    if (w <= 0 || h <= 0) return false;
    if (x < 0 || y < 0 || x + w > k->view_w || y + h > k->view_h) return false;
    if ((unsigned)mode > KOBOY_REFRESH_FULL) return false;

    FBInkConfig cfg = k->fb;
    cfg.wfm_mode    = k->wfm[mode];
    cfg.is_flashing = k->flash[mode];

    /* Off by default, and the only way to see from off-device which rectangle
       each waveform is actually being asked for -- in particular that the
       periodic cleanup asks for the game rect and not the whole panel. SDL
       ignores waveform modes entirely, so this is not observable on the
       desktop at all. */
    if (k->trace)
        kobo_say(k, "koboy: refresh %-4s %s %dx%d at (%d,%d) -> fb (%d,%d)\n",
                 k->wfm_name[mode], k->flash[mode] ? "flash" : "     ",
                 w, h, x, y, x + k->origin_x, y + k->origin_y);

    return fbink_refresh(k->fbfd,
                         (uint32_t)(y + k->origin_y), (uint32_t)(x + k->origin_x),
                         (uint32_t)w, (uint32_t)h, &cfg) == EXIT_SUCCESS;
}

/* Drains one node. Every event is handed to input.c as a koboy_ev, including
   EV_KEY and the SYN boundaries, so the protocol-B slot tracking and the
   packet-coherency rule live in the one tested place rather than being
   half-reimplemented here. */
static void drain(kobo_ctx *k, int fd, bool is_key, koboy_input *in)
{
    struct input_event ev[EV_BATCH];
    koboy_ev           out[EV_BATCH];

    /* Bounded rather than "until EAGAIN": a wedged or flooding node must not be
       able to hold the emulator loop open indefinitely. 16 full batches is
       1024 events, orders of magnitude more than one 60Hz tick can produce. */
    for (int pass = 0; pass < 16; pass++) {
        ssize_t n = read(fd, ev, sizeof ev);
        if (n <= 0) return;                    /* EAGAIN, or the node vanished */
        size_t cnt = (size_t)n / sizeof ev[0];
        if (cnt == 0) return;

        size_t m = 0;
        for (size_t i = 0; i < cnt; i++) {
            if (ev[i].type == EV_KEY) {
                bool down = (ev[i].value != 0);
                /* The power button is the way out. With Nickel stopped nothing
                   manages sleep, so there is no suspend to fight over it;
                   real suspend/resume is out of scope for v1. */
                if (ev[i].code == KEY_POWER) {
                    if (down) k->quit = true;
                    continue;
                }
                /* Only the key node feeds calibration. A touchscreen's
                   BTN_TOUCH would otherwise be offered as a "button". */
                if (is_key && down && ev[i].value == 1) raw_push(k, ev[i].code);
            }
            out[m].type  = ev[i].type;
            out[m].code  = ev[i].code;
            out[m].value = ev[i].value;
            m++;
        }
        if (in && m) input_feed(in, out, m);
        if (cnt < EV_BATCH) return;            /* short read: nothing left */
    }
}

static bool kobo_poll_input(void *ctx, struct koboy_input *in)
{
    kobo_ctx *k = ctx;
    if (k->touch_fd >= 0) drain(k, k->touch_fd, false, in);
    for (int i = 0; i < k->n_key; i++) drain(k, k->key_fd[i], true, in);
    return true;
}

static uint64_t kobo_now_us(void *ctx)
{
    (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

static bool kobo_should_quit(void *ctx) { return ((kobo_ctx *)ctx)->quit; }

/* ------------------------------------------------------------------- ctor */

koboy_platform *platform_kobo_create(void);
bool            platform_poll_raw_key(koboy_platform *pf, uint16_t *code);
void            platform_kobo_setup_touch(koboy_platform *pf, struct koboy_input *in);
void            platform_kobo_selftest(koboy_platform *pf);
void            platform_kobo_fatal(void *ctx, const char *msg);

koboy_platform *platform_kobo_create(void)
{
    koboy_platform *pf = calloc(1, sizeof *pf);
    if (!pf) return NULL;
    kobo_ctx *k = calloc(1, sizeof *k);
    if (!k) { free(pf); return NULL; }

    k->fbfd     = -1;
    k->touch_fd = -1;
    for (int i = 0; i < MAX_KEY_NODES; i++) k->key_fd[i] = -1;
    /* Quiet by default: stderr goes to the launcher's log file, and FBInk's
       init banner is several lines of hardware recap per run. */
    k->fb.is_quiet = true;
    /* koboy draws and refreshes as two explicit steps, so nothing FBInk itself
       draws (the fatal screen) should sneak in a refresh of its own. */
    k->fb.no_refresh = true;

    pf->ctx         = k;
    pf->init        = kobo_init;
    pf->shutdown    = kobo_shutdown;
    pf->screen_info = kobo_screen_info;
    pf->blit_gray8  = kobo_blit_gray8;
    pf->refresh     = kobo_refresh;
    pf->poll_input  = kobo_poll_input;
    pf->now_us      = kobo_now_us;
    pf->should_quit = kobo_should_quit;
    return pf;
}

/* Same seam as the SDL backend: first-run calibration is the only caller that
   wants a raw key code instead of a normalised button, so it stays beside the
   vtable rather than inside it. */
bool platform_poll_raw_key(koboy_platform *pf, uint16_t *code)
{
    kobo_ctx *k = pf->ctx;
    kobo_poll_input(k, NULL);
    if (k->raw_tail == k->raw_head) return false;
    if (code) *code = k->raw[k->raw_tail];
    k->raw_tail = (k->raw_tail + 1) % RAWKEY_RING;
    return true;
}

/* The touch transform is backend knowledge, not main-loop knowledge: the
   desktop mouse already reports panel coordinates, this panel's touch layer is
   mounted rotated and scaled to its own raw range. */
void platform_kobo_setup_touch(koboy_platform *pf, struct koboy_input *in)
{
    kobo_ctx *k = pf->ctx;
    if (k->touch_fd < 0) {
        /* No touchscreen: leave input.c's identity transform alone rather than
           installing a bogus one. Buttons still work. */
        return;
    }
    input_set_touch_transform(in, k->raw_max_x, k->raw_max_y,
                              k->transpose, k->flip_x, k->flip_y);
}

/* Machine-readable facts for tests/smoke_device.sh, which has no other way to
   see what the backend decided. */
void platform_kobo_selftest(koboy_platform *pf)
{
    kobo_ctx *k = pf->ctx;
    printf("panel=%dx%d\n", k->view_w, k->view_h);
    printf("wfm_fast=%s\n", k->wfm_name[KOBOY_REFRESH_FAST]);
    printf("wfm_gray=%s\n", k->wfm_name[KOBOY_REFRESH_GRAY]);
    printf("wfm_full=%s\n", k->wfm_name[KOBOY_REFRESH_FULL]);
    /* Stride in pixels, which is the number that must not be confused with the
       visible width: 5120 bytes at 32bpp is 1280 px against 1264 visible. */
    printf("stride=%u\n", k->stride / (k->bpp / 8));
    printf("stride_bytes=%u\n", k->stride);
    printf("bpp=%u\n", k->bpp);
    printf("origin=%d,%d\n", k->origin_x, k->origin_y);
    printf("touch_transpose=%d\n", k->transpose ? 1 : 0);
    printf("touch_raw=%dx%d\n", k->raw_max_x, k->raw_max_y);
    printf("input_touch=%d\n", k->touch_fd >= 0 ? 1 : 0);
    printf("input_keys=%d\n", k->n_key);
    fflush(stdout);
}

/* On a device with no terminal, an error that only reaches stderr is
   indistinguishable from a crash: the screen keeps whatever was on it and the
   user power-cycles. So every fatal path draws. */
void platform_kobo_fatal(void *ctx, const char *msg)
{
    kobo_ctx *k = ctx;
    if (!k || k->fbfd < 0 || !msg) return;

    FBInkConfig cfg = k->fb;
    cfg.no_refresh  = true;          /* one refresh, at the end, not per call */
    cfg.is_centered = true;
    cfg.is_halfway  = true;
    cfg.fontmult    = 3;

    fbink_cls(k->fbfd, &cfg, NULL, false);
    fbink_print(k->fbfd, msg, &cfg);
    kobo_refresh(k, 0, 0, k->view_w, k->view_h, KOBOY_REFRESH_FULL);

    /* Wait for an acknowledgement, but never forever: a scripted run has
       nobody to press anything, and hanging a smoke test is worse than
       clearing the message after a while. */
    const uint64_t deadline = kobo_now_us(k) + 20ull * 1000000ull;
    bool           acked    = false;
    while (!acked && kobo_now_us(k) < deadline) {
        struct input_event ev[EV_BATCH];
        int                fds[MAX_KEY_NODES + 1];
        int                nfd = 0;
        if (k->touch_fd >= 0) fds[nfd++] = k->touch_fd;
        for (int i = 0; i < k->n_key; i++) fds[nfd++] = k->key_fd[i];
        if (nfd == 0) break;                     /* nothing to wait on */

        for (int i = 0; i < nfd && !acked; i++) {
            ssize_t n = read(fds[i], ev, sizeof ev);
            if (n <= 0) continue;
            size_t cnt = (size_t)n / sizeof ev[0];
            for (size_t j = 0; j < cnt; j++) {
                if (ev[j].type == EV_KEY && ev[j].value == 1) { acked = true; break; }
                if (ev[j].type == EV_ABS && ev[j].code == ABS_MT_TRACKING_ID &&
                    ev[j].value >= 0) { acked = true; break; }
            }
        }
        if (!acked) usleep(20000);
    }
}
