/* Kobo backend for the platform seam: FBInk for the panel, raw evdev for input.
 *
 * The counterpart of platform_sdl.c, driving the same vtable, so everything
 * above the seam is code already debugged on the desktop. What is new here is
 * only what a monitor cannot model: e-ink waveform modes, a framebuffer whose
 * scanline is wider than the visible panel, and an input layer another process
 * may hold exclusively.
 *
 * Three things here are load-bearing and easy to get wrong:
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
#include "platform_kobo.h"
#include "btinput.h"

#include <fbink.h>

#include <dirent.h>
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

    /* The gamepad slot. Separate from key_fd[] rather than one more entry in
       it: it is the only node that can appear and disappear mid-session (see
       gamepad_rescan and drain's ENODEV handling), and folding hot-plug logic
       into the fixed-node array would make every key_fd[] loop above carry a
       branch the built-in nodes never need. */
    int      pad_fd;
    char     pad_node[32];        /* "/dev/input/eventN", for the lost-pad log */
    uint64_t pad_scan_at;         /* next now_us() a rescan is allowed to run */

    int  raw_max_x, raw_max_y;
    bool transpose, flip_x, flip_y;

    bool quit;
    bool trace;                     /* KOBOY_TRACE_REFRESH in the environment */
    bool du4_capable;               /* panel/platform can really do DU4 */

    /* Refresh timing, accumulated per mode so AUTO and forced DU4 can be
       compared fresh in one session rather than against a stale figure. */
    uint64_t rf_n[3], rf_us[3], rf_max_us[3];

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

   THE GATE IS NOT THE MARK NUMBER ALONE. On Kobo's mxcfb path FBInk maps
   WFM_DU4 to the real WAVEFORM_MODE_DU4 only when deviceQuirks.hasEclipseWfm
   is set, and SILENTLY DOWNGRADES IT TO GC4 otherwise -- so asking for DU4
   without the quirk leaves us reporting DU4 while the panel runs something
   else. So the quirk is required too, and sunxi (a different waveform enum) is
   excluded. Failing the gate falls back to A2 for FAST and GL16 for GRAY, the
   classic pre-Mk.9 pairing. FULL is GC16 everywhere: it is the ghost-clearing
   refresh, and a non-flashing one does not clear ghosting. */
/* Split from map_waveforms so the mapping can be REDONE on a running session
   -- the in-game MOTION entry changes wfm_fast_policy mid-game -- without
   needing an FBInkState the backend no longer has. The capability question is
   answered once, at init, off the state FBInk reported then; nothing about a
   panel's waveform table changes while koboy is running. */
static void probe_du4_capable(kobo_ctx *k, const FBInkState *st)
{
    int mark = platform_mark(st->device_platform);
    k->du4_capable = !st->is_sunxi && st->has_eclipse_wfm && mark >= 9;
}

static void map_waveforms(kobo_ctx *k)
{
    bool has_du4 = k->du4_capable;

    /* AUTO hands the choice to the EPDC driver, which looks at the actual pixel
       transitions in the region. That is the right default: it is the only
       party that knows whether a given update is erasing (dark -> light), which
       is exactly what a non-flashing waveform cannot do. Forcing a waveform
       means overriding that judgement on every single refresh. */
    if (k->cfg.wfm_fast_policy == KOBOY_WFM_DU) {
        /* TWO-LEVEL, and NO capability gate: WFM_DU is in FBInk's "Common"
           block, so every mxcfb-era Kobo has it -- no quirk, no silent
           downgrade, unlike DU4 below.

           Here for `force_dither`, not on its own. FBInk's header:
           "on-screen pixels will be left as-is for new content that is *not*
           B&W". Against four-level output that means the two MIDDLE levels are
           pixels the panel declines to touch -- the failed forced-DU4
           experiment under a different number. Against genuinely 1-bit output
           every new value is black or white. docs/FOLLOWUPS.md #25.

           GRAY stays AUTO rather than following FAST, deliberately:
           KOBOY_REFRESH_GRAY means "this update has intermediate levels", and
           DU is by definition the waveform that cannot render them. */
        k->wfm[KOBOY_REFRESH_FAST] = WFM_DU;   k->wfm_name[KOBOY_REFRESH_FAST] = "DU";
        k->wfm[KOBOY_REFRESH_GRAY] = WFM_AUTO; k->wfm_name[KOBOY_REFRESH_GRAY] = "AUTO";
    } else if (k->cfg.wfm_fast_policy == KOBOY_WFM_AUTO) {
        k->wfm[KOBOY_REFRESH_FAST] = WFM_AUTO; k->wfm_name[KOBOY_REFRESH_FAST] = "AUTO";
        k->wfm[KOBOY_REFRESH_GRAY] = WFM_AUTO; k->wfm_name[KOBOY_REFRESH_GRAY] = "AUTO";
    } else if (has_du4) {
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

    /* No EVIOCGRAB to release here -- the pad is never grabbed in the first
       place, see gamepad_rescan. Just close it. */
    if (k->pad_fd >= 0) { close(k->pad_fd); k->pad_fd = -1; }
}

static kobo_ctx *g_emergency;

/* Only for signals meaning "this process is about to die without cleanup".
   SIGTERM/SIGINT are deliberately NOT handled here -- main.c installs its own
   after platform init and reaches shutdown() normally. ioctl() and close() are
   async-signal-safe, so this is legal in a handler; nothing else is
   attempted. */
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

/* Forward-declared here (defined below, beside the rest of the vtable) so
   both open_input()'s callees and gamepad_rescan() can time-gate against the
   same clock the main loop uses, instead of a second clock_gettime call site
   that could drift from it. */
static uint64_t kobo_now_us(void *ctx);

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

static bool node_has_abs(int fd, unsigned int code)
{
    unsigned char bits[(ABS_MAX + 7) / 8];
    memset(bits, 0, sizeof bits);
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof bits), bits) < 0) return false;
    return (bits[code / 8] & (1u << (code % 8))) != 0;
}

/* WHAT THE PANEL SAYS IT CAN SEND, logged unconditionally at open time.
 *
 * This line exists because of what it cost not to have it. koboy could not be
 * tapped on a Kobo Aura H2O (github issue #1); the fix was designed against the
 * event streams FBInk's button injector synthesises, because that was the only
 * description of those panels anywhere to hand -- and an injector is a guess
 * about a driver, not a recording of one. The first fix did not work, and the
 * log from the device could not say why, because it reported the axis MAXIMA
 * and nothing about the PROTOCOL.
 *
 * Every field below is one ioctl and answers a question that otherwise takes a
 * round trip to a stranger's device:
 *   slot=1   protocol B -- contacts are addressed by ABS_MT_SLOT.
 *   slot=0 with mt=1  protocol A -- contacts are separated by SYN_MT_REPORT
 *            and the decode has to count them (input.c).
 *   btn_touch=0  the lift CANNOT be read from BTN_TOUCH on this panel, which
 *            is the assumption the first fix rested on.
 *   st=1 mt=0  a pre-multitouch panel: position is ABS_X/ABS_Y only.
 * `trace_touch = true` in koboy.ini then dumps the events themselves. */
static void touch_caps(kobo_ctx *k, int fd)
{
    kobo_say(k, "koboy: touch caps mt=%d slot=%d st=%d btn_touch=%d "
                "tool_finger=%d pressure=%d mt_pressure=%d touch_major=%d\n",
             node_has_abs(fd, ABS_MT_POSITION_X) ? 1 : 0,
             node_has_abs(fd, ABS_MT_SLOT)       ? 1 : 0,
             node_has_abs(fd, ABS_X)             ? 1 : 0,
             node_has_key(fd, BTN_TOUCH)         ? 1 : 0,
             node_has_key(fd, BTN_TOOL_FINGER)   ? 1 : 0,
             node_has_abs(fd, ABS_PRESSURE)      ? 1 : 0,
             node_has_abs(fd, ABS_MT_PRESSURE)   ? 1 : 0,
             node_has_abs(fd, ABS_MT_TOUCH_MAJOR)? 1 : 0);
}

/* Classification from fbink_input_scan, NOT hardcoded event<N> numbers: the
   numbering is not stable across firmwares (on the reference device touch is
   event1, keys event0, the accelerometer event2). SCAN_ONLY makes FBInk close
   every fd it opened; we reopen the two we want, so the open flags and grabs
   are ours. */
/* Reads the axis maxima and derives the transposition for an already-open
   touchscreen fd. Shared by the scan path and the override path below. */
static void touch_axes(kobo_ctx *k, int fd, const char *path, const char *name)
{
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
             path, name, mx, my, k->transpose ? 1 : 0);
    /* Both scan paths reach here, so the capability line cannot be missed by
       taking the KOBOY_TOUCH_DEV override instead of the classification. */
    touch_caps(k, fd);
}

static void open_input(kobo_ctx *k)
{
    /* Diagnostic override, in the same spirit as KOBOY_TRACE_REFRESH: name the
       touchscreen node explicitly instead of trusting classification. Needed to
       drive the emulator from a synthetic uinput device during testing, and a
       genuine escape hatch on a Kobo whose touch node FBInk does not classify
       as a touchscreen. */
    const char *forced = getenv("KOBOY_TOUCH_DEV");
    if (forced && forced[0]) {
        int fd = open(forced, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            kobo_say(k, "koboy: KOBOY_TOUCH_DEV=%s cannot be opened: %s\n",
                     forced, strerror(errno));
        } else {
            k->touch_fd = fd;
            touch_axes(k, fd, forced, "forced");
        }
    }

    /* NO SCAN_ONLY, and this is not a preference. FBInk opens every node it
       classifies; SCAN_ONLY makes it close them again, and koboy used to
       reopen the two it wanted so that the open flags and the grabs were its
       own. On one panel that close-and-reopen is a hazard FBInk documents by
       name -- fbink_input_scan.c, on `zForce-ir-touch`:
    
         "Some zForce panels have a weird race condition around
          Active/Deactivate commands, which can potentially lead to no reports
          being generated if the Active command fumbles. [...] A better bet is
          to avoid using SCAN_ONLY and keep the fd around if you actually need
          it."
    
       open() and close() on that node are Active and Deactivate to the panel's
       firmware, so the old path sent Active/Deactivate/Active on every launch
       and a fumbled third command means a touchscreen that reports NOTHING for
       the whole session. The Kobo Aura H2O of github issue #1 has exactly that
       panel. Taking FBInk's fd is one command instead of three.
    
       The flags survive the change: without OPEN_BLOCKING FBInk opens
       O_NONBLOCK, which is what drain() needs, and CLOEXEC is set below
       because a launcher that later spawns anything must not leak the panel.
       The grabs are unaffected -- they are ioctls on whatever fd we hold. */
    size_t            n   = 0;
    FBInkInputDevice *dev = fbink_input_scan(INPUT_TOUCHSCREEN | INPUT_KEY, 0,
                                             NO_RECAP, &n);
    if (!dev) {
        kobo_say(k, "koboy: fbink_input_scan found no input devices\n");
        return;
    }

    for (size_t i = 0; i < n; i++) {
        const FBInkInputDevice *d = &dev[i];
        /* EVERY fd this loop does not adopt has to be closed here: FBInk left
           them open for us, so `continue` now leaks one. */
        int fd = d->fd;

        /* The accelerometer is on its own node and reports a continuous stream
           of nonsense as far as we are concerned. Never read it. */
        if (d->type & INPUT_ACCELEROMETER) { if (fd >= 0) close(fd); continue; }
        if (fd < 0) continue;              /* FBInk could not open it */

        if ((d->type & INPUT_TOUCHSCREEN) && k->touch_fd < 0) {
            fcntl(fd, F_SETFD, FD_CLOEXEC);
            k->touch_fd = fd;

            /* Protocol B first, protocol A as the fallback. The transposition
               is DETECTED, not tabulated: on the reference device ABS_X tops
               out at 1680 and ABS_Y at 1264, on a panel 1264 wide and 1680
               tall, i.e. the touch layer is mounted rotated. Comparing the two
               maxima catches that on any device without a per-model table. */
            touch_axes(k, fd, d->path, d->name);
            continue;
        }

        /* A key node. Touchscreens also advertise EV_KEY (BTN_TOUCH), so skip
           anything already claimed as the touchscreen. */
        if ((d->type & INPUT_KEY) && !(d->type & INPUT_TOUCHSCREEN) &&
            k->n_key < MAX_KEY_NODES) {
            fcntl(fd, F_SETFD, FD_CLOEXEC);
            int idx = k->n_key++;
            k->key_fd[idx]        = fd;
            k->key_has_power[idx] = (d->type & INPUT_POWER_BUTTON) ||
                                    node_has_key(fd, KEY_POWER);
            kobo_say(k, "koboy: keys %s (%s)%s\n", d->path, d->name,
                     k->key_has_power[idx] ? " [carries KEY_POWER]" : "");
            continue;
        }

        /* Classified, opened by FBInk, and not wanted: a second touchscreen, a
           key node past MAX_KEY_NODES, or the node KOBOY_TOUCH_DEV already
           took by hand. */
        close(fd);
    }
    free(dev);

    /* Grabbing keeps a stray Nickel restart from stealing or double-handling
       input. But NEVER grab a node carrying KEY_POWER: the page-turn buttons
       and the power button are the same gpio-keys node here, and swallowing
       power presses on a device whose only other way out is a paperclip reset
       is not a trade worth making. The node is still READ, so both work. */
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

/* A gamepad is found through btinput_scan (/proc/bus/input/devices), not
   fbink_input_scan: FBInk's scan classifies only touchscreen/key/power/
   accelerometer nodes. Called on its own schedule rather than once at start-up
   because MEASURED: BlueZ reconnects a paired gamepad on its own timetable,
   frequently AFTER koboy has started, so a start-up-only scan would find
   nothing and never look again.

   Rate-limited to once a second via now_us(): this reads /proc, and calling it
   from the 60 Hz poll loop would turn a hot-plug convenience into a syscall
   storm. No-ops when a pad is already open. */
static void gamepad_rescan(kobo_ctx *k)
{
    if (k->pad_fd >= 0) return;

    uint64_t now = kobo_now_us(k);
    if (now < k->pad_scan_at) return;
    k->pad_scan_at = now + 1000000ull;   /* live: caps a /proc read to 1/s */

    char node[32];
    if (btinput_scan(node, sizeof node) != 1) return;   /* none found this pass */

    int fd = open(node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        kobo_say(k, "koboy: cannot open gamepad %s: %s\n", node, strerror(errno));
        return;
    }

    /* Deliberately NOT EVIOCGRAB'd. MEASURED 2026-08-26: Nickel grabs the
       touchscreen and key nodes but reads the gamepad node WITHOUT grabbing
       it, so there is nothing to compete with. A grabbed fd must also be
       tracked and released on every exit path -- skipping the grab avoids that
       class of bug for a device where grabbing buys nothing. */
    k->pad_fd = fd;
    snprintf(k->pad_node, sizeof k->pad_node, "%s", node);
    kobo_say(k, "koboy: gamepad %s\n", k->pad_node);
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

    /* ORIGIN. FBInkState.view_vert_origin is NOT the framebuffer viewport
       origin: FBInk documents it as "viewport + viewVertOffset", where
       viewVertOffset is a TEXT-LAYOUT shift that vertically balances whole
       character rows. The reference device sets no koboVertOffset
       (fbink_device_id.c), so its real viewport origin is 0 and the reported
       viewVertOrigin of 8 is purely font-row centring -- adding it to a pixel
       blit pushes the image down 8 px and clips the bottom 8 rows.
       So origin = origin - offset: 0 here, non-zero only on the few Kobos that
       really do hide rows behind the bezel. */
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

    probe_du4_capable(k, &st);
    map_waveforms(k);

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

    /* One attempt right away, so a pad already connected before launch shows
       up in --selftest beside the touch/keys lines rather than only after the
       first poll_input. kobo_poll_input's own call covers the commoner case:
       the pad reconnecting on BlueZ's schedule, after koboy started. */
    gamepad_rescan(k);

    /* input.c applies transpose first, then the mirrors -- the same order
       FBInk defines its quirks in, so the flags carry over directly, BUT ONLY
       IF the two agree about the swap. If they disagree the mirrors came from
       a different convention: trust the measured maxima for the swap and drop
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
 * NOT fbink_print_raw_data: that would have to be told the offsets anyway and
 * copies the whole input buffer once to normalise the pixel format. Here the
 * destination format is known from fbink_get_state and the source is already
 * the exact gray8 the panel wants, so the one thing that must not be wrong is
 * the STRIDE -- spelled out per row rather than folded into a library call.
 *
 * All three plausible depths are handled, so `fbdepth -d 8` in the launcher is
 * an OPTIMISATION (a memcpy per row, a quarter of the bandwidth), not a
 * precondition. */
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

/* Submit and return. fbink_wait_for_complete() is NOT called, here or
   anywhere: at the shipped 5x rect the same refresh measured 15.0 ms submitted
   against 39.2 ms waited, and the emulator has 16.7 ms per core frame. The
   EPDC queues and merges on its own. (The reference device is also flagged
   unreliableWaitFor, so the wait can time out outright.) */
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

    uint64_t t0 = kobo_now_us(k);
    bool ok = fbink_refresh(k->fbfd,
                            (uint32_t)(y + k->origin_y), (uint32_t)(x + k->origin_x),
                            (uint32_t)w, (uint32_t)h, &cfg) == EXIT_SUCCESS;
    uint64_t dt = kobo_now_us(k) - t0;

    /* Submission cost only -- we never wait for the panel, so this is what the
       emulator loop actually pays, not how long the update takes to appear. */
    k->rf_n[mode]++;
    k->rf_us[mode] += dt;
    if (dt > k->rf_max_us[mode]) k->rf_max_us[mode] = dt;

    /* Off by default, and the only way to see from off-device which rectangle
       each waveform is actually being asked for -- in particular that the
       periodic cleanup asks for the game rect and not the whole panel. SDL
       ignores waveform modes entirely, so this is not observable on the
       desktop at all. */
    if (k->trace)
        kobo_say(k, "koboy: refresh %-4s %s %dx%d at (%d,%d) -> fb (%d,%d) %luus\n",
                 k->wfm_name[mode], k->flash[mode] ? "flash" : "     ",
                 w, h, x, y, x + k->origin_x, y + k->origin_y,
                 (unsigned long)dt);
    return ok;
}

/* Drains one node. EVERY event goes to input.c as a koboy_ev, including EV_KEY
   and the SYN boundaries, so protocol-B slot tracking and packet coherency
   live in one tested place rather than half here.

   Returns false ONLY when the node is gone for good: read(2) on an fd whose
   device node was removed returns -1/ENODEV, which is how a hot-plugged
   gamepad announces a disconnect (an EAGAIN from an empty queue, the ordinary
   case, is NOT that). The built-in nodes never take this path, but checking
   unconditionally saves a second parallel drain function. */
static bool drain(kobo_ctx *k, int fd, bool is_key, koboy_input *in)
{
    struct input_event ev[EV_BATCH];
    koboy_ev           out[EV_BATCH];

    /* Bounded rather than "until EAGAIN": a wedged or flooding node must not be
       able to hold the emulator loop open indefinitely. 16 full batches is
       1024 events, orders of magnitude more than one 60Hz tick can produce. */
    for (int pass = 0; pass < 16; pass++) {
        errno = 0;
        ssize_t n = read(fd, ev, sizeof ev);
        if (n <= 0) return !(n < 0 && errno == ENODEV);
        size_t cnt = (size_t)n / sizeof ev[0];
        if (cnt == 0) return true;

        size_t m = 0;
        for (size_t i = 0; i < cnt; i++) {
            /* THE RAW STREAM, when koboy.ini asks for it. One line per event,
               before koboy interprets any of it -- the point is to record what
               the PANEL said, not what the decode made of it, because the two
               disagreeing is the whole class of bug this exists for. SYN
               (type 0) is printed too: on a panel with no ABS_MT_SLOT the
               SYN_MT_REPORT boundaries are the only thing separating one
               finger from another, so a trace that dropped them would hide
               exactly what it was opened to find. */
            if (k->cfg.trace_touch && !is_key)
                kobo_say(k, "koboy: ev type=%u code=%u value=%d\n",
                         (unsigned)ev[i].type, (unsigned)ev[i].code,
                         (int)ev[i].value);
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
        /* The SOURCE, not just the events: `is_key` is exactly the
           touch-versus-buttons distinction input.c needs, because ABS_X/ABS_Y
           are a finger on one node and an analog stick on the other, and
           BTN_TOUCH is a contact flag only on the touchscreen. */
        if (in && m)
            input_feed_from(in, is_key ? KOBOY_EV_SRC_BUTTONS
                                       : KOBOY_EV_SRC_TOUCH, out, m);
        if (cnt < EV_BATCH) return true;       /* short read: nothing left */
    }
    return true;
}

static bool kobo_poll_input(void *ctx, struct koboy_input *in)
{
    kobo_ctx *k = ctx;
    if (k->touch_fd >= 0) drain(k, k->touch_fd, false, in);
    for (int i = 0; i < k->n_key; i++) drain(k, k->key_fd[i], true, in);

    /* is_key=true: the pad's buttons feed raw_push() the way the page-turn
       keys do, so calibration can capture them the same way. The hat's
       ABS_HAT0X/Y events go through regardless -- drain() forwards every event
       type, and input.c's hat decode makes d-pad bits of them. */
    if (k->pad_fd >= 0 && !drain(k, k->pad_fd, true, in)) {
        kobo_say(k, "koboy: gamepad %s lost\n", k->pad_node);
        close(k->pad_fd);
        k->pad_fd = -1;
    }

    /* Cheap when a pad is already open (see gamepad_rescan's own early
       return) or when the last scan is still within its one-second window,
       so calling this every poll costs nothing extra in the common case. */
    gamepad_rescan(k);
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

/* Kobo exposes battery capacity through the standard power-supply class. The
   node name differs by model, so the directory is scanned rather than
   hardcoded -- the same capability-detection rule the rest of the backend
   follows. A missing or unreadable node is -1, not an error. */
/* Contract in platform_if.h. Re-runs the whole mapping rather than poking one
   slot, so FAST and GRAY cannot drift apart: map_waveforms is the one place
   that decides what a policy means.
   NOTHING IS REFRESHED here on purpose -- a waveform selects how the NEXT
   update is drawn, and the caller is already repainting with GC16. */
static void kobo_set_wfm_policy(void *ctx, koboy_wfm_policy policy)
{
    kobo_ctx *k = ctx;
    if (!k) return;
    k->cfg.wfm_fast_policy = (int)policy;
    map_waveforms(k);
}

/* The REAL waveform, not an echo of the policy: a DU4 request on a panel
   without the eclipse quirk lands on A2 (see map_waveforms), and this is the
   only thing that says so out loud. */
static const char *kobo_wfm_fast_name(void *ctx)
{
    kobo_ctx *k = ctx;
    return (k && k->wfm_name[KOBOY_REFRESH_FAST]) ? k->wfm_name[KOBOY_REFRESH_FAST]
                                                  : "AUTO";
}

static int kobo_battery_percent(void *ctx)
{
    (void)ctx;
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return -1;
    int pct = -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof path, "/sys/class/power_supply/%s/capacity",
                 e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        int v = -1;
        if (fscanf(f, "%d", &v) == 1 && v >= 0 && v <= 100) pct = v;
        fclose(f);
        if (pct >= 0) break;
    }
    closedir(d);
    return pct;
}

/* ------------------------------------------------------------------- ctor */

bool            platform_poll_raw_key(koboy_platform *pf, uint16_t *code);

koboy_platform *platform_kobo_create(void)
{
    koboy_platform *pf = calloc(1, sizeof *pf);
    if (!pf) return NULL;
    kobo_ctx *k = calloc(1, sizeof *k);
    if (!k) { free(pf); return NULL; }

    k->fbfd     = -1;
    k->touch_fd = -1;
    k->pad_fd   = -1;
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
    pf->battery_percent = kobo_battery_percent;
    pf->set_wfm_policy  = kobo_set_wfm_policy;
    pf->wfm_fast_name   = kobo_wfm_fast_name;
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
    printf("wfm_du4_capable=%d\n", k->du4_capable ? 1 : 0);
    printf("touch_transpose=%d\n", k->transpose ? 1 : 0);
    printf("touch_raw=%dx%d\n", k->raw_max_x, k->raw_max_y);
    printf("input_touch=%d\n", k->touch_fd >= 0 ? 1 : 0);
    printf("input_keys=%d\n", k->n_key);
    printf("input_pad=%d\n", k->pad_fd >= 0 ? 1 : 0);
    fflush(stdout);
}

/* Printed after the loop, not with the facts above: these are measured in this
   run. Cross-session comparison of refresh cost on this panel is worthless --
   Appendix B records ~45% run-to-run variance -- so AUTO versus forced DU4 has
   to be measured fresh, side by side, on the same content. */
void platform_kobo_refresh_stats(koboy_platform *pf)
{
    kobo_ctx          *k     = pf->ctx;
    static const char *nm[3] = { "fast", "gray", "full" };
    for (int i = 0; i < 3; i++) {
        if (!k->rf_n[i]) continue;
        printf("refresh_%s=%s n=%lu mean_us=%lu max_us=%lu\n",
               nm[i], k->wfm_name[i], (unsigned long)k->rf_n[i],
               (unsigned long)(k->rf_us[i] / k->rf_n[i]),
               (unsigned long)k->rf_max_us[i]);
    }
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
