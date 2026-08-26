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
#include "recent.h"
#include "romlist.h"
#include "safefile.h"
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

/* Named in the log because "why are the controls gone?" is otherwise
   unanswerable on a device with no terminal: the layout is chosen from the
   ROM's extension, so a mis-named file is a plausible cause of a faceplate
   nobody expected. */
static const char *layout_name(int mode)
{
    return mode == KOBOY_LAYOUT_LCD ? "LCD" : "DMG";
}

/* The other half of the input surface, for a core that reads a pointer rather
   than (only) buttons -- see core_set_pointer_fn. input.c fills this in
   whenever the LCD layout is live and leaves pressed false otherwise, so
   installing it unconditionally costs a Game Boy session three stores per
   frame and changes nothing it sees. */
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
        "  --rom-dir PATH    directory the ALL GAMES list scans\n"
        "  --ui-script PATH  replay synthetic UI input into the startup flow\n"
        "                    (MAIN MENU, then RECENT or ALL GAMES);\n"
        "                    exits 4 if the script selects nothing\n",
        argv0, DEFAULT_INI);
}

/* MODE_BROWSE is gone: the startup file browser is now reached only via
   MODE_MAIN -> ALL GAMES (run_list, inline in main()), not a mode of its
   own -- see task 5's MAIN MENU. */
typedef enum { MODE_MAIN, MODE_PLAY, MODE_MENU, MODE_QUIT } koboy_mode;

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

   `script`/`script_n` make the startup flow reachable in a bounded unattended
   run. Without them every automated test would pass --rom and skip every
   list screen entirely -- the same blind spot that hid v1's first-run
   deadlock through twenty reviews. MODE_MENU is NOT scripted: nothing passes
   a script to run_menu or run_slot_picker (they are only ever reached from
   the emulator loop, which has no --ui-script hook), and saying otherwise
   here would overclaim coverage the suite does not have.

   `script_i`, when not NULL, is a CURSOR shared across every screen one
   --ui-script run drives (MAIN MENU, then RECENT or ALL GAMES) -- a pointer
   rather than a local index so a script written as one flat sequence of taps
   can walk through several run_list calls in a row, each screen picking up
   exactly where the previous one's last consumed state left off. Every call
   still primes with one synthetic released state regardless of the cursor's
   position (see the `primed` logic below): each fresh koboy_ui_list demands
   its own release before its first tap, independent of what the PREVIOUS
   screen's script tap left the finger doing. Callers that never script
   (run_menu, run_slot_picker) pass NULL here, same as they pass NULL for
   `script`.

   `disabled_index`, when not -1, is a row that SELECTS nothing: the ROM
   browser's synthetic "+N MORE ROMS NOT SHOWN" row uses this so a tap on it
   cannot be handed to romlist_path as if it were a real ROM (which would try
   to load a file that does not exist). The loop just keeps polling instead
   of breaking, the same as any other no-op input. */
static int run_list(koboy_platform *pf, koboy_input *in, koboy_ui_list *u,
                    uint8_t *panel, int stride, int pw, int ph,
                    const koboy_input_state *script, int *script_i, int script_n,
                    int disabled_index)
{
    int  chosen = -1;
    int  si = script_i ? *script_i : 0;
    bool need_draw = true;
    bool primed = false;

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
        koboy_input_state synth;
        if (script) {
            /* One RELEASED state before the script's first entry, always.
               ui_list_init sets prev_touch = true (a fresh list demands a
               release before it accepts a tap, so a still-down finger cannot
               carry a selection in from the previous screen), so a script
               whose first verb is `tap` had its press swallowed and its
               release consumed as the priming edge -- selecting nothing and
               exiting 0, i.e. a green CI run that tested nothing. Confirmed
               on hardware with `printf 'tap 300 300\n'`.
               Primed here rather than documented in uiscript.h: a note relies
               on every future author reading it, and the scripted path is
               precisely the one nobody's tests exercise honestly. */
            if (!primed) {
                primed = true;
                memset(&synth, 0, sizeof synth);
                st = &synth;
            } else if (si >= script_n) {
                break;                      /* script exhausted: give up */
            } else {
                st = &script[si++];
            }
        } else {
            pf->poll_input(pf->ctx, in);
            /* NOT input_state(): the faceplate's A/B touch zones stay live
               under a full-panel list, and their synthesised joypad bits are
               eaten by ui_list_feed as page-turns before any row hit-test
               runs. input_ui_state passes the hardware keys and the touch
               coordinates and drops the synthesised bits -- see input.h. */
            input_ui_state(in, &synth);
            st = &synth;
        }

        int idx = -1;
        ui_action a = ui_list_feed(u, st, &idx);
        if (a == UI_SELECT && idx != disabled_index) { chosen = idx; break; }
        if (a == UI_PAGE_NEXT || a == UI_PAGE_PREV || a == UI_JUMP) need_draw = true;

        if (!script) usleep(5000);
    }
    if (script_i) *script_i = si;
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

    /* NULL script_i: run_menu is never scripted (see run_list's own comment
       on why -- no --ui-script hook reaches here), so there is no cursor to
       share across screens. */
    int pick = run_list(pf, in, &list, panel, stride, pw, ph, script, NULL, script_n, -1);
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

    int pick = run_list(pf, in, &list, panel, stride, pw, ph, script, NULL, script_n, -1);
    if (pick < 0 || pick >= KOBOY_STATE_SLOTS) return 0;
    return pick + 1;
}

enum { MAIN_RECENT = 0, MAIN_ALL_GAMES, MAIN_QUIT, MAIN_COUNT };

/* Returns the chosen MAIN_* action, or -1 if the run was stopped (signal, or
   should_quit()) or a script ran out before choosing anything. Unlike
   run_menu/run_slot_picker, THIS screen IS scripted -- it is the new first
   screen of the startup flow, in front of both the ROM browser and the
   RECENT picker, so a --ui-script run has to navigate it to reach either. */
static int run_main_menu(koboy_platform *pf, koboy_input *in, uint8_t *panel,
                         int stride, int pw, int ph,
                         const koboy_input_state *script, int *script_i, int script_n)
{
    static const char *const items[MAIN_COUNT] = { "RECENT", "ALL GAMES", "QUIT" };

    koboy_ui_list list;
    ui_list_init(&list, "KOBOY", items, MAIN_COUNT,
                 KOBOY_CHROME_MARGIN, KOBOY_CHROME_MARGIN,
                 pw - 2 * KOBOY_CHROME_MARGIN, ph - 2 * KOBOY_CHROME_MARGIN);

    return run_list(pf, in, &list, panel, stride, pw, ph, script, script_i, script_n, -1);
}

/* Returns the chosen index into `rc` (0-based), or -1 if the user backed out
   by tapping BACK.

   BACK is a real, always-present trailing row -- the same device
   run_slot_picker uses -- rather than "no cancel gesture" the way the
   top-level MAIN MENU and the ROM browser both get away with (their only
   ways out are picking something or the whole app quitting, which is fine
   because THEY are reachable only by deliberate user choice already). A
   RECENT list can be genuinely empty on a first run or right after clearing
   history, and staring at a screen with nothing to tap and no way back is a
   worse first experience than one more row. When `rc` is empty, a single
   disabled placeholder row explains why, using run_list's disabled_index the
   same way the ROM browser's "+N MORE ROMS" overflow row does. */
static int run_recent_picker(koboy_platform *pf, koboy_input *in, uint8_t *panel,
                             int stride, int pw, int ph, const koboy_recent *rc,
                             const koboy_input_state *script, int *script_i, int script_n)
{
    enum { RECENT_UI_MAX = KOBOY_RECENT_MAX + 2 };   /* entries + placeholder + BACK */
    static char labels[RECENT_UI_MAX][KOBOY_RECENT_DISPLAY];
    const char *items[RECENT_UI_MAX];
    int n = 0, placeholder = -1;

    if (rc->count == 0) {
        snprintf(labels[n], sizeof labels[n], "NO RECENT GAMES YET");
        items[n] = labels[n];
        placeholder = n;
        n++;
    } else {
        for (int i = 0; i < rc->count; i++) {
            snprintf(labels[n], sizeof labels[n], "%s", recent_display(rc, i));
            items[n] = labels[n];
            n++;
        }
    }
    snprintf(labels[n], sizeof labels[n], "BACK");
    items[n] = labels[n];
    int back_index = n;
    n++;

    koboy_ui_list list;
    ui_list_init(&list, "RECENT", items, n,
                 KOBOY_CHROME_MARGIN, KOBOY_CHROME_MARGIN,
                 pw - 2 * KOBOY_CHROME_MARGIN, ph - 2 * KOBOY_CHROME_MARGIN);

    int pick = run_list(pf, in, &list, panel, stride, pw, ph, script, script_i, script_n,
                        placeholder);
    if (pick < 0 || pick == back_index) return -1;
    return pick;    /* placeholder, when present, is index 0 and never reached here */
}

/* ------------------------------------------------------------- the browser
   One directory at a time, not the whole tree flattened. The flatten was
   fine for a hundred ROMs in one folder and unreadable for a real collection:
   59 Game & Watch titles under roms/Game and Watch/ produced 59 rows whose
   first 15 characters were identical, and ui_fit_label's middle ellipsis then
   spent the row's width on that shared prefix and ate the actual title.

   The header says where you are, so a folder you descended into is not a
   mystery -- ui_path_title builds it (and owns the truncation rule), because
   how much a title row can carry is the list widget's fact, not the
   browser's. "ALL GAMES" rather than the old "CHOOSE A GAME" so the header
   names the MAIN MENU row that got you here, and the breadcrumb below it
   reads as a path. */
#define BROWSER_TITLE_HEAD "ALL GAMES"

/* Drives the ROM browser until the user picks a ROM, backs out of the root, or
   the run ends. Returns a BROWSE_*; on BROWSE_PICKED it writes the ROM's full
   path (out_path) and the row text the RECENT list should display (out_name).

   Both entry points -- startup ALL GAMES and the in-game MENU's CHOOSE ROM --
   call this. They used to carry a hand-copied browser each, which was already
   two copies of the scan/geometry/alpha-strip setup before navigation added a
   loop to each of them. */
enum { BROWSE_PICKED = 0, BROWSE_NONE, BROWSE_ERR_DIR, BROWSE_ERR_EMPTY };

static int run_browser(koboy_platform *pf, koboy_input *in, uint8_t *panel,
                       int stride, int pw, int ph, const char *rom_dir,
                       char *out_path, size_t out_path_n,
                       char *out_name, size_t out_name_n,
                       const koboy_input_state *script, int *script_i,
                       int script_n)
{
    /* memset, not `= {0}`: the Linaro 4.9 cross compiler warns
       -Wmissing-braces on `= {0}` for a struct whose first member is itself
       an array, and this project ships at zero warnings. */
    koboy_romlist rl;
    memset(&rl, 0, sizeof rl);

    int n = romlist_scan(&rl, rom_dir);
    if (n < 0) { romlist_free(&rl); return BROWSE_ERR_DIR; }
    /* rl.count, not n: n also counts the synthetic overflow row when
       rl.hidden > 0, and a rom_dir holding nothing but one oversized-name ROM
       (hidden > 0, count == 0) must still report "no roms" rather than open a
       browser whose only row selects nothing. count rather than rl.roms
       because a root with no loose ROMs but a folder full of them is a
       perfectly good collection -- it just needs one tap first. */
    if (rl.count == 0) { romlist_free(&rl); return BROWSE_ERR_EMPTY; }

    int result = BROWSE_NONE;
    for (;;) {
        char title[UI_TITLE_CHARS + 8];
        ui_path_title(title, sizeof title, BROWSER_TITLE_HEAD,
                      romlist_subpath(&rl));

        /* Rebuilt after every navigation, never reused: romlist's arrays are
           reallocated wholesale by each rescan (see romlist.h), so a
           koboy_ui_list that outlived one would be holding freed pointers. */
        koboy_ui_list list;
        ui_list_init(&list, title, romlist_items(&rl), n,
                     KOBOY_CHROME_MARGIN, KOBOY_CHROME_MARGIN,
                     pw - 2 * KOBOY_CHROME_MARGIN, ph - 2 * KOBOY_CHROME_MARGIN);
        /* Letter index strip: only the ROM browser gets one, never MENU,
           MAIN MENU, RECENT or the slot picker, which are all short
           fixed-ish lists a strip would just clutter. */
        ui_list_enable_alpha_jump(&list, true);

        int pick = run_list(pf, in, &list, panel, stride, pw, ph,
                            script, script_i, script_n,
                            rl.hidden > 0 ? rl.count : -1);
        if (pick < 0) {
            /* Stopped (signal or should_quit) or the script ran out. This
               leaves the BROWSER, from whatever directory it happened to be
               in -- it does not walk back up one level per iteration, which
               would make a Ctrl-C in a nested folder take several passes to
               notice. Backing out of the ROOT is the same thing and therefore
               still behaves exactly as it did before navigation existed; the
               ".." row is what goes up one level. */
            result = BROWSE_NONE;
            break;
        }

        int kind = romlist_kind(&rl, pick);
        if (kind == ROMLIST_ROM) {
            romlist_path(&rl, pick, out_path, out_path_n);
            snprintf(out_name, out_name_n, "%s", romlist_name(&rl, pick));
            result = BROWSE_PICKED;
            break;
        }
        if (kind == ROMLIST_DIR)      n = romlist_enter(&rl, pick);
        else if (kind == ROMLIST_UP)  n = romlist_up(&rl);
        else                          continue;   /* the overflow row: not selectable */

        if (n < 0) {
            /* The directory we navigated to could not be listed at all, and
               romlist has already tried to fall back to where we were. There
               is nothing left to show. */
            result = BROWSE_ERR_DIR;
            break;
        }
    }

    romlist_free(&rl);
    return result;
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
       it did in v1 -- this is task 5's "the --rom fast path must still bypass
       everything" requirement. The shipped ini leaves rom commented out, so a
       real user starts on the MAIN MENU, not the file browser: landing
       someone in a 300-entry list as the very first screen is the wrong
       default now that RECENT exists. */
    koboy_mode mode = cfg.rom_path[0] ? MODE_PLAY : MODE_MAIN;

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

    /* A PLACEHOLDER profile, resolved against the Game Boy's fixed 160x144
       rather than any real core's geometry: no ROM has been chosen yet at
       this point (MODE_MAIN sends the user to a menu first), and a core's
       geometry is only meaningful after retro_load_game -- there is nothing
       else honest to lay chrome/calibration/the menu out against. Once a ROM
       is actually loaded, below, this gets re-resolved against
       core_get_geometry's answer and the faceplate is redrawn if that
       changed anything; for the Game Boy it never does, which is what keeps
       this generalisation a no-op for the only core wired up today. */
    koboy_profile prof;
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

    /* Where the RECENT list lives: beside save_dir (recent.dat next to the
       .srm/.stN files), not beside koboy.ini. Two reasons, either one
       sufficient on its own: save_dir is GUARANTEED writable by the time
       execution reaches here -- config_resolve_paths already resolved it
       install-relative, and the save-state/SRAM paths below already trust it
       for exactly this reason -- while koboy.ini can legitimately live
       somewhere the user chose for CONFIGURATION, not for data, and may not
       even be writable from a menu-launched process. And recent.dat is data,
       exactly like a .srm: it belongs with the rest of what koboy owns, not
       with what the user edits. */
    char recents_file[600];
    snprintf(recents_file, sizeof recents_file, "%s/recent.dat", cfg.save_dir);

    /* Shared across every list screen one --ui-script run drives (MAIN MENU,
       then either RECENT or ALL GAMES) -- see run_list's script_i comment for
       why a single flat script can walk through more than one screen. NULL
       when there is no script, same as every other script-less run_list
       call in this file. */
    int script_i = 0;
    const koboy_input_state *ui_scr = ui_script_n > 0 ? ui_script : NULL;
    int *ui_scr_i = ui_script_n > 0 ? &script_i : NULL;

    /* Captured before the loop below can change `mode`: only a run that
       actually went through a UI screen painted over the faceplate, and only
       such a run needs it redrawn afterward. The --rom/rom= fast path skips
       this whole loop (mode is already MODE_PLAY) and must not pay for a
       redraw of chrome nothing has touched. */
    bool used_startup_ui = (mode == MODE_MAIN);

    while (mode == MODE_MAIN) {
        koboy_input *ui_in = input_create(&cfg, &prof);
        if (!ui_in) { fatal("out of memory"); free(panel); pf->shutdown(pf->ctx); return 1; }
#ifdef KOBOY_PLATFORM_KOBO
        platform_kobo_setup_touch(pf, ui_in);
#else
        input_set_touch_transform(ui_in, pw, ph, false, false, false);
#endif
        int choice = run_main_menu(pf, ui_in, panel, panel_stride, pw, ph,
                                   ui_scr, ui_scr_i, ui_script_n);

        if (choice == MAIN_RECENT) {
            koboy_recent rc;
            recent_load(&rc, recents_file);      /* corrupt/missing -> empty, never fatal */
            recent_prune_missing(&rc);
            int ri = run_recent_picker(pf, ui_in, panel, panel_stride, pw, ph, &rc,
                                       ui_scr, ui_scr_i, ui_script_n);
            input_destroy(ui_in);
            if (ri >= 0) {
                snprintf(cfg.rom_path, sizeof cfg.rom_path, "%s", recent_path(&rc, ri));
                say("koboy: chose %s (recent)\n", cfg.rom_path);
                recent_touch(&rc, cfg.rom_path, recent_display(&rc, ri));
                recent_save(&rc, recents_file);
                mode = MODE_PLAY;
            }
            /* else: BACK was tapped, or the run was stopped/exhausted while
               ON the recent screen -- loop back to MAIN MENU either way. A
               stopped or script-exhausted run converges on the SAME terminal
               exit one iteration later, when run_main_menu itself reports
               it (the `else` branch below) -- this does not need its own
               copy of that handling. */
        } else if (choice == MAIN_ALL_GAMES) {
            char chosen_name[ROMLIST_NAME];
            int br = run_browser(pf, ui_in, panel, panel_stride, pw, ph,
                                 cfg.rom_dir, cfg.rom_path, sizeof cfg.rom_path,
                                 chosen_name, sizeof chosen_name,
                                 ui_scr, ui_scr_i, ui_script_n);
            input_destroy(ui_in);

            if (br == BROWSE_ERR_DIR || br == BROWSE_ERR_EMPTY) {
                /* Two distinct messages, deliberately: a wrong rom_dir and an
                   empty one are different mistakes, and this is the only
                   diagnostic a user with no terminal gets. */
                if (br == BROWSE_ERR_DIR)
                    fatal("cannot read rom directory\n%s", cfg.rom_dir);
                else
                    fatal("no .gb, .gbc or .mgw files in\n%s", cfg.rom_dir);
                free(panel); pf->shutdown(pf->ctx); return 2;
            }
            if (br != BROWSE_PICKED) {
                /* A SCRIPTED run that ends without a rom chosen is a
                   failure, not a clean exit -- see run_list's own comment on
                   why this needs its own exit code. Backing out of ALL GAMES
                   interactively (only reachable via a signal/should_quit;
                   the ".." row goes UP a level, it does not leave the
                   browser) still exits 0. */
                if (ui_script_n > 0) {
                    fatal("ui script selected nothing");
                    free(panel); pf->shutdown(pf->ctx); return 4;
                }
                say("koboy: no rom chosen, exiting\n");
                free(panel); pf->shutdown(pf->ctx); return 0;
            }
            say("koboy: chose %s\n", cfg.rom_path);
            {
                /* Recorded here too, not only from RECENT: "played" means
                   actually loaded, and ALL GAMES is the other of the two
                   entry points that can load a rom at startup. */
                koboy_recent rc;
                recent_load(&rc, recents_file);
                recent_touch(&rc, cfg.rom_path, chosen_name);
                recent_save(&rc, recents_file);
            }
            mode = MODE_PLAY;
        } else {
            /* MAIN_QUIT, or run_main_menu itself was stopped/exhausted
               (choice == -1: g_stop, should_quit, or -- for a script -- the
               verbs ran out before landing on anything). Every one of these
               is a deliberate or forced end with nothing to resume to. */
            input_destroy(ui_in);
            if (ui_script_n > 0) {
                fatal("ui script selected nothing");
                free(panel); pf->shutdown(pf->ctx); return 4;
            }
            say("koboy: no rom chosen, exiting\n");
            free(panel); pf->shutdown(pf->ctx); return 0;
        }
    }

    if (used_startup_ui) {
        /* Whichever screen led here (RECENT or ALL GAMES) painted over the
           faceplate. */
        redraw_chrome(pf, panel, panel_stride, pw, ph, &prof, &cfg.layout);
    }
    /* mode is MODE_PLAY from here on, until the emulator loop below reads and
       writes it: MODE_QUIT ends the loop from inside the menu (a chosen QUIT,
       or CHOOSE ROM leaving nothing loaded), kept distinct from g_stop so the
       final status line does not call a menu-driven quit "stopped by
       signal". */

    /* --------------------------------------------------------- core, ROM */
    /* Opened, and the ROM loaded, BEFORE video_create/input_create below:
       the real game rect depends on the core's geometry (config_resolve_profile
       call further down), which libretro only answers honestly once a game is
       loaded -- so the buffers that rect sizes have to wait for it too. */
    /* The core is picked from the chosen ROM's extension, and only here:
       everything above ran before the browser knew which file the user would
       tap, and config_resolve_paths (way up in the argument parsing) could
       only resolve the DEFAULT. An explicit `core=` or --core wins outright
       -- core_explicit is the only way to tell "the user named gambatte"
       from "config_defaults wrote gambatte because it always does".
       The name goes back through config_join_sibling rather than being used
       raw, because dlopen never searches the cwd (see config.c's essay): a
       bare "gw_libretro.so" would be looked for everywhere except beside the
       binary. If the exe directory cannot be determined the bare name is
       kept, matching config_resolve_paths' own "leave paths as-is" fallback. */
    /* WHICH PRESENTATION this ROM gets, decided here and nowhere else. Set
       unconditionally, unlike the core just below, and config.h says why: an
       explicit --core cannot make a Game Boy faceplate right for a Game &
       Watch unit whose buttons are drawn into its own artwork.

       It has to happen BEFORE the config_resolve_profile call further down
       (that is what reads it) and it is deliberately NOT re-derived at the
       mid-session MENU -> CHOOSE ROM reload: that path reuses the SAME core
       handle without re-picking one, so switching systems mid-session already
       cannot work (gambatte would be handed a .mgw and reject it, and koboy
       quits with the message). Re-deriving the layout there would dress up a
       path that fails one step later as if it were supported. */
    cfg.layout_mode = config_layout_for_rom(cfg.rom_path);
    /* And the button complement, from the same extension and for the same
       reasons -- see config.h. Must come before config_resolve_profile too:
       chrome_controls_top counts a C button when there is one, and the
       profile is resolved against what that returns. */
    config_face_c_for_rom(&cfg.layout, cfg.rom_path);

    if (!cfg.core_explicit) {
        const char *want = config_core_for_rom(cfg.rom_path);
        char        dir[PATH_MAX], joined[512];
        if (config_exe_dir(dir, sizeof dir) &&
            config_join_sibling(joined, sizeof joined, want, dir))
            snprintf(cfg.core_path, sizeof cfg.core_path, "%s", joined);
        else
            snprintf(cfg.core_path, sizeof cfg.core_path, "%s", want);
    }

    /* Logged, not silent: four cores ship now and "which one did it pick?" is
       otherwise unanswerable on a device with no terminal, where the only
       symptom of a wrong pick is a core that rejects the ROM. */
    say("koboy: core %s\n", cfg.core_path);
    /* And the faceplate, for the same reason and in the same breath. The
       button complement is decided a few lines above and drawn hundreds of
       lines later; the only symptom of this file forgetting to ask for it is
       a button that quietly is not there, which is indistinguishable from a
       system that never had one. tests/smoke_host.sh reads this line. */
    say("koboy: faceplate %s%s\n", layout_name(cfg.layout_mode),
        cfg.layout.c_r > 0 ? ", with a C button" : "");

    char err[512];
    koboy_core *core = core_open(cfg.core_path, cfg.save_dir, err, sizeof err);
    if (!core) {
        fatal("%s", err);
        free(panel);
        pf->shutdown(pf->ctx); return 1;
    }

    /* Fully braced/enumerated zero-init: a bare {0} zeroes every field on
       every compiler that matters here, but Linaro GCC 4.9 (the ARM cross
       compiler; the host's newer GCC does not) applies -Wmissing-braces to
       the nested path[] array and -Wmissing-field-initializers to the rest,
       so a bare {0} is warning-free on host and warning-*full* on-device.
       Spell out every field so both toolchains agree it is zeroed. */
    koboy_sram_binding sb = {{0}, NULL, 0, false};
    if (!load_rom_into(core, &cfg, &sb, err, sizeof err)) {
        fatal("%s", err);
        core_close(core);
        free(panel);
        pf->shutdown(pf->ctx); return 1;
    }

    /* --------------------------------------------- re-fit for real geometry */
    /* `prof` above is still the Game-Boy-shaped placeholder. Re-resolve it
       against what the just-loaded ROM's core actually reports, and only
       redraw the faceplate (an extra full-panel refresh, so worth avoiding
       when nothing changed) when that answer differs from the placeholder --
       which for the Game Boy it never does, since base and max are both
       always 160x144 and the placeholder above was seeded with the same
       numbers. This is what keeps `bash tests/smoke_host.sh` and the video
       goldens byte-identical for the one core wired up today. */
    {
        int rbw, rbh, rmw, rmh;
        /* The LAYOUT is part of what makes the placeholder profile stale, not
           just the geometry: a .mgw whose core happened to report the same
           numbers the Game Boy placeholder was seeded with would still need
           the whole faceplate replaced. It cannot happen with the one core
           wired up today (gw answers 128x128 here, never 160x144), which is
           exactly why it is written down rather than relied on. */
        if (core_get_geometry(core, &rbw, &rbh, &rmw, &rmh) &&
            (rbw != prof.base_w || rbh != prof.base_h ||
             rmw != prof.max_w  || rmh != prof.max_h ||
             cfg.layout_mode != prof.layout_mode)) {
            koboy_profile real_prof;
            if (!config_resolve_profile(&real_prof, &cfg, pw, ph, rbw, rbh, rmw, rmh)) {
                fatal("panel %dx%d is too small for this core's %dx%d game rect",
                     pw, ph, rmw, rmh);
                core_close(core);
                free(panel);
                pf->shutdown(pf->ctx); return 1;
            }
            prof = real_prof;
            say("koboy: core geometry %dx%d (max %dx%d), %s layout, "
                "game %dx%d at (%d,%d)\n", rbw, rbh, rmw, rmh,
                layout_name(prof.layout_mode),
                prof.game_w, prof.game_h, prof.game_x, prof.game_y);
            /* The rect chrome/calibration/the menu were drawn against just
               changed shape or position -- put the faceplate back before any
               game pixel lands on the panel. */
            redraw_chrome(pf, panel, panel_stride, pw, ph, &prof, &cfg.layout);
        }
    }

    /* ------------------------------------------------------- video, input */
    koboy_video *vid = video_create(&prof, cfg.force_dither);
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
       only the backend knows by how much, so it installs the transform. */
    platform_kobo_setup_touch(pf, in);
#else
    /* The desktop mouse already reports panel coordinates: no transposition,
       no flips, raw range == panel range. */
    input_set_touch_transform(in, pw, ph, false, false, false);
#endif
    core_set_frame_cb(core, on_frame, NULL);
    core_set_input_fn(core, on_input, in);
    /* Additive: gambatte never issues a POINTER query, so this is inert for
       the Game Boy. It is installed unconditionally rather than only for the
       LCD layout because the state it forwards is already gated in input.c,
       and one install site is one fewer place for the two to disagree. */
    core_set_pointer_fn(core, on_pointer, in);

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

                /* Points at the MAIN MENU, not straight at the browser --
                   task 5's decision: this is the ONLY way a mid-session
                   switch can reach RECENT, so "recently played" stays useful
                   past the first pick of the session, not just at startup.
                   Never scripted (MODE_MENU has no --ui-script hook -- see
                   run_list's comment), so NULL/0 for every script argument
                   below, same as run_menu/run_slot_picker above. */
                bool picked = false;
                while (!picked && !g_stop && !pf->should_quit(pf->ctx)) {
                    int choice = run_main_menu(pf, in, panel, panel_stride,
                                               pw, ph, NULL, NULL, 0);
                    if (choice == MAIN_RECENT) {
                        koboy_recent rc;
                        recent_load(&rc, recents_file);
                        recent_prune_missing(&rc);
                        int ri = run_recent_picker(pf, in, panel, panel_stride,
                                                   pw, ph, &rc, NULL, NULL, 0);
                        if (ri >= 0) {
                            snprintf(cfg.rom_path, sizeof cfg.rom_path, "%s",
                                    recent_path(&rc, ri));
                            recent_touch(&rc, cfg.rom_path, recent_display(&rc, ri));
                            recent_save(&rc, recents_file);
                            picked = true;
                        }
                        /* else: BACK -- loop shows MAIN MENU again. */
                    } else if (choice == MAIN_ALL_GAMES) {
                        /* Never scripted here -- MODE_MENU has no --ui-script
                           hook (see run_list's comment) -- so NULL/0 for
                           every script argument, same as run_menu and
                           run_slot_picker. */
                        char chosen_name[ROMLIST_NAME];
                        int br = run_browser(pf, in, panel, panel_stride, pw, ph,
                                             cfg.rom_dir, cfg.rom_path,
                                             sizeof cfg.rom_path,
                                             chosen_name, sizeof chosen_name,
                                             NULL, NULL, 0);
                        if (br == BROWSE_ERR_DIR) {
                            /* Distinct from the empty case, matching the
                               startup browser's two messages: romlist.h
                               documents why they must stay distinguishable --
                               "your rom_dir is wrong" and "you have no ROMs"
                               are different diagnoses to a user with no
                               terminal, and this is the only diagnostic they
                               get. */
                            fatal("cannot read rom directory\n%s", cfg.rom_dir);
                        } else if (br == BROWSE_ERR_EMPTY) {
                            fatal("no .gb, .gbc or .mgw files in\n%s", cfg.rom_dir);
                        } else if (br == BROWSE_PICKED) {
                            koboy_recent rc;
                            recent_load(&rc, recents_file);
                            recent_touch(&rc, cfg.rom_path, chosen_name);
                            recent_save(&rc, recents_file);
                            picked = true;
                        }
                        /* BROWSE_NONE: nothing chosen from ALL GAMES either
                           -- loop back to MAIN MENU, same as BACK from
                           RECENT above. */
                    } else {
                        /* MAIN_QUIT, or the while loop's own g_stop/
                           should_quit -- either way there is nothing left to
                           resume (the running game was already flushed and
                           unloaded above), so this ends the session. */
                        break;
                    }
                }
                /* No "return to the game" option when nothing was picked:
                   the running game was already flushed and unloaded above so
                   CHOOSE ROM could have the core to itself, and by the time
                   the flow above ends without a pick there is nothing left to
                   resume. Quitting is the only coherent option: the
                   alternative is a black or frozen screen with no
                   explanation, which this project's own constraint calls
                   "indistinguishable from a crash". A DIAGNOSABLE failure
                   (rom_dir missing or empty) already told the user why on the
                   panel above, via fatal(); a plain QUIT or backing all the
                   way out needs no extra message. */
                if (!picked) {
                    mode = MODE_QUIT;
                } else {
                    /* This mid-session load deliberately does NOT re-query
                       core_get_geometry, and that is not the gap it looks
                       like. core_load_rom clears geom_dirty (core.c), the new
                       ROM's first retro_run re-announces via
                       SET_GEOMETRY/SET_SYSTEM_AV_INFO, and the per-frame
                       core_geometry_changed() poll below re-fits prof, the
                       faceplate and the video buffer. MEASURED, not assumed:
                       three .mgw titles loaded back-to-back into ONE
                       gw-libretro instance each re-announced (Parachute
                       658x395, Mario Bros. 973x532, Donkey Kong Circus
                       498x771 -- 2 env calls per load, every load). An
                       earlier version of this comment named a multi-title
                       Game & Watch core as the case that would need work
                       here; that is exactly the case the poll already
                       handles, so the warning pointed at the wrong thing.

                       The residual gap is narrower and nothing in scope hits
                       it: a core that varies geometry per ROM AND reports it
                       only from retro_get_system_av_info at load time, never
                       via either environment command. gambatte is fixed
                       160x144, so it cannot. Such a core would need this path
                       to redo the video_create/redraw_chrome dance the
                       startup path does, or frames past the allocated max
                       would be silently dropped by the bounds guard. */
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
            /* REBASE, not re-init: pacer_init zeroes p->frames, and the
               bounded-run test above is `pace.frames >= frame_limit`, so a
               --frames N run used to restart its whole budget every time the
               menu closed. The wall clock does need re-anchoring (the menu may
               have been open for a minute) -- that is all pacer_rebase does. */
            pacer_rebase(&pace, pf->now_us(pf->ctx));
            continue;
        }

        uint64_t delay = pacer_delay_us(&pace, pf->now_us(pf->ctx));
        if (delay) usleep((useconds_t)delay);

        g_frame = NULL;
        uint64_t t0 = pf->now_us(pf->ctx);
        core_run_frame(core);
        stats_add(&stats, KOBOY_STAGE_CORE, pf->now_us(pf->ctx) - t0);

        /* Some cores do not know their real geometry until INSIDE a
           retro_run() -- measured: the Game & Watch core reports a 128x128
           placeholder from retro_get_system_av_info called right after
           retro_load_game, on every one of 59 measured titles, and only
           resolves the real canvas (Parachute 658x395, Mario Bros. 973x532,
           Donkey Kong 606x748, ...) from inside its FIRST retro_run(), via
           the SET_GEOMETRY/SET_SYSTEM_AV_INFO environment calls core.c now
           handles. Checked after EVERY retro_run(), not just the first: a
           core may call either command again later too (a Multi Screen
           title toggling between a folded and an unfolded view is the
           plausible case for this exact core), and polling
           core_geometry_changed() here is the one mechanism that covers
           "resolves late, once" and "changes again mid-session" without
           special-casing either. For a core that never touches either
           command -- the Game Boy core, still the only one that does not
           need any of this -- core_geometry_changed() is always false, so
           this costs one cheap boolean check per frame and changes nothing
           else about existing Game Boy behaviour. */
        if (core_geometry_changed(core)) {
            int rbw, rbh, rmw, rmh;
            /* Only a change to MAX re-fits. The reserved rect, the chrome
               drawn around it and video's buffers are all sized from max
               (koboy.h), so a change to base alone leaves every one of them
               correct and there is nothing to redo: video_fit places each
               frame inside the existing rect per submit.

               Testing base here as well -- which this did -- made every base
               change tear down video, rebuild it and repaint the whole
               faceplate. That is not a theoretical cost: a Game & Watch title
               alternates between the whole unit and the LCD alone several
               times a second, so the device log showed the pair
               654x396 <-> 305x191 repeating, each toggle paying a full
               video_destroy/video_create plus a chrome redraw plus the
               forced full-rect refresh a fresh video_create implies. The
               Game Boy never reaches either branch (base == max == 160x144,
               and gambatte never sends these commands at all). */
            if (core_get_geometry(core, &rbw, &rbh, &rmw, &rmh) &&
                (rbw != prof.base_w || rbh != prof.base_h ||
                 rmw != prof.max_w  || rmh != prof.max_h)) {
                if (rmw == prof.max_w && rmh == prof.max_h) {
                    /* Base-only: record what the core is rendering now, for
                       the log and for anything that asks, and keep going. */
                    prof.base_w = rbw; prof.base_h = rbh;
                    goto geometry_done;
                }
                koboy_profile real_prof;
                if (!config_resolve_profile(&real_prof, &cfg, pw, ph, rbw, rbh, rmw, rmh)) {
                    fatal("panel %dx%d is too small for this core's %dx%d game rect",
                         pw, ph, rmw, rmh);
                    mode = MODE_QUIT;
                    goto sram_check;
                }
                prof = real_prof;
                say("koboy: core geometry settled at %dx%d (max %dx%d), "
                    "%s layout, game %dx%d at (%d,%d)\n",
                    rbw, rbh, rmw, rmh, layout_name(prof.layout_mode),
                    prof.game_w, prof.game_h, prof.game_x, prof.game_y);
                /* The faceplate drawn against the old (possibly placeholder)
                   rect no longer matches -- redraw it before this frame's
                   pixels land on the panel. */
                redraw_chrome(pf, panel, panel_stride, pw, ph, &prof, &cfg.layout);
                /* video_create's buffer was sized for the OLD geometry;
                   video_submit_rects' bounds guard would just drop a bigger
                   frame rather than corrupt memory, so without this the game
                   would render nothing from here on. Rebuilding is the only
                   way to grow it. The diff history the old buffer carried is
                   not a loss: the new one is being shown for the first time
                   (video_create seeds prev to force a full-dirty first
                   submit), and the panel does not yet show anything from it
                   either way. */
                video_destroy(vid);
                vid = video_create(&prof, cfg.force_dither);
                if (!vid) {
                    fatal("out of memory");
                    mode = MODE_QUIT;
                    goto sram_check;
                }
            }
        }
    /* The `;` is required, not stylistic: a label must be followed by a
       STATEMENT, and the next line is a declaration. Newer host GCC accepts
       the label-before-declaration form as an extension; the device's Linaro
       4.9 rejects it outright, so dropping this breaks the ARM build only. */
    geometry_done: ;

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

        /* Keep the LCD layout's touch->pointer normalisation pointed at the
           pixels the artwork is actually on. video_frame_rect reports where
           the frame just submitted landed inside the reserved rect, which is
           SMALLER than that rect whenever the core renders below its max
           geometry -- a Game & Watch title zooming to the LCD alone does
           exactly that, several times a second, and the core normalises the
           pointer it receives against what it is currently showing. Done here
           rather than in input.c because input.c has no video dependency, and
           unconditionally rather than only for the LCD layout because
           input.c ignores the rect entirely in the other one.
           Before nrects is tested: an unchanged frame still occupies the same
           rect, and skipping the update on a duplicate would leave the
           pointer mapped to whatever the last CHANGED frame's size was. */
        {
            koboy_rect fr;
            video_frame_rect(vid, &fr);
            input_set_pointer_rect(in, prof.game_x + fr.x, prof.game_y + fr.y,
                                   fr.w, fr.h);
        }

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
