#ifndef KOBOY_CONFIG_H
#define KOBOY_CONFIG_H
#include "koboy.h"

/* What v1 shipped uncommented in config/koboy.ini, back when gambatte was the
   only core. An ini that names exactly this is indistinguishable from one that
   was never edited, so config.c does NOT treat it as an explicit pick. See the
   long comment at the `core=` parse site. */
#define KOBOY_CORE_LEGACY_DEFAULT "gambatte_libretro.so"

/* The same trap, one field over. v1 shipped `scale = 5` uncommented when the
   Game Boy was the only system, so an ini naming 5 records packaging rather
   than preference and does NOT mark the scale explicit. See the `scale=` parse
   site and config_resolve_profile. */
#define KOBOY_SCALE_LEGACY_DEFAULT 5

typedef struct {
    int      scale;              /* 0 = pick the largest that fits */
    bool     scale_explicit;     /* the user named a scale, as opposed to
                                    config_defaults' Game-Boy 5. Without this
                                    a 96x64 Pokemon Mini rendered at 480x320 --
                                    a postage stamp on a 1264x1680 panel --
                                    because a number measured for the Game Boy
                                    was being applied to every system. See
                                    config_resolve_profile. */
    int      present_divisor;    /* core frames per presented frame */
    int      cleanup_interval;   /* presented frames between game-rect cleanups */
    int      cleanup_max_ms;     /* wall-clock ceiling between cleanups; <=0 off */
    int      wfm_fast_policy;    /* koboy_wfm_policy for KOBOY_REFRESH_FAST */
    int      full_refresh_permille; /* dirty area (permille of game rect) above
                                       which a frame is refreshed with FULL
                                       instead of FAST; <= 0 disables */
    int      refresh_fixed_tiles; /* fixed per-refresh cost, in 8x8 tiles, used
                                     by video_split_dirty's cost model. A
                                     config key rather than a constant because
                                     every absolute timing this project has
                                     measured moved by up to a factor of 2.2
                                     between sessions -- see config/koboy.ini. */
    bool     force_dither;
    bool     grab_input;
    int      dpad_mode;
    int      dpad_deadzone;      /* px */
    int      dpad_hysteresis;    /* px */
    uint16_t key_a, key_b;       /* 0 = not yet calibrated */
    uint16_t key_start, key_select; /* 0 = not yet calibrated; gamepad shoulder
                                        buttons by default, see config_defaults */
    char     rom_path[512];
    char     rom_dir[512];       /* where the browser looks; install-relative */
    char     core_path[512];
    bool     core_explicit;      /* the user named a core (ini `core=` or
                                    --core), as opposed to config_defaults'
                                    gambatte fallback. Needed because the
                                    default is written into core_path
                                    unconditionally, so the string alone
                                    cannot say whether anyone asked for it --
                                    and only an unasked-for core may be
                                    overridden by the ROM's extension. */
    char     save_dir[512];
    /* koboy_layout_mode. NOT an ini key and not a user preference: it is a
       fact about the loaded system, derived from the ROM's own extension by
       config_layout_for_rom the same way the core is by config_core_for_rom,
       and written here by main.c before config_resolve_profile reads it.
       config_defaults leaves it at KOBOY_LAYOUT_DMG (0), which is what every
       caller that never sets it -- every existing test, and the placeholder
       profile main.c resolves before a ROM has been chosen -- gets. */
    int      layout_mode;
    koboy_layout layout;
} koboy_config;

void config_defaults(koboy_config *c);
bool config_load(koboy_config *c, const char *path);

/* base_w/base_h/max_w/max_h come from core_get_geometry (src/core.h), queried
   once after retro_load_game -- config.c has no core.h dependency of its own
   (layering: config is lower-level than core), so the caller resolves the
   query and passes the four numbers through as plain ints rather than this
   header taking on a struct koboy_core it does not otherwise need. max_w and
   max_h drive the scale search and the resulting game rect (see the
   koboy_profile comment in koboy.h for why max, not base); both must be >= 1
   or this returns false the same way an impossibly small panel already does.
   base_w/base_h are carried into the profile unchanged, for callers that want
   to know what the core is actually rendering right now. */
bool config_resolve_profile(koboy_profile *p, const koboy_config *c,
                            int panel_w, int panel_h,
                            int base_w, int base_h, int max_w, int max_h);
bool config_save_keys(const char *path, uint16_t key_a, uint16_t key_b);

/* Should a frame whose dirty rect covers dirty_px of a whole_px game rect be
   promoted from the fast waveform to the flashing one? Lives here, and is
   tested, because the shipped default turns the promotion off and "off" is only
   trustworthy if the comparison itself is pinned: a >= that became a > would
   quietly reintroduce flashing with nothing failing. */
bool config_promote_full(const koboy_config *c, long dirty_px, long whole_px);

/* Resolve a slashless core/rom/save path against the directory containing the
   running executable. See the long comment in config.c: dlopen() never looks in
   the cwd for a name with no slash, so a bare core name could not be found on
   the device at all. */
/* Which core should load this ROM? Returns a bare core filename (no slash) --
   config_resolve_paths' sibling-join turns it into an absolute path later, for
   the reason dlopen documents and this project learned the hard way. */
const char *config_core_for_rom(const char *rom_path);

/* Which PRESENTATION should this ROM get -- a koboy_layout_mode. Same
   predicate as config_core_for_rom (the extension, and nothing else), and
   deliberately a second function rather than a second return value from that
   one: the core can be overridden by an explicit `core=` or --core, and the
   layout must NOT follow that override. A .mgw is a Game & Watch unit whose
   buttons are drawn into its own artwork whichever shared object ends up
   interpreting it, so naming a core by hand cannot make a DMG faceplate the
   right answer for it. */
int config_layout_for_rom(const char *rom_path);

/* Fill or clear the DMG faceplate's EXTRA discs for this ROM -- see
   koboy_extra_btn in koboy.h. A third function rather than more return values
   from the two above for the same reason those two are separate: the core can
   be overridden by `core=` or --core, and neither the layout nor the physical
   button complement may follow that override. A Pokemon Mini cartridge has a
   C button, and a WonderSwan an A and a B that land on L1/R1 once the screen
   is rotated, whatever shared object ends up interpreting them. Idempotent,
   and it CLEARS as well as sets, because the config outlives one game. */
void config_extra_buttons_for_rom(koboy_layout *l, const char *rom_path);

bool config_join_sibling(char *out, size_t n, const char *name, const char *dir);
bool config_exe_dir(char *out, size_t n);
void config_resolve_paths(koboy_config *c);
#endif
