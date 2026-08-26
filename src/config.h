#ifndef KOBOY_CONFIG_H
#define KOBOY_CONFIG_H
#include "koboy.h"

/* What v1 shipped uncommented in config/koboy.ini, back when gambatte was the
   only core. An ini that names exactly this is indistinguishable from one that
   was never edited, so config.c does NOT treat it as an explicit pick. See the
   long comment at the `core=` parse site. */
#define KOBOY_CORE_LEGACY_DEFAULT "gambatte_libretro.so"

typedef struct {
    int      scale;              /* 0 = pick the largest that fits */
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

bool config_join_sibling(char *out, size_t n, const char *name, const char *dir);
bool config_exe_dir(char *out, size_t n);
void config_resolve_paths(koboy_config *c);
#endif
