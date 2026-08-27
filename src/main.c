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
 * THIS NOTICE IS IN THIS FILE ONLY, and that is deliberate rather than an
 * unfinished job: no source file in this project carries a per-file header,
 * so adding thirty of them would bury a year of real history in `git blame`
 * for no legal gain that LICENSE and README.md do not already supply. The
 * entry point is the customary place for the one that does exist.
 * LICENSES.md covers the fourteen emulator cores, which are NOT under this
 * licence and are not linked into this binary -- they are separate shared
 * objects, dlopen'd at runtime, each with its own terms, and three of them
 * restrict commercial use.
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
/* The pixel aspect of the frames THIS core is currently rendering, from the
   display aspect it reported and the base geometry it reported alongside it.
   KOBOY_ASPECT_ONE for a square-pixel core, which is what makes every rect and
   every fit below identical to what they were before non-square pixels
   existed. Base and not max: see config_resolve_profile_par in config.h. */
/* The core's DISPLAY aspect as the pipeline should treat it: what the core
   reported, or "absent" when the owner has turned the correction off. Every
   reader of the core's aspect goes through this or through core_par below --
   video_set_aspect included, which is a second path into the same decision
   and would otherwise leave video scaling corrected while the rect was not. */
static uint32_t core_aspect(const koboy_config *cfg, const koboy_core *c)
{
    return cfg->pixel_aspect ? core_display_aspect(c) : 0u;
}

static uint32_t core_par(const koboy_config *cfg, const koboy_core *c,
                         int base_w, int base_h)
{
    /* The one place the pixel_aspect key is read, so turning it off cannot
       leave half the pipeline corrected and half not: every rect and every
       fit downstream derives from this value, and KOBOY_ASPECT_ONE is
       exactly what they all saw before non-square pixels existed. */
    return video_pixel_aspect(core_aspect(cfg, c), base_w, base_h);
}

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

/* Every message the user must SEE goes through here. On the desktop that is
   just stderr; on the device there is no terminal, so an error that only
   reaches stderr is indistinguishable from a crash -- the panel keeps whatever
   was on it and the user power-cycles. The Kobo backend draws the message on
   the panel and waits for an acknowledgement (bounded: 20s, so an unattended
   run cannot hang here).

   Nothing in here ends the process, and nothing ever did -- the CALLERS did.
   Two of them already did not, and the split below says which is which by
   name: notify() for a condition the session survives, fatal() for one it
   does not. The names are the whole point. "fatal" sitting above a return to
   the ROM browser reads as a bug for as long as it takes to check, and a
   failed ROM load is now the most common thing this function prints. */
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

/* One place for "that game did not start", because there are three sites --
   the core would not open, the ROM would not load, and the same pair again
   mid-session -- and they must all reach the same two decisions.

   Returns true if the caller should go BACK TO THE MAIN MENU, false if it
   must end the run. The difference is whether there is anywhere to go back
   to: `recoverable` is the caller's used_startup_ui / mid-session state, and
   a run given its ROM on the command line has no list behind it.

   The message names the game FIRST, on its own line, and the technical
   reason after it. `err` already contains the full path (core.c writes it
   into every one of its messages), so the panel repeats the name -- that is
   deliberate: fbink wraps at the column edge rather than at word boundaries
   and at fontmult 3 a full path is several unreadable lines, while the head
   of the message has to be legible at a glance to be worth drawing at all.
   The log line, which has room, keeps the path. */
static bool load_failed_recoverable(bool recoverable, const char *rom_path,
                                    const char *err)
{
    if (!recoverable) {
        fatal("%s", err);
        return false;
    }
    char name[KOBOY_RECENT_DISPLAY];
    recent_name_from_path(name, sizeof name, rom_path);
    /* Explicit precisions, not snprintf's own bound: `name` and `err` can
       each fill the 512-byte message buffer on their own, and left to
       snprintf the FIRST one to overflow would silently eat the other. The
       two numbers are a panel budget, not a guess -- at fontmult 3 the Libra
       2's 1264px width takes about 20 characters, so 60 is three lines of
       filename and 300 is the reason under it. */
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

/* --------------------------------------------------------- SCREENSHOT note
 *
 * The owner is looking at the panel, not at koboy.log, so a capture has to
 * confirm itself where he is looking. Two constraints shape how:
 *
 *  - IT MUST NOT PAINT OVER THE FRAME BEING SAVED. The file is written
 *    first, from the composited frame, and only then is anything drawn --
 *    so what lands on disk is the game exactly as it was, never a game with
 *    a notification on it.
 *  - IT MUST NOT COST A FULL-PANEL FLASH. notify() repaints everything and
 *    is right for an error you have stopped playing to read; mid-game it
 *    would be a worse interruption than the thing it is reporting. This
 *    paints a small plaque in the background band BETWEEN the game rect and
 *    the drawn controls, refreshes that rectangle alone, and takes it away
 *    a couple of seconds later.
 *
 * A WHITE PLAQUE WITH BLACK TEXT rather than text drawn onto whatever chrome
 * put there: the band is not uniform on every layout (the DMG faceplate has
 * a case shade, a wordmark and a bezel edge through parts of it), and text
 * over an unknown background is text that might not be readable. The plaque
 * is legible on any of them. Erasing is exact for the same reason it is
 * cheap: chrome is re-rendered into the panel buffer and only the plaque's
 * rectangle is blitted back, so whatever was underneath returns byte for
 * byte without anyone here having to know what it was. */
#define SHOT_NOTE_PX  3          /* 5x7 font scale: 21px glyphs, as the menus */
#define SHOT_NOTE_PAD 12
#define SHOT_NOTE_MS  2500

/* Where the plaque goes, or false when there is nowhere for it. The band
   between the bottom of the game rect and the top of the controls is the one
   part of the panel that is neither the picture nor a touch target -- but its
   height is a consequence of the fitted game rect, not a reservation, so on
   some layouts (the Game & Watch strip, or a small panel with a tall game) it
   is too short. False there, and the capture is reported to the log only:
   a plaque over the controls, or over the game, would be worse than none. */
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
    /* Centred on the GAME rect, not on the panel: on a layout whose rect is
       off-centre the plaque belongs under the picture it is about. Clamped,
       because a rect near an edge could otherwise put it off the panel. */
    out->x = prof->game_x + (prof->game_w - w) / 2;
    if (out->x < 0) out->x = 0;
    if (out->x + w > pw) out->x = pw - w;
    return true;
}

/* Drives one list widget to a selection. Returns the chosen index, or -1 if
   the user quit, the run was stopped, or a script ran out.

   `script`/`script_n` make the startup flow reachable in a bounded unattended
   run. Without them every automated test would pass --rom and skip every
   list screen entirely -- the same blind spot that hid v1's first-run
   deadlock through twenty reviews. MODE_MENU joined them with the `menu`
   verb: run_menu is scripted, and so is everything CHOOSE ROM opens
   underneath it. run_slot_picker is the one screen nothing drives yet -- it
   is wired for a script (see its own comment) but no test walks into it, and
   saying otherwise here would overclaim coverage the suite does not have.

   `script_i`, when not NULL, is a CURSOR shared across every screen one
   --ui-script run drives (MAIN MENU, then RECENT or ALL GAMES; then, past a
   `menu` verb, the in-game MENU and the same two lists again) -- a pointer
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
    /* LIVE GUARD, and the only one in this project that exists to stop a
       CRASH rather than a wrong picture: snes9x2005 raises SIGFPE inside
       retro_load_game for a .sfc/.smc under 8192 bytes, which kills koboy
       outright -- no error screen, no way back to the browser, on a device
       whose whole point is that it recovers. config_min_rom_bytes carries
       the measurement and the reason the floor is per-system; here it is
       enough to know that a file below it must never reach c->load_game.

       Checked HERE rather than in core.c because core.c must not know what
       it is loading, and here rather than in romlist.c because the browser
       lists names without stat()ing them -- this is the one place that has
       both the path and a reason to touch the filesystem. Reported as an
       ordinary load failure, so the browser shows it the same way it shows
       a core's own refusal. */
    size_t floor_bytes = config_min_rom_bytes(cfg->rom_path);
    if (floor_bytes) {
        struct stat st;
        if (stat(cfg->rom_path, &st) == 0 && st.st_size >= 0 &&
            (size_t)st.st_size < floor_bytes) {
            /* The REASON is formatted before the path, deliberately: err is a
               fixed buffer and rom_path can fill it on its own, so putting
               the path last means a long name truncates the name rather than
               the explanation. A message that says only "rom
               /very/long/pa..." helps nobody. The %.200s precision clips the
               path explicitly rather than letting snprintf do it silently --
               same result, but it is the thing that keeps this line free of
               a -Wformat-truncation warning, and a warning nobody can fix is
               a warning everybody learns to ignore. */
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

/* MOTION sits BELOW FRAMES, and SCREENSHOT below MOTION: the order is not
   free, because tests/smoke_host.sh drives this menu by hardcoded pixel
   coordinates derived from the row index, so a row inserted ABOVE an existing
   one silently strands every tap below it outside the row it names. Adding at
   the end of the settings group costs one comment update per tap below it and
   nothing else -- and SCREENSHOT going in above CHOOSE ROM moved every one of
   those, which is what that sentence is worth. */
enum {
    MENU_SAVE = 0, MENU_LOAD, MENU_RESET, MENU_GRAY, MENU_FRAMES, MENU_MOTION,
    MENU_SHOT, MENU_CHOOSE_ROM, MENU_RESUME, MENU_QUIT,
    MENU_COUNT
};

/* Returns the chosen MENU_* action, or MENU_RESUME if the user backed out.
   `has_states` greys nothing out visually -- the label says so instead, which
   is cheaper on a panel with no colour and no hover. `map` is likewise shown
   in the row rather than behind it: on a panel with no hover and no second
   screen, a setting you cannot read without opening something is a setting
   nobody knows the value of. */
static int run_menu(koboy_platform *pf, koboy_input *in, uint8_t *panel,
                    int stride, int pw, int ph, bool has_states,
                    koboy_gray_map map, int divisor,
                    bool dither, koboy_wfm_policy wfm, int shot_next,
                    const koboy_input_state *script, int *script_i, int script_n)
{
    const char *items[MENU_COUNT];
    static char gray_label[48], divisor_label[48], motion_label[48], shot_label[48];
    ui_gray_label(gray_label, sizeof gray_label, map);
    ui_divisor_label(divisor_label, sizeof divisor_label, divisor);
    ui_motion_label(motion_label, sizeof motion_label, dither, wfm);
    ui_shot_label(shot_label, sizeof shot_label, shot_next);
    items[MENU_SAVE]        = has_states ? "SAVE STATE" : "SAVE STATE (UNSUPPORTED)";
    items[MENU_LOAD]        = has_states ? "LOAD STATE" : "LOAD STATE (UNSUPPORTED)";
    items[MENU_RESET]       = "RESET GAME";
    items[MENU_GRAY]        = gray_label;
    items[MENU_FRAMES]      = divisor_label;
    items[MENU_MOTION]      = motion_label;
    items[MENU_SHOT]        = shot_label;
    items[MENU_CHOOSE_ROM]  = "CHOOSE ROM";
    items[MENU_RESUME]      = "RESUME";
    items[MENU_QUIT]        = "QUIT";

    koboy_ui_list list;
    ui_list_init(&list, "MENU", items, MENU_COUNT,
                 KOBOY_CHROME_MARGIN, KOBOY_CHROME_MARGIN,
                 pw - 2 * KOBOY_CHROME_MARGIN, ph - 2 * KOBOY_CHROME_MARGIN);

    /* The cursor is shared with every other screen a --ui-script run drives:
       the `menu` verb opens this screen from inside the emulator loop, and
       whatever the script has left over goes on driving it. See run_list's
       script_i comment. */
    int pick = run_list(pf, in, &list, panel, stride, pw, ph, script, script_i, script_n, -1);
    if (pick < 0) return MENU_RESUME;
    if ((pick == MENU_SAVE || pick == MENU_LOAD) && !has_states) return MENU_RESUME;
    return pick;
}

/* Returns the chosen slot (1-based), or 0 if the user backed out. */
static int run_slot_picker(koboy_platform *pf, koboy_input *in, uint8_t *panel,
                           int stride, int pw, int ph, const char *title,
                           const char *save_dir, const char *rom_path,
                           const koboy_input_state *script, int *script_i,
                           int script_n)
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

    /* The same shared cursor run_menu uses. Wired even though nothing scripts
       SAVE/LOAD yet, because the alternative is a TRAP: this screen is one
       tap past a row a script can now reach, and an unscripted run_list with
       no live input does not exit -- it polls until the run is killed. A
       script that tapped SAVE STATE would hang rather than fail. */
    int pick = run_list(pf, in, &list, panel, stride, pw, ph, script, script_i, script_n, -1);
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
   path into out_path. It used to hand back the row's TEXT as well, for the
   RECENT list to store; the row text is the path's last component and
   recent.c derives it (recent_name_from_path), so a second output that could
   disagree with the first no longer exists.

   Both entry points -- startup ALL GAMES and the in-game MENU's CHOOSE ROM --
   call this. They used to carry a hand-copied browser each, which was already
   two copies of the scan/geometry/alpha-strip setup before navigation added a
   loop to each of them. */
enum { BROWSE_PICKED = 0, BROWSE_NONE, BROWSE_ERR_DIR, BROWSE_ERR_EMPTY };

static int run_browser(koboy_platform *pf, koboy_input *in, uint8_t *panel,
                       int stride, int pw, int ph, const char *rom_dir,
                       char *out_path, size_t out_path_n,
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
    /* THE PIXEL ASPECT `prof` WAS RESOLVED WITH, carried alongside it because
       koboy_profile does not hold it and the staleness test below needs it.
       Square here, and not for want of asking: no ROM is loaded at this point,
       so there is no core to ask for a display aspect. The real rect is
       resolved below from the real geometry AND the real pixel aspect. */
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

    /* ------------------------------------------------------- the session loop
       ONE GAME IS ONE SESSION: pick a ROM, derive everything from it, open a
       core, play, tear all of it down. Two different things send execution
       round again, and it is the same trip for both.

       A FAILED LOAD. A ROM chosen from RECENT or the browser that does not
       load is an ORDINARY condition -- the file was deleted between the scan
       and the tap, an SD card is not mounted, a partial copy is short, a core
       is missing, the core simply refuses it -- and it used to end the
       process. On the device that is indistinguishable from a crash: koboy
       vanishes and Nickel comes back, with whatever the user was doing gone.
       Reported twice from the device, both times from RECENT.

       A MID-SESSION SWITCH. MENU -> CHOOSE ROM used to call load_rom_into on
       the LIVE core -- the one opened at startup for the FIRST ROM's
       extension -- and that killed a device: a Mega Drive .md handed to gpSP,
       which executed it as ARM code and took SIGSEGV ("bad jump 8000000", no
       second "koboy: core" line in the log because no second core was ever
       opened). Everything derived from the extension was equally stale, not
       just the core: layout, extra buttons, scale ceiling, the save binding.
       The comment that used to sit above that call reasoned only about
       geometry and dated from when koboy was one core per session; fifteen
       systems made it wrong and nobody revisited it. So CHOOSE ROM now ends
       the session and comes back here, where all of that is derived from
       scratch.

       UNCONDITIONALLY, with no same-core fast path. Switching games is not a
       hot operation -- it is a human tapping through three screens -- and
       "reload into the live core when the extension happens to match" is a
       second code path that only the rarest case exercises, which is exactly
       how the bug above survived. Reopening the same .so for a second Game
       Boy ROM costs one dlopen.

       Back to the MAIN MENU, not to the exact list the ROM came from: this
       is the loop's own top, so it costs no new state and no new mode, both
       lists are one tap away, and the RECENT list in particular must be
       rebuilt from disk anyway (recent_load/prune below) rather than resumed.
       Do not add a mode for this.

       A run that did NOT come through the startup UI (--rom, or rom= in the
       ini) still dies on a failed load -- see the branches below. There is no
       list to go back to, and "koboy --rom nonsense.gb" exiting 0 would be a
       lie to whatever launched it. used_startup_ui is what tells them apart.

       ITS BODY IS DELIBERATELY NOT RE-INDENTED. Everything down to the
       matching brace (marked "end of the session loop") is the same code at
       the same indentation it has always had; re-indenting four hundred lines
       to add one enclosing loop would bury the behaviour change in a diff
       nobody can read. */

    /* THE RUN'S counters, not the session's: they live out here so a run that
       plays three games reports one total rather than three summaries, which
       is what `presented=` has always meant and what the smoke tests grep
       for. `stats` is cumulative for the same reason it always was across a
       mid-session switch -- nothing reset it before either. */
    koboy_stats stats;
    stats_reset(&stats);
    unsigned long presented = 0, since_cleanup = 0, cleanups = 0, big_refreshes = 0;
    unsigned long rects_emitted = 0;
    /* Run-scoped, like presented and frames_done, because the pacer is
       per-SESSION: pacer_init zeroes its counters, so reading pace.held at
       session_end would report only the last game a switching run played. */
    unsigned long settle_held = 0;
    uint64_t last_sram_us = 0, last_cleanup_us = 0;
    /* SCREENSHOT state, and `armed` is the whole design of the feature.
       Selecting MENU -> SCREENSHOT does NOT take a picture: the menu is drawn
       OVER the game, so a capture taken there would be a photograph of the
       menu. It sets this, the menu closes, and the capture happens from the
       next COMPOSITED frame -- see the arming branch and the capture site for
       why "next composited frame" is well defined even with present_divisor
       and the settle pacer both deferring presentation.
       Run-scoped rather than session-scoped only because everything else here
       is; an arm that survives to a different game is impossible, since the
       only ways out of this menu that reach another ROM leave through
       MODE_MAIN, and this is cleared before the capture either way. */
    bool       shot_armed = false;
    /* The on-panel confirmation, and when it gets taken away again. Zero
       means nothing is showing. */
    uint64_t   shot_note_until_us = 0;
    koboy_rect shot_note = { 0, 0, 0, 0 };
    /* --frames N is a budget for the RUN. Each session gets its own pacer
       (a new core reports its own frame rate), and pacer_init zeroes
       p->frames, so a switch would otherwise hand the second game a fresh
       budget of N. The pacer keeps counting this session; frames_done holds
       what the finished ones spent. */
    unsigned long frames_done = 0;
    /* Whether anything has been PLAYED yet, which is what decides how a
       "nothing chosen" exit behaves: at startup there is no run to report on
       and koboy exits straight out (0, or 4 for a script that selected
       nothing); once a game has run, the same choice has to fall through to
       the run's summary instead of throwing it away. */
    bool any_game_ran = false;

    char err[512];
    koboy_core *core = NULL;
    /* Fully braced/enumerated zero-init: a bare {0} zeroes every field on
       every compiler that matters here, but Linaro GCC 4.9 (the ARM cross
       compiler; the host's newer GCC does not) applies -Wmissing-braces to
       the nested path[] array and -Wmissing-field-initializers to the rest,
       so a bare {0} is warning-free on host and warning-*full* on-device.
       Spell out every field so both toolchains agree it is zeroed. */
    koboy_sram_binding sb = {{0}, NULL, 0, false};

    for (;;) {

    /* Captured before the picker below can change `mode`: only a session that
       actually went through a UI screen painted over the faceplate, and only
       such a session needs it redrawn afterward. The --rom/rom= fast path
       skips the picker entirely (mode is already MODE_PLAY) and must not pay
       for a redraw of chrome nothing has touched. Re-evaluated every session,
       because the SECOND game always comes from the UI even when the first
       one came from the command line. */
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
                /* NOT recorded here. "Played" means LOADED -- the recording
                   moved below the load for both entry points, and this is
                   the one where it matters most: recording at pick time
                   promoted a ROM that then failed to the top of the very
                   list the user has to walk past to try something else. */
                mode = MODE_PLAY;
            }
            /* else: BACK was tapped, or the run was stopped/exhausted while
               ON the recent screen -- loop back to MAIN MENU either way. A
               stopped or script-exhausted run converges on the SAME terminal
               exit one iteration later, when run_main_menu itself reports
               it (the `else` branch below) -- this does not need its own
               copy of that handling. */
        } else if (choice == MAIN_ALL_GAMES) {
            int br = run_browser(pf, ui_in, panel, panel_stride, pw, ph,
                                 cfg.rom_dir, cfg.rom_path, sizeof cfg.rom_path,
                                 ui_scr, ui_scr_i, ui_script_n);
            input_destroy(ui_in);

            if (br == BROWSE_ERR_DIR || br == BROWSE_ERR_EMPTY) {
                /* Two distinct messages, deliberately: a wrong rom_dir and an
                   empty one are different mistakes, and this is the only
                   diagnostic a user with no terminal gets.

                   Terminal only on the FIRST session. Once a game has run,
                   an unreadable or empty rom_dir is a dead end in one list,
                   not a reason to end the run -- RECENT is still there, and
                   this is exactly what the mid-session picker did before it
                   was folded into this one. notify()/fatal() picks the
                   wording to match which of those two it is. */
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
                /* A SCRIPTED run that ends without a rom chosen is a
                   failure, not a clean exit -- see run_list's own comment on
                   why this needs its own exit code. Backing out of ALL GAMES
                   interactively (only reachable via a signal/should_quit;
                   the ".." row goes UP a level, it does not leave the
                   browser) still exits 0. */
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
            /* MAIN_QUIT, or run_main_menu itself was stopped/exhausted
               (choice == -1: g_stop, should_quit, or -- for a script -- the
               verbs ran out before landing on anything). Every one of these
               is a deliberate or forced end with nothing to resume to. */
            input_destroy(ui_in);
            /* A session already played: this is QUIT (or a signal) closing a
               run that has numbers to report, so it joins the ordinary end
               of the loop instead of throwing the summary away. That is what
               the mid-session picker's `mode = MODE_QUIT` did, and koboy.log
               would otherwise lose the only record of the session. */
            if (any_game_ran) goto session_end;
            if (ui_script_n > 0) {
                fatal("ui script selected nothing");
                free(panel); pf->shutdown(pf->ctx); return 4;
            }
            say("koboy: no rom chosen, exiting\n");
            free(panel); pf->shutdown(pf->ctx); return 0;
        }
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

       It has to happen BEFORE the config_resolve_profile call further down,
       which is what reads it. It is re-derived on EVERY session, including a
       mid-session switch, and the comment that used to stand here said the
       opposite: that CHOOSE ROM reused one core handle, so switching systems
       could not work anyway and re-deriving the layout would dress up a path
       that failed one step later. That was true when koboy was one core per
       session and it went on being read as true after fifteen systems made
       it false -- the switch did not fail one step later, it handed a Mega
       Drive ROM to gpSP and the device took SIGSEGV. Which is why the switch
       now comes back through this loop, and why this line runs for it. */
    cfg.layout_mode = config_layout_for_rom(cfg.rom_path);
    /* And the button complement, from the same extension and for the same
       reasons -- see config.h. Must come before config_resolve_profile too:
       chrome_controls_top counts the extra discs when there are any, and the
       profile is resolved against what that returns. */
    config_extra_buttons_for_rom(&cfg.layout, cfg.rom_path);
    /* And what the LCD strip's controls SAY, from the same extension. Only
       the LCD faceplate reads these, but they are set unconditionally beside
       the two calls above so there is one place a reader can see everything
       the ROM's extension decides. Order does not matter to the resolver --
       labels have no geometry -- but keeping the three together is what stops
       a fourth being added somewhere else. */
    config_lcd_pad_for_rom(&cfg.layout, cfg.rom_path);
    /* Which geometry the LCD rect is sized from. MUST come before
       config_resolve_profile, which is what reads it, and it is a fact about
       the system exactly like layout_mode above. */
    cfg.lcd_rect_from_max = config_lcd_rect_from_max_for_rom(cfg.rom_path);
    /* Set from the ROM, and OUTSIDE the core_explicit branch below on
       purpose: a scale ceiling is a property of the system, not of which
       core the owner pointed at it. Someone running a .sfc through their own
       --core still wants the SNES ceiling. */
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

    /* Logged, not silent: six cores ship now and "which one did it pick?" is
       otherwise unanswerable on a device with no terminal, where the only
       symptom of a wrong pick is a core that rejects the ROM. */
    say("koboy: core %s\n", cfg.core_path);
    /* And the faceplate, for the same reason and in the same breath. The
       button complement is decided a few lines above and drawn hundreds of
       lines later; the only symptom of this file forgetting to ask for it is
       a button that quietly is not there, which is indistinguishable from a
       system that never had one. tests/smoke_host.sh reads this line. */
    {
        /* Every extra disc by NAME, not a yes/no: two systems now add discs
           and they add different ones, so "with a C button" could no longer
           tell a WonderSwan's L1/R1 pair from a missing pair. Built into one
           buffer so the line stays one say() call, which is what
           tests/smoke_host.sh matches against. */
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
        /* A MISSING CORE IS A FAILED LOAD, not a broken installation. Fifteen
           systems ship now and the core is picked from the extension, so
           "tapped a .gba on a device whose gba_libretro.so did not survive
           the copy" arrives here -- and it is exactly the same user-visible
           event as a ROM the core refuses: this game did not start. It is
           handled the same way. */
        if (!load_failed_recoverable(used_startup_ui, cfg.rom_path, err)) {
            free(panel);
            pf->shutdown(pf->ctx); return 1;
        }
        mode = MODE_MAIN;
        continue;
    }

    if (!load_rom_into(core, &cfg, &sb, err, sizeof err)) {
        /* Closed, not kept: the next ROM through this loop picks its own
           core from its own extension, and a handle left open here would
           leak one .so per failed tap. core_close on a core with no ROM
           loaded is the ordinary shutdown path (core.c). */
        core_close(core);
        core = NULL;
        if (!load_failed_recoverable(used_startup_ui, cfg.rom_path, err)) {
            free(panel);
            pf->shutdown(pf->ctx); return 1;
        }
        mode = MODE_MAIN;
        continue;
    }

    /* --------------------------------------------- re-fit for real geometry */
    /* `prof` is still the shape of the PREVIOUS game -- the Game-Boy-shaped
       placeholder on the first session, the outgoing game's real profile on
       any later one. Re-resolve it against what the just-loaded ROM's core
       actually reports, and only redraw the faceplate (an extra full-panel
       refresh, so worth avoiding when nothing changed) when that answer
       differs -- which for a second Game Boy ROM it never does, since base
       and max are both always 160x144. This is what keeps
       `bash tests/smoke_host.sh` and the video goldens byte-identical for the
       one core wired up today.

       Comparing against the OUTGOING game rather than against the placeholder
       is not a shortcut: two ROMs of the same system need no re-fit and no
       repaint, and two of different systems differ in geometry or in
       layout_mode, which is the last term of the condition below. */
    bool chrome_stale = used_startup_ui;
    {
        int rbw, rbh, rmw, rmh;
        /* The LAYOUT is part of what makes the placeholder profile stale, not
           just the geometry: a .mgw whose core happened to report the same
           numbers the Game Boy placeholder was seeded with would still need
           the whole faceplate replaced. It cannot happen with the one core
           wired up today (gw answers 128x128 here, never 160x144), which is
           exactly why it is written down rather than relied on. */
        /* Assigned INSIDE the condition below, after core_get_geometry has
           filled rbw/rbh -- computing it here would read them uninitialised. */
        uint32_t rpar = prof_par;
        if (core_get_geometry(core, &rbw, &rbh, &rmw, &rmh) &&
            ((rpar = core_par(&cfg, core, rbw, rbh)) != prof_par ||
             rbw != prof.base_w || rbh != prof.base_h ||
             rmw != prof.max_w  || rmh != prof.max_h ||
             cfg.layout_mode != prof.layout_mode)) {
            koboy_profile real_prof;
            if (!config_resolve_profile_par(&real_prof, &cfg, pw, ph,
                                            rbw, rbh, rmw, rmh, rpar)) {
                /* A PER-ROM FAILURE, handled like every other one. This can
                   only be discovered after the core is open and the ROM is
                   loaded -- it is the core's own max geometry that does not
                   fit -- so it lands here rather than beside the other
                   refusals, but to the user it is the same event: this game
                   did not start. Ending the run for it would be the same
                   defect the loop exists to close, one screen later.

                   snprintf'd first because load_failed_recoverable takes a
                   ready-made reason, and this is the one caller whose reason
                   is composed here rather than by core.c. */
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

    /* ONE repaint, not two. The rect chrome was drawn against may have just
       changed shape or position, and a UI screen may have painted over it --
       both wanted the faceplate back, and doing them separately meant a
       system switch drew the OUTGOING game's faceplate for one full-panel
       refresh on its way to drawing the incoming one's. On e-ink that is a
       visible flash of the wrong console. */
    if (chrome_stale)
        redraw_chrome(pf, panel, panel_stride, pw, ph, &prof, &cfg.layout);

    /* Recorded HERE, once, for both entry points, and only now: "played"
       means the game actually started -- it survived the load AND the re-fit
       above, which is the last thing that can refuse it. Recording at pick
       time (where this used to live, twice) put a ROM that failed at the top
       of the RECENT list -- the wall the user just hit, promoted to row 0 of
       the screen they have to get past to try anything else. A row that fails
       is left in the list, deliberately: see the note above the session
       loop. */
    if (used_startup_ui) {
        koboy_recent rc;
        recent_load(&rc, recents_file);
        recent_touch(&rc, cfg.rom_path);
        recent_save(&rc, recents_file);
    }
    /* SECOND AND LATER SESSIONS ONLY -- any_game_ran is still describing the
       PREVIOUS one here, which is exactly what makes this the switch line.
       Without it koboy.log reads as one continuous run and the moment a
       different game took over is invisible; with fifteen systems that was
       also the only way to see which core a switch actually opened. It is
       printed after the load AND the re-fit, so the line means the new game
       started, not merely that it was picked -- tests/smoke_host.sh leans on
       exactly that. */
    if (any_game_ran) say("koboy: switched to %s\n", cfg.rom_path);

    /* From here to the teardown at the bottom of the loop, this session owns
       a loaded ROM: any later "nothing chosen" ends the RUN through its
       summary rather than exiting out from under it. */
    any_game_ran = true;

    /* ------------------------------------------------------- video, input */
    /* Logged, not merely applied. This is the setting that changes the most
       of what a colour system looks like, it is reachable from two places
       (koboy.ini and the in-game MENU), and the panel itself cannot tell you
       which mapping produced the frame you are looking at -- so koboy.log has
       to. Read back off the LIVE koboy_video rather than off cfg, which is
       what makes tests/smoke_host.sh's assertion end-to-end: it fails if
       main.c hands video_create the wrong value, not merely if config.c
       parses the wrong one. */
    koboy_video *vid = video_create(&prof, cfg.force_dither,
                                    (koboy_gray_map)cfg.gray_map);
    /* The quarter turn a golden-age arcade board asks for. Set here rather
       than inside video_create because the rotation belongs to the CORE and
       koboy_profile belongs to the config -- config_resolve_profile is called
       from tests that have no core at all. prof was already resolved from
       core_get_geometry's transposed answer, so the two agree by
       construction; every later rebuild below repeats both halves together
       for the same reason. */
    if (vid) video_set_rotation(vid, (int)core_rotation(core));
    /* Paired with the rotation for the same reason it is paired with it in
       every rebuild below: both are facts the core announced about how its
       frames are to be PRESENTED, both are lost when a koboy_video is
       destroyed, and a presentation that has one without the other is wrong
       in a way that looks like a broken core rather than a missing line. */
    if (vid) video_set_aspect(vid, core_aspect(&cfg, core));
    if (vid) say("koboy: gray_map %s\n", video_gray_map_name(video_get_gray_map(vid)));
    /* Both halves of the MOTION pair, read back off the LIVE pipeline and the
       LIVE platform rather than off cfg -- same trick as the line above and
       the pacer's below. A line printed from cfg would prove the parser; this
       one fails if video_create ignored force_dither or the backend never got
       the policy, which are the two ways this setting can be on in the file
       and off on the panel. */
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
            notify("Save file unreadable.\nStarting fresh.\nSaving is OFF this run.");
            /* The message drew over the faceplate; put it back. */
            redraw_chrome(pf, panel, panel_stride, pw, ph, &prof, &cfg.layout);
        }
    } else {
        say("koboy: cartridge has no save RAM\n");
    }

    /* ------------------------------------------------------------- the loop */
    koboy_pacer pace;
    /* Paced at the rate THIS core reports, not at the Game Boy's 59.7275 Hz
       that KOBOY_FRAME_US hardcodes -- see docs/FOLLOWUPS.md #38 and #57,
       where two FinalBurn Neo boards report 30 fps and were therefore running
       at double speed. Both numbers are logged, the raw one and the resolved
       one, because a core whose fps is refused by the plausibility bound in
       pacer_frame_us_from_fps is otherwise indistinguishable on a device from
       one that genuinely runs at 59.7275. */
    double core_hz = core_fps(core);
    uint32_t frame_us = pacer_frame_us_from_fps(core_hz);
    say("koboy: core reports %.4f fps; pacing at %u us/frame\n", core_hz, frame_us);
    pacer_init(&pace, pf->now_us(pf->ctx), cfg.present_divisor, frame_us);
    /* Read back off the LIVE pacer, not off cfg, for the reason the gray_map
       line above it is: a log line that reports what config.c parsed proves
       config.c, while this one fails if main.c hands the pacer the wrong
       value -- which is the plumbing nothing else can see. It is also the
       only place the clamp inside pacer_set_divisor is observable. */
    say("koboy: present_divisor %d\n", pace.divisor);
    /* Printed unconditionally, INCLUDING the disabled 0/0 case, because "did
       the throttle engage" is the first question any report about scrolling
       will ask and a line that only appears when it is on cannot answer it in
       the negative. The full-rect figure is spelled out rather than left to
       be recomputed from the two halves: it is the number that has to be
       compared against 1000/present_divisor*frame_ms to see whether the
       throttle can bind at all. */
    say("koboy: settle model %d ms + %d ms/full rect (%u us at a full %dx%d)\n",
        cfg.settle_base_ms, cfg.settle_full_ms,
        pacer_settle_us((uint32_t)cfg.settle_base_ms * 1000u,
                        (uint32_t)cfg.settle_full_ms * 1000u, 1, 1),
        prof.game_w, prof.game_h);

    /* Re-anchored per session, not reset: both are wall-clock marks for
       "how long since the last flush / cleanup", and a session that starts
       after a minute in the menus must not immediately believe both are
       overdue. The COUNTERS they gate live outside the loop, with the rest of
       the run's numbers. */
    last_sram_us = pf->now_us(pf->ctx);
    last_cleanup_us = last_sram_us;

    /* mode != MODE_QUIT joins g_stop and should_quit() as a third way out: the
       in-game menu sets it (QUIT, or CHOOSE ROM leaving nothing loaded) from
       inside the loop body below. Kept separate from g_stop on purpose -- that
       flag is the signal handler's, set from outside any call frame, and
       reusing it for a menu-driven exit would make the final status line call
       a chosen QUIT "stopped by signal". */
    while (mode != MODE_QUIT && mode != MODE_MAIN &&
           !g_stop && !pf->should_quit(pf->ctx)) {
        /* frames_done + pace.frames, not pace.frames: --frames N is a budget
           for the RUN, and each session gets a fresh pacer (see frames_done's
           declaration). */
        if (frame_limit && frames_done + pace.frames >= frame_limit) break;

        /* Poll EVERY core iteration (60Hz), not once per presented frame.
           Polling only on presentation would drop short presses and add up to
           50ms of latency on top of the panel's own. */
        pf->poll_input(pf->ctx, in);

        /* The in-game MENU is the one screen no tap on a previous screen leads
           to: it is entered by ASKING, and the ask is a touch zone this loop
           polls. So --ui-script gets a verb of its own for it, and this is
           where that verb is consumed -- the emulator loop, not run_list.

           This closes docs/FOLLOWUPS.md #47, which was filed against
           MENU_GRAY's handler and applies word for word to every other branch
           below it: each one was verified by READING, because the scripted
           path -- the only path an automated test can take -- could not get
           here at all. That is the v1 first-run-deadlock shape exactly ("every
           automated test took the one path that could not reach the bug"), and
           it had already collected five inhabitants.

           The real request is tested FIRST and the marker only consumed if it
           did not fire, so a scripted run that somehow also sees a live MENU
           tap spends one, not both. */
        bool want_menu = input_take_menu_request(in);
        if (!want_menu && ui_scr) {
            /* Step over the cleared states a list screen left behind before
               looking for a marker -- uiscript_state_is_idle says why, and
               why this cannot swallow a tap. Without it a script can open the
               in-game MENU exactly once: every screen selects on touch-down
               and leaves the release, so the cursor parks on that release and
               nothing here ever moves it again. A second `menu` verb was
               unreachable, and so was any test that switches games twice --
               which is the one shape the session loop most needs tested. */
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
            /* Read off the DIRECTORY every time the menu opens, not carried
               in a variable: the number in the row is the file that is about
               to be written, and after a relaunch (or a shot deleted over
               USB) an in-memory counter would name a file that is not the
               next one. One opendir per menu open costs nothing a human
               opening a menu can perceive. */
            char shot_stem[96];
            shot_stem_for_rom(shot_stem, sizeof shot_stem, cfg.rom_path);
            int shot_next = shot_last_seq(cfg.shot_dir, shot_stem) + 1;

            int act = run_menu(pf, in, panel, panel_stride, pw, ph, ssz > 0,
                               (koboy_gray_map)cfg.gray_map, cfg.present_divisor,
                               cfg.force_dither,
                               (koboy_wfm_policy)cfg.wfm_fast_policy, shot_next,
                               ui_scr, ui_scr_i, ui_script_n);

            if (act == MENU_SAVE || act == MENU_LOAD) {
                int slot = run_slot_picker(pf, in, panel, panel_stride, pw, ph,
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
            } else if (act == MENU_GRAY) {
                /* Cycles, and returns to the GAME rather than reopening the
                   menu. That is the point: this is a subjective judgement
                   about how a reflective panel looks, and it can only be made
                   while looking at the game. Two taps to advance one step is
                   the price of seeing the step.

                   It takes effect on the very next presented frame, not next
                   launch: video_set_gray_map rebuilds the LUT here, and the
                   return-from-menu path below already does redraw_chrome +
                   video_invalidate unconditionally. Without that invalidate
                   the dirty diff would leave every unchanged tile carrying
                   pixels from the OLD mapping -- half-old, half-new, and on
                   e-ink it persists until something else happens to touch
                   those tiles. */
                cfg.gray_map = (cfg.gray_map + 1) % KOBOY_GRAY_COUNT;
                video_set_gray_map(vid, (koboy_gray_map)cfg.gray_map);
                /* The ini key and this menu are ONE setting. A failure here
                   is not fatal and must not be silent: the mapping is live
                   for this session either way, it just will not survive a
                   relaunch (a read-only .adds, most likely). */
                if (config_save_gray_map(ini_path, (koboy_gray_map)cfg.gray_map))
                    say("koboy: gray_map = %s\n",
                        video_gray_map_name((koboy_gray_map)cfg.gray_map));
                else
                    say("koboy: gray_map = %s (this session only -- "
                        "could not write %s)\n",
                        video_gray_map_name((koboy_gray_map)cfg.gray_map), ini_path);
            } else if (act == MENU_FRAMES) {
                /* Cycles and returns to the GAME, exactly like GREYSCALE above
                   and for the same reason: how many updates per second an
                   e-ink panel wants is a judgement about smearing against
                   choppiness, and it can only be made while looking at the
                   game in motion.

                   THE LADDER GOES ABOVE THE DEFAULT, which is the point of
                   this entry rather than an afterthought. Residue accumulates
                   per panel UPDATE, so the untested direction -- fewer
                   updates -- is the one the evidence points at, and every
                   value ever tried was 3 or below (docs/FOLLOWUPS.md #26).

                   No pacer_rebase and no video_invalidate needed here beyond
                   what the return-from-menu path below already does: the
                   divisor changes WHICH core frames reach the panel, not what
                   any of them contain and not when the core runs, so nothing
                   half-drawn can survive it. The unconditional redraw_chrome
                   + video_invalidate below is what puts a whole frame back on
                   the panel after the menu, and it runs whatever was picked.

                   Live immediately: pacer_set_divisor changes the running
                   pacer, so the very next presented frame is already on the
                   new pacing -- no reload, and nothing stale in between,
                   because the pacer holds no per-divisor state other than the
                   divisor itself. */
                cfg.present_divisor = config_next_present_divisor(cfg.present_divisor);
                pacer_set_divisor(&pace, cfg.present_divisor);
                /* One setting, two doors. A failure to write is not fatal and
                   must not be silent -- the new pacing is live for this
                   session either way, it just will not survive a relaunch
                   (a read-only .adds being the likely cause). */
                if (config_save_present_divisor(ini_path, cfg.present_divisor))
                    say("koboy: present_divisor = %d\n", cfg.present_divisor);
                else
                    say("koboy: present_divisor = %d (this session only -- "
                        "could not write %s)\n", cfg.present_divisor, ini_path);
            } else if (act == MENU_MOTION) {
                /* Cycles and returns to the GAME, exactly like GREYSCALE and
                   FRAMES above and for the same reason: whether a dithered
                   1-bit picture under a two-level waveform smears LESS than
                   four greys under AUTO is a judgement about a reflective
                   panel in motion, and no framebuffer measurement can make
                   it -- residue is panel-side and koboy's dirty diff only
                   ever sees what koboy itself wrote.

                   ONE ROW, TWO KEYS, because the thing being tested is the
                   PAIR. FBInk's header says a DU-class waveform leaves
                   on-screen pixels as-is for new content that is not black
                   or white, so four-level content under DU is the forced-DU4
                   experiment that already failed here, and 1-bit content
                   under AUTO gives the driver less to work with rather than
                   more. config_next_motion holds the ladder and the argument
                   for its three rungs.

                   BOTH HALVES GO LIVE HERE, and each needs its own call:
                   video_set_dither changes what the next frame CONTAINS,
                   pf->set_wfm_policy changes how the panel is asked to draw
                   it, and a setting where only one of the two moved would be
                   a combination the owner never picked. The unconditional
                   redraw_chrome + video_invalidate below is what makes the
                   change whole-frame rather than half-old: without it the
                   dirty diff would leave untouched tiles carrying the
                   previous rendering, which on e-ink persists until
                   something else writes them. */
                {
                    bool             nd = cfg.force_dither;
                    koboy_wfm_policy nw = (koboy_wfm_policy)cfg.wfm_fast_policy;
                    config_next_motion(&nd, &nw);
                    cfg.force_dither    = nd;
                    cfg.wfm_fast_policy = (int)nw;
                    video_set_dither(vid, nd);
                    /* Optional in the seam (platform_if.h), so null-checked --
                       both shipped backends implement it, and a future one
                       that cannot change waveforms should degrade to "the
                       dithering half still works" rather than crash. */
                    if (pf->set_wfm_policy) pf->set_wfm_policy(pf->ctx, nw);
                    /* Read back off the LIVE pipeline and the LIVE platform,
                       not off cfg, for the reason the gray_map and
                       present_divisor lines do it: a line reporting what this
                       branch just assigned proves this branch, while these
                       fail if either setter never landed. */
                    const char *wn = pf->wfm_fast_name ? pf->wfm_fast_name(pf->ctx)
                                                       : config_wfm_policy_name(nw);
                    /* One choice, two keys, ONE write. A failure is not fatal
                       and must not be silent: the pair is live for this
                       session either way, it just will not survive a relaunch
                       (a read-only .adds, most likely). */
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
                /* ARMS a capture; it does not take one. THE MENU IS DRAWN
                   OVER THE GAME -- run_list paints the whole panel white and
                   renders the list into it -- so a screenshot taken from
                   here would be a photograph of this menu, which is not what
                   anybody opening it wants.

                   WHAT "THE NEXT FRAME" MEANS, given that present_divisor and
                   the settle pacer can each defer presentation indefinitely:
                   the capture site sits after the blit/refresh loop, so it
                   fires on the next frame that actually REACHES THE PANEL,
                   however many core frames that takes. That frame is
                   guaranteed to arrive rather than being suppressed as
                   unchanged, because the return-from-menu path below calls
                   video_invalidate: the next submit is full-dirty by
                   construction, so nrects is non-zero even if the game is
                   sitting on a static screen.

                   And it is a COMPLETE panel, not a dirty-rect fragment:
                   shot_compose builds it from the whole chrome buffer plus
                   the whole game rect (video.c's buffer always holds the
                   entire rect, not just the changed tiles), so what is saved
                   is what the panel shows even when only eight pixels of it
                   were updated this frame.

                   Nothing is written here and nothing is said here: a run
                   that arms and then quits before a frame is presented leaves
                   no file, which is correct -- there was no frame to take. */
                shot_armed = true;
            } else if (act == MENU_QUIT) {
                mode = MODE_QUIT;
            } else if (act == MENU_CHOOSE_ROM) {
                /* ALL of it is torn down and rebuilt: this ends the session
                   and the session loop picks the next ROM, opens a core for
                   ITS extension, and derives the faceplate, the buttons, the
                   ceiling and the save binding from it.

                   What used to be here was a hundred lines of second picker
                   that reloaded into the LIVE core, and it killed a device: a
                   Mega Drive .md handed to gpSP faulted executing it as ARM
                   code. The core was never the only stale thing -- it was
                   simply the one that crashed. See the session loop's own
                   comment.

                   No teardown here either. The flush-before-unload ordering
                   that used to live in this branch is now the loop's single
                   teardown, which every way out of a session goes through --
                   QUIT, a signal, the frame limit and this. One copy cannot
                   disagree with itself. */
                mode = MODE_MAIN;
                break;
            }

            /* Whatever happened, the panel is now showing a menu. */
            redraw_chrome(pf, panel, panel_stride, pw, ph, &prof, &cfg.layout);
            video_invalidate(vid);
            /* That repaint already took any screenshot plaque with it, so the
               pending erase has nothing left to erase. Left set, it would
               spend a panel update rubbing out something that is not there.
               It is also what keeps a plaque out of the NEXT capture: the
               only route to a second screenshot goes through this menu, so
               `panel` is guaranteed to be plaque-free by the time
               shot_compose reads it. Two shots of a static screen are
               therefore byte-identical, which tests/smoke_host.sh asserts. */
            shot_note_until_us = 0;
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
            /* And charge the redraw_chrome above, which repainted the WHOLE
               panel. Rebasing does not do this and must not: hold_until_us is
               an absolute wall-clock mark, so a menu open for thirty seconds
               leaves it long expired -- correctly, because the panel finished
               that work thirty seconds ago. What it did NOT finish is the
               repaint we just issued, and the first frame back from a menu is
               exactly where a collision would be most visible: everything on
               screen changed at once. A full-rect charge is a lower bound on a
               full-PANEL repaint, which is the right direction to be wrong in. */
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
            /* Timing first, and unconditionally, for the same reason the
               rotation below is unconditional: SET_SYSTEM_AV_INFO can move
               the frame rate without moving a single one of the four numbers
               the base/max comparison below looks at, so a rate change would
               otherwise be seen only when it happened to arrive alongside a
               resize. pacer_set_frame_us is a no-op when the rate has not
               changed, which is the overwhelmingly common case (only the
               Game & Watch core sends these commands at all today, and it
               sends the same 60 fps every time). */
            pacer_set_frame_us(&pace, pf->now_us(pf->ctx),
                               pacer_frame_us_from_fps(core_fps(core)));
            /* Rotation first, and unconditionally, because it is the one
               announcement that can arrive WITHOUT the numbers moving: a
               square frame turned a quarter turn is the same width and
               height, so the base/max comparison below would see nothing to
               do and the pipeline would keep presenting the old orientation
               forever. Cheap when nothing changed (an int compare), and when
               something did the whole picture moves, so prev is worthless --
               hence the invalidate, which is exactly the obligation
               video_set_rotation's own comment hands to a caller that flips
               it on a live pipeline. The rebuild path below re-applies it
               anyway; that is a harmless repeat, not a second mechanism. */
            if (video_get_rotation(vid) != (int)core_rotation(core)) {
                video_set_rotation(vid, (int)core_rotation(core));
                video_invalidate(vid);
            }
            /* Same shape, same reason: a core can re-announce its aspect
               without moving base or max (SET_GEOMETRY carries aspect_ratio
               too), and every pixel of the fit moves when it does -- hence the
               invalidate, which prev would otherwise make a half-old frame
               out of. Cheap when nothing changed: one uint32 compare. */
            if (video_get_aspect(vid) != core_aspect(&cfg, core)) {
                video_set_aspect(vid, core_aspect(&cfg, core));
                video_invalidate(vid);
            }
            int rbw, rbh, rmw, rmh;
            /* RESOLVE FIRST, THEN COMPARE THE ANSWER -- not the inputs.

               This used to test which INPUT had moved and skip the whole
               rebuild for a base-only change, on the reasoning that the rect,
               the chrome and video's buffers were all sized from max. Two
               thirds of that is still true (the buffers are max-sized, and
               the LCD rect still is), but the DMG rect is now sized from BASE
               (config_resolve_profile_par), so a base change there really can
               move the rect and the input test would leave the faceplate
               drawn around the wrong one.

               The cost that test existed to avoid has not gone away, and it
               is not theoretical: a Game & Watch title alternates
               654x396 <-> 305x191 several times a second, and a
               video_destroy/video_create + chrome redraw + forced full-rect
               refresh at that rate is what the device log showed. So the
               skip is kept -- keyed on the RESOLVED PRESENTATION being
               identical rather than on which field changed. That is both
               safer and stricter: it catches a base change that does move the
               rect (SNES dropping into a 512-wide hi-res mode) and still
               skips one that does not (Game & Watch, whose LCD rect comes
               from max; PC Engine's 256 <-> 352 switch, whose display width
               is the same in both modes so the rect is byte for byte the
               same). The Game Boy never reaches any of this: base == max ==
               160x144 and gambatte never sends these commands at all. */
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
                /* Nothing a rebuild would change: same rect in the same
                   place, same scale, and the same max the buffers were
                   allocated from. Record what the core is rendering now --
                   base is what changed, and the log and video's own fit both
                   want it current -- and keep going. */
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
                vid = video_create(&prof, cfg.force_dither,
                                   (koboy_gray_map)cfg.gray_map);
                /* Re-applied, not carried over: video_destroy took the old
                   rotation with it, and a rotation CHANGE is one of the two
                   things that can have brought us here (core.c sets
                   geom_dirty for it, precisely so this rebuild happens). A
                   WonderSwan toggling its display orientation mid-session is
                   the case that exercises it. */
                if (vid) video_set_rotation(vid, (int)core_rotation(core));
                if (vid) video_set_aspect(vid, core_aspect(&cfg, core));
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

        /* now_us is read ONCE and handed to pacer_tick rather than letting the
           pacer call the platform: pacer.c has no platform dependency and must
           keep none -- it is pure enough to test against a synthetic clock,
           which is the only way the settle hold can be asserted at all
           (tests/test_pacing.c). */
        bool present = pacer_tick(&pace, pf->now_us(pf->ctx));
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

        /* AREA-AWARE PACING: charge the panel time this update just cost, so
           the next divisor-eligible frame is vetoed until the panel has had a
           chance to finish it.

           Charged from dirty_px -- the SAME sum config_promote_full is given a
           few lines up -- rather than from the game rect, because that is the
           whole point: a two-tile sprite move gets base alone and keeps
           present_divisor's rate exactly, while a full-screen scroll gets
           base + full and drops to whatever the panel can actually complete.
           A run where every present is a sprite move is bit-identical to one
           without this call.

           AFTER the refresh loop, not before it: the hold measures from when
           the panel was handed the work, and pf->refresh is non-blocking, so
           `now` here is within a millisecond of the submission it is timing --
           whereas taking it before the blit would charge the panel for
           koboy's own blit time as well.

           Note what is NOT here: no band-splitting, no dropping part of a
           frame. The update that goes out is the whole update; only the NEXT
           one waits. Splitting a scroll across frames trades a flash for
           tearing, and tearing on a scroll is worse. */
        pacer_presented(&pace, pf->now_us(pf->ctx),
                        pacer_settle_us((uint32_t)cfg.settle_base_ms * 1000u,
                                        (uint32_t)cfg.settle_full_ms * 1000u,
                                        dirty_px,
                                        (long)prof.game_w * (long)prof.game_h));
        presented++;
        rects_emitted += (unsigned long)nrects;

        /* ------------------------------------------------ the SCREENSHOT
           HERE, and not one line earlier, is what makes this the shot the
           owner asked for. The frame has been composited, blitted and handed
           to the panel; the game is on screen and the menu is gone. The file
           is written BEFORE anything is drawn to confirm it, so the
           confirmation cannot end up in the picture.

           shot_capture builds the whole panel itself, because koboy never
           has it in one place: `panel` is the faceplate and video's buffer is
           the game, blitted separately at an offset. Both are complete
           regardless of how little of this frame was dirty, so a capture
           taken on a frame where two tiles moved is still the entire
           picture -- which is the point.

           After pacer_presented, not before: shot_capture allocates 2 MB,
           composites and writes a file, and charging the panel's settle time
           from a clock reading taken after all that would tell the pacer the
           update finished later than it did. */
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
                /* Not fatal and not silent, exactly like a failed ini write:
                   the game is unaffected, but a screenshot that silently went
                   nowhere is indistinguishable from a feature that does not
                   work. The directory is named because that is nearly always
                   the reason -- a read-only .adds, or a full card. */
                say("koboy: screenshot FAILED (could not write into %s)\n",
                    cfg.shot_dir);
                snprintf(msg, sizeof msg, "SCREENSHOT FAILED");
            }
            /* And now, with the file already on disk, say so on the panel.
               shot_note_rect returns false when there is no band to put it
               in, and then the log line above is the only report -- see its
               comment for why a plaque over the controls would be worse. */
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
                   residue, and text that ghosts where it was is exactly what
                   this must not leave behind on a panel the owner goes on
                   playing on. It is a small rectangle, so the flash is
                   local. */
                pf->refresh(pf->ctx, shot_note.x, shot_note.y,
                            shot_note.w, shot_note.h, KOBOY_REFRESH_FULL);
                uint64_t now = pf->now_us(pf->ctx);
                /* Charged, because the panel does not care which line of this
                   file asked it for an update -- the same reasoning the
                   cleanup refresh below carries. */
                pacer_presented(&pace, now,
                                pacer_settle_us((uint32_t)cfg.settle_base_ms * 1000u,
                                                (uint32_t)cfg.settle_full_ms * 1000u,
                                                (long)shot_note.w * shot_note.h,
                                                (long)prof.game_w * (long)prof.game_h));
                shot_note_until_us = now + (uint64_t)SHOT_NOTE_MS * 1000ull;
            }
        }

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
            /* And charged, because the panel does not care which line of this
               file asked. A cleanup is by definition a WHOLE-rect update, so
               it costs the full settle; not charging it would let the next
               presented frame land on top of the one update in the whole loop
               that is guaranteed to be the most expensive. This overwrites the
               charge the presented frame just made rather than adding to it --
               the panel is doing one thing at a time and the flash is what it
               is now doing. */
            pacer_presented(&pace, last_cleanup_us,
                            pacer_settle_us((uint32_t)cfg.settle_base_ms * 1000u,
                                            (uint32_t)cfg.settle_full_ms * 1000u,
                                            1, 1));
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

        /* Take the screenshot plaque away again. Here, past the sram_check
           label, because this is the one point EVERY path through the loop
           body reaches: the early exits above (`goto sram_check` on a
           suppressed present, on an unchanged frame) must not be able to
           leave a confirmation stuck on the panel for the rest of the
           session. Wall clock, not a frame count, for the same reason the
           cleanup has a wall-clock ceiling -- a static screen presents almost
           nothing and would keep the plaque up for minutes.

           The erase re-renders the whole faceplate into `panel` and then
           blits back ONLY the plaque's rectangle. That is exact by
           construction -- whatever chrome had drawn under it returns byte for
           byte, without this code knowing what it was -- and the panel update
           stays the size of the plaque rather than the size of the screen. */
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
       the frame limit, and MENU -> CHOOSE ROM -- so there is exactly one copy
       of this ordering and nothing can disagree with it.

       THE ORDER IS THE WHOLE POINT, and two of the three steps are load-
       bearing:

         - The SRAM flush comes BEFORE core_close, because retro_unload_game
           takes the buffer with it: sb.mem belongs to the core's cartridge
           and is freed by the close. Flushing after would write freed memory
           to the user's save file, or crash.
         - sb is cleared right after, so a session that ends without ever
           loading (a failed load coming round again) cannot leave a dangling
           pointer for the NEXT trip's flush to find.
         - video and input are built from `prof`, which the next session
           re-resolves for its own core. Reusing them across a system switch
           is how a Mega Drive frame would land in a buffer sized for a Game
           Boy; destroying them here is what makes the rebuild above
           unconditional rather than something to remember. */
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
       a build where the hold never binds and a build without the hold at all
       print the same presented= count, and differ only in this number. It is a
       running total across every session in the process, like `presented`. */
    say("koboy: %s, %lu presented frames, %lu settle-held, %lu game-rect cleanups, "
        "%lu large-area full refreshes, %lu rects emitted\n",
        g_stop ? "stopped by signal" : "stopped", presented,
        settle_held, cleanups, big_refreshes, rects_emitted);
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

    /* The core, the video pipeline and the input state were released by the
       session teardown above -- every path to this label goes through it, or
       (the "nothing chosen" gotos) never had them. */
    free(panel);
    pf->shutdown(pf->ctx);
    free(pf);
    return 0;
}
