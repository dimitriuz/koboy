/* koboy: the emulator loop.
 *
 * Everything here is platform-independent: pixels reach the panel as gray8 at
 * final scale and input arrives already normalised to libretro joypad bits,
 * so this file is the same on the desktop and on the device.
 *
 * Copyright (C) 2026 the koboy authors.
 *
 * koboy is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. It is distributed WITHOUT ANY WARRANTY; see the LICENSE file at
 * the root of this repository, or <https://www.gnu.org/licenses/>.
 *
 * THIS NOTICE IS IN THIS FILE ONLY, deliberately: no source file here carries
 * a per-file header, and adding thirty would bury a year of `git blame` for no
 * legal gain LICENSE and README.md do not already supply. LICENSES.md covers
 * the emulator cores, which are NOT under this licence and are not linked into
 * this binary -- separate shared objects, dlopen'd at runtime, each with its
 * own terms, three restricting commercial use.
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
#include "recent.h"
#include "romlist.h"
#include "safefile.h"
#include "screens.h"
#include "shot.h"
#include "sram.h"
#include "state.h"
#include "stats.h"
#include "text.h"
#include "ui.h"
#include "uiscript.h"
#include "video.h"

#include <limits.h>        /* PATH_MAX, for the exe-dir join below core selection */
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>      /* stat(), for the too-short-to-be-a-cartridge floor */
#include <unistd.h>

/* Provided by the backend translation unit; exactly one is linked in
   (platform_sdl.c on the desktop, platform_kobo.c + -DKOBOY_PLATFORM_KOBO on
   the device). These few lines are the whole of the difference. */
extern bool platform_poll_raw_key(koboy_platform *pf, uint16_t *code);

#ifdef KOBOY_PLATFORM_KOBO
#include "platform_kobo.h"
#else
extern koboy_platform *platform_sdl_create(void);
extern void            platform_sdl_set_panel(koboy_platform *pf, int w, int h);
#endif

#define DEFAULT_INI "config/koboy.ini"

/* koboy_stop is DEFINED IN screens.c (screens.h says why: the screen loops
   poll it, and that file links into test binaries with no main.c). This
   handler is still its only writer. */
static void on_signal(int sig) { (void)sig; koboy_stop = 1; }

/* set by the frame callback each time the core emits a frame */
static const void *g_frame; static unsigned g_fw, g_fh; static size_t g_fpitch;
static void on_frame(void *ud, const void *d, unsigned w, unsigned h, size_t pitch)
{ (void)ud; g_frame = d; g_fw = w; g_fh = h; g_fpitch = pitch; }

static uint16_t on_input(void *ud) { return input_state((koboy_input *)ud)->buttons; }

/* The core's DISPLAY aspect as the pipeline should treat it: what the core
   reported, or "absent" when the owner turned the correction off. Every reader
   of the core's aspect goes through this or core_par -- video_set_aspect
   included, which is a second path into the same decision and would otherwise
   leave video scaling corrected while the rect was not. */
static uint32_t core_aspect(const koboy_config *cfg, const koboy_core *c)
{
    return cfg->pixel_aspect ? core_display_aspect(c) : 0u;
}

/* The pixel aspect of the frames this core is rendering NOW, from its reported
   display aspect and BASE geometry (not max -- see config_resolve_profile_par).
   KOBOY_ASPECT_ONE for a square-pixel core, which is what makes every rect and
   fit identical to what they were before non-square pixels existed. */
static uint32_t core_par(const koboy_config *cfg, const koboy_core *c,
                         int base_w, int base_h)
{
    return video_pixel_aspect(core_aspect(cfg, c), base_w, base_h);
}

/* Named in the log because "why are the controls gone?" is otherwise
   unanswerable on a device with no terminal: the layout comes from the ROM's
   extension, so a mis-named file is a plausible cause. */
static const char *layout_name(int mode)
{
    return mode == KOBOY_LAYOUT_LCD ? "LCD" : "DMG";
}

/* The other half of the input surface, for a core that reads a pointer -- see
   core_set_pointer_fn. input.c fills it in whenever the LCD layout is live and
   leaves pressed false otherwise, so installing it unconditionally costs a
   Game Boy session three stores per frame. */
static void on_pointer(void *ud, int16_t *x, int16_t *y, bool *pressed)
{
    const koboy_input_state *st = input_state((koboy_input *)ud);
    *x = st->pointer.x; *y = st->pointer.y; *pressed = st->pointer.pressed;
}

static bool g_quiet;
static void say(const char *fmt, ...)
{
    if (g_quiet) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* Every message the user must SEE goes through here. On the device there is
   no terminal, so an error reaching only stderr is indistinguishable from a
   crash; the Kobo backend draws it on the panel and waits for an
   acknowledgement (bounded at 20 s, so an unattended run cannot hang).

   NOTHING HERE ENDS THE PROCESS -- the CALLERS do. The split below says which
   by name: notify() for a condition the session survives, fatal() for one it
   does not. The names are the point: "fatal" above a return to the ROM browser
   reads as a bug until someone checks. */
static koboy_platform *g_pf;
static void message_v(const char *fmt, va_list ap)
{
    char msg[512];
    vsnprintf(msg, sizeof msg, fmt, ap);
    fprintf(stderr, "koboy: %s\n", msg);
#ifdef KOBOY_PLATFORM_KOBO
    if (g_pf) platform_kobo_fatal(g_pf->ctx, msg);
#endif
}

/* The session goes on after this one. */
static void notify(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    message_v(fmt, ap);
    va_end(ap);
}

/* The caller is about to end the run. */
static void fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    message_v(fmt, ap);
    va_end(ap);
}

/* One place for "that game did not start" -- three sites reach the same two
   decisions. Returns true if the caller should go BACK TO THE MAIN MENU, false
   if it must end the run; `recoverable` is the caller's used_startup_ui, and a
   run given its ROM on the command line has no list behind it.

   The message names the game FIRST on its own line, then the reason. `err`
   already contains the full path, so the panel deliberately repeats the name:
   fbink wraps at the column edge, not at word boundaries, so at fontmult 3 a
   full path is several unreadable lines. The log line keeps the path. */
static bool load_failed_recoverable(bool recoverable, const char *rom_path,
                                    const char *err)
{
    if (!recoverable) {
        fatal("%s", err);
        return false;
    }
    char name[KOBOY_RECENT_DISPLAY];
    recent_name_from_path(name, sizeof name, rom_path);
    /* Explicit precisions, not snprintf's own bound: `name` and `err` can each
       fill the 512-byte buffer alone, and the first to overflow would silently
       eat the other. The numbers are a panel budget -- at fontmult 3 the Libra
       2's 1264 px takes ~20 characters, so 60 is three lines of filename and
       300 is the reason under it. */
    notify("COULD NOT LOAD\n%.60s\n\n%.300s", name, err);
    return true;
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
        "  --quiet           suppress everything but the presented= counter\n"
        "  --rom-dir PATH    directory the ALL GAMES list scans\n"
        "  --ui-script PATH  replay synthetic UI input into the startup flow\n"
        "                    (MAIN MENU, then RECENT or ALL GAMES) and, via\n"
        "                    the `menu` verb, the in-game MENU;\n"
        "                    exits 4 if the script selects nothing\n",
        argv0, DEFAULT_INI);
}

/* No MODE_BROWSE: the startup file browser is reached only via MODE_MAIN ->
   ALL GAMES (screen_browser, screens.c). */
typedef enum { MODE_MAIN, MODE_PLAY, MODE_MENU, MODE_QUIT } koboy_mode;

/* One definition of "put the faceplate back", used by every exit from a UI
   mode and by the post-calibration/fatal/SRAM-warning paths. */
static void redraw_chrome(koboy_platform *pf, uint8_t *panel, int stride,
                          int pw, int ph, const koboy_profile *prof,
                          const koboy_layout *layout)
{
    memset(panel, 0xFF, (size_t)stride * (size_t)ph);
    chrome_render(panel, stride, prof, layout);
    /* Free, because the panel is already being repainted. */
    chrome_render_battery(panel, stride, prof, layout,
                          pf->battery_percent ? pf->battery_percent(pf->ctx) : -1);
    pf->blit_gray8(pf->ctx, panel, pw, ph, stride, 0, 0);
    pf->refresh(pf->ctx, 0, 0, pw, ph, KOBOY_REFRESH_FULL);
}

/* --------------------------------------------------------- SCREENSHOT note
 *
 * A capture has to confirm itself on the panel, under two constraints:
 *
 *  - IT MUST NOT PAINT OVER THE FRAME BEING SAVED. The file is written first,
 *    from the composited frame; only then is anything drawn.
 *  - IT MUST NOT COST A FULL-PANEL FLASH. notify() repaints everything, which
 *    mid-game is a worse interruption than what it reports. This paints a
 *    small plaque in the band BETWEEN the game rect and the controls,
 *    refreshes that rectangle alone, and takes it away seconds later.
 *
 * A WHITE PLAQUE WITH BLACK TEXT rather than text on whatever chrome put
 * there: the band is not uniform across layouts (the DMG faceplate has a case
 * shade, a wordmark and a bezel edge through parts of it). Erasing is exact
 * and cheap for the same reason: chrome is re-rendered into the panel buffer
 * and only the plaque's rectangle is blitted back, so what was underneath
 * returns byte for byte without this code knowing what it was. */
#define SHOT_NOTE_PX  3          /* 5x7 font scale: 21px glyphs, as the menus */
#define SHOT_NOTE_PAD 12
#define SHOT_NOTE_MS  2500

/* Where the plaque goes, or false when there is nowhere for it. The band
   between the game rect and the controls is the one part of the panel that is
   neither picture nor touch target, but its height is a consequence of the
   fitted rect rather than a reservation, so on some layouts (the Game & Watch
   strip, a small panel with a tall game) it is too short. Then the log line is
   the only report: a plaque over the controls or the game is worse than
   none. */
static bool shot_note_rect(const koboy_profile *prof, const koboy_layout *layout,
                           int pw, int ph, const char *msg, koboy_rect *out)
{
    int w = text_measure(msg, SHOT_NOTE_PX) + 2 * SHOT_NOTE_PAD;
    int h = TEXT_GLYPH_H * SHOT_NOTE_PX + 2 * SHOT_NOTE_PAD;
    int top = prof->game_y + prof->game_h;
    int bot = chrome_controls_top(prof->layout_mode, layout, pw, ph);
    if (w > pw || h > bot - top) return false;

    out->w = w;
    out->h = h;
    out->y = top + (bot - top - h) / 2;
    /* Centred on the GAME rect, not the panel: the plaque belongs under the
       picture it is about. LIVE CLAMP -- a rect near an edge would otherwise
       put it off the panel. */
    out->x = prof->game_x + (prof->game_w - w) / 2;
    if (out->x < 0) out->x = 0;
    if (out->x + w > pw) out->x = pw - w;
    return true;
}

/* Everything that must happen when a ROM becomes the current game, in the one
   safe order. Three hazards, all silent if got wrong:
     - core_sram() is re-fetched every time: the pointer belongs to the core's
       freshly loaded cartridge, so caching it across unload/load is a
       use-after-free waiting for a second game.
     - The OUTGOING game's SRAM is flushed by the caller BEFORE unload:
       retro_unload_game takes the buffer and its last minutes with it.
     - sram_writeback stays false for the session when a save file exists but
       could not be read whole. */
typedef struct {
    char     path[512];        /* .srm path for the current rom */
    uint8_t *mem;
    size_t   len;
    bool     writeback;
} koboy_sram_binding;

static bool load_rom_into(koboy_core *core, koboy_config *cfg,
                          koboy_sram_binding *sb, char *err, size_t errlen)
{
    /* LIVE GUARD, the only one here that stops a CRASH rather than a wrong
       picture: snes9x2005 raises SIGFPE inside retro_load_game for a .sfc/.smc
       under 8192 bytes, killing koboy outright -- no error screen, no way back
       to the browser. config_min_rom_bytes has the measurement and why the
       floor is per-system.

       HERE and not core.c (which must not know what it is loading) or
       romlist.c (which lists names without stat()ing them): this is the one
       place with both the path and a reason to touch the filesystem. Reported
       as an ordinary load failure. */
    size_t floor_bytes = config_min_rom_bytes(cfg->rom_path);
    if (floor_bytes) {
        struct stat st;
        if (stat(cfg->rom_path, &st) == 0 && st.st_size >= 0 &&
            (size_t)st.st_size < floor_bytes) {
            /* REASON BEFORE PATH: err is a fixed buffer and rom_path can fill
               it alone, so putting the path last truncates the name rather
               than the explanation. The explicit %.200s (rather than letting
               snprintf clip silently) is what keeps this line free of a
               -Wformat-truncation warning. */
            if (err && errlen)
                snprintf(err, errlen,
                         "too short to be a cartridge: %lld bytes, minimum "
                         "%lu -- the core would crash on it. rom %.200s",
                         (long long)st.st_size, (unsigned long)floor_bytes,
                         cfg->rom_path);
            return false;
        }
    }

    if (!core_load_rom(core, cfg->rom_path, err, errlen)) return false;

    sram_path_for_rom(sb->path, sizeof sb->path, cfg->save_dir, cfg->rom_path);
    sb->len = 0;
    sb->mem = core_sram(core, &sb->len);
    sb->writeback = true;
    return true;
}

int main(int argc, char **argv)
{
    koboy_config cfg;
    const char  *ini_path = DEFAULT_INI;
    unsigned long frame_limit = 0;
    int panel_w = 0, panel_h = 0;
    bool selftest = false;
    const char *message = NULL;
    const char *ui_script_path = NULL;
    bool        rom_from_argv  = false;

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
        else if (!strcmp(a, "--rom"))      { snprintf(cfg.rom_path,  sizeof cfg.rom_path,  "%s", argv[++i]); rom_from_argv = true; }
        /* Explicit, so the ROM's extension does not override it below. */
        else if (!strcmp(a, "--core"))     { snprintf(cfg.core_path, sizeof cfg.core_path, "%s", argv[++i]); cfg.core_explicit = true; }
        else if (!strcmp(a, "--save-dir")) snprintf(cfg.save_dir,  sizeof cfg.save_dir,  "%s", argv[++i]);
        else if (!strcmp(a, "--config"))   i++;   /* already handled */
        else if (!strcmp(a, "--message"))  message = argv[++i];
        else if (!strcmp(a, "--frames"))   frame_limit = strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--rom-dir"))  snprintf(cfg.rom_dir, sizeof cfg.rom_dir, "%s", argv[++i]);
        else if (!strcmp(a, "--ui-script")) ui_script_path = argv[++i];
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

    /* After both the ini and the command line, so a bare name from either is
       treated the same. dlopen never searches the cwd. */
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

    /* Only now can fatal() draw: before init there is no framebuffer. */
    if (!pf->init(pf->ctx, &cfg)) { fprintf(stderr, "koboy: platform init failed\n"); return 1; }
    g_pf = pf;

    /* Here, not beside the emulator loop: the calibration wait below tests
       koboy_stop, and a handler installed after it would leave that test dead.
       After platform init on purpose, so it overrides any handler the
       backend's own library installed. */
    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);

    int pw = 0, ph = 0;
    pf->screen_info(pf->ctx, &pw, &ph);

    /* Before anything else can fail, so the smoke test still gets its facts
       out of a run that then dies on a missing core or ROM. */
    if (selftest) {
#ifdef KOBOY_PLATFORM_KOBO
        platform_kobo_selftest(pf);
#else
        printf("panel=%dx%d\nwfm_fast=none\nstride=%d\ntouch_transpose=0\n",
               pw, ph, pw);
        fflush(stdout);
#endif
    }

    /* --message is for the launcher script, which has something to tell the
       user and no terminal to put it in. It reuses fatal() rather than the
       device's own fbink so there is exactly one on-panel error presentation.
       Always non-zero: it only ever reports a failure. */
    if (message) {
        fatal("%s", message);
        pf->shutdown(pf->ctx);
        free(pf);
        return 3;
    }

    static koboy_input_state ui_script[UISCRIPT_MAX];
    /* Parallel to ui_script, never inside it -- see uiscript.h for why the
       marker must not be a field of koboy_input_state. */
    static unsigned char ui_script_menu[UISCRIPT_MAX];
    int ui_script_n = 0;
    if (ui_script_path) {
        ui_script_n = uiscript_load(ui_script_path, ui_script, ui_script_menu,
                                    UISCRIPT_MAX);
        if (ui_script_n < 0) {
            fatal("cannot read ui script %s", ui_script_path);
            pf->shutdown(pf->ctx);
            return 2;
        }
        /* uiscript.h's contract: an error must FAIL the run rather than
           silently pass a test that exercised nothing. An empty or
           comment-only script is not a read error (uiscript_load returns 0,
           not -1), but treating it as "no script" falls back to screen_list's
           live-polling branch -- and nobody is at the panel to poll, so the
           run blocks forever instead of failing. */
        if (ui_script_n == 0) {
            fatal("ui script %s is empty (no verbs)", ui_script_path);
            pf->shutdown(pf->ctx);
            return 2;
        }
    }

    /* An explicit --rom or rom= goes straight to play, keeping every smoke
       test, --frames run and scripted path behaving as it did in v1. The
       shipped ini leaves rom commented out, so a real user starts on the MAIN
       MENU: a 300-entry list is the wrong first screen now RECENT exists. */
    koboy_mode mode = cfg.rom_path[0] ? MODE_PLAY : MODE_MAIN;

    /* Genuinely unused. Groundwork for offering a --rom's own directory as the
       CHOOSE ROM listing, which would mean silently overriding cfg.rom_dir
       whenever it still held the compiled-in default -- a real feature with
       its own failure modes and no request for it. Left alone rather than
       guessed at. */
    (void)rom_from_argv;

    /* A PLACEHOLDER profile against the Game Boy's fixed 160x144: no ROM has
       been chosen yet, and a core's geometry is only meaningful after
       retro_load_game, so there is nothing else honest to lay
       chrome/calibration/the menu out against. Re-resolved below once a ROM is
       loaded, with the faceplate redrawn if anything changed -- which for a
       Game Boy it never does. */
    koboy_profile prof;
    /* THE PIXEL ASPECT `prof` WAS RESOLVED WITH, carried alongside because
       koboy_profile does not hold it and the staleness test below needs it.
       Square here because no ROM is loaded yet, so there is no core to ask. */
    uint32_t prof_par = KOBOY_ASPECT_ONE;
    if (!config_resolve_profile(&prof, &cfg, pw, ph,
                                KOBOY_GB_W, KOBOY_GB_H, KOBOY_GB_W, KOBOY_GB_H)) {
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
    /* Free, because the panel is already being repainted. */
    chrome_render_battery(panel, panel_stride, &prof, &cfg.layout,
                          pf->battery_percent ? pf->battery_percent(pf->ctx) : -1);
    pf->blit_gray8(pf->ctx, panel, pw, ph, panel_stride, 0, 0);
    pf->refresh(pf->ctx, 0, 0, pw, ph, KOBOY_REFRESH_FULL);

    /* ------------------------------------------------------- calibration */
    if (calib_needed(&cfg)) {
        if (frame_limit) {
            /* A scripted, bounded run has nobody to press a button. */
            say("koboy: uncalibrated, skipping calibration for a scripted run\n");
        } else {
            /* A throwaway input object, whose only job is to let this loop SEE
               A TOUCH: the loop used to advance on platform_poll_raw_key()
               alone, so a Kobo with no page-turn buttons had no way to answer
               the prompt and only the power button (which quits) responded. Not
               reused as the session's input object on purpose -- input_create
               copies the config by value, so one made here would carry the
               PRE-calibration key mapping for the whole session. */
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
            while (!done && !escaped && !koboy_stop && !pf->should_quit(pf->ctx)) {
                if (k.stage != last_stage) {
                    last_stage = k.stage;
                    memset(panel, 0xFF, (size_t)panel_stride * (size_t)ph);
                    text_draw_centred(panel, panel_stride, pw, ph, ph / 2 - 40,
                                 calib_prompt(&k), 5, 0x00);
                    /* The escape has to be ON THE PANEL: a device that cannot
                       answer the prompt has no terminal either. */
                    text_draw_centred(panel, panel_stride, pw, ph, ph / 2 + 40,
                                 calib_escape_prompt(), 3, 0x00);
                    pf->blit_gray8(pf->ctx, panel, pw, ph, panel_stride, 0, 0);
                    pf->refresh(pf->ctx, 0, 0, pw, ph, KOBOY_REFRESH_FULL);
                    say("koboy: %s (%s)\n", calib_prompt(&k), calib_escape_prompt());
                }
                /* Touch FIRST, keys second, and the order is LOAD-BEARING:
                   platform_poll_raw_key drains the same event nodes with a NULL
                   input, throwing away every touch event it passes over. */
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
                /* Not just a break: leaving the zero sentinel would make
                   input_feed_key ignore every key for the rest of the run. */
                calib_escape(&cfg);
                say("koboy: calibration skipped by touch, keeping a=%u b=%u\n",
                    (unsigned)cfg.key_a, (unsigned)cfg.key_b);
            }

            /* Put the faceplate back: calibration wrote over it. */
            redraw_chrome(pf, panel, panel_stride, pw, ph, &prof, &cfg.layout);
        }
    }

    /* recent.dat lives beside save_dir, not beside koboy.ini. Two reasons,
       either sufficient: save_dir is guaranteed writable by the time execution
       reaches here (config_resolve_paths resolved it install-relative, and the
       SRAM/state paths already trust it), while koboy.ini may live wherever
       the user chose for CONFIGURATION and need not be writable from a
       menu-launched process -- and recent.dat is data, like a .srm. */
    char recents_file[600];
    snprintf(recents_file, sizeof recents_file, "%s/recent.dat", cfg.save_dir);

    /* Shared across every list screen one --ui-script run drives -- see
       screen_list's script_i comment for why one flat script walks more than
       one screen. NULL when there is no script. */
    int script_i = 0;
    const koboy_input_state *ui_scr = ui_script_n > 0 ? ui_script : NULL;
    int *ui_scr_i = ui_script_n > 0 ? &script_i : NULL;

    /* ------------------------------------------------------- the session loop
       ONE GAME IS ONE SESSION: pick a ROM, derive everything from it, open a
       core, play, tear all of it down. Two things send execution round again.

       A FAILED LOAD is an ORDINARY condition -- the file was deleted between
       the scan and the tap, an SD card is not mounted, a partial copy is
       short, a core is missing, the core refuses it. It used to end the
       process, which on the device is indistinguishable from a crash: koboy
       vanishes and Nickel comes back. Reported twice from the device.

       A MID-SESSION SWITCH. MENU -> CHOOSE ROM used to call load_rom_into on
       the LIVE core -- the one opened for the FIRST ROM's extension -- and it
       killed a device: a Mega Drive .md handed to gpSP, executed as ARM code,
       SIGSEGV ("bad jump 8000000", with no second "koboy: core" line because
       no second core was ever opened). The core was not the only stale thing:
       layout, extra buttons, scale ceiling and the save binding all derive
       from the extension. So CHOOSE ROM now ends the session and comes back
       here, where all of it is re-derived.

       UNCONDITIONALLY, with no same-core fast path: switching games is a human
       tapping through three screens, and "reload into the live core when the
       extension happens to match" is a second path only the rarest case
       exercises -- which is how the bug above survived. Reopening the same .so
       costs one dlopen.

       Back to the MAIN MENU, not the list the ROM came from: this is the
       loop's own top, both lists are one tap away, and RECENT must be rebuilt
       from disk anyway. DO NOT ADD A MODE FOR THIS.

       A run that did NOT come through the startup UI (--rom, or rom= in the
       ini) still dies on a failed load: there is no list to go back to, and
       `koboy --rom nonsense.gb` exiting 0 would lie to whatever launched it.
       used_startup_ui tells them apart.

       ITS BODY IS DELIBERATELY NOT RE-INDENTED down to the matching brace
       ("end of the session loop") -- re-indenting four hundred lines to add
       one enclosing loop would bury the behaviour change. */

    /* THE RUN'S counters, not the session's: a run that plays three games
       reports one total, which is what `presented=` has always meant and what
       the smoke tests grep for. */
    koboy_stats stats;
    stats_reset(&stats);
    unsigned long presented = 0, since_cleanup = 0, cleanups = 0, big_refreshes = 0;
    unsigned long rects_emitted = 0;
    /* Run-scoped because the pacer is per-SESSION: pacer_init zeroes its
       counters, so pace.held at session_end would report only the last game. */
    unsigned long settle_held = 0;
    uint64_t last_sram_us = 0, last_cleanup_us = 0;
    /* SCREENSHOT state, and `armed` IS the design: MENU -> SCREENSHOT does not
       take a picture, because the menu is drawn OVER the game and a capture
       there would photograph the menu. It sets this; the capture happens from
       the next COMPOSITED frame (see the arming branch and the capture site
       for why that is well defined under present_divisor and the pacer).
       Run-scoped only because everything else here is. */
    bool       shot_armed = false;
    /* The on-panel confirmation and its expiry; zero = nothing showing. */
    uint64_t   shot_note_until_us = 0;
    koboy_rect shot_note = { 0, 0, 0, 0 };
    /* --frames N is a budget for the RUN. Each session gets its own pacer and
       pacer_init zeroes p->frames, so a switch would otherwise hand the second
       game a fresh budget of N. frames_done holds what finished sessions
       spent. */
    unsigned long frames_done = 0;
    /* Whether anything has been PLAYED yet, which decides how a "nothing
       chosen" exit behaves: at startup koboy exits straight out (0, or 4 for a
       script that selected nothing); once a game has run, the same choice must
       fall through to the run's summary. */
    bool any_game_ran = false;

    char err[512];
    koboy_core *core = NULL;
    /* Fully braced/enumerated zero-init, NOT a bare {0}: Linaro GCC 4.9 (the
       ARM cross compiler; the host's newer GCC does not) applies
       -Wmissing-braces to the nested path[] array and
       -Wmissing-field-initializers to the rest, so {0} is warning-free on host
       and warning-FULL on-device. */
    koboy_sram_binding sb = {{0}, NULL, 0, false};

    for (;;) {

    /* Captured before the picker can change `mode`: only a session that went
       through a UI screen painted over the faceplate, and the --rom fast path
       must not pay for a redraw of chrome nothing touched. Re-evaluated every
       session -- the SECOND game always comes from the UI. */
    bool used_startup_ui = (mode == MODE_MAIN);

    while (mode == MODE_MAIN) {
        koboy_input *ui_in = input_create(&cfg, &prof);
        if (!ui_in) { fatal("out of memory"); free(panel); pf->shutdown(pf->ctx); return 1; }
#ifdef KOBOY_PLATFORM_KOBO
        platform_kobo_setup_touch(pf, ui_in);
#else
        input_set_touch_transform(ui_in, pw, ph, false, false, false);
#endif
        int choice = screen_main_menu(pf, ui_in, panel, panel_stride, pw, ph,
                                   ui_scr, ui_scr_i, ui_script_n);

        if (choice == MAIN_RECENT) {
            koboy_recent rc;
            recent_load(&rc, recents_file);      /* corrupt/missing -> empty, never fatal */
            recent_prune_missing(&rc);
            int ri = screen_recent_picker(pf, ui_in, panel, panel_stride, pw, ph, &rc,
                                       ui_scr, ui_scr_i, ui_script_n);
            input_destroy(ui_in);
            if (ri >= 0) {
                snprintf(cfg.rom_path, sizeof cfg.rom_path, "%s", recent_path(&rc, ri));
                say("koboy: chose %s (recent)\n", cfg.rom_path);
                /* NOT recorded here: "played" means LOADED. Recording at pick
                   time promoted a ROM that then failed to the top of the very
                   list the user must walk past to try something else. */
                mode = MODE_PLAY;
            }
            /* else: BACK, or the run was stopped/exhausted on the recent
               screen -- back to MAIN MENU either way, where the `else` branch
               below converges on the same terminal exit one iteration later. */
        } else if (choice == MAIN_ALL_GAMES) {
            int br = screen_browser(pf, ui_in, panel, panel_stride, pw, ph,
                                 cfg.rom_dir, cfg.rom_path, sizeof cfg.rom_path,
                                 ui_scr, ui_scr_i, ui_script_n);
            input_destroy(ui_in);

            if (br == BROWSE_ERR_DIR || br == BROWSE_ERR_EMPTY) {
                /* Two distinct messages: a wrong rom_dir and an empty one are
                   different mistakes, and this is the only diagnostic a user
                   with no terminal gets. Terminal only on the FIRST session --
                   once a game has run, an unreadable rom_dir is a dead end in
                   one list, not a reason to end the run; RECENT is still
                   there. */
                const char *why = (br == BROWSE_ERR_DIR)
                    ? "cannot read rom directory\n%s"
                    : "no .gb, .gbc or .mgw files in\n%s";
                if (any_game_ran) {
                    notify(why, cfg.rom_dir);
                    continue;                     /* back to the MAIN MENU */
                }
                fatal(why, cfg.rom_dir);
                free(panel); pf->shutdown(pf->ctx); return 2;
            }
            if (br != BROWSE_PICKED) {
                /* A SCRIPTED run that ends without a rom chosen is a failure,
                   not a clean exit -- screen_list says why it needs its own
                   exit code. Backing out interactively (only via a signal or
                   should_quit; ".." goes UP a level) still exits 0. */
                if (any_game_ran) goto session_end;
                if (ui_script_n > 0) {
                    fatal("ui script selected nothing");
                    free(panel); pf->shutdown(pf->ctx); return 4;
                }
                say("koboy: no rom chosen, exiting\n");
                free(panel); pf->shutdown(pf->ctx); return 0;
            }
            say("koboy: chose %s\n", cfg.rom_path);
            mode = MODE_PLAY;
        } else {
            /* MAIN_QUIT, or screen_main_menu was stopped/exhausted
               (choice == -1: koboy_stop, should_quit, or a script's verbs
               running out). All are ends with nothing to resume to. */
            input_destroy(ui_in);
            /* A session already played: this closes a run that has numbers to
               report, so it joins the ordinary end of the loop rather than
               throwing the summary -- koboy.log's only record -- away. */
            if (any_game_ran) goto session_end;
            if (ui_script_n > 0) {
                fatal("ui script selected nothing");
                free(panel); pf->shutdown(pf->ctx); return 4;
            }
            say("koboy: no rom chosen, exiting\n");
            free(panel); pf->shutdown(pf->ctx); return 0;
        }
    }

    /* mode is MODE_PLAY from here until the emulator loop writes it. MODE_QUIT
       ends the loop from inside the menu, kept distinct from koboy_stop so the
       final status line does not call a menu-driven quit "stopped by
       signal". */

    /* --------------------------------------------------------- core, ROM
       The core is opened and the ROM loaded BEFORE video_create/input_create:
       the real game rect depends on the core's geometry, which libretro only
       answers honestly once a game is loaded.

       Everything the ROM's EXTENSION decides is derived here, and re-derived
       on EVERY session including a mid-session switch (that omission is what
       handed a Mega Drive ROM to gpSP and took SIGSEGV). All of it must come
       BEFORE the config_resolve_profile call further down, which reads it. */
    /* WHICH PRESENTATION this ROM gets. Set unconditionally, unlike the core
       below: an explicit --core cannot make a Game Boy faceplate right for a
       Game & Watch unit whose buttons are in its own artwork (config.h). */
    cfg.layout_mode = config_layout_for_rom(cfg.rom_path);
    /* And the button complement (config.h). Also before
       config_resolve_profile: chrome_controls_top counts the extra discs. */
    config_extra_buttons_for_rom(&cfg.layout, cfg.rom_path);
    /* And what the LCD strip's controls SAY. Only the LCD faceplate reads
       these, but they are set beside the calls above so one place shows
       everything the extension decides -- which is what stops a fourth being
       added elsewhere. */
    config_lcd_pad_for_rom(&cfg.layout, cfg.rom_path);
    /* Which geometry the LCD rect is sized from -- a fact about the system,
       like layout_mode. MUST precede config_resolve_profile. */
    cfg.lcd_rect_from_max = config_lcd_rect_from_max_for_rom(cfg.rom_path);
    /* OUTSIDE the core_explicit branch on purpose: a scale ceiling is a
       property of the SYSTEM, not of which core the owner pointed at it. */
    cfg.scale_ceiling = config_scale_ceiling_for_rom(cfg.rom_path);

    if (!cfg.core_explicit) {
        const char *want = config_core_for_rom(cfg.rom_path);
        char        dir[PATH_MAX], joined[512];
        if (config_exe_dir(dir, sizeof dir) &&
            config_join_sibling(joined, sizeof joined, want, dir))
            snprintf(cfg.core_path, sizeof cfg.core_path, "%s", joined);
        else
            snprintf(cfg.core_path, sizeof cfg.core_path, "%s", want);
    }

    /* Logged: "which core did it pick?" is otherwise unanswerable on a device
       with no terminal, where the only symptom of a wrong pick is a core that
       rejects the ROM. */
    say("koboy: core %s\n", cfg.core_path);
    /* And the faceplate, for the same reason: the button complement is decided
       above and drawn hundreds of lines later, and the only symptom of
       forgetting to ask for it is a button that quietly is not there --
       indistinguishable from a system that never had one.
       tests/smoke_host.sh reads this line. */
    {
        /* Every extra disc by NAME, not a yes/no: different systems add
           different discs, so "with a C button" could not tell a WonderSwan's
           L1/R1 pair from a missing pair. One buffer so the line stays one
           say() call, which is what tests/smoke_host.sh matches. */
        char extras[64] = "";
        for (int i = 0; i < KOBOY_MAX_EXTRA_BTNS; i++)
            if (cfg.layout.extra[i].r > 0) {
                size_t n = strlen(extras);
                snprintf(extras + n, sizeof extras - n, "%s%s",
                         n ? " " : ", extra buttons: ", cfg.layout.extra[i].label);
            }
        say("koboy: faceplate %s%s\n", layout_name(cfg.layout_mode), extras);
    }

    core = core_open(cfg.core_path, cfg.save_dir, err, sizeof err);
    if (!core) {
        /* A MISSING CORE IS A FAILED LOAD, not a broken installation: the core
           is picked from the extension, so "tapped a .gba on a device whose
           gpsp_libretro.so did not survive the copy" arrives here, and to the
           user it is the same event as a ROM the core refuses. */
        if (!load_failed_recoverable(used_startup_ui, cfg.rom_path, err)) {
            free(panel);
            pf->shutdown(pf->ctx); return 1;
        }
        mode = MODE_MAIN;
        continue;
    }

    if (!load_rom_into(core, &cfg, &sb, err, sizeof err)) {
        /* Closed, not kept: the next ROM picks its own core, and a handle left
           open here leaks one .so per failed tap. core_close on a core with no
           ROM loaded is the ordinary shutdown path. */
        core_close(core);
        core = NULL;
        if (!load_failed_recoverable(used_startup_ui, cfg.rom_path, err)) {
            free(panel);
            pf->shutdown(pf->ctx); return 1;
        }
        mode = MODE_MAIN;
        continue;
    }

    /* --------------------------------------------- re-fit for real geometry
       `prof` is still the PREVIOUS game's shape -- the Game-Boy placeholder on
       the first session, the outgoing game's profile later. Re-resolve against
       what the loaded core reports and redraw the faceplate (an extra
       full-panel refresh, worth avoiding) only when the answer differs.

       Comparing against the OUTGOING game rather than the placeholder is not a
       shortcut: two ROMs of the same system need no re-fit, and two of
       different systems differ in geometry or in layout_mode, which is the
       last term of the condition below. */
    bool chrome_stale = used_startup_ui;
    {
        int rbw, rbh, rmw, rmh;
        /* The LAYOUT is part of what makes the placeholder stale, not just the
           geometry: a .mgw reporting the Game Boy's numbers would still need
           the whole faceplate replaced (gw answers 128x128 here, so it cannot
           happen today -- written down rather than relied on).
           rpar is assigned INSIDE the condition, after core_get_geometry has
           filled rbw/rbh; computing it here would read them uninitialised. */
        uint32_t rpar = prof_par;
        if (core_get_geometry(core, &rbw, &rbh, &rmw, &rmh) &&
            ((rpar = core_par(&cfg, core, rbw, rbh)) != prof_par ||
             rbw != prof.base_w || rbh != prof.base_h ||
             rmw != prof.max_w  || rmh != prof.max_h ||
             cfg.layout_mode != prof.layout_mode)) {
            koboy_profile real_prof;
            if (!config_resolve_profile_par(&real_prof, &cfg, pw, ph,
                                            rbw, rbh, rmw, rmh, rpar)) {
                /* A PER-ROM FAILURE like every other, discoverable only after
                   the core is open and the ROM loaded (it is the core's own
                   max geometry that does not fit). Ending the run for it would
                   be the defect this loop exists to close, one screen later.
                   snprintf'd first because load_failed_recoverable takes a
                   ready-made reason. */
                char gerr[512];
                snprintf(gerr, sizeof gerr,
                         "panel %dx%d is too small for this core's %dx%d game "
                         "rect. rom %.200s", pw, ph, rmw, rmh, cfg.rom_path);
                core_close(core);
                core = NULL;
                sb.mem = NULL; sb.len = 0; sb.writeback = false;
                if (!load_failed_recoverable(used_startup_ui, cfg.rom_path, gerr)) {
                    free(panel);
                    pf->shutdown(pf->ctx); return 1;
                }
                mode = MODE_MAIN;
                continue;
            }
            prof = real_prof;
            prof_par = rpar;
            say("koboy: core geometry %dx%d (max %dx%d), %s layout, "
                "game %dx%d at (%d,%d)\n", rbw, rbh, rmw, rmh,
                layout_name(prof.layout_mode),
                prof.game_w, prof.game_h, prof.game_x, prof.game_y);
            chrome_stale = true;
        }
    }

    /* ONE repaint, not two: the rect may have changed AND a UI screen may have
       painted over it. Done separately, a system switch drew the OUTGOING
       game's faceplate for one full-panel refresh on the way to the incoming
       one's -- on e-ink, a visible flash of the wrong console. */
    if (chrome_stale)
        redraw_chrome(pf, panel, panel_stride, pw, ph, &prof, &cfg.layout);

    /* Recorded HERE, once, for both entry points: "played" means the game
       actually started -- it survived the load AND the re-fit above, the last
       thing that can refuse it. Recording at pick time put a ROM that failed
       at the top of the RECENT list, the wall the user just hit promoted to
       row 0 of the screen they must get past. */
    if (used_startup_ui) {
        koboy_recent rc;
        recent_load(&rc, recents_file);
        recent_touch(&rc, cfg.rom_path);
        recent_save(&rc, recents_file);
    }
    /* SECOND AND LATER SESSIONS ONLY -- any_game_ran still describes the
       PREVIOUS one here, which is what makes this the switch line. Without it
       koboy.log reads as one continuous run. Printed after the load AND the
       re-fit, so it means the new game STARTED, not that it was picked;
       tests/smoke_host.sh leans on that. */
    if (any_game_ran) say("koboy: switched to %s\n", cfg.rom_path);

    /* From here to the teardown, this session owns a loaded ROM: any later
       "nothing chosen" ends the RUN through its summary. */
    any_game_ran = true;

    /* ------------------------------------------------------- video, input */
    /* Logged, and read back off the LIVE koboy_video rather than off cfg --
       which is what makes tests/smoke_host.sh's assertion end-to-end: it fails
       if main.c hands video_create the wrong value, not merely if config.c
       parsed the wrong one. The panel cannot tell you which mapping produced
       the frame, and the setting is reachable from two places. */
    koboy_video *vid = video_create(&prof, cfg.force_dither,
                                    (koboy_gray_map)cfg.gray_map);
    /* The quarter turn an arcade board asks for. Set here rather than in
       video_create because the rotation belongs to the CORE while
       koboy_profile belongs to the config, and config_resolve_profile is
       called from tests with no core at all. prof was resolved from
       core_get_geometry's TRANSPOSED answer, so the two agree by
       construction. */
    if (vid) video_set_rotation(vid, (int)core_rotation(core));
    /* Paired with the rotation everywhere, including every rebuild below: both
       are facts the core announced about how its frames are PRESENTED, both
       are lost when a koboy_video is destroyed, and one without the other
       looks like a broken core rather than a missing line. */
    if (vid) video_set_aspect(vid, core_aspect(&cfg, core));
    if (vid) say("koboy: gray_map %s\n", video_gray_map_name(video_get_gray_map(vid)));
    /* Both halves of the MOTION pair, read back off the LIVE pipeline and
       platform: a line printed from cfg proves only the parser, while this one
       fails if video_create ignored force_dither or the backend never got the
       policy -- the two ways this setting is on in the file and off on the
       panel. */
    if (vid)
        say("koboy: motion %s / %s\n",
            video_get_dither(vid) ? "1-bit" : "4-level",
            pf->wfm_fast_name ? pf->wfm_fast_name(pf->ctx)
                              : config_wfm_policy_name((koboy_wfm_policy)cfg.wfm_fast_policy));
    if (vid && core_rotation(core))
        say("koboy: core asked for %u quarter turn%s; presenting %dx%d\n",
            core_rotation(core), core_rotation(core) == 1 ? "" : "s",
            prof.max_w, prof.max_h);
    koboy_input *in  = input_create(&cfg, &prof);
    if (!vid || !in) {
        fatal("out of memory");
        video_destroy(vid); input_destroy(in);
        core_close(core);
        free(panel);
        pf->shutdown(pf->ctx); return 1;
    }
#ifdef KOBOY_PLATFORM_KOBO
    /* The device's touch layer has its own raw range and is mounted rotated;
       only the backend knows by how much. */
    platform_kobo_setup_touch(pf, in);
#else
    /* The desktop mouse already reports panel coordinates. */
    input_set_touch_transform(in, pw, ph, false, false, false);
#endif
    core_set_frame_cb(core, on_frame, NULL);
    core_set_input_fn(core, on_input, in);
    /* Additive: gambatte never issues a POINTER query, so this is inert for
       the Game Boy. Installed unconditionally rather than only for the LCD
       layout because input.c already gates the state it forwards. */
    core_set_pointer_fn(core, on_pointer, in);

    /* sb.mem is NULL/0 for a cartridge type with no battery-backed SRAM
       (Tetris is 0x00) and must not be dereferenced.
       writeback goes false when a save file EXISTS but could not be read
       whole, and then nothing is written over it for the rest of the session.
       "Saving is off" looks like a bug otherwise: such a file is truncated or
       from another cartridge, and either way it is the only copy of something
       the user cares about. It is left exactly as found, the user is told on
       the panel, and the fix is theirs (move it aside). */
    if (sb.mem && sb.len) {
        if (sram_load(sb.path, sb.mem, sb.len)) {
            say("koboy: loaded %s\n", sb.path);
        } else if (access(sb.path, F_OK) == 0) {
            sb.writeback = false;
            say("koboy: %s could not be read whole; SRAM left as the core "
                "initialised it and saving is disabled this session\n", sb.path);
            /* On the panel, not just the log: a save that silently did not
               load is how a user loses hours. Short lines -- FBInk wraps at the
               column edge, not at word boundaries. */
            notify("Save file unreadable.\nStarting fresh.\nSaving is OFF this run.");
            /* The message drew over the faceplate; put it back. */
            redraw_chrome(pf, panel, panel_stride, pw, ph, &prof, &cfg.layout);
        }
    } else {
        say("koboy: cartridge has no save RAM\n");
    }

    /* ------------------------------------------------------------- the loop */
    koboy_pacer pace;
    /* Paced at THIS core's reported rate, not the Game Boy's 59.7275 Hz that
       KOBOY_FRAME_US hardcodes -- docs/FOLLOWUPS.md #38 and #57, where two
       FinalBurn Neo boards report 30 fps and ran at double speed. Both the raw
       and resolved numbers are logged, because a core whose fps is refused by
       pacer_frame_us_from_fps's plausibility bound is otherwise
       indistinguishable from one that really runs at 59.7275. */
    double core_hz = core_fps(core);
    uint32_t frame_us = pacer_frame_us_from_fps(core_hz);
    say("koboy: core reports %.4f fps; pacing at %u us/frame\n", core_hz, frame_us);
    pacer_init(&pace, pf->now_us(pf->ctx), cfg.present_divisor, frame_us);
    /* Read back off the LIVE pacer for the reason the gray_map line is: it
       fails if main.c hands the pacer the wrong value, which nothing else can
       see. Also the only place pacer_set_divisor's clamp is observable. */
    say("koboy: present_divisor %d\n", pace.divisor);
    /* Printed unconditionally, INCLUDING the disabled 0/0 case: "did the
       throttle engage" is the first question any scrolling report asks, and a
       line that appears only when it is on cannot answer in the negative. The
       full-rect figure is spelled out because it is what gets compared against
       1000/present_divisor*frame_ms to see whether the throttle can bind. */
    say("koboy: settle model %d ms + %d ms/full rect (%u us at a full %dx%d)\n",
        cfg.settle_base_ms, cfg.settle_full_ms,
        pacer_settle_us((uint32_t)cfg.settle_base_ms * 1000u,
                        (uint32_t)cfg.settle_full_ms * 1000u, 1, 1),
        prof.game_w, prof.game_h);

    /* Re-anchored per session, not reset: both are wall-clock marks, and a
       session starting after a minute in the menus must not immediately
       believe both are overdue. */
    last_sram_us = pf->now_us(pf->ctx);
    last_cleanup_us = last_sram_us;

    /* mode != MODE_QUIT is a third way out, beside koboy_stop and
       should_quit(): the in-game menu sets it from inside the loop body.
       Separate from koboy_stop, which is the signal handler's -- reusing it
       would make the final status line call a chosen QUIT "stopped by
       signal". */
    while (mode != MODE_QUIT && mode != MODE_MAIN &&
           !koboy_stop && !pf->should_quit(pf->ctx)) {
        /* frames_done + pace.frames: --frames N is a RUN budget and each
           session gets a fresh pacer. */
        if (frame_limit && frames_done + pace.frames >= frame_limit) break;

        /* EVERY core iteration (60 Hz), not once per presented frame: that
           would drop short presses and add up to 50 ms of latency on top of
           the panel's own. */
        pf->poll_input(pf->ctx, in);

        /* The in-game MENU is entered by ASKING -- a touch zone this loop
           polls -- so --ui-script gets a verb of its own, consumed here rather
           than in screen_list. This closes docs/FOLLOWUPS.md #47: every branch
           below had been verified by READING only, because the scripted path
           (the only one an automated test can take) could not reach here at
           all -- the v1 first-run-deadlock shape exactly, with five
           inhabitants.

           The real request is tested FIRST and the marker consumed only if it
           did not fire, so a scripted run that also sees a live MENU tap spends
           one, not both. */
        bool want_menu = input_take_menu_request(in);
        if (!want_menu && ui_scr) {
            /* Step over the cleared states a list screen left behind --
               uiscript_state_is_idle says why this cannot swallow a tap.
               Without it a script opens the in-game MENU exactly once: every
               screen selects on touch-DOWN and leaves the release, so the
               cursor parks there forever. A second `menu` verb was
               unreachable, and so was any test that switches games twice. */
            while (script_i < ui_script_n && !ui_script_menu[script_i] &&
                   uiscript_state_is_idle(&ui_script[script_i]))
                script_i++;
            if (script_i < ui_script_n && ui_script_menu[script_i]) {
                script_i++;
                want_menu = true;
            }
        }
        if (want_menu) {
            size_t ssz = core_state_size(core);
            /* Read off the DIRECTORY every menu open, not carried in a
               variable: the row names the file about to be written, and after
               a relaunch (or a shot deleted over USB) an in-memory counter
               would name the wrong one. One opendir costs nothing a human
               opening a menu can perceive. */
            char shot_stem[96];
            shot_stem_for_rom(shot_stem, sizeof shot_stem, cfg.rom_path);
            int shot_next = shot_last_seq(cfg.shot_dir, shot_stem) + 1;

            int act = screen_menu(pf, in, panel, panel_stride, pw, ph, ssz > 0,
                               (koboy_gray_map)cfg.gray_map, cfg.present_divisor,
                               cfg.force_dither,
                               (koboy_wfm_policy)cfg.wfm_fast_policy, shot_next,
                               ui_scr, ui_scr_i, ui_script_n);

            if (act == MENU_SAVE || act == MENU_LOAD) {
                int slot = screen_slot_picker(pf, in, panel, panel_stride, pw, ph,
                                           act == MENU_SAVE ? "SAVE TO" : "LOAD FROM",
                                           cfg.save_dir, cfg.rom_path,
                                           ui_scr, ui_scr_i, ui_script_n);
                if (slot) {
                    char sp[512];
                    state_path(sp, sizeof sp, cfg.save_dir, cfg.rom_path, slot);
                    uint8_t *blob = malloc(ssz);
                    if (!blob) {
                        fatal("out of memory for a save state");
                    } else if (act == MENU_SAVE) {
                        if (core_state_save(core, blob, ssz) &&
                            safefile_write(sp, blob, ssz))
                            say("koboy: saved state %d\n", slot);
                        else
                            fatal("could not write\nsave state %d", slot);
                    } else {
                        /* All or nothing: safefile_read_exact leaves blob
                           untouched on a short file. */
                        if (safefile_read_exact(sp, blob, ssz) &&
                            core_state_load(core, blob, ssz)) {
                            say("koboy: loaded state %d\n", slot);
                            /* The core's cartridge RAM was just rewritten (the
                               blob includes it), so the periodic flush will
                               write that to .srm -- a state load is indirectly
                               a save-file write. Re-fetch: the pointer may have
                               moved. */
                            sb.mem = core_sram(core, &sb.len);
                        } else {
                            fatal("could not load\nsave state %d", slot);
                        }
                    }
                    free(blob);
                }
            } else if (act == MENU_RESET) {
                core_reset(core);
            } else if (act == MENU_GRAY) {
                /* Cycles and returns to the GAME rather than reopening the
                   menu: this is a subjective judgement about a reflective
                   panel and can only be made while looking at the game. Two
                   taps per step is the price of seeing the step.

                   Live on the next presented frame: video_set_gray_map
                   rebuilds the LUT, and the return-from-menu path below does
                   redraw_chrome + video_invalidate unconditionally. Without
                   that invalidate the dirty diff leaves unchanged tiles
                   carrying OLD-mapping pixels, which on e-ink persist until
                   something else touches them. */
                cfg.gray_map = (cfg.gray_map + 1) % KOBOY_GRAY_COUNT;
                video_set_gray_map(vid, (koboy_gray_map)cfg.gray_map);
                /* The ini key and this menu are ONE setting. A failed write is
                   not fatal and must not be silent: live this session either
                   way, but it will not survive a relaunch (a read-only .adds,
                   most likely). */
                if (config_save_gray_map(ini_path, (koboy_gray_map)cfg.gray_map))
                    say("koboy: gray_map = %s\n",
                        video_gray_map_name((koboy_gray_map)cfg.gray_map));
                else
                    say("koboy: gray_map = %s (this session only -- "
                        "could not write %s)\n",
                        video_gray_map_name((koboy_gray_map)cfg.gray_map), ini_path);
            } else if (act == MENU_FRAMES) {
                /* Cycles and returns to the GAME, like GREYSCALE and for the
                   same reason: updates per second is a judgement about
                   smearing against choppiness, made while looking at motion.

                   THE LADDER GOES ABOVE THE DEFAULT, which is the point of this
                   entry: residue accumulates per panel UPDATE, so FEWER updates
                   is the direction the evidence points at, and every value ever
                   tried was 3 or below (docs/FOLLOWUPS.md #26).

                   Nothing extra needed here: the divisor changes WHICH core
                   frames reach the panel, not what any contains, so nothing
                   half-drawn survives it. Live immediately -- the pacer holds
                   no per-divisor state beyond the divisor. */
                cfg.present_divisor = config_next_present_divisor(cfg.present_divisor);
                pacer_set_divisor(&pace, cfg.present_divisor);
                /* One setting, two doors. A failed write is not fatal and must
                   not be silent: live this session, gone next relaunch. */
                if (config_save_present_divisor(ini_path, cfg.present_divisor))
                    say("koboy: present_divisor = %d\n", cfg.present_divisor);
                else
                    say("koboy: present_divisor = %d (this session only -- "
                        "could not write %s)\n", cfg.present_divisor, ini_path);
            } else if (act == MENU_MOTION) {
                /* Cycles and returns to the GAME, like GREYSCALE and FRAMES:
                   whether a dithered 1-bit picture under a two-level waveform
                   smears LESS than four greys under AUTO is a judgement about a
                   reflective panel in motion, and NO framebuffer measurement
                   can make it -- residue is panel-side and koboy's dirty diff
                   only sees what koboy wrote.

                   ONE ROW, TWO KEYS, because the thing tested is the PAIR.
                   FBInk's header says a DU-class waveform leaves on-screen
                   pixels as-is for content that is not black or white, so
                   four-level content under DU is the forced-DU4 experiment that
                   already failed here. config_next_motion holds the ladder.

                   BOTH HALVES GO LIVE HERE, each needing its own call:
                   video_set_dither changes what the next frame CONTAINS,
                   pf->set_wfm_policy changes how the panel is asked to draw it,
                   and moving only one is a combination nobody picked. The
                   redraw_chrome + video_invalidate below makes the change
                   whole-frame rather than half-old. */
                {
                    bool             nd = cfg.force_dither;
                    koboy_wfm_policy nw = (koboy_wfm_policy)cfg.wfm_fast_policy;
                    config_next_motion(&nd, &nw);
                    cfg.force_dither    = nd;
                    cfg.wfm_fast_policy = (int)nw;
                    video_set_dither(vid, nd);
                    /* Optional in the seam (platform_if.h), so null-checked: a
                       backend that cannot change waveforms should degrade to
                       "the dithering half still works" rather than crash. */
                    if (pf->set_wfm_policy) pf->set_wfm_policy(pf->ctx, nw);
                    /* Read back off the LIVE pipeline and platform: a line
                       reporting what this branch assigned proves only this
                       branch, while these fail if either setter never
                       landed. */
                    const char *wn = pf->wfm_fast_name ? pf->wfm_fast_name(pf->ctx)
                                                       : config_wfm_policy_name(nw);
                    /* One choice, two keys, ONE write. A failure is not fatal
                       and must not be silent: live this session, gone next
                       relaunch. */
                    if (config_save_motion(ini_path, nd, nw))
                        say("koboy: motion = %s / %s\n",
                            video_get_dither(vid) ? "1-bit" : "4-level", wn);
                    else
                        say("koboy: motion = %s / %s (this session only -- "
                            "could not write %s)\n",
                            video_get_dither(vid) ? "1-bit" : "4-level", wn,
                            ini_path);
                }
            } else if (act == MENU_SHOT) {
                /* ARMS a capture; it does not take one. THE MENU IS DRAWN OVER
                   THE GAME, so a screenshot from here would photograph the
                   menu.

                   "THE NEXT FRAME" is well defined even though present_divisor
                   and the settle pacer can each defer presentation: the capture
                   site sits after the blit/refresh loop, so it fires on the
                   next frame that REACHES THE PANEL. That frame is guaranteed
                   to arrive rather than be suppressed as unchanged, because the
                   return-from-menu path calls video_invalidate -- the next
                   submit is full-dirty by construction.

                   It is a COMPLETE panel, not a dirty-rect fragment:
                   shot_compose builds it from the whole chrome buffer plus the
                   whole game rect, so a frame where eight pixels changed still
                   saves the entire picture.

                   Nothing is written or said here: a run that arms and quits
                   before a frame is presented leaves no file, correctly. */
                shot_armed = true;
            } else if (act == MENU_QUIT) {
                mode = MODE_QUIT;
            } else if (act == MENU_CHOOSE_ROM) {
                /* ALL of it is torn down and rebuilt: this ends the session,
                   and the loop picks the next ROM, opens a core for ITS
                   extension and re-derives the faceplate, buttons, ceiling and
                   save binding. What used to be here reloaded into the LIVE
                   core and killed a device (see the session loop's comment).
                   No teardown here either -- the flush-before-unload ordering
                   is the loop's single teardown, which every way out goes
                   through, so one copy cannot disagree with itself. */
                mode = MODE_MAIN;
                break;
            }

            /* Whatever happened, the panel is now showing a menu. */
            redraw_chrome(pf, panel, panel_stride, pw, ph, &prof, &cfg.layout);
            video_invalidate(vid);
            /* That repaint already took any screenshot plaque with it, so a
               pending erase would spend a panel update rubbing out nothing. It
               also keeps a plaque out of the NEXT capture: the only route to a
               second screenshot is through this menu, so `panel` is plaque-free
               when shot_compose reads it. Two shots of a static screen are
               therefore byte-identical -- tests/smoke_host.sh asserts it. */
            shot_note_until_us = 0;
            /* DRAIN, don't ignore: the screen calls above polled input while a
               menu -- not the faceplate -- was on the panel, and recompute()
               latches a MENU-zone tap regardless of what is drawn there. At the
               default layout that zone overlaps the list's page-forward arrow,
               so paging a long list latches a request nothing consumed, and the
               next iteration reopens the menu ("it keeps reopening by itself").
               The return value is discarded on purpose. */
            (void)input_take_menu_request(in);
            /* REBASE, not re-init: pacer_init zeroes p->frames, so a --frames
               N run used to restart its whole budget every time the menu
               closed. The wall clock does need re-anchoring. */
            pacer_rebase(&pace, pf->now_us(pf->ctx));
            /* Charge the redraw_chrome above, which repainted the WHOLE panel.
               Rebasing does not and must not: hold_until_us is absolute, so a
               menu open for thirty seconds leaves it correctly expired -- but
               the repaint just issued is NOT finished, and the first frame back
               from a menu is where a collision is most visible. A full-rect
               charge is a lower bound on a full-PANEL repaint, which is the
               right direction to be wrong in. */
            pacer_presented(&pace, pf->now_us(pf->ctx),
                            pacer_settle_us((uint32_t)cfg.settle_base_ms * 1000u,
                                            (uint32_t)cfg.settle_full_ms * 1000u,
                                            1, 1));
            continue;
        }

        uint64_t delay = pacer_delay_us(&pace, pf->now_us(pf->ctx));
        if (delay) usleep((useconds_t)delay);

        g_frame = NULL;
        uint64_t t0 = pf->now_us(pf->ctx);
        core_run_frame(core);
        stats_add(&stats, KOBOY_STAGE_CORE, pf->now_us(pf->ctx) - t0);

        /* SOME CORES DO NOT KNOW THEIR GEOMETRY UNTIL INSIDE retro_run().
           MEASURED: gw-libretro reports a 128x128 placeholder from
           retro_get_system_av_info on all 59 titles and only announces the
           real canvas (Parachute 658x395, Mario Bros. 973x532, Donkey Kong
           606x748) from its FIRST retro_run(), via SET_GEOMETRY /
           SET_SYSTEM_AV_INFO. Checked after EVERY retro_run(), not just the
           first: a core may re-announce later (a Multi Screen title folding
           and unfolding), and polling here covers "resolves late, once" and
           "changes again mid-session" without special-casing either. For a core
           that never sends them this is one boolean per frame. */
        if (core_geometry_changed(core)) {
            /* Timing first and unconditionally: SET_SYSTEM_AV_INFO can move the
               frame rate without moving any of the four numbers the base/max
               comparison looks at, so a rate change would otherwise be seen
               only when it arrived alongside a resize. pacer_set_frame_us is a
               no-op when the rate has not changed. */
            pacer_set_frame_us(&pace, pf->now_us(pf->ctx),
                               pacer_frame_us_from_fps(core_fps(core)));
            /* Rotation first, and unconditionally, because it is the one
               announcement that can arrive WITHOUT the numbers moving: a
               square frame turned a quarter turn is the same width and height,
               so the base/max comparison would see nothing to do and the
               pipeline would present the old orientation forever. Cheap when
               nothing changed; when something did, the whole picture moves and
               prev is worthless -- hence the invalidate, the obligation
               video_set_rotation hands a caller that flips it live. The rebuild
               path below repeats it harmlessly. */
            if (video_get_rotation(vid) != (int)core_rotation(core)) {
                video_set_rotation(vid, (int)core_rotation(core));
                video_invalidate(vid);
            }
            /* Same shape, same reason: SET_GEOMETRY carries aspect_ratio too,
               so a core can re-announce its aspect without moving base or max,
               and every pixel of the fit moves when it does. */
            if (video_get_aspect(vid) != core_aspect(&cfg, core)) {
                video_set_aspect(vid, core_aspect(&cfg, core));
                video_invalidate(vid);
            }
            int rbw, rbh, rmw, rmh;
            /* RESOLVE FIRST, THEN COMPARE THE ANSWER -- not the inputs.

               This used to test which INPUT moved and skip the rebuild for a
               base-only change, because the rect, the chrome and video's
               buffers were all max-sized. The buffers and the LCD rect still
               are, but the DMG rect now comes from BASE
               (config_resolve_profile_par), so a base change there really can
               move the rect and the input test would leave the faceplate drawn
               around the wrong one.

               The cost that test avoided is real: a Game & Watch title
               alternates 654x396 <-> 305x191 several times a second, and a
               video rebuild + chrome redraw + forced full-rect refresh at that
               rate is what the device log showed. So the skip is KEPT, keyed on
               the RESOLVED PRESENTATION being identical -- safer and stricter.
               It catches a base change that moves the rect (SNES entering
               512-wide hi-res) and still skips one that does not (Game & Watch,
               whose LCD rect comes from max; PC Engine's 256 <-> 352, whose
               display width is identical in both modes). The Game Boy never
               reaches any of this. */
            uint32_t rpar2 = prof_par;
            if (core_get_geometry(core, &rbw, &rbh, &rmw, &rmh) &&
                ((rpar2 = core_par(&cfg, core, rbw, rbh)) != prof_par ||
                 rbw != prof.base_w || rbh != prof.base_h ||
                 rmw != prof.max_w  || rmh != prof.max_h)) {
                koboy_profile real_prof;
                if (!config_resolve_profile_par(&real_prof, &cfg, pw, ph,
                                                rbw, rbh, rmw, rmh, rpar2)) {
                    fatal("panel %dx%d is too small for this core's %dx%d game rect",
                         pw, ph, rmw, rmh);
                    mode = MODE_QUIT;
                    goto sram_check;
                }
                /* Nothing a rebuild would change: same rect, place, scale and
                   max. Record the new base (the log and video's own fit both
                   want it current) and keep going. */
                if (config_profile_presentation_same(&real_prof, &prof)) {
                    prof.base_w = rbw; prof.base_h = rbh;
                    prof_par = rpar2;
                    goto geometry_done;
                }
                prof = real_prof;
                prof_par = rpar2;
                say("koboy: core geometry settled at %dx%d (max %dx%d), "
                    "%s layout, game %dx%d at (%d,%d)\n",
                    rbw, rbh, rmw, rmh, layout_name(prof.layout_mode),
                    prof.game_w, prof.game_h, prof.game_x, prof.game_y);
                /* The faceplate no longer matches the rect -- redraw before
                   this frame's pixels land. */
                redraw_chrome(pf, panel, panel_stride, pw, ph, &prof, &cfg.layout);
                /* video_create's buffer was sized for the OLD geometry, and
                   video_submit_rects' bounds guard would DROP a bigger frame
                   rather than corrupt memory -- so without this the game
                   renders nothing from here on. The lost diff history costs
                   nothing: video_create seeds prev to force a full-dirty first
                   submit. */
                video_destroy(vid);
                vid = video_create(&prof, cfg.force_dither,
                                   (koboy_gray_map)cfg.gray_map);
                /* Re-applied, not carried over: video_destroy took the old
                   rotation, and a rotation CHANGE is one of the two things that
                   can have brought us here (core.c sets geom_dirty for it).
                   A WonderSwan toggling its orientation exercises this. */
                if (vid) video_set_rotation(vid, (int)core_rotation(core));
                if (vid) video_set_aspect(vid, core_aspect(&cfg, core));
                if (!vid) {
                    fatal("out of memory");
                    mode = MODE_QUIT;
                    goto sram_check;
                }
            }
        }
    /* The `;` is REQUIRED, not stylistic: a label must be followed by a
       STATEMENT and the next line is a declaration. Newer host GCC accepts the
       label-before-declaration form as an extension; Linaro 4.9 rejects it, so
       dropping this breaks the ARM build only. */
    geometry_done: ;

        /* now_us is read ONCE and handed to pacer_tick: pacer.c has no platform
           dependency and must keep none, so it can be tested against a
           synthetic clock -- the only way the settle hold can be asserted
           (tests/test_pacing.c). */
        bool present = pacer_tick(&pace, pf->now_us(pf->ctx));
        if (!present) goto sram_check;

        /* A NULL g_frame is the core's can-dupe signal, which
           video_submit_rects turns into zero rects, so an unchanged frame costs
           no refresh. Up to KOBOY_MAX_RECTS rects: video_split_dirty only
           splits when the pieces' summed cost beats the merged box's, so a
           full-screen scroller still comes back as one. The rects are NOT
           guaranteed disjoint (a capped merge can leave one containing
           another), so blitting each in turn can redo a small overlap -- it
           never misses one. */
        koboy_rect rects[KOBOY_MAX_RECTS];
        t0 = pf->now_us(pf->ctx);
        int nrects = video_submit_rects(vid, g_frame, (int)g_fw, (int)g_fh,
                                        g_fpitch, core_pixfmt(core),
                                        cfg.refresh_fixed_tiles,
                                        rects, KOBOY_MAX_RECTS);
        stats_add(&stats, KOBOY_STAGE_SUBMIT, pf->now_us(pf->ctx) - t0);

        /* Keep the LCD layout's touch->pointer normalisation on the pixels the
           artwork is actually on. video_frame_rect reports where the submitted
           frame landed inside the reserved rect, which is SMALLER whenever the
           core renders below its max geometry -- a Game & Watch title zooming
           to the LCD alone does that several times a second, and the core
           normalises the pointer against what it is currently showing. Here
           rather than input.c (no video dependency there), unconditionally
           because input.c ignores the rect in the other layout.
           BEFORE nrects is tested: an unchanged frame still occupies the same
           rect, and skipping on a duplicate would leave the pointer mapped to
           the last CHANGED frame's size. */
        {
            koboy_rect fr;
            video_frame_rect(vid, &fr);
            input_set_pointer_rect(in, prof.game_x + fr.x, prof.game_y + fr.y,
                                   fr.w, fr.h);
        }

        if (nrects == 0) goto sram_check;       /* nothing changed: skip the panel */

        /* Waveform by TOTAL dirty area across every emitted rect.
           KOBOY_REFRESH_FAST maps to a non-flashing waveform that never fully
           resets pixel state, so residue accumulates on every update regardless
           of rect size -- observed on the device as several Tetris scenes
           layered on each other. A dirty area covering most of the rect means
           the scene changed, which is both when layered residue is most
           objectionable and when the refresh is already expensive, so a
           flashing waveform is cheap in relative terms there.
           NOT a substitute for the cleanup: the dirty diff compares our OWN
           output buffers, so a region that ghosts and then stops changing is
           never revisited by this test.
           SUMMED before deciding, not promoted per rect: two small pieces each
           far below the threshold can together be most of the game rect, and
           deciding rect-by-rect would miss exactly the scene change this
           promotion exists to catch. */
        long dirty_px = 0;
        for (int i = 0; i < nrects; i++) dirty_px += (long)rects[i].w * rects[i].h;

        /* Named wfm, NOT mode: `mode` is the outer loop's live control
           variable. Reusing the name here shadowed it silently (-Wshadow is not
           in this project's flags), and the code was correct only by accident
           of line ordering -- a `mode = MODE_QUIT` anywhere after this point
           would have assigned a waveform instead of ending the run. */
        koboy_refresh_mode wfm = KOBOY_REFRESH_FAST;
        if (config_promote_full(&cfg, dirty_px,
                                (long)prof.game_w * (long)prof.game_h)) {
            wfm = KOBOY_REFRESH_FULL;
            big_refreshes++;
        }

        /* ONE stats_add per FRAME, accumulating BLIT and REFRESH across every
           rect -- not one per rect. CORE and SUBMIT fire once per presented
           frame and stats_mean_us divides by count[stage], so a per-rect
           stats_add would silently change what the mean MEANS, from "cost per
           presented frame" to "cost per rect", with nothing in the printed line
           saying so. It would also bias any split-tuning run by construction:
           quartering into four rects mechanically quarters a per-rect mean even
           if total refresh time went UP. */
        uint64_t blit_us = 0, refresh_us = 0;
        for (int i = 0; i < nrects; i++) {
            const koboy_rect *r = &rects[i];
            t0 = pf->now_us(pf->ctx);
            pf->blit_gray8(pf->ctx,
                           video_buffer(vid) + (size_t)r->y * video_stride(vid) + r->x,
                           r->w, r->h, video_stride(vid),
                           prof.game_x + r->x, prof.game_y + r->y);
            blit_us += pf->now_us(pf->ctx) - t0;

            t0 = pf->now_us(pf->ctx);
            pf->refresh(pf->ctx, prof.game_x + r->x, prof.game_y + r->y,
                        r->w, r->h, wfm);
            refresh_us += pf->now_us(pf->ctx) - t0;
        }
        stats_add(&stats, KOBOY_STAGE_BLIT, blit_us);
        stats_add(&stats, KOBOY_STAGE_REFRESH, refresh_us);

        /* AREA-AWARE PACING: charge the panel time this update cost, so the
           next divisor-eligible frame is vetoed until the panel could finish
           it.

           Charged from dirty_px -- the SAME sum config_promote_full got --
           rather than from the game rect, which is the whole point: a two-tile
           sprite move gets base alone and keeps present_divisor's rate exactly,
           while a full-screen scroll gets base + full and drops to what the
           panel can complete. A run of nothing but sprite moves is
           bit-identical to one without this call.

           AFTER the refresh loop: the hold measures from when the panel was
           handed the work, and pf->refresh is non-blocking, so `now` is within
           a millisecond of the submission. Before the blit it would charge the
           panel for koboy's own blit time.

           NOT here: band-splitting or dropping part of a frame. The update that
           goes out is whole; only the NEXT one waits. Splitting a scroll trades
           a flash for tearing, and tearing on a scroll is worse. */
        pacer_presented(&pace, pf->now_us(pf->ctx),
                        pacer_settle_us((uint32_t)cfg.settle_base_ms * 1000u,
                                        (uint32_t)cfg.settle_full_ms * 1000u,
                                        dirty_px,
                                        (long)prof.game_w * (long)prof.game_h));
        presented++;
        rects_emitted += (unsigned long)nrects;

        /* ------------------------------------------------ the SCREENSHOT
           HERE and not one line earlier: the frame has been composited,
           blitted and handed to the panel, so the game is on screen and the
           menu is gone. The file is written BEFORE anything confirms it, so the
           confirmation cannot end up in the picture.

           shot_capture builds the whole panel itself, because koboy never holds
           it in one place: `panel` is the faceplate and video's buffer is the
           game, blitted separately. Both are complete however little of this
           frame was dirty.

           AFTER pacer_presented: shot_capture allocates 2 MB, composites and
           writes a file, and charging the settle time from a clock read after
           all that would tell the pacer the update finished later than it
           did. */
        if (shot_armed) {
            shot_armed = false;
            koboy_rect gr = { prof.game_x, prof.game_y, prof.game_w, prof.game_h };
            char sp[512];
            int  sq = 0;
            bool shot_ok = shot_capture(cfg.shot_dir, cfg.rom_path,
                                        panel, panel_stride, pw, ph,
                                        video_buffer(vid), video_stride(vid), &gr,
                                        sp, sizeof sp, &sq);
            char msg[64];
            if (shot_ok) {
                say("koboy: screenshot %s\n", sp);
                snprintf(msg, sizeof msg, "SCREENSHOT %03d SAVED", sq);
            } else {
                /* Not fatal and not silent, like a failed ini write: a
                   screenshot that silently went nowhere is indistinguishable
                   from a feature that does not work. The directory is named
                   because that is nearly always the reason. */
                say("koboy: screenshot FAILED (could not write into %s)\n",
                    cfg.shot_dir);
                snprintf(msg, sizeof msg, "SCREENSHOT FAILED");
            }
            /* With the file already on disk, say so on the panel.
               shot_note_rect returns false when there is no band for it, and
               then the log line above is the only report. */
            if (shot_note_rect(&prof, &cfg.layout, pw, ph, msg, &shot_note)) {
                for (int y = 0; y < shot_note.h; y++)
                    memset(panel + (size_t)(shot_note.y + y) * panel_stride + shot_note.x,
                           0xFF, (size_t)shot_note.w);
                text_draw(panel, panel_stride, pw, ph,
                          shot_note.x + SHOT_NOTE_PAD, shot_note.y + SHOT_NOTE_PAD,
                          msg, SHOT_NOTE_PX, 0x00);
                pf->blit_gray8(pf->ctx,
                               panel + (size_t)shot_note.y * panel_stride + shot_note.x,
                               shot_note.w, shot_note.h, panel_stride,
                               shot_note.x, shot_note.y);
                /* FULL, not FAST: the fast waveform is two-level and leaves
                   residue, and ghosted text is exactly what this must not leave
                   behind. Small rectangle, so the flash is local. */
                pf->refresh(pf->ctx, shot_note.x, shot_note.y,
                            shot_note.w, shot_note.h, KOBOY_REFRESH_FULL);
                uint64_t now = pf->now_us(pf->ctx);
                /* Charged: the panel does not care which line asked. */
                pacer_presented(&pace, now,
                                pacer_settle_us((uint32_t)cfg.settle_base_ms * 1000u,
                                                (uint32_t)cfg.settle_full_ms * 1000u,
                                                (long)shot_note.w * shot_note.h,
                                                (long)prof.game_w * (long)prof.game_h));
                shot_note_until_us = now + (uint64_t)SHOT_NOTE_MS * 1000ull;
            }
        }

        /* LIVE GUARD: <= 0 disables cleanup. Without the explicit test, 0 makes
           this always true (a full refresh every presented frame, the inverse
           of "never") and a negative value wraps the cast so cleanup never
           runs. */
        bool due = (cfg.cleanup_interval > 0 &&
                    ++since_cleanup >= (unsigned long)cfg.cleanup_interval);
        /* Wall-clock ceiling: the presented-frame counter fires on no
           particular schedule, because unchanged frames are suppressed -- 70 s
           of measured Tetris presented only 45 frames. Ghosting accumulates
           with time, so time is the backstop. */
        if (!due && cfg.cleanup_max_ms > 0) {
            uint64_t now = pf->now_us(pf->ctx);
            if (now - last_cleanup_us >= (uint64_t)cfg.cleanup_max_ms * 1000ull)
                due = true;
        }
        if (due) {
            since_cleanup = 0;
            last_cleanup_us = pf->now_us(pf->ctx);
            cleanups++;
            /* Game rect, never the full panel: a full-panel flash would
               disturb chrome that has no reason to change. */
            pf->refresh(pf->ctx, prof.game_x, prof.game_y, prof.game_w, prof.game_h,
                        KOBOY_REFRESH_FULL);
            /* Charged: a cleanup is by definition a WHOLE-rect update, so it
               costs the full settle, and not charging it would let the next
               presented frame land on top of the most expensive update in the
               loop. It OVERWRITES the presented frame's charge rather than
               adding: the panel does one thing at a time. */
            pacer_presented(&pace, last_cleanup_us,
                            pacer_settle_us((uint32_t)cfg.settle_base_ms * 1000u,
                                            (uint32_t)cfg.settle_full_ms * 1000u,
                                            1, 1));
        }

sram_check:
        /* Periodic flush: e-readers get suspended and killed unceremoniously,
           and sram_save is atomic so a kill mid-write is safe. */
        if (sb.mem && sb.len && sb.writeback) {
            uint64_t now = pf->now_us(pf->ctx);
            if (now - last_sram_us > 10ull * 1000000ull) {
                sram_save(sb.path, sb.mem, sb.len);
                last_sram_us = now;
            }
        }

        /* Take the plaque away again. PAST the sram_check label because that
           is the one point EVERY path through the loop body reaches: the early
           `goto sram_check` exits must not leave a confirmation stuck on the
           panel for the rest of the session. Wall clock, not a frame count --
           a static screen presents almost nothing and would keep it up for
           minutes.

           The erase re-renders the whole faceplate into `panel` and blits back
           ONLY the plaque's rectangle: exact by construction, and the panel
           update stays plaque-sized. */
        if (shot_note_until_us && pf->now_us(pf->ctx) >= shot_note_until_us) {
            shot_note_until_us = 0;
            memset(panel, 0xFF, (size_t)panel_stride * (size_t)ph);
            chrome_render(panel, panel_stride, &prof, &cfg.layout);
            chrome_render_battery(panel, panel_stride, &prof, &cfg.layout,
                                  pf->battery_percent ? pf->battery_percent(pf->ctx) : -1);
            pf->blit_gray8(pf->ctx,
                           panel + (size_t)shot_note.y * panel_stride + shot_note.x,
                           shot_note.w, shot_note.h, panel_stride,
                           shot_note.x, shot_note.y);
            pf->refresh(pf->ctx, shot_note.x, shot_note.y,
                        shot_note.w, shot_note.h, KOBOY_REFRESH_FULL);
            pacer_presented(&pace, pf->now_us(pf->ctx),
                            pacer_settle_us((uint32_t)cfg.settle_base_ms * 1000u,
                                            (uint32_t)cfg.settle_full_ms * 1000u,
                                            (long)shot_note.w * shot_note.h,
                                            (long)prof.game_w * (long)prof.game_h));
        }
    }

    /* ------------------------------------------------------ session teardown
       EVERY way out of a session arrives here -- QUIT, a signal, should_quit,
       the frame limit, MENU -> CHOOSE ROM -- so there is one copy of this
       ordering. THE ORDER IS LOAD-BEARING:

         - The SRAM flush comes BEFORE core_close: retro_unload_game takes the
           buffer with it, so flushing after writes freed memory to the user's
           save file, or crashes.
         - sb is cleared right after, so a session that ended without ever
           loading cannot leave a dangling pointer for the NEXT trip's flush.
         - video and input are built from `prof`, which the next session
           re-resolves. Reusing them across a system switch is how a Mega Drive
           frame lands in a Game-Boy-sized buffer. */
    if (sb.mem && sb.len && sb.writeback) sram_save(sb.path, sb.mem, sb.len);
    core_close(core);
    core = NULL;
    sb.mem = NULL; sb.len = 0; sb.writeback = false;
    video_destroy(vid);
    input_destroy(in);
    frames_done += pace.frames;
    settle_held += (unsigned long)pace.held;

    if (mode != MODE_MAIN) break;
    }   /* end of the session loop */

session_end:
    /* `settle-held` is the run's only evidence that area pacing did anything:
       a build where the hold never binds and one without the hold at all print
       the same presented= count and differ only here. Running total across
       every session, like `presented`. */
    say("koboy: %s, %lu presented frames, %lu settle-held, %lu game-rect cleanups, "
        "%lu large-area full refreshes, %lu rects emitted\n",
        koboy_stop ? "stopped by signal" : "stopped", presented,
        settle_held, cleanups, big_refreshes, rects_emitted);
    /* Always printed, even under --quiet: this is the run's evidence, and a
       run whose numbers were suppressed has to be done again. */
    {
        char line[256];
        stats_format(&stats, line, sizeof line);
        fprintf(stderr, "koboy: stages %s\n", line);
    }
    /* Always printed, even under --quiet: the smoke tests grep for it.
       --quiet suppresses other chatter only. */
    printf("presented=%lu\n", presented);
    fflush(stdout);
    if (selftest) {
#ifdef KOBOY_PLATFORM_KOBO
        platform_kobo_refresh_stats(pf);
#endif
    }

    /* The core, video and input were released by the session teardown above:
       every path here goes through it, or never had them. */
    free(panel);
    pf->shutdown(pf->ctx);
    free(pf);
    return 0;
}
