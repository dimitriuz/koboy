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
#include "romlist.h"
#include "safefile.h"
#include "sram.h"
#include "state.h"
#include "stats.h"
#include "text.h"
#include "ui.h"
#include "uiscript.h"
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
#include "platform_kobo.h"
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
        "  --rom-dir PATH    directory the ROM browser lists\n"
        "  --ui-script PATH  replay synthetic UI input (scripted runs)\n",
        argv0, DEFAULT_INI);
}

typedef enum { MODE_BROWSE, MODE_PLAY, MODE_MENU, MODE_QUIT } koboy_mode;

/* One definition of "put the faceplate back", replacing three hand-copied
   blocks (post-calibration, post-fatal, post-SRAM-warning) and now used by
   every exit from a UI mode as well. */
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

/* Drives one list widget to a selection. Returns the chosen index, or -1 if
   the user quit, the run was stopped, or a script ran out.

   `script`/`script_n` make MODE_BROWSE and MODE_MENU reachable in a bounded
   unattended run. Without them every automated test would pass --rom and skip
   these screens entirely -- the same blind spot that hid v1's first-run
   deadlock through twenty reviews. */
static int run_list(koboy_platform *pf, koboy_input *in, koboy_ui_list *u,
                    uint8_t *panel, int stride, int pw, int ph,
                    const koboy_input_state *script, int script_n)
{
    int  chosen = -1;
    int  si = 0;
    bool need_draw = true;

    while (!g_stop && !pf->should_quit(pf->ctx)) {
        if (need_draw) {
            need_draw = false;
            memset(panel, 0xFF, (size_t)stride * (size_t)ph);
            ui_list_render(u, panel, stride, pw, ph);
            pf->blit_gray8(pf->ctx, panel, pw, ph, stride, 0, 0);
            /* FULL, i.e. GC16: a list is about to sit still, and the game
               rect's four-level ceiling does not apply to it. */
            pf->refresh(pf->ctx, 0, 0, pw, ph, KOBOY_REFRESH_FULL);
        }

        const koboy_input_state *st;
        if (script) {
            if (si >= script_n) break;      /* script exhausted: give up */
            st = &script[si++];
        } else {
            pf->poll_input(pf->ctx, in);
            st = input_state(in);
        }

        int idx = -1;
        ui_action a = ui_list_feed(u, st, &idx);
        if (a == UI_SELECT) { chosen = idx; break; }
        if (a == UI_PAGE_NEXT || a == UI_PAGE_PREV) need_draw = true;

        if (!script) usleep(5000);
    }
    return chosen;
}

/* Everything that must happen when a ROM becomes the current game, in the one
   order that is safe.

   Three hazards live here, all of them silent if got wrong:
     - core_sram() is re-fetched every time. The pointer belongs to the core's
       freshly loaded cartridge; caching it across unload/load is a
       use-after-free waiting for a second game.
     - The OUTGOING game's SRAM is flushed by the caller BEFORE unload, never
       after: retro_unload_game takes the buffer, and its last minutes with it.
     - sram_writeback stays false for the session when a save file exists but
       could not be read whole, so nothing is written back over it. */
typedef struct {
    char     path[512];        /* .srm path for the current rom */
    uint8_t *mem;
    size_t   len;
    bool     writeback;
} koboy_sram_binding;

static bool load_rom_into(koboy_core *core, koboy_config *cfg,
                          koboy_sram_binding *sb, char *err, size_t errlen)
{
    if (!core_load_rom(core, cfg->rom_path, err, errlen)) return false;

    sram_path_for_rom(sb->path, sizeof sb->path, cfg->save_dir, cfg->rom_path);
    sb->len = 0;
    sb->mem = core_sram(core, &sb->len);
    sb->writeback = true;
    return true;
}

enum {
    MENU_SAVE = 0, MENU_LOAD, MENU_RESET, MENU_CHOOSE_ROM, MENU_RESUME, MENU_QUIT,
    MENU_COUNT
};

/* Returns the chosen MENU_* action, or MENU_RESUME if the user backed out.
   `has_states` greys nothing out visually -- the label says so instead, which
   is cheaper on a panel with no colour and no hover. */
static int run_menu(koboy_platform *pf, koboy_input *in, uint8_t *panel,
                    int stride, int pw, int ph, bool has_states,
                    const koboy_input_state *script, int script_n)
{
    const char *items[MENU_COUNT];
    items[MENU_SAVE]        = has_states ? "SAVE STATE" : "SAVE STATE (UNSUPPORTED)";
    items[MENU_LOAD]        = has_states ? "LOAD STATE" : "LOAD STATE (UNSUPPORTED)";
    items[MENU_RESET]       = "RESET GAME";
    items[MENU_CHOOSE_ROM]  = "CHOOSE ROM";
    items[MENU_RESUME]      = "RESUME";
    items[MENU_QUIT]        = "QUIT";

    koboy_ui_list list;
    ui_list_init(&list, "MENU", items, MENU_COUNT,
                 KOBOY_CHROME_MARGIN, KOBOY_CHROME_MARGIN,
                 pw - 2 * KOBOY_CHROME_MARGIN, ph - 2 * KOBOY_CHROME_MARGIN);

    int pick = run_list(pf, in, &list, panel, stride, pw, ph, script, script_n);
    if (pick < 0) return MENU_RESUME;
    if ((pick == MENU_SAVE || pick == MENU_LOAD) && !has_states) return MENU_RESUME;
    return pick;
}

/* Returns the chosen slot (1-based), or 0 if the user backed out. */
static int run_slot_picker(koboy_platform *pf, koboy_input *in, uint8_t *panel,
                           int stride, int pw, int ph, const char *title,
                           const char *save_dir, const char *rom_path,
                           const koboy_input_state *script, int script_n)
{
    static char labels[KOBOY_STATE_SLOTS + 1][64];
    const char *items[KOBOY_STATE_SLOTS + 1];
    for (int s = 1; s <= KOBOY_STATE_SLOTS; s++) {
        state_slot_label(labels[s - 1], sizeof labels[s - 1], save_dir, rom_path, s);
        items[s - 1] = labels[s - 1];
    }
    snprintf(labels[KOBOY_STATE_SLOTS], sizeof labels[KOBOY_STATE_SLOTS], "BACK");
    items[KOBOY_STATE_SLOTS] = labels[KOBOY_STATE_SLOTS];

    koboy_ui_list list;
    ui_list_init(&list, title, items, KOBOY_STATE_SLOTS + 1,
                 KOBOY_CHROME_MARGIN, KOBOY_CHROME_MARGIN,
                 pw - 2 * KOBOY_CHROME_MARGIN, ph - 2 * KOBOY_CHROME_MARGIN);

    int pick = run_list(pf, in, &list, panel, stride, pw, ph, script, script_n);
    if (pick < 0 || pick >= KOBOY_STATE_SLOTS) return 0;
    return pick + 1;
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
        else if (!strcmp(a, "--core"))     snprintf(cfg.core_path, sizeof cfg.core_path, "%s", argv[++i]);
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

    static koboy_input_state ui_script[UISCRIPT_MAX];
    int ui_script_n = 0;
    if (ui_script_path) {
        ui_script_n = uiscript_load(ui_script_path, ui_script, UISCRIPT_MAX);
        if (ui_script_n < 0) {
            fatal("cannot read ui script %s", ui_script_path);
            pf->shutdown(pf->ctx);
            return 2;
        }
        /* uiscript.h's own contract: "an error must fail the run rather than
           silently pass a test that exercised nothing". An empty or
           comment-only script is not a read error -- uiscript_load returns 0,
           not -1 -- but treating it as "no script" here would fall back to
           run_list's live-polling branch, and a --ui-script was explicitly
           requested precisely because nobody is at the panel to poll. That
           run then blocks forever instead of failing, which is a worse
           silence than a bad read: this is exactly the unattended-run blind
           spot uiscript.h exists to close. */
        if (ui_script_n == 0) {
            fatal("ui script %s is empty (no verbs)", ui_script_path);
            pf->shutdown(pf->ctx);
            return 2;
        }
    }

    /* An explicit --rom or rom= goes straight to play, which keeps every
       existing smoke test, --frames run and scripted path behaving exactly as
       it did in v1. The shipped ini leaves rom commented out, so a real user
       starts in the browser. */
    koboy_mode mode = cfg.rom_path[0] ? MODE_PLAY : MODE_BROWSE;

    /* Genuinely unused past this point. It was carried here as groundwork for
       MODE_MENU's CHOOSE ROM entry, on the theory that a ROM picked from
       --rom (rather than the browser or the ini) might want its containing
       directory offered as the CHOOSE ROM listing. That would mean silently
       overriding cfg.rom_dir whenever it still held the compiled-in default,
       which is a real feature with its own failure modes (what if rom_dir was
       explicitly set to the same value on purpose?) and no request for it
       exists -- so it is left alone rather than guessed at. `mode` is the
       piece of this task's groundwork that actually gets consumed, below. */
    (void)rom_from_argv;

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
    /* Free, because the panel is already being repainted. */
    chrome_render_battery(panel, panel_stride, &prof, &cfg.layout,
                          pf->battery_percent ? pf->battery_percent(pf->ctx) : -1);
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
                    text_draw_centred(panel, panel_stride, pw, ph, ph / 2 - 40,
                                 calib_prompt(&k), 5, 0x00);
                    /* The escape has to be ON THE PANEL. A device that cannot
                       answer the prompt is exactly the device whose user has no
                       terminal and no other way to find out. */
                    text_draw_centred(panel, panel_stride, pw, ph, ph / 2 + 40,
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
            redraw_chrome(pf, panel, panel_stride, pw, ph, &prof, &cfg.layout);
        }
    }

    koboy_romlist roms;
    if (mode == MODE_BROWSE) {
        int n = romlist_scan(&roms, cfg.rom_dir);
        if (n < 0) {
            /* Distinct from "no roms": a wrong rom_dir and an empty one are
               different mistakes, and this is the only diagnostic a user with
               no terminal gets. */
            fatal("cannot read rom directory\n%s", cfg.rom_dir);
            free(panel); pf->shutdown(pf->ctx); return 2;
        }
        if (n == 0) {
            fatal("no .gb or .gbc files in\n%s", cfg.rom_dir);
            free(panel); pf->shutdown(pf->ctx); return 2;
        }

        koboy_input *ui_in = input_create(&cfg, &prof);
        if (!ui_in) { fatal("out of memory"); free(panel); pf->shutdown(pf->ctx); return 1; }
#ifdef KOBOY_PLATFORM_KOBO
        platform_kobo_setup_touch(pf, ui_in);
#else
        input_set_touch_transform(ui_in, pw, ph, false, false, false);
#endif
        koboy_ui_list list;
        ui_list_init(&list, "CHOOSE A GAME", romlist_items(&roms), n,
                     KOBOY_CHROME_MARGIN, KOBOY_CHROME_MARGIN,
                     pw - 2 * KOBOY_CHROME_MARGIN, ph - 2 * KOBOY_CHROME_MARGIN);

        int pick = run_list(pf, ui_in, &list, panel, panel_stride, pw, ph,
                            ui_script_n > 0 ? ui_script : NULL, ui_script_n);
        input_destroy(ui_in);

        if (pick < 0) {
            say("koboy: no rom chosen, exiting\n");
            free(panel); pf->shutdown(pf->ctx); return 0;
        }
        romlist_path(&roms, pick, cfg.rom_path, sizeof cfg.rom_path);
        say("koboy: chose %s\n", cfg.rom_path);
        mode = MODE_PLAY;

        /* The browser painted over the faceplate. */
        redraw_chrome(pf, panel, panel_stride, pw, ph, &prof, &cfg.layout);
    }
    /* mode is MODE_PLAY from here on, until the emulator loop below reads and
       writes it: MODE_QUIT ends the loop from inside the menu (a chosen QUIT,
       or CHOOSE ROM leaving nothing loaded), kept distinct from g_stop so the
       final status line does not call a menu-driven quit "stopped by
       signal". */

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

    koboy_sram_binding sb = {0};
    if (!load_rom_into(core, &cfg, &sb, err, sizeof err)) {
        fatal("%s", err);
        core_close(core);
        video_destroy(vid); input_destroy(in); free(panel);
        pf->shutdown(pf->ctx); return 1;
    }

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
    if (sb.mem && sb.len) {
        if (sram_load(sb.path, sb.mem, sb.len)) {
            say("koboy: loaded %s\n", sb.path);
        } else if (access(sb.path, F_OK) == 0) {
            sb.writeback = false;
            say("koboy: %s could not be read whole; SRAM left as the core "
                "initialised it and saving is disabled this session\n", sb.path);
            /* On the panel, not just the log: a save that silently did not load
               is how a user loses hours without ever being told. Short lines --
               FBInk wraps at the column edge, not at word boundaries. */
            fatal("Save file unreadable.\nStarting fresh.\nSaving is OFF this run.");
            /* fatal() drew over the faceplate; put it back. */
            redraw_chrome(pf, panel, panel_stride, pw, ph, &prof, &cfg.layout);
        }
    } else {
        say("koboy: cartridge has no save RAM\n");
    }

    /* ------------------------------------------------------------- the loop */
    koboy_pacer pace;
    pacer_init(&pace, pf->now_us(pf->ctx), cfg.present_divisor);

    koboy_stats stats;
    stats_reset(&stats);

    unsigned long presented = 0, since_cleanup = 0, cleanups = 0, big_refreshes = 0;
    unsigned long rects_emitted = 0;
    uint64_t last_sram_us = pf->now_us(pf->ctx);
    uint64_t last_cleanup_us = last_sram_us;

    /* mode != MODE_QUIT joins g_stop and should_quit() as a third way out: the
       in-game menu sets it (QUIT, or CHOOSE ROM leaving nothing loaded) from
       inside the loop body below. Kept separate from g_stop on purpose -- that
       flag is the signal handler's, set from outside any call frame, and
       reusing it for a menu-driven exit would make the final status line call
       a chosen QUIT "stopped by signal". */
    while (mode != MODE_QUIT && !g_stop && !pf->should_quit(pf->ctx)) {
        if (frame_limit && pace.frames >= frame_limit) break;

        /* Poll EVERY core iteration (60Hz), not once per presented frame.
           Polling only on presentation would drop short presses and add up to
           50ms of latency on top of the panel's own. */
        pf->poll_input(pf->ctx, in);

        if (input_take_menu_request(in)) {
            size_t ssz = core_state_size(core);
            int act = run_menu(pf, in, panel, panel_stride, pw, ph, ssz > 0, NULL, 0);

            if (act == MENU_SAVE || act == MENU_LOAD) {
                int slot = run_slot_picker(pf, in, panel, panel_stride, pw, ph,
                                           act == MENU_SAVE ? "SAVE TO" : "LOAD FROM",
                                           cfg.save_dir, cfg.rom_path, NULL, 0);
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
                           untouched on a short file, and only a complete blob
                           ever reaches the running core. */
                        if (safefile_read_exact(sp, blob, ssz) &&
                            core_state_load(core, blob, ssz)) {
                            say("koboy: loaded state %d\n", slot);
                            /* The core's cartridge RAM was just rewritten --
                               gambatte's blob includes it -- so the periodic
                               flush will now write that to .srm. Correct, and
                               worth knowing: a state load is indirectly a
                               save-file write. Re-fetch in case the pointer
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
            } else if (act == MENU_QUIT) {
                mode = MODE_QUIT;
            } else if (act == MENU_CHOOSE_ROM) {
                /* Flush BEFORE unload: retro_unload_game takes the buffer. */
                if (sb.mem && sb.len && sb.writeback)
                    sram_save(sb.path, sb.mem, sb.len);
                core_unload_rom(core);
                /* Cleared, not left stale: retro_unload_game takes the buffer,
                   so sb.mem is dangling until a new load re-fetches it. If the
                   picker below is cancelled or the next load fails, mode goes
                   to MODE_QUIT and this run's final flush (after the loop)
                   must see mem==NULL rather than dereference freed memory. */
                sb.mem = NULL;
                sb.len = 0;

                koboy_romlist rl;
                int n = romlist_scan(&rl, cfg.rom_dir);
                int pick = -1;
                if (n < 0) {
                    /* Distinct from n == 0, matching the startup browser's two
                       messages: romlist.h documents why they must stay
                       distinguishable -- "your rom_dir is wrong" and "you
                       have no ROMs" are different diagnoses to a user with no
                       terminal, and this is the only diagnostic they get. */
                    fatal("cannot read rom directory\n%s", cfg.rom_dir);
                } else if (n == 0) {
                    fatal("no .gb or .gbc files in\n%s", cfg.rom_dir);
                } else {
                    koboy_ui_list list;
                    ui_list_init(&list, "CHOOSE A GAME", romlist_items(&rl), n,
                                 KOBOY_CHROME_MARGIN, KOBOY_CHROME_MARGIN,
                                 pw - 2 * KOBOY_CHROME_MARGIN,
                                 ph - 2 * KOBOY_CHROME_MARGIN);
                    pick = run_list(pf, in, &list, panel, panel_stride,
                                    pw, ph, NULL, 0);
                }
                /* No "return to the game" option in any of the three failure
                   cases (n < 0, n == 0, or pick < 0): the running game was
                   already flushed and unloaded above so CHOOSE ROM could have
                   the core to itself, and by the time rom_dir turns out empty
                   or unreadable -- or the user backs out of the picker --
                   there is nothing left to resume. Quitting, after telling
                   the user why on the panel, is the only coherent option: the
                   alternative is a black or frozen screen with no
                   explanation, which this project's own constraint calls
                   "indistinguishable from a crash". */
                if (pick < 0) { mode = MODE_QUIT; }
                else {
                    romlist_path(&rl, pick, cfg.rom_path, sizeof cfg.rom_path);
                    char lerr[512];
                    if (!load_rom_into(core, &cfg, &sb, lerr, sizeof lerr)) {
                        fatal("%s", lerr);
                        mode = MODE_QUIT;
                    } else if (sb.mem && sb.len &&
                               !sram_load(sb.path, sb.mem, sb.len) &&
                               access(sb.path, F_OK) == 0) {
                        sb.writeback = false;
                        fatal("Save file unreadable.\nStarting fresh.\n"
                              "Saving is OFF this run.");
                    }
                }
            }

            /* Whatever happened, the panel is now showing a menu. */
            redraw_chrome(pf, panel, panel_stride, pw, ph, &prof, &cfg.layout);
            video_invalidate(vid);
            /* Drain, don't just ignore: every run_list/run_menu call above
               polled input while a menu screen -- not the faceplate -- was on
               the panel, and recompute() latches a MENU-zone tap regardless
               of what is drawn there. At the default layout the zone
               overlaps the list's page-forward arrow, so a tap made to page
               a long list can latch a pending request that nothing here has
               consumed yet. Left alone, the very next iteration's
               input_take_menu_request() would see it and reopen the menu
               immediately -- "the menu keeps reopening by itself". The
               return value is discarded on purpose: this call exists only to
               clear the latch, not to act on it. */
            (void)input_take_menu_request(in);
            pacer_init(&pace, pf->now_us(pf->ctx), cfg.present_divisor);
            continue;
        }

        uint64_t delay = pacer_delay_us(&pace, pf->now_us(pf->ctx));
        if (delay) usleep((useconds_t)delay);

        g_frame = NULL;
        uint64_t t0 = pf->now_us(pf->ctx);
        core_run_frame(core);
        stats_add(&stats, KOBOY_STAGE_CORE, pf->now_us(pf->ctx) - t0);
        bool present = pacer_tick(&pace);
        if (!present) goto sram_check;

        /* A NULL g_frame is the core's can-dupe signal, which video_submit_rects
           turns into zero rects -- so an unchanged frame costs no refresh at
           all. Up to KOBOY_MAX_RECTS rects instead of one merged box:
           video_split_dirty (src/video.c) only splits when the summed cost of
           the pieces beats the merged box's, so a full-screen scroller still
           comes back as a single rect here. The rects are not guaranteed
           disjoint (video_split_dirty's own comment has the detail) -- a
           capped merge can leave one rect containing another -- so blitting
           and refreshing each in turn can redo a small overlap; it never
           misses one. */
        koboy_rect rects[KOBOY_MAX_RECTS];
        t0 = pf->now_us(pf->ctx);
        int nrects = video_submit_rects(vid, g_frame, (int)g_fw, (int)g_fh,
                                        g_fpitch, core_pixfmt(core),
                                        cfg.refresh_fixed_tiles,
                                        rects, KOBOY_MAX_RECTS);
        stats_add(&stats, KOBOY_STAGE_SUBMIT, pf->now_us(pf->ctx) - t0);
        if (nrects == 0) goto sram_check;       /* nothing changed: skip the panel */

        /* Waveform by TOTAL dirty area across every emitted rect, not one
           waveform for every frame. KOBOY_REFRESH_FAST maps to a non-flashing
           waveform (DU4 on this panel), which never fully resets pixel state
           -- residue accumulates on every update regardless of rect size.
           Observed on the device as several Tetris scenes layered on top of
           each other.
           A dirty area covering most of the game rect means the scene has
           substantially changed, which is both when layered residue is most
           objectionable and when the refresh is already expensive, so paying
           for a flashing waveform there is cheap in relative terms. Small
           incremental updates keep the fast waveform, and the periodic cleanup
           sweeps whatever they leave behind.
           Note this cannot be a substitute for the cleanup: the dirty diff
           compares our own output buffers, so it tracks what we sent, not what
           the panel shows. A region that ghosts and then stops changing is
           never revisited by this test at all.
           Summing before deciding, rather than promoting per rect, matters
           precisely because splitting exists now: two small pieces that
           individually look nowhere near the promotion threshold could
           together represent most of the game rect having changed, and
           deciding rect-by-rect would miss exactly the scene change this
           promotion exists to catch. */
        long dirty_px = 0;
        for (int i = 0; i < nrects; i++) dirty_px += (long)rects[i].w * rects[i].h;

        /* Named wfm, not mode: koboy_mode mode is the outer loop's live
           control variable (MODE_PLAY/MODE_QUIT), declared far above this
           point. Reusing "mode" here for the waveform shadowed it silently --
           -Wshadow is not in this project's flags, so nothing caught it, and
           the code was correct only by accident of line ordering: moving this
           block above the outer mode's use, or adding a `mode = MODE_QUIT`
           anywhere after this point in the loop, would have assigned a
           waveform instead of ending the run. */
        koboy_refresh_mode wfm = KOBOY_REFRESH_FAST;
        if (config_promote_full(&cfg, dirty_px,
                                (long)prof.game_w * (long)prof.game_h)) {
            wfm = KOBOY_REFRESH_FULL;
            big_refreshes++;
        }

        /* Accumulate BLIT and REFRESH across every rect of THIS frame and
           call stats_add once, not once per rect. CORE and SUBMIT still fire
           once per presented frame, and stats_mean_us divides by count[stage]
           -- so a per-rect stats_add would silently change what the mean
           MEANS, from "cost per presented frame" (CORE, SUBMIT, and every
           reading before Task 13) to "cost per rect," with nothing in the
           printed line saying so. That would bias Step 10's on-device tuning
           run toward splitting by construction: quartering into four rects
           mechanically quarters a per-rect mean even if the total refresh
           time went up, which is the opposite of what the tuning run is
           trying to measure. One stats_add per frame keeps all four stages'
           means on the same "per presented frame" footing. */
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
        presented++;
        rects_emitted += (unsigned long)nrects;

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
        if (sb.mem && sb.len && sb.writeback) {
            uint64_t now = pf->now_us(pf->ctx);
            if (now - last_sram_us > 10ull * 1000000ull) {
                sram_save(sb.path, sb.mem, sb.len);
                last_sram_us = now;
            }
        }
    }

    if (sb.mem && sb.len && sb.writeback) sram_save(sb.path, sb.mem, sb.len);
    say("koboy: %s, %lu presented frames, %lu game-rect cleanups, "
        "%lu large-area full refreshes, %lu rects emitted\n",
        g_stop ? "stopped by signal" : "stopped", presented, cleanups,
        big_refreshes, rects_emitted);
    /* Always printed, even under --quiet, for the same reason presented= is:
       this is the run's evidence, and a run whose numbers were suppressed is a
       run that has to be done again. */
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

    core_close(core);
    video_destroy(vid);
    input_destroy(in);
    free(panel);
    pf->shutdown(pf->ctx);
    free(pf);
    return 0;
}
