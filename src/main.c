/* koboy: the emulator loop.
 *
 * Everything here is platform-independent: pixels reach the panel as gray8 at
 * final scale and input arrives already normalised to libretro joypad bits,
 * so this file is the same on the desktop and on the device.
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "input.h"          /* must precede platform_if.h: declares struct koboy_input */
#include "platform_if.h"

#include "calib.h"
#include "chrome.h"
#include "config.h"
#include "core.h"
#include "koboy.h"
#include "pacing.h"
#include "sram.h"
#include "video.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Provided by the backend translation unit. Exactly one is linked in: the
   desktop build takes platform_sdl.c, the device build platform_kobo.c and
   -DKOBOY_PLATFORM_KOBO. Everything between here and the end of main() is the
   same code either way; these few lines are the whole of the difference. */
extern bool platform_poll_raw_key(koboy_platform *pf, uint16_t *code);

#ifdef KOBOY_PLATFORM_KOBO
extern koboy_platform *platform_kobo_create(void);
extern void            platform_kobo_setup_touch(koboy_platform *pf, koboy_input *in);
extern void            platform_kobo_selftest(koboy_platform *pf);
extern void            platform_kobo_refresh_stats(koboy_platform *pf);
extern void            platform_kobo_fatal(void *ctx, const char *msg);
#else
extern koboy_platform *platform_sdl_create(void);
extern void            platform_sdl_set_panel(koboy_platform *pf, int w, int h);
#endif

#define DEFAULT_INI "config/koboy.ini"

static volatile sig_atomic_t g_stop;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* set by the frame callback each time the core emits a frame */
static const void *g_frame; static unsigned g_fw, g_fh; static size_t g_fpitch;
static void on_frame(void *ud, const void *d, unsigned w, unsigned h, size_t pitch)
{ (void)ud; g_frame = d; g_fw = w; g_fh = h; g_fpitch = pitch; }

static uint16_t on_input(void *ud) { return input_state((koboy_input *)ud)->buttons; }

static bool g_quiet;
static void say(const char *fmt, ...)
{
    if (g_quiet) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* Every fatal path goes through here. On the desktop that is just stderr; on
   the device there is no terminal, so an error that only reaches stderr is
   indistinguishable from a crash -- the panel keeps whatever was on it and the
   user power-cycles. The Kobo backend draws the message on the panel and waits
   for an acknowledgement. */
static koboy_platform *g_pf;
static void fatal(const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "koboy: %s\n", msg);
#ifdef KOBOY_PLATFORM_KOBO
    if (g_pf) platform_kobo_fatal(g_pf->ctx, msg);
#endif
}

/* ------------------------------------------------------- calibration text */

/* A 5x7 bitmap font, one byte per column, bit 0 = top row. Only what the
   calibration prompts need; anything else renders as a space. Text is the one
   thing chrome.c does not draw, and pulling in a font library for two
   sentences shown once would be absurd. */
static const uint8_t FONT5x7[][5] = {
    { 0x00,0x00,0x00,0x00,0x00 }, /* space */
    { 0x7E,0x11,0x11,0x11,0x7E }, /* A */
    { 0x7F,0x49,0x49,0x49,0x36 }, { 0x3E,0x41,0x41,0x41,0x22 },
    { 0x7F,0x41,0x41,0x22,0x1C }, { 0x7F,0x49,0x49,0x49,0x41 },
    { 0x7F,0x09,0x09,0x01,0x01 }, { 0x3E,0x41,0x49,0x49,0x7A },
    { 0x7F,0x08,0x08,0x08,0x7F }, { 0x00,0x41,0x7F,0x41,0x00 },
    { 0x20,0x40,0x41,0x3F,0x01 }, { 0x7F,0x08,0x14,0x22,0x41 },
    { 0x7F,0x40,0x40,0x40,0x40 }, { 0x7F,0x02,0x04,0x02,0x7F },
    { 0x7F,0x04,0x08,0x10,0x7F }, { 0x3E,0x41,0x41,0x41,0x3E },
    { 0x7F,0x09,0x09,0x09,0x06 }, { 0x3E,0x41,0x51,0x21,0x5E },
    { 0x7F,0x09,0x19,0x29,0x46 }, { 0x46,0x49,0x49,0x49,0x31 },
    { 0x01,0x01,0x7F,0x01,0x01 }, { 0x3F,0x40,0x40,0x40,0x3F },
    { 0x1F,0x20,0x40,0x20,0x1F }, { 0x7F,0x20,0x18,0x20,0x7F },
    { 0x63,0x14,0x08,0x14,0x63 }, { 0x03,0x04,0x78,0x04,0x03 },
    { 0x61,0x51,0x49,0x45,0x43 }, /* Z */
};

static const uint8_t *glyph(char ch)
{
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    if (ch >= 'A' && ch <= 'Z') return FONT5x7[1 + (ch - 'A')];
    return FONT5x7[0];
}

static void draw_text(uint8_t *fb, int stride, int W, int H, int x, int y,
                      const char *s, int px, uint8_t ink)
{
    for (const char *p = s; *p; p++, x += 6 * px) {
        const uint8_t *g = glyph(*p);
        for (int col = 0; col < 5; col++) {
            for (int row = 0; row < 7; row++) {
                if (!(g[col] & (1u << row))) continue;
                for (int dy = 0; dy < px; dy++) {
                    int fy = y + row * px + dy;
                    if (fy < 0 || fy >= H) continue;
                    for (int dx = 0; dx < px; dx++) {
                        int fx = x + col * px + dx;
                        if (fx >= 0 && fx < W) fb[(size_t)fy * stride + fx] = ink;
                    }
                }
            }
        }
    }
}

static void draw_centred(uint8_t *fb, int stride, int W, int H, int y,
                         const char *s, int px, uint8_t ink)
{
    int w = (int)strlen(s) * 6 * px;
    draw_text(fb, stride, W, H, (W - w) / 2, y, s, px, ink);
}

/* ------------------------------------------------------------------- args */

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [options]\n"
        "  --rom PATH        Game Boy ROM to load\n"
        "  --core PATH       libretro core shared object\n"
        "  --config PATH     ini file (default %s)\n"
        "  --save-dir PATH   directory for .srm saves\n"
        "  --panel WxH       synthetic panel size for the desktop backend\n"
        "  --frames N        stop after N emulated frames (scripted runs)\n"
        "  --selftest        print machine-readable backend facts and continue\n"
        "  --message TEXT    draw TEXT on the panel and exit 3 (launcher errors)\n"
        "  --waveform auto|du4  waveform for fast refreshes (default auto)\n"
        "  --quiet           suppress everything but the presented= counter\n",
        argv0, DEFAULT_INI);
}

int main(int argc, char **argv)
{
    koboy_config cfg;
    const char  *ini_path = DEFAULT_INI;
    unsigned long frame_limit = 0;
    int panel_w = 0, panel_h = 0;
    bool selftest = false;
    const char *message = NULL;

    /* --config is read before the file so an alternate ini can be chosen. */
    for (int i = 1; i < argc - 1; i++)
        if (!strcmp(argv[i], "--config")) ini_path = argv[i + 1];

    config_defaults(&cfg);
    config_load(&cfg, ini_path);

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool has_val = (i + 1 < argc);
        if      (!strcmp(a, "--quiet")) g_quiet = true;
        else if (!strcmp(a, "--selftest")) selftest = true;
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(argv[0]); return 0; }
        else if (!has_val) { fprintf(stderr, "koboy: missing value for %s\n", a); return 2; }
        else if (!strcmp(a, "--rom"))      snprintf(cfg.rom_path,  sizeof cfg.rom_path,  "%s", argv[++i]);
        else if (!strcmp(a, "--core"))     snprintf(cfg.core_path, sizeof cfg.core_path, "%s", argv[++i]);
        else if (!strcmp(a, "--save-dir")) snprintf(cfg.save_dir,  sizeof cfg.save_dir,  "%s", argv[++i]);
        else if (!strcmp(a, "--config"))   i++;   /* already handled */
        else if (!strcmp(a, "--message"))  message = argv[++i];
        else if (!strcmp(a, "--frames"))   frame_limit = strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--waveform")) {
            const char *w = argv[++i];
            if (!strcmp(w, "auto"))      cfg.wfm_fast_policy = KOBOY_WFM_AUTO;
            else if (!strcmp(w, "du4"))  cfg.wfm_fast_policy = KOBOY_WFM_DU4;
            else { fprintf(stderr, "koboy: --waveform wants auto or du4\n"); return 2; }
        }
        else if (!strcmp(a, "--panel")) {
            if (sscanf(argv[++i], "%dx%d", &panel_w, &panel_h) != 2) {
                fprintf(stderr, "koboy: --panel wants WxH\n"); return 2;
            }
        } else { fprintf(stderr, "koboy: unknown option %s\n", a); usage(argv[0]); return 2; }
    }

    /* Applied after both the ini and the command line, so a bare name from
       either source is treated the same. A slashless core name is otherwise
       unloadable on the device: dlopen never searches the cwd. */
    config_resolve_paths(&cfg);

    /* ------------------------------------------------ platform and profile */
#ifdef KOBOY_PLATFORM_KOBO
    (void)panel_w; (void)panel_h;      /* the panel is whatever the device has */
    koboy_platform *pf = platform_kobo_create();
#else
    koboy_platform *pf = platform_sdl_create();
#endif
    if (!pf) { fprintf(stderr, "koboy: cannot create platform\n"); return 1; }
#ifndef KOBOY_PLATFORM_KOBO
    if (panel_w > 0 && panel_h > 0) platform_sdl_set_panel(pf, panel_w, panel_h);
#endif

    /* Only now can fatal() draw: before init there is no framebuffer to draw
       on, so anything above this point can do no better than stderr. */
    if (!pf->init(pf->ctx, &cfg)) { fprintf(stderr, "koboy: platform init failed\n"); return 1; }
    g_pf = pf;

    /* Installed here, not beside the emulator loop: the calibration wait below
       tests g_stop, and a handler installed after it would leave that test dead
       and let a signal during a first-run calibration terminate outright. This
       is also after platform init on purpose, so it overrides any handler the
       backend's own library installed. */
    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);

    int pw = 0, ph = 0;
    pf->screen_info(pf->ctx, &pw, &ph);

    /* Printed before anything else can fail, so the smoke test still gets its
       facts out of a run that then dies on a missing core or ROM. */
    if (selftest) {
#ifdef KOBOY_PLATFORM_KOBO
        platform_kobo_selftest(pf);
#else
        printf("panel=%dx%d\nwfm_fast=none\nstride=%d\ntouch_transpose=0\n",
               pw, ph, pw);
        fflush(stdout);
#endif
    }

    /* --message exists for the launcher script, which has a message for the user
       ("this needs a reboot") and no terminal to put it in. It reuses fatal()
       rather than shelling out to the device's own fbink, so there is exactly
       one on-panel error presentation and it looks the same wherever the error
       came from. Always non-zero: it is only ever used to report a failure. */
    if (message) {
        fatal("%s", message);
        pf->shutdown(pf->ctx);
        free(pf);
        return 3;
    }

    /* Checked here rather than during argument parsing so that it is reportable:
       before platform init there is no panel to draw on, and "you forgot to set
       rom= in koboy.ini" is exactly the mistake a user makes with no terminal
       in front of them. */
    if (!cfg.rom_path[0]) {
        fatal("no rom configured -- set rom= in the ini or pass --rom");
        pf->shutdown(pf->ctx);
        return 2;
    }

    koboy_profile prof;
    if (!config_resolve_profile(&prof, &cfg, pw, ph)) {
        fatal("panel %dx%d is too small for a 1x game rect", pw, ph);
        pf->shutdown(pf->ctx);
        return 1;
    }
    say("koboy: panel %dx%d, scale %d, game %dx%d at (%d,%d)\n",
        pw, ph, prof.scale, prof.game_w, prof.game_h, prof.game_x, prof.game_y);

    /* ----------------------------------------------------- chrome, drawn once */
    int panel_stride = pw;
    uint8_t *panel = malloc((size_t)panel_stride * (size_t)ph);
    if (!panel) { fatal("out of memory"); pf->shutdown(pf->ctx); return 1; }
    memset(panel, 0xFF, (size_t)panel_stride * (size_t)ph);
    chrome_render(panel, panel_stride, &prof, &cfg.layout);
    pf->blit_gray8(pf->ctx, panel, pw, ph, panel_stride, 0, 0);
    pf->refresh(pf->ctx, 0, 0, pw, ph, KOBOY_REFRESH_FULL);

    /* ------------------------------------------------------- calibration */
    if (calib_needed(&cfg)) {
        if (frame_limit) {
            /* A scripted, bounded run has nobody to press a button; blocking
               here would hang the smoke test forever. */
            say("koboy: uncalibrated, skipping calibration for a scripted run\n");
        } else {
            /* A throwaway input object, whose only job is to let this loop SEE A
               TOUCH. It exists because the loop used to advance on nothing but
               platform_poll_raw_key(): on a Kobo with no page-turn buttons there
               was no key to press, no way to answer the prompt, and the only
               thing that responded at all was the power button, which quits. The
               real input object is created below instead of being reused here on
               purpose -- input_create copies the config by value, so one made
               before calibration would carry the pre-calibration key mapping for
               the whole session. */
            koboy_input *cal_in = input_create(&cfg, &prof);
            if (!cal_in) {
                fatal("out of memory");
                free(panel); pf->shutdown(pf->ctx); return 1;
            }
#ifdef KOBOY_PLATFORM_KOBO
            platform_kobo_setup_touch(pf, cal_in);
#else
            input_set_touch_transform(cal_in, pw, ph, false, false, false);
#endif
            koboy_calib k;
            calib_begin(&k, &cfg);
            bool done = false, escaped = false;
            int last_stage = -1;
            while (!done && !escaped && !g_stop && !pf->should_quit(pf->ctx)) {
                if (k.stage != last_stage) {
                    last_stage = k.stage;
                    memset(panel, 0xFF, (size_t)panel_stride * (size_t)ph);
                    draw_centred(panel, panel_stride, pw, ph, ph / 2 - 40,
                                 calib_prompt(&k), 5, 0x00);
                    /* The escape has to be ON THE PANEL. A device that cannot
                       answer the prompt is exactly the device whose user has no
                       terminal and no other way to find out. */
                    draw_centred(panel, panel_stride, pw, ph, ph / 2 + 40,
                                 calib_escape_prompt(), 3, 0x00);
                    pf->blit_gray8(pf->ctx, panel, pw, ph, panel_stride, 0, 0);
                    pf->refresh(pf->ctx, 0, 0, pw, ph, KOBOY_REFRESH_FULL);
                    say("koboy: %s (%s)\n", calib_prompt(&k), calib_escape_prompt());
                }
                /* Touch FIRST, keys second, and the order is load-bearing:
                   platform_poll_raw_key drains the same event nodes with a NULL
                   input, which throws away every touch event it passes over. */
                pf->poll_input(pf->ctx, cal_in);
                const koboy_input_state *cal_st = input_state(cal_in);
                for (int t = 0; t < KOBOY_MAX_TOUCH; t++)
                    if (cal_st->touch[t].down) { escaped = true; break; }

                uint16_t code;
                while (platform_poll_raw_key(pf, &code))
                    if (calib_feed_key(&k, code)) { done = true; break; }
                usleep(5000);
            }
            input_destroy(cal_in);
            if (done && calib_commit(&k, &cfg, ini_path))
                say("koboy: calibrated a=%u b=%u -> %s\n",
                    (unsigned)cfg.key_a, (unsigned)cfg.key_b, ini_path);
            if (escaped) {
                /* Not just "break out of the loop": leaving the zero sentinel in
                   place would make input_feed_key ignore every key for the rest
                   of the session. */
                calib_escape(&cfg);
                say("koboy: calibration skipped by touch, keeping a=%u b=%u\n",
                    (unsigned)cfg.key_a, (unsigned)cfg.key_b);
            }

            /* Put the faceplate back: calibration wrote over it. */
            memset(panel, 0xFF, (size_t)panel_stride * (size_t)ph);
            chrome_render(panel, panel_stride, &prof, &cfg.layout);
            pf->blit_gray8(pf->ctx, panel, pw, ph, panel_stride, 0, 0);
            pf->refresh(pf->ctx, 0, 0, pw, ph, KOBOY_REFRESH_FULL);
        }
    }

    /* --------------------------------------------------- video, input, core */
    koboy_video *vid = video_create(&prof, cfg.force_dither);
    koboy_input *in  = input_create(&cfg, &prof);
    if (!vid || !in) {
        fatal("out of memory");
        video_destroy(vid); input_destroy(in); free(panel);
        pf->shutdown(pf->ctx); return 1;
    }
#ifdef KOBOY_PLATFORM_KOBO
    /* The device's touch layer has its own raw range and is mounted rotated;
       only the backend knows by how much, so it installs the transform. */
    platform_kobo_setup_touch(pf, in);
#else
    /* The desktop mouse already reports panel coordinates: no transposition,
       no flips, raw range == panel range. */
    input_set_touch_transform(in, pw, ph, false, false, false);
#endif

    char err[512];
    koboy_core *core = core_open(cfg.core_path, cfg.save_dir, err, sizeof err);
    if (!core) {
        fatal("%s", err);
        video_destroy(vid); input_destroy(in); free(panel);
        pf->shutdown(pf->ctx); return 1;
    }
    core_set_frame_cb(core, on_frame, NULL);
    core_set_input_fn(core, on_input, in);
    if (!core_load_rom(core, cfg.rom_path, err, sizeof err)) {
        fatal("%s", err);
        core_close(core);
        video_destroy(vid); input_destroy(in); free(panel);
        pf->shutdown(pf->ctx); return 1;
    }

    char sram_path[512];
    sram_path_for_rom(sram_path, sizeof sram_path, cfg.save_dir, cfg.rom_path);
    size_t sram_len = 0;
    uint8_t *sram = core_sram(core, &sram_len);
    /* Tetris is cartridge type 0x00: no battery-backed SRAM at all, so this is
       NULL/0 on the development ROM and must not be dereferenced. */
    /* Set false when a save file exists but could not be loaded whole. Then
       NOTHING is written back over it for the rest of the session.
       The reasoning, because "saving is off" looks like a bug otherwise: a file
       that fails to load is either truncated or from another cartridge, and in
       both cases it is the only copy of something the user cares about. Writing
       this session's SRAM over it destroys whatever is recoverable, for the sake
       of progress made in a game that started from a blank save anyway. So the
       file is left exactly as found, the user is told on the panel, and the fix
       is theirs to make (move the file aside, and saving resumes next run). */
    bool sram_writeback = true;
    if (sram && sram_len) {
        if (sram_load(sram_path, sram, sram_len)) {
            say("koboy: loaded %s\n", sram_path);
        } else if (access(sram_path, F_OK) == 0) {
            sram_writeback = false;
            say("koboy: %s could not be read whole; SRAM left as the core "
                "initialised it and saving is disabled this session\n", sram_path);
            /* On the panel, not just the log: a save that silently did not load
               is how a user loses hours without ever being told. Short lines --
               FBInk wraps at the column edge, not at word boundaries. */
            fatal("Save file unreadable.\nStarting fresh.\nSaving is OFF this run.");
            /* fatal() drew over the faceplate; put it back. */
            memset(panel, 0xFF, (size_t)panel_stride * (size_t)ph);
            chrome_render(panel, panel_stride, &prof, &cfg.layout);
            pf->blit_gray8(pf->ctx, panel, pw, ph, panel_stride, 0, 0);
            pf->refresh(pf->ctx, 0, 0, pw, ph, KOBOY_REFRESH_FULL);
        }
    } else {
        say("koboy: cartridge has no save RAM\n");
    }

    /* ------------------------------------------------------------- the loop */
    koboy_pacer pace;
    pacer_init(&pace, pf->now_us(pf->ctx), cfg.present_divisor);

    unsigned long presented = 0, since_cleanup = 0, cleanups = 0, big_refreshes = 0;
    uint64_t last_sram_us = pf->now_us(pf->ctx);
    uint64_t last_cleanup_us = last_sram_us;

    while (!g_stop && !pf->should_quit(pf->ctx)) {
        if (frame_limit && pace.frames >= frame_limit) break;

        /* Poll EVERY core iteration (60Hz), not once per presented frame.
           Polling only on presentation would drop short presses and add up to
           50ms of latency on top of the panel's own. */
        pf->poll_input(pf->ctx, in);

        uint64_t delay = pacer_delay_us(&pace, pf->now_us(pf->ctx));
        if (delay) usleep((useconds_t)delay);

        g_frame = NULL;
        core_run_frame(core);
        bool present = pacer_tick(&pace);
        if (!present) goto sram_check;

        /* A NULL g_frame is the core's can-dupe signal, which video_submit turns
           into an empty rect -- so an unchanged frame costs no refresh at all. */
        koboy_rect r = video_submit(vid, g_frame, (int)g_fw, (int)g_fh,
                                    g_fpitch, core_pixfmt(core));
        if (r.w == 0) goto sram_check;          /* nothing changed: skip the panel */

        pf->blit_gray8(pf->ctx, video_buffer(vid) + (size_t)r.y * video_stride(vid) + r.x,
                       r.w, r.h, video_stride(vid),
                       prof.game_x + r.x, prof.game_y + r.y);

        /* Waveform by dirty area, not one waveform for every frame.
           KOBOY_REFRESH_FAST maps to a non-flashing waveform (DU4 on this
           panel), which never fully resets pixel state -- residue accumulates
           on every update regardless of rect size. Observed on the device as
           several Tetris scenes layered on top of each other.
           A dirty rect covering most of the game rect means the scene has
           substantially changed, which is both when layered residue is most
           objectionable and when the refresh is already expensive, so paying
           for a flashing waveform there is cheap in relative terms. Small
           incremental updates keep the fast waveform, and the periodic cleanup
           sweeps whatever they leave behind.
           Note this cannot be a substitute for the cleanup: the dirty diff
           compares our own output buffers, so it tracks what we sent, not what
           the panel shows. A region that ghosts and then stops changing is
           never revisited by this test at all. */
        koboy_refresh_mode mode = KOBOY_REFRESH_FAST;
        if (config_promote_full(&cfg, (long)r.w * (long)r.h,
                                (long)prof.game_w * (long)prof.game_h)) {
            mode = KOBOY_REFRESH_FULL;
            big_refreshes++;
        }
        pf->refresh(pf->ctx, prof.game_x + r.x, prof.game_y + r.y, r.w, r.h, mode);
        presented++;

        /* A value <= 0 disables cleanup. The explicit guard is required:
           without it, 0 makes this always true (a full refresh every presented
           frame, the inverse of "never") and a negative value wraps the cast so
           cleanup never runs at all. */
        bool due = (cfg.cleanup_interval > 0 &&
                    ++since_cleanup >= (unsigned long)cfg.cleanup_interval);
        /* Wall-clock ceiling. The presented-frame counter above cannot be
           trusted to fire on any particular schedule: unchanged frames are
           suppressed, and 70s of measured Tetris gameplay presented only 45
           frames. Ghosting accumulates with time, so time is the backstop. */
        if (!due && cfg.cleanup_max_ms > 0) {
            uint64_t now = pf->now_us(pf->ctx);
            if (now - last_cleanup_us >= (uint64_t)cfg.cleanup_max_ms * 1000ull)
                due = true;
        }
        if (due) {
            since_cleanup = 0;
            last_cleanup_us = pf->now_us(pf->ctx);
            cleanups++;
            /* Scoped to the game rect, never the full panel: a full-panel flash
               would disturb chrome that has no reason to change. */
            pf->refresh(pf->ctx, prof.game_x, prof.game_y, prof.game_w, prof.game_h,
                        KOBOY_REFRESH_FULL);
        }

sram_check:
        /* Periodic flush while dirty: e-readers get suspended and killed
           unceremoniously, and sram_save is atomic so a kill mid-write is safe. */
        if (sram && sram_len && sram_writeback) {
            uint64_t now = pf->now_us(pf->ctx);
            if (now - last_sram_us > 10ull * 1000000ull) {
                sram_save(sram_path, sram, sram_len);
                last_sram_us = now;
            }
        }
    }

    if (sram && sram_len && sram_writeback) sram_save(sram_path, sram, sram_len);
    say("koboy: %s, %lu presented frames, %lu game-rect cleanups, "
        "%lu large-area full refreshes\n",
        g_stop ? "stopped by signal" : "stopped", presented, cleanups,
        big_refreshes);
    /* Always printed, even under --quiet: the smoke tests grep for it.
       --quiet suppresses other chatter only. */
    printf("presented=%lu\n", presented);
    fflush(stdout);
    if (selftest) {
#ifdef KOBOY_PLATFORM_KOBO
        platform_kobo_refresh_stats(pf);
#endif
    }

    core_close(core);
    video_destroy(vid);
    input_destroy(in);
    free(panel);
    pf->shutdown(pf->ctx);
    free(pf);
    return 0;
}
