/* koboy-probe: a two-mode device profiler.
 *
 * koboy has only ever run on one physical device -- a Kobo Libra 2, see
 * TESTED.md -- because that is the only one anyone building it owns. Every
 * other Kobo family FBInk supports (Clara, Sage, Elipsa, Libra Colour) is a
 * hardware unknown: right panel size on paper, but an unmeasured stride, an
 * unmeasured waveform table, unmeasured touch geometry. This is how someone
 * who *does* own one of those turns "unmeasured" into a pasteable TESTED.md
 * row without reading a line of the rest of this project's C.
 *
 * Two modes exist because Nickel's input grab (see platform_kobo.c) splits
 * the job in half:
 *
 *  --coexist (default). Everything that does NOT require reading an actual
 *  input event: device identity, panel geometry, input node *capabilities*
 *  (names, advertised keys, axis ranges -- all queryable without taking a
 *  single event Nickel is entitled to), and a refresh-timing sweep across
 *  waveform modes and region sizes. It never calls EVIOCGRAB, never changes
 *  framebuffer depth, and has no restore path to get wrong, so it is safe to
 *  run with the reader UI on screen. This is the mode that produced the
 *  design spec's Appendix A.
 *
 *  --takeover. For the one thing coexisting mode structurally cannot do:
 *  read real button presses and touch points, which needs a readable input
 *  node, and Nickel's EVIOCGRAB means no other process gets a single event
 *  while it holds one. An earlier version of this design had takeover mode
 *  stop Nickel itself and restore it afterwards -- but that duplicates the
 *  launcher's entire restore path (scripts/koboy.sh: hindenburg, the Wi-Fi
 *  teardown, the device-identity hazard documented in README.md) inside a
 *  tool whose only job is to write a text file, for no measurement this
 *  mode actually needs. So instead it REFUSES to run while Nickel is up --
 *  checked by looking for the process -- and says so, rather than silently
 *  reading nothing and leaving whoever ran it to wonder why every field came
 *  back empty. That silent-empty-result outcome is the trap the original
 *  design fell into. Stopping Nickel first is the caller's job; koboy.sh's
 *  own stop sequence is the reference for how.
 *
 * Either mode writes /mnt/onboard/koboy-probe-<device>.txt as key=value
 * lines, one fact per line, so a TESTED.md row can be assembled by eye.
 * --coexist opens it fresh (a re-run should not accumulate stale sweep
 * data); --takeover opens it in APPEND mode and adds a dated block, so the
 * documented workflow of running --coexist and then --takeover on the same
 * device leaves one file carrying both -- panel/stride/touch/sweep facts
 * plus captured key codes and touch samples -- rather than the takeover run
 * quietly truncating away everything --coexist had just written.
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <fbink.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <linux/input.h>

#ifndef INPUT_PROP_ACCELEROMETER
#define INPUT_PROP_ACCELEROMETER 0x06
#endif

/* ------------------------------------------------------------- output sink */

/* Every fact is printed to stdout (so `koboy-probe --coexist` run
   interactively shows something immediately) AND to the device-facing
   report file, in lockstep, rather than building the file in memory and
   dumping it once at the end -- a crash or a killed ssh session midway still
   leaves a partial, still-useful file rather than nothing at all. */
static FILE *g_sinks[2];
static int   g_nsinks;

static void kv(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void kv(const char *fmt, ...)
{
    for (int i = 0; i < g_nsinks; i++) {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(g_sinks[i], fmt, ap);
        va_end(ap);
        fflush(g_sinks[i]);
    }
}

static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------ misc helpers */

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

/* Sanitised for use inside a filename: FBInk's device_name/device_codename
   fields are short human strings ("Libra 2", "Io") that may contain spaces
   FAT filesystems tolerate but shells and TESTED.md pasting do not need. */
static void sanitize(char *out, size_t n, const char *in)
{
    size_t j = 0;
    for (size_t i = 0; in && in[i] && j + 1 < n; i++) {
        unsigned char c = (unsigned char)in[i];
        out[j++] = (isalnum(c) || c == '-') ? (char)c : '_';
    }
    out[j] = 0;
    if (!j) snprintf(out, n, "unknown");
}

/* Is a process named exactly `name` (per /proc/<pid>/comm, which the kernel
   truncates to 15 bytes -- fine for "nickel") currently alive? Scanning
   /proc rather than shelling out to pgrep/pkill: this binary has no shell
   dependency otherwise, and a missing busybox applet must not silently make
   --takeover think Nickel is gone. */
static bool process_running(const char *name)
{
    DIR *d = opendir("/proc");
    if (!d) return false;
    bool found = false;
    struct dirent *de;
    while (!found && (de = readdir(d)) != NULL) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        char path[64];
        snprintf(path, sizeof path, "/proc/%s/comm", de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char buf[32] = { 0 };
        if (fgets(buf, sizeof buf, f)) {
            size_t l = strlen(buf);
            while (l && (buf[l - 1] == '\n' || buf[l - 1] == '\r')) buf[--l] = 0;
            if (!strcmp(buf, name)) found = true;
        }
        fclose(f);
    }
    closedir(d);
    return found;
}

/* ---------------------------------------------------------- median helper */

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static uint64_t median_u64(uint64_t *v, int n)
{
    if (n <= 0) return 0;
    qsort(v, (size_t)n, sizeof *v, cmp_u64);
    return v[n / 2];
}

/* =========================================================================
 * Waveform gating -- mirrors platform_kobo.c's map_waveforms(), on purpose:
 * that logic is already validated on the reference device (Appendix B), and
 * this probe's DU4-capability finding must agree with what the emulator
 * itself would decide on the same hardware, or a contributor's TESTED.md row
 * would describe a different device than the one koboy actually runs on.
 * ========================================================================= */

static int platform_mark(const char *plat)
{
    int n = 0;
    if (!plat) return 0;
    if (sscanf(plat, "Mark %d", &n) == 1) return n;
    return 0;
}

static bool has_du4_capability(const FBInkState *st)
{
    return !st->is_sunxi && st->has_eclipse_wfm && platform_mark(st->device_platform) >= 9;
}

/* =========================================================================
 * Input capabilities, parsed from /proc/bus/input/devices.
 *
 * Deliberately NOT fbink_input_scan(): that call is exactly what
 * platform_kobo.c uses at runtime, and reusing it here would make this
 * report only as trustworthy as fbink_input_scan's own classification. A
 * probe's job is to describe the hardware in a way a human can cross-check,
 * so it reads the same kernel-exposed bitmasks a person could read by hand
 * over `cat /proc/bus/input/devices`, and says exactly which bit test led to
 * each classification (see the comments below).
 *
 * The one thing that file does NOT carry is axis range (min/max) -- only
 * "this axis exists", not "and its maximum is". For that this still opens
 * the node, read-only and non-blocking, and calls EVIOCGABS. That ioctl (and
 * EVIOCGBIT, not used here because /proc's KEY= line already gives us the
 * same bitmask) queries the device's own capability tables, not the event
 * queue delivery a grab controls -- so doing this never takes anything away
 * from Nickel, which is why it is safe in --coexist mode at all.
 * ========================================================================= */

#define MAX_NODES     16
#define MAX_KEY_NODES 4
#define MAX_KEY_CODES 24
#define BITLINE_WORDS 64

typedef struct {
    char     path[32];   /* /dev/input/eventN */
    char     name[256];
    bool     have;       /* this record actually named an event node */
    unsigned long ev[BITLINE_WORDS];   int n_ev;
    unsigned long key[BITLINE_WORDS];  int n_key;
    unsigned long abs[BITLINE_WORDS];  int n_abs;
    unsigned long prop[BITLINE_WORDS]; int n_prop;
} raw_node;

/* Parses "e630000 0 3" (the part of a "B: ABS=..." line after the '=') into
   an array of words IN THE ORDER PRINTED, i.e. most-significant word first.
   The kernel's own bitmap printer (bitmap_print_to_pagebuf) walks from the
   highest populated word down to word 0, so bit N lives in
   words[count - 1 - N/BITS_PER_LONG], not words[N/BITS_PER_LONG] -- callers
   must go through bit_test()/bit_list() below, never index `words` raw. */
static int parse_bitline(const char *s, unsigned long *words, int cap)
{
    int n = 0;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", s);
    char *save = NULL;
    for (char *tok = strtok_r(buf, " \t\r\n", &save); tok && n < cap;
         tok = strtok_r(NULL, " \t\r\n", &save))
        words[n++] = strtoul(tok, NULL, 16);
    return n;
}

static bool bit_test(const unsigned long *words, int n, int bit)
{
    if (n <= 0 || bit < 0) return false;
    int word_idx    = bit / (int)(sizeof(unsigned long) * 8);
    int pos_from_lo = n - 1 - word_idx;
    if (pos_from_lo < 0 || pos_from_lo >= n) return false;
    return (words[pos_from_lo] >> (bit % (int)(sizeof(unsigned long) * 8))) & 1UL;
}

/* Lists every set bit up to max_bit, for reporting "advertised key codes"
   rather than just a yes/no per code. */
static int bit_list(const unsigned long *words, int n, int max_bit,
                    uint16_t *out, int out_cap)
{
    int cnt = 0;
    for (int bit = 0; bit <= max_bit && cnt < out_cap; bit++)
        if (bit_test(words, n, bit)) out[cnt++] = (uint16_t)bit;
    return cnt;
}

/* Reads /proc/bus/input/devices once and returns the nodes that named a
   /dev/input/eventN handler (i.e. skips mice/js/other non-evdev handlers,
   and skips devices with no handler at all). */
static int scan_proc_input(raw_node *out, int cap)
{
    FILE *f = fopen("/proc/bus/input/devices", "r");
    if (!f) {
        note("koboy-probe: cannot open /proc/bus/input/devices: %s\n", strerror(errno));
        return 0;
    }

    int       n   = 0;
    raw_node  cur; memset(&cur, 0, sizeof cur);
    char      line[1200];

    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;

        if (line[0] == 0) {                 /* blank line: record separator */
            if (cur.have && n < cap) out[n++] = cur;
            memset(&cur, 0, sizeof cur);
            continue;
        }
        if (!strncmp(line, "N: Name=", 8)) {
            const char *q1 = strchr(line + 8, '"');
            const char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            if (q1 && q2 && q2 > q1) {
                size_t len = (size_t)(q2 - q1 - 1);
                if (len >= sizeof cur.name) len = sizeof cur.name - 1;
                memcpy(cur.name, q1 + 1, len);
                cur.name[len] = 0;
            }
        } else if (!strncmp(line, "H: Handlers=", 12)) {
            char buf[256];
            snprintf(buf, sizeof buf, "%s", line + 12);
            char *save = NULL;
            for (char *tok = strtok_r(buf, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
                if (!strncmp(tok, "event", 5) && isdigit((unsigned char)tok[5])) {
                    snprintf(cur.path, sizeof cur.path, "/dev/input/%s", tok);
                    cur.have = true;
                }
            }
        } else if (!strncmp(line, "B: EV=", 6)) {
            cur.n_ev = parse_bitline(line + 6, cur.ev, BITLINE_WORDS);
        } else if (!strncmp(line, "B: KEY=", 7)) {
            cur.n_key = parse_bitline(line + 7, cur.key, BITLINE_WORDS);
        } else if (!strncmp(line, "B: ABS=", 7)) {
            cur.n_abs = parse_bitline(line + 7, cur.abs, BITLINE_WORDS);
        } else if (!strncmp(line, "B: PROP=", 8)) {
            cur.n_prop = parse_bitline(line + 8, cur.prop, BITLINE_WORDS);
        }
    }
    if (cur.have && n < cap) out[n++] = cur;   /* file need not end in a blank line */
    fclose(f);
    return n;
}

static int abs_max_ioctl(int fd, unsigned int axis, int fallback)
{
    struct input_absinfo ai;
    memset(&ai, 0, sizeof ai);
    if (ioctl(fd, EVIOCGABS(axis), &ai) < 0) return fallback;
    if (ai.maximum <= ai.minimum) return fallback;
    return ai.maximum;
}

/* ========================================================================
 * Refresh-timing sweep.
 * ======================================================================== */

typedef struct { const char *name; uint8_t wfm; bool flash; } wfm_case;

typedef struct { int w, h; } region_size;

/* Trimmed from Appendix A's full table (which also carried DU and GL16) to
   keep a probe run under about a minute: DU4-vs-A2 is the gate that matters
   per device, GC16 is what the periodic full-flash pays, and AUTO is what
   koboy actually ships. Appendix A already established DU/GL16's place in
   the hierarchy on the one device anyone has measured; re-deriving that
   ranking is not this tool's job. */
static const region_size REGIONS[] = {
    { 1120, 1008 },  /* Appendix A's own region -- the direct cross-check */
    { 800, 720 },    /* koboy's shipped default game rect (5x) */
    { 560, 504 },
    { 320, 288 },
    { 160, 144 },
};
#define N_REGIONS (int)(sizeof(REGIONS) / sizeof(REGIONS[0]))
#define SWEEP_PASSES 3

/* Solid fill, honouring stride/bpp/inversion exactly like
   platform_kobo.c's kobo_blit_gray8 -- duplicated rather than shared because
   that function blits a caller-owned buffer and this only ever needs one
   flat value, and pulling platform_kobo.c's vtable machinery into a
   standalone profiler for one memset would be the wrong direction of
   dependency. */
static void fb_fill(unsigned char *fbmem, uint32_t stride, uint32_t bpp, bool inv,
                    int origin_x, int origin_y, int x, int y, int w, int h, uint8_t val)
{
    uint32_t       bytes = bpp / 8;
    unsigned char *base  = fbmem + (size_t)(y + origin_y) * stride
                                  + (size_t)(x + origin_x) * bytes;
    for (int row = 0; row < h; row++) {
        unsigned char *dp = base + (size_t)row * stride;
        switch (bpp) {
        case 8:
            memset(dp, inv ? (uint8_t)(0xFFu - val) : val, (size_t)w);
            break;
        case 16: {
            uint16_t v = (uint16_t)(((val & 0xF8u) << 8) | ((val & 0xFCu) << 3) | (val >> 3));
            for (int col = 0; col < w; col++) memcpy(dp + (size_t)col * 2, &v, sizeof v);
            break;
        }
        default:
            for (int col = 0; col < w; col++) {
                unsigned char *p = dp + (size_t)col * 4;
                p[0] = p[1] = p[2] = val; p[3] = 0xFF;
            }
            break;
        }
    }
}

/* One (mode, region) cell of the sweep: SWEEP_PASSES refreshes, each timed
   twice -- once for bare submission, once including
   fbink_wait_for_complete(). The fill value alternates white/black across
   every single call in the whole sweep (via *toggle), never just within one
   cell, so every measured refresh is a genuine erase-or-set transition and
   not a no-op the EPDC (especially under AUTO, which inspects the actual
   pixel histogram) could shortcut. */
static void sweep_cell(int fbfd, unsigned char *fbmem, uint32_t stride, uint32_t bpp,
                       bool inv, int origin_x, int origin_y, int w, int h,
                       const wfm_case *wc, FBInkConfig base_cfg, uint8_t *toggle,
                       uint64_t *submit_us, uint64_t *block_us, uint64_t *submit_max,
                       uint64_t *block_max)
{
    FBInkConfig cfg  = base_cfg;
    cfg.wfm_mode     = wc->wfm;
    cfg.is_flashing  = wc->flash;
    uint64_t s[SWEEP_PASSES], b[SWEEP_PASSES];
    *submit_max = 0; *block_max = 0;

    for (int p = 0; p < SWEEP_PASSES; p++) {
        *toggle = (*toggle == 0xFF) ? 0x00 : 0xFF;
        fb_fill(fbmem, stride, bpp, inv, origin_x, origin_y, 0, 0, w, h, *toggle);

        uint64_t t0 = now_us();
        fbink_refresh(fbfd, (uint32_t)origin_y, (uint32_t)origin_x,
                      (uint32_t)w, (uint32_t)h, &cfg);
        uint64_t t1 = now_us();
        fbink_wait_for_complete(fbfd, LAST_MARKER);
        uint64_t t2 = now_us();

        s[p] = t1 - t0;
        b[p] = t2 - t0;
        if (s[p] > *submit_max) *submit_max = s[p];
        if (b[p] > *block_max)  *block_max  = b[p];
    }
    *submit_us = median_u64(s, SWEEP_PASSES);
    *block_us  = median_u64(b, SWEEP_PASSES);
}

/* ------------------------------------------------ sustained update period */

/* How many back-to-back updates one sustain cell issues, and how many of them
   are discarded before the median is taken. The EPDC hands out a small pool of
   update descriptors, so the first submissions return immediately regardless
   of how long the panel needs; only once the pool is exhausted does
   MXCFB_SEND_UPDATE start blocking on a slot, and only from then on does the
   loop run at the panel's rate. Two warmups empty the pool on every driver
   depth this code has seen; ten timed iterations then keep a cell under three
   seconds even at GC16 speeds. */
#define SUSTAIN_PASSES 12
#define SUSTAIN_WARMUP 2

/* A checkerboard of `cell`-sized squares, phase-shifted by `phase`. Two
   consecutive phases differ on about half the pixels, which is what a
   DITHERED scene scrolling under koboy actually asks the panel for; a solid
   black/white flip (fb_fill, above) is the 100%-transition worst case and
   nothing on screen ever looks like it. Both are measured because the gap
   between them is the difference between a pacing constant that is merely
   safe and one that is honest. */
static void fb_checker(unsigned char *fbmem, uint32_t stride, uint32_t bpp, bool inv,
                       int origin_x, int origin_y, int w, int h, int cell, int phase)
{
    uint32_t bytes = bpp / 8;
    for (int row = 0; row < h; row++) {
        unsigned char *dp = fbmem + (size_t)(row + origin_y) * stride
                                  + (size_t)origin_x * bytes;
        for (int col = 0; col < w; col++) {
            uint8_t val = (((col + phase) / cell + (row + phase) / cell) & 1)
                          ? 0xFFu : 0x00u;
            if (inv) val = (uint8_t)(0xFFu - val);
            unsigned char *p = dp + (size_t)col * bytes;
            switch (bpp) {
            case 8:  p[0] = val; break;
            case 16: { uint16_t v = (uint16_t)(((val & 0xF8u) << 8) |
                                               ((val & 0xFCu) << 3) | (val >> 3));
                       memcpy(p, &v, sizeof v); break; }
            default: p[0] = p[1] = p[2] = val; p[3] = 0xFF; break;
            }
        }
    }
}

/* THE MEASUREMENT THIS FILE GAINED FOR AREA-AWARE PACING, and it deliberately
   does NOT use fbink_wait_for_complete().

   Appendix B records why: this device reports `unreliable_wait_for=1`, and
   that flag applies to exactly the MXCFB_WAIT_FOR_UPDATE_COMPLETE ioctl every
   blocking figure in the sweep above depends on. A pacing constant derived
   from a suspect ioctl would be a guess wearing a measurement's clothes.

   What this measures instead is the rate at which the panel will ACCEPT work.
   Submit updates to one region back to back with no wait at all; once the
   driver's descriptor pool is full, each further submission blocks until an
   earlier update retires, so the loop settles at one iteration per completed
   update. The interval between iteration starts, in that steady state, IS the
   panel's period for that (waveform, area) -- measured through the same
   non-blocking path koboy's main loop uses, with no privileged ioctl in it.

   `fill_us` is reported alongside and is not noise: the loop period is
   max(panel period, fill + submit), so a cell whose fill cost approaches its
   period is measuring this process, not the panel. Read the two together or
   do not read either. */
static void sustain_cell(int fbfd, unsigned char *fbmem, uint32_t stride, uint32_t bpp,
                         bool inv, int origin_x, int origin_y, int w, int h,
                         const wfm_case *wc, FBInkConfig base_cfg, uint8_t *toggle,
                         bool pattern,
                         uint64_t *period_us, uint64_t *submit_us, uint64_t *fill_us)
{
    FBInkConfig cfg = base_cfg;
    cfg.wfm_mode    = wc->wfm;
    cfg.is_flashing = wc->flash;

    uint64_t iv[SUSTAIN_PASSES], su[SUSTAIN_PASSES], fu[SUSTAIN_PASSES];
    uint64_t prev = 0;
    int      n = 0;

    for (int p = 0; p < SUSTAIN_PASSES + SUSTAIN_WARMUP; p++) {
        uint64_t t0 = now_us();
        *toggle = (*toggle == 0xFF) ? 0x00 : 0xFF;
        if (pattern)
            /* 8 px cells at phase 0/8 -- an 8x8 tile is also koboy's dirty-diff
               granularity, so this is the coarsest pattern its own pipeline can
               still call "everything changed". */
            fb_checker(fbmem, stride, bpp, inv, origin_x, origin_y, w, h, 8,
                       (*toggle == 0xFF) ? 0 : 8);
        else
            fb_fill(fbmem, stride, bpp, inv, origin_x, origin_y, 0, 0, w, h, *toggle);
        uint64_t t1 = now_us();
        fbink_refresh(fbfd, (uint32_t)origin_y, (uint32_t)origin_x,
                      (uint32_t)w, (uint32_t)h, &cfg);
        uint64_t t2 = now_us();

        if (p >= SUSTAIN_WARMUP) {
            if (prev) iv[n] = t0 - prev;
            else      iv[n] = 0;          /* replaced below; first has no interval */
            su[n] = t2 - t1;
            fu[n] = t1 - t0;
            n++;
        }
        prev = t0;
    }
    /* The first retained pass has no predecessor inside the retained window,
       so its interval slot carries the warmup's -- which is exactly the
       not-yet-saturated iteration this window exists to exclude. Drop it by
       taking the median over [1, n) rather than [0, n). */
    *period_us = median_u64(iv + 1, n - 1);
    *submit_us = median_u64(su, n);
    *fill_us   = median_u64(fu, n);
}

/* ========================================================================
 * --coexist
 * ======================================================================== */

static int run_coexist(void)
{
    int fbfd = fbink_open();
    if (fbfd < 0) {
        note("koboy-probe: fbink_open failed: %s\n", strerror(errno));
        return 1;
    }
    FBInkConfig fb_cfg;
    memset(&fb_cfg, 0, sizeof fb_cfg);
    fb_cfg.is_quiet   = true;
    fb_cfg.no_refresh = true;
    if (fbink_init(fbfd, &fb_cfg) < 0) {
        note("koboy-probe: fbink_init failed\n");
        fbink_close(fbfd);
        return 1;
    }

    FBInkState st;
    memset(&st, 0, sizeof st);
    fbink_get_state(&fb_cfg, &st);

    /* Same ORIGIN derivation as platform_kobo.c's kobo_init(), and for the
       same reason: FBInkState.view_vert_origin already has FBInk's own
       text-row-balancing offset folded in (view_vert_origin =
       viewport-origin + view_vert_offset), so subtracting view_vert_offset
       back out is what recovers the real framebuffer viewport origin. On
       the reference device this is 0, not the raw field's 8. */
    int origin_x = (int)st.view_hori_origin;
    int origin_y = (int)st.view_vert_origin - (int)st.view_vert_offset;
    if (origin_x < 0) origin_x = 0;
    if (origin_y < 0) origin_y = 0;

    int      view_w = (int)st.view_width, view_h = (int)st.view_height;
    uint32_t stride = st.scanline_stride, bpp = st.bpp;

    if (view_w <= 0 || view_h <= 0 || stride == 0 ||
        (bpp != 8 && bpp != 16 && bpp != 32)) {
        note("koboy-probe: implausible framebuffer geometry %dx%d stride=%u bpp=%u\n",
             view_w, view_h, stride, bpp);
        fbink_close(fbfd);
        return 1;
    }

    char dev_id[64];
    sanitize(dev_id, sizeof dev_id, st.device_codename[0] ? st.device_codename : st.device_name);
    char path[256];
    snprintf(path, sizeof path, "/mnt/onboard/koboy-probe-%s.txt", dev_id);

    FILE *fout = fopen(path, "w");
    if (!fout) note("koboy-probe: cannot open %s for writing: %s (stdout only)\n",
                     path, strerror(errno));

    g_nsinks = 0;
    g_sinks[g_nsinks++] = stdout;
    if (fout) g_sinks[g_nsinks++] = fout;

    time_t t = time(NULL);
    char   tbuf[32];
    strftime(tbuf, sizeof tbuf, "%Y-%m-%dT%H:%M:%S", localtime(&t));

    kv("# koboy-probe report, mode=coexist, generated %s\n", tbuf);
    kv("# e-ink refresh timing varies roughly 45%% run to run on the one\n");
    kv("# measured device (see TESTED.md); treat sweep_* figures below as\n");
    kv("# order-of-magnitude, not as constants. Medians are over %d passes.\n",
       SWEEP_PASSES);
    kv("mode=coexist\n");
    kv("fbink_version=%s\n", fbink_version());

    /* ------------------------------------------------------ identity/panel */
    kv("device=%s\n", st.device_name);
    kv("device_id=%u\n", (unsigned)st.device_id);
    kv("device_codename=%s\n", st.device_codename);
    kv("platform=%s\n", st.device_platform);
    kv("panel=%dx%d\n", view_w, view_h);
    kv("screen=%ux%u\n", st.screen_width, st.screen_height);
    kv("dpi=%u\n", st.screen_dpi);
    kv("bpp=%u\n", bpp);
    kv("stride=%u\n", stride / (bpp / 8));
    kv("stride_bytes=%u\n", stride);
    kv("origin=%d,%d\n", origin_x, origin_y);
    kv("rotation=%u\n", st.current_rota);
    kv("can_rotate=%d\n", st.can_rotate ? 1 : 0);
    kv("inverted_gray=%d\n", st.inverted_grayscale ? 1 : 0);
    kv("has_eclipse_wfm=%d\n", st.has_eclipse_wfm ? 1 : 0);
    kv("is_sunxi=%d\n", st.is_sunxi ? 1 : 0);
    kv("is_mtk=%d\n", st.is_mtk ? 1 : 0);
    kv("is_tolino=%d\n", st.is_tolino ? 1 : 0);
    kv("is_kobo_non_mt=%d\n", st.is_kobo_non_mt ? 1 : 0);
    kv("can_hw_invert=%d\n", st.can_hw_invert ? 1 : 0);
    kv("can_wake_epdc=%d\n", st.can_wake_epdc ? 1 : 0);
    kv("unreliable_wait_for=%d\n", st.unreliable_wait_for ? 1 : 0);
    kv("can_wait_for_submission=%d\n", st.can_wait_for_submission ? 1 : 0);
    kv("touch_swap_axes_fbink=%d\n", st.touch_swap_axes ? 1 : 0);
    kv("touch_mirror_x_fbink=%d\n", st.touch_mirror_x ? 1 : 0);
    kv("touch_mirror_y_fbink=%d\n", st.touch_mirror_y ? 1 : 0);
    bool du4_capable = has_du4_capability(&st);
    kv("wfm_du4_capable=%d\n", du4_capable ? 1 : 0);

    /* ------------------------------------------------------- input capability */
    raw_node nodes[MAX_NODES];
    int      n_nodes = scan_proc_input(nodes, MAX_NODES);
    kv("input_node_count=%d\n", n_nodes);

    bool have_touch = false;
    char touch_path[32] = "", touch_name[256] = "";
    int  touch_slots = 0, touch_ax = 0, touch_ay = 0;
    bool touch_transpose = false;
    int  n_key_nodes = 0;

    for (int i = 0; i < n_nodes; i++) {
        raw_node *nd = &nodes[i];

        bool is_accel = bit_test(nd->prop, nd->n_prop, INPUT_PROP_ACCELEROMETER);
        bool has_slot = bit_test(nd->abs, nd->n_abs, ABS_MT_SLOT);
        bool has_xy   = bit_test(nd->abs, nd->n_abs, ABS_X) && bit_test(nd->abs, nd->n_abs, ABS_Y);
        bool has_btn_touch = bit_test(nd->key, nd->n_key, BTN_TOUCH);
        bool is_key_ev     = bit_test(nd->ev, nd->n_ev, EV_KEY);
        bool is_touch_node = !is_accel && (has_slot || (has_xy && has_btn_touch));

        const char *type = is_accel ? "accel" : is_touch_node ? "touch"
                          : is_key_ev            ? "key" : "other";
        kv("input_node_%d_path=%s\n", i, nd->path);
        kv("input_node_%d_name=%s\n", i, nd->name);
        kv("input_node_%d_type=%s\n", i, type);

        if (is_touch_node && !have_touch) {
            have_touch = true;
            snprintf(touch_path, sizeof touch_path, "%s", nd->path);
            snprintf(touch_name, sizeof touch_name, "%s", nd->name);

            int fd = open(nd->path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd >= 0) {
                /* Read-only ioctl queries, no EVIOCGRAB: these ask the
                   device's capability tables, not its event queue, so they
                   take nothing away from whichever process (Nickel) is
                   actually reading events. */
                if (has_slot) {
                    touch_ax = abs_max_ioctl(fd, ABS_MT_POSITION_X, 0);
                    touch_ay = abs_max_ioctl(fd, ABS_MT_POSITION_Y, 0);
                    touch_slots = abs_max_ioctl(fd, ABS_MT_SLOT, 0) + 1;
                } else {
                    touch_ax = abs_max_ioctl(fd, ABS_X, view_w - 1);
                    touch_ay = abs_max_ioctl(fd, ABS_Y, view_h - 1);
                    touch_slots = 1;
                }
                close(fd);
            }
            touch_transpose = (touch_ax > touch_ay);
        } else if (is_key_ev && !is_touch_node && n_key_nodes < MAX_KEY_NODES) {
            int idx = n_key_nodes++;
            bool has_power = bit_test(nd->key, nd->n_key, KEY_POWER);
            uint16_t codes[MAX_KEY_CODES];
            int      n_codes = bit_list(nd->key, nd->n_key, KEY_MAX, codes, MAX_KEY_CODES);

            kv("key_node_%d_path=%s\n", idx, nd->path);
            kv("key_node_%d_name=%s\n", idx, nd->name);
            kv("key_node_%d_has_power=%d\n", idx, has_power ? 1 : 0);
            kv("key_node_%d_code_count=%d\n", idx, n_codes);
            kv("key_node_%d_codes=", idx);
            for (int c = 0; c < n_codes; c++) kv("%s%u", c ? "," : "", codes[c]);
            kv("\n");
        }
    }
    kv("key_node_count=%d\n", n_key_nodes);

    kv("touch_node=%s\n", have_touch ? touch_path : "none");
    kv("touch_name=%s\n", have_touch ? touch_name : "none");
    kv("touch_protocol=%s\n", !have_touch ? "none" : (touch_slots > 1 ? "B" : "A"));
    kv("touch_slots=%d\n", touch_slots);
    kv("touch_axis_x_max=%d\n", touch_ax);
    kv("touch_axis_y_max=%d\n", touch_ay);
    kv("touch_transpose=%d\n", touch_transpose ? 1 : 0);

    /* --------------------------------------------------------- refresh sweep */
    size_t         fbmem_len = 0;
    unsigned char *fbmem     = fbink_get_fb_pointer(fbfd, &fbmem_len);
    /* Same guard as platform_kobo.c's kobo_init(): trust stride arithmetic
       only once the mapping is proven big enough for the last visible row.
       A refresh sweep that scribbles past the mapping is a worse failure
       mode than a probe that skips it and says why. */
    size_t need = (size_t)stride * (size_t)(origin_y + view_h);
    if (!fbmem || fbmem_len < need) {
        note("koboy-probe: framebuffer mapping unusable (%s, %zu bytes, need %zu)"
             " -- skipping the refresh sweep\n",
             fbmem ? "too small" : "fbink_get_fb_pointer failed", fbmem_len, need);
        kv("wfm_fast_name=none\n");
        kv("wfm_fast_ms=0.0\n");
        if (fout) fclose(fout);
        fbink_close(fbfd);
        return 0;
    }

    /* Five are filled below (DU4 only when the device has it); the slack is
       so that adding a waveform to the sweep is a one-line change and not a
       silent write past the end. */
    wfm_case cases[8];
    int      n_cases = 0;
    cases[n_cases++] = (wfm_case){ "AUTO", WFM_AUTO, false };
    if (du4_capable) cases[n_cases++] = (wfm_case){ "DU4", WFM_DU4, false };
    /* DU, the TWO-level fast waveform, ungated: it is in FBInk's "Common"
       block, so unlike DU4 there is no quirk to check. It is here because
       `waveform_fast = du` is now a shipped option and nobody has a number
       for what it costs on ANY panel -- the reference device's Appendix A
       sweep predates it. A probe of an unknown device that reported AUTO,
       DU4, A2 and GC16 but not the one waveform koboy might newly be told to
       use would leave exactly the gap this file exists to close. */
    cases[n_cases++] = (wfm_case){ "DU",   WFM_DU,   false };
    cases[n_cases++] = (wfm_case){ "A2",   WFM_A2,   false };
    cases[n_cases++] = (wfm_case){ "GC16", WFM_GC16, true  };

    /* Save/restore courtesy: the sweep writes real pixels straight into the
       shared framebuffer while Nickel is drawing to the same one, so the
       reader UI visibly flickers through test patterns for the duration --
       expected and accepted (see the file header), not a bug. What is worth
       avoiding is leaving our last test pattern on screen once the sweep is
       done, so the largest tested region that fits is saved before the
       first fill and written back (with one final flashing refresh) after
       the last one. This is a courtesy, not a restore path in the sense
       that matters for safety: no process state and no framebuffer depth is
       touched, so there is nothing here that can be gotten wrong badly
       enough to strand the device. */
    int max_w = 0, max_h = 0;
    for (int i = 0; i < N_REGIONS; i++)
        if (REGIONS[i].w <= view_w && REGIONS[i].h <= view_h) {
            if (REGIONS[i].w > max_w) max_w = REGIONS[i].w;
            if (REGIONS[i].h > max_h) max_h = REGIONS[i].h;
        }

    unsigned char *saved = NULL;
    if (max_h > 0) {
        saved = malloc((size_t)stride * (size_t)max_h);
        if (saved)
            memcpy(saved, fbmem + (size_t)origin_y * stride, (size_t)stride * (size_t)max_h);
    }

    uint8_t toggle = 0xFF;   /* shared across the whole sweep -- see sweep_cell() */
    int     fast_region_w = 0, fast_region_h = 0;
    uint64_t fast_block_us = 0, fast_submit_us = 0;
    const char *fast_name = du4_capable ? "DU4" : "A2";

    for (int r = 0; r < N_REGIONS; r++) {
        int w = REGIONS[r].w, h = REGIONS[r].h;
        if (w > view_w || h > view_h) {
            kv("sweep_%dx%d_skipped=panel too small\n", w, h);
            continue;
        }
        note("koboy-probe: sweeping %dx%d ...\n", w, h);

        for (int c = 0; c < n_cases; c++) {
            uint64_t submit_us, block_us, submit_max, block_max;
            sweep_cell(fbfd, fbmem, stride, bpp, st.inverted_grayscale,
                      origin_x, origin_y, w, h, &cases[c], fb_cfg, &toggle,
                      &submit_us, &block_us, &submit_max, &block_max);

            kv("sweep_%s_%dx%d_n=%d\n", cases[c].name, w, h, SWEEP_PASSES);
            kv("sweep_%s_%dx%d_submit_us_median=%llu\n", cases[c].name, w, h,
               (unsigned long long)submit_us);
            kv("sweep_%s_%dx%d_submit_us_max=%llu\n", cases[c].name, w, h,
               (unsigned long long)submit_max);
            kv("sweep_%s_%dx%d_block_us_median=%llu\n", cases[c].name, w, h,
               (unsigned long long)block_us);
            kv("sweep_%s_%dx%d_block_us_max=%llu\n", cases[c].name, w, h,
               (unsigned long long)block_max);

            /* wfm_fast_ms is the one number Appendix A can be cross-checked
               against directly: DU4 (or A2, on a platform without it) at
               1120x1008, blocking. Prefer that exact region; fall back to
               whichever region actually got tested first, so a smaller
               panel still gets a usable convenience figure instead of none
               at all. */
            if (!strcmp(cases[c].name, fast_name) &&
                (fast_region_w == 0 || (w == 1120 && h == 1008))) {
                fast_region_w = w; fast_region_h = h;
                fast_block_us = block_us; fast_submit_us = submit_us;
            }
        }
    }

    /* ------------------------------------------------- sustained update rate */

    /* Only the two waveforms koboy can be told to use WHILE A GAME IS RUNNING
       (MENU -> MOTION cycles AUTO and DU), because this section exists to
       feed one specific consumer: the area-aware present pacer, which needs
       to know how fast the panel will accept full-area work. A2/DU4/GC16 keep
       their place in the blocking sweep above, where the question is "how does
       this device rank its waveforms", not "how fast may we feed it". */
    wfm_case sustain_cases[2];
    int      n_sustain = 0;
    sustain_cases[n_sustain++] = (wfm_case){ "AUTO", WFM_AUTO, false };
    sustain_cases[n_sustain++] = (wfm_case){ "DU",   WFM_DU,   false };

    for (int r = 0; r < N_REGIONS; r++) {
        int w = REGIONS[r].w, h = REGIONS[r].h;
        if (w > view_w || h > view_h) continue;
        note("koboy-probe: sustaining %dx%d ...\n", w, h);

        for (int c = 0; c < n_sustain; c++) {
            for (int pat = 0; pat < 2; pat++) {
                uint64_t period_us, submit_us, fill_us;
                sustain_cell(fbfd, fbmem, stride, bpp, st.inverted_grayscale,
                             origin_x, origin_y, w, h, &sustain_cases[c], fb_cfg,
                             &toggle, pat != 0, &period_us, &submit_us, &fill_us);
                const char *kind = pat ? "checker" : "solid";
                kv("sustain_%s_%s_%dx%d_n=%d\n", sustain_cases[c].name, kind, w, h,
                   SUSTAIN_PASSES);
                kv("sustain_%s_%s_%dx%d_period_us=%llu\n", sustain_cases[c].name, kind,
                   w, h, (unsigned long long)period_us);
                kv("sustain_%s_%s_%dx%d_submit_us=%llu\n", sustain_cases[c].name, kind,
                   w, h, (unsigned long long)submit_us);
                kv("sustain_%s_%s_%dx%d_fill_us=%llu\n", sustain_cases[c].name, kind,
                   w, h, (unsigned long long)fill_us);
            }
        }
    }

    if (saved && max_h > 0) {
        memcpy(fbmem + (size_t)origin_y * stride, saved, (size_t)stride * (size_t)max_h);
        FBInkConfig restore_cfg = fb_cfg;
        restore_cfg.wfm_mode    = WFM_GC16;
        restore_cfg.is_flashing = true;
        fbink_refresh(fbfd, (uint32_t)origin_y, (uint32_t)origin_x,
                      (uint32_t)max_w, (uint32_t)max_h, &restore_cfg);
        free(saved);
    }

    kv("wfm_fast_name=%s\n", fast_name);
    kv("wfm_fast_region=%dx%d\n", fast_region_w, fast_region_h);
    kv("wfm_fast_submit_ms=%.1f\n", fast_submit_us / 1000.0);
    kv("wfm_fast_ms=%.1f\n", fast_block_us / 1000.0);
    /* unreliable_wait_for (printed above, with the rest of the FBInk quirk
       fields) means MXCFB_WAIT_FOR_UPDATE_COMPLETE can time out rather than
       return when the panel genuinely finishes -- so on a device that sets
       it, every *_block_us_* figure in this file, wfm_fast_ms included, may
       be measuring a stall rather than real panel latency. Restated here,
       next to the one number a TESTED.md row is most likely to quote
       verbatim, rather than trusting a reader to connect it back to a flag
       printed a hundred lines earlier. submit_us is unaffected -- it never
       calls fbink_wait_for_complete -- and is the more trustworthy figure on
       such a device. */
    if (st.unreliable_wait_for)
        kv("wfm_fast_ms_caveat=unreliable_wait_for=1 on this device -- block_us/wfm_fast_ms above may reflect a stalled wait, not real panel latency; prefer submit_us\n");

    note("koboy-probe: wrote %s\n", fout ? path : "(stdout only -- see above for why)");
    if (fout) fclose(fout);
    fbink_close(fbfd);
    return 0;
}

/* ========================================================================
 * --takeover
 * ======================================================================== */

#define TAKEOVER_WINDOW_US (15ull * 1000000ull)

static int run_takeover(void)
{
    /* THE check. Nickel holds EVIOCGRAB on every input node it opens, and a
       grabbed node delivers events exclusively to the grabbing process -- so
       running this mode with Nickel up would not fail loudly, it would just
       read nothing, ever, and whoever ran it would have no way to tell
       "no buttons were pressed" apart from "this device is broken". Refusing
       up front, with a reason, is the entire point of splitting this out of
       --coexist. */
    if (process_running("nickel")) {
        note("koboy-probe --takeover: refusing to run -- Nickel is still running.\n");
        note("  Nickel holds EVIOCGRAB on every input node, so a takeover run right\n");
        note("  now would read no events and report an empty result, not a hardware\n");
        note("  fact. Stop Nickel first, e.g.:\n");
        note("    killall -TERM nickel hindenburg sickel\n");
        note("  then re-run --takeover. See scripts/koboy.sh for the full stop/restore\n");
        note("  sequence used when koboy itself launches.\n");
        return 2;
    }

    int fbfd = fbink_open();
    if (fbfd < 0) {
        note("koboy-probe: fbink_open failed: %s\n", strerror(errno));
        return 1;
    }
    FBInkConfig fb_cfg;
    memset(&fb_cfg, 0, sizeof fb_cfg);
    fb_cfg.is_quiet = true;
    if (fbink_init(fbfd, &fb_cfg) < 0) {
        note("koboy-probe: fbink_init failed\n");
        fbink_close(fbfd);
        return 1;
    }
    FBInkState st;
    memset(&st, 0, sizeof st);
    fbink_get_state(&fb_cfg, &st);

    char dev_id[64];
    sanitize(dev_id, sizeof dev_id, st.device_codename[0] ? st.device_codename : st.device_name);
    char path[256];
    snprintf(path, sizeof path, "/mnt/onboard/koboy-probe-%s.txt", dev_id);

    /* "a", deliberately NOT "w". The documented workflow (docs/probe-readme.md)
       is --coexist first, then --takeover on the same device, so that one file
       ends up with panel/stride/touch/sweep facts AND captured key codes and
       touch samples -- the single pasteable TESTED.md row this tool exists to
       produce. Opening "w" here would truncate that file back to nothing but
       the takeover_* fields the moment this mode ran, silently discarding
       every fact --coexist had just written. If no coexist run has happened
       yet, "a" still creates the file fresh, so a standalone --takeover run
       behaves exactly as before. */
    FILE *fout = fopen(path, "a");
    if (!fout) note("koboy-probe: cannot open %s for appending: %s (stdout only)\n",
                     path, strerror(errno));
    g_nsinks = 0;
    g_sinks[g_nsinks++] = stdout;
    if (fout) g_sinks[g_nsinks++] = fout;

    time_t t = time(NULL);
    char   tbuf[32];
    strftime(tbuf, sizeof tbuf, "%Y-%m-%dT%H:%M:%S", localtime(&t));
    /* A visible seam, so a file that already carries a --coexist report reads
       as two clearly-dated blocks rather than one confusing run-on of keys --
       and so re-running --takeover more than once appends another dated block
       instead of silently blending with the last one. */
    kv("\n# koboy-probe report, mode=takeover, generated %s\n", tbuf);

    raw_node nodes[MAX_NODES];
    int      n_nodes = scan_proc_input(nodes, MAX_NODES);

    int  fds[MAX_NODES];
    bool grabbed[MAX_NODES];
    int  n_fds = 0;

    for (int i = 0; i < n_nodes; i++) {
        raw_node *nd = &nodes[i];
        bool is_accel = bit_test(nd->prop, nd->n_prop, INPUT_PROP_ACCELEROMETER);
        if (is_accel) continue;    /* never wanted, see platform_kobo.c */
        bool has_slot      = bit_test(nd->abs, nd->n_abs, ABS_MT_SLOT);
        bool has_xy        = bit_test(nd->abs, nd->n_abs, ABS_X) && bit_test(nd->abs, nd->n_abs, ABS_Y);
        bool has_btn_touch = bit_test(nd->key, nd->n_key, BTN_TOUCH);
        bool is_key_ev     = bit_test(nd->ev, nd->n_ev, EV_KEY);
        bool is_touch_node = has_slot || (has_xy && has_btn_touch);
        if (!is_touch_node && !is_key_ev) continue;

        int fd = open(nd->path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            note("koboy-probe: cannot open %s: %s\n", nd->path, strerror(errno));
            continue;
        }
        /* Nickel is confirmed stopped above, so grabbing here is safe and,
           unlike in the emulator itself, has nothing else to conflict with
           -- there is no page-turn-vs-power tradeoff to make, because this
           tool exits in TAKEOVER_WINDOW_US regardless. */
        grabbed[n_fds] = (ioctl(fd, EVIOCGRAB, 1) == 0);
        fds[n_fds] = fd;
        n_fds++;
    }

    if (n_fds == 0) {
        note("koboy-probe: no touch or key input node found -- nothing to capture\n");
        kv("mode=takeover\n");
        kv("device=%s\n", st.device_name);
        kv("takeover_key_codes_seen=\n");
        kv("takeover_key_press_count=0\n");
        kv("takeover_touch_contacts_seen=0\n");
        if (fout) fclose(fout);
        fbink_close(fbfd);
        return 0;
    }

    note("koboy-probe --takeover: capturing for %llus -- press every button and\n"
         "  touch the screen a few times now.\n",
         (unsigned long long)(TAKEOVER_WINDOW_US / 1000000ull));

    uint16_t seen_keys[32]; int n_seen_keys = 0;
    /* touch_contacts and last_x/last_y are a rough characterisation, not a
       protocol decode: this counts every ABS_MT_TRACKING_ID >= 0 seen on ANY
       slot, with no per-slot state, so a genuine two-finger touch (one on the
       d-pad, one on A/B -- exactly the case koboy's own touch-only layouts
       depend on) can double-count contacts, and last_x/last_y can blend
       reports from two different slots rather than tracking one. Good enough
       to tell "does this panel report multitouch at all", not to reconstruct
       simultaneous gestures -- input.c's real slot tracking is what the
       emulator itself uses at runtime, and is a different, stateful job. */
    int      touch_contacts = 0;
    int      last_x = -1, last_y = -1;

    uint64_t deadline = now_us() + TAKEOVER_WINDOW_US;
    while (now_us() < deadline) {
        for (int i = 0; i < n_fds; i++) {
            struct input_event ev[64];
            ssize_t n = read(fds[i], ev, sizeof ev);
            if (n <= 0) continue;
            size_t cnt = (size_t)n / sizeof ev[0];
            for (size_t j = 0; j < cnt; j++) {
                if (ev[j].type == EV_KEY && ev[j].value == 1) {
                    bool known = false;
                    for (int k = 0; k < n_seen_keys; k++)
                        if (seen_keys[k] == ev[j].code) { known = true; break; }
                    if (!known && n_seen_keys < (int)(sizeof seen_keys / sizeof seen_keys[0])) {
                        seen_keys[n_seen_keys++] = ev[j].code;
                        note("koboy-probe: key code %u pressed\n", ev[j].code);
                    }
                } else if (ev[j].type == EV_ABS && ev[j].code == ABS_MT_TRACKING_ID &&
                          ev[j].value >= 0) {
                    touch_contacts++;
                } else if (ev[j].type == EV_ABS &&
                          (ev[j].code == ABS_MT_POSITION_X || ev[j].code == ABS_X)) {
                    last_x = ev[j].value;
                } else if (ev[j].type == EV_ABS &&
                          (ev[j].code == ABS_MT_POSITION_Y || ev[j].code == ABS_Y)) {
                    last_y = ev[j].value;
                }
            }
        }
        usleep(10000);
    }

    for (int i = 0; i < n_fds; i++) {
        if (grabbed[i]) ioctl(fds[i], EVIOCGRAB, 0);
        close(fds[i]);
    }

    kv("mode=takeover\n");
    kv("device=%s\n", st.device_name);
    kv("platform=%s\n", st.device_platform);
    kv("takeover_window_s=%llu\n", (unsigned long long)(TAKEOVER_WINDOW_US / 1000000ull));
    kv("takeover_key_press_count=%d\n", n_seen_keys);
    kv("takeover_key_codes_seen=");
    for (int i = 0; i < n_seen_keys; i++) kv("%s%u", i ? "," : "", seen_keys[i]);
    kv("\n");
    kv("takeover_touch_contacts_seen=%d\n", touch_contacts);
    if (last_x >= 0 && last_y >= 0) {
        kv("takeover_touch_last_x=%d\n", last_x);
        kv("takeover_touch_last_y=%d\n", last_y);
    } else {
        kv("takeover_touch_last_xy=none\n");
    }

    note("koboy-probe: captured %d distinct key code(s), %d touch contact(s)\n",
         n_seen_keys, touch_contacts);
    if (fout) fclose(fout);
    fbink_close(fbfd);
    return 0;
}

/* ------------------------------------------------------------------- main */

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [--coexist | --takeover]\n"
        "  --coexist   (default) safe alongside Nickel: identity, panel\n"
        "              geometry, input capabilities, refresh-timing sweep.\n"
        "  --takeover  reads real input events; refuses unless Nickel is\n"
        "              already stopped.\n"
        "Writes /mnt/onboard/koboy-probe-<device>.txt.\n",
        argv0);
}

int main(int argc, char **argv)
{
    bool takeover = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--coexist")) takeover = false;
        else if (!strcmp(argv[i], "--takeover")) takeover = true;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(argv[0]); return 0; }
        else { fprintf(stderr, "koboy-probe: unknown option %s\n", argv[i]); usage(argv[0]); return 2; }
    }
    return takeover ? run_takeover() : run_coexist();
}
