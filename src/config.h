#ifndef KOBOY_CONFIG_H
#define KOBOY_CONFIG_H
#include "koboy.h"

typedef struct {
    int      scale;              /* 0 = pick the largest that fits */
    int      present_divisor;    /* core frames per presented frame */
    int      cleanup_interval;   /* presented frames between game-rect cleanups */
    int      cleanup_max_ms;     /* wall-clock ceiling between cleanups; <=0 off */
    int      wfm_fast_policy;    /* koboy_wfm_policy for KOBOY_REFRESH_FAST */
    int      full_refresh_permille; /* dirty area (permille of game rect) above
                                       which a frame is refreshed with FULL
                                       instead of FAST; <= 0 disables */
    bool     force_dither;
    bool     grab_input;
    int      dpad_mode;
    int      dpad_deadzone;      /* px */
    int      dpad_hysteresis;    /* px */
    uint16_t key_a, key_b;       /* 0 = not yet calibrated */
    char     rom_path[512];
    char     core_path[512];
    char     save_dir[512];
    koboy_layout layout;
} koboy_config;

void config_defaults(koboy_config *c);
bool config_load(koboy_config *c, const char *path);
bool config_resolve_profile(koboy_profile *p, const koboy_config *c,
                            int panel_w, int panel_h);
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
bool config_join_sibling(char *out, size_t n, const char *name, const char *dir);
bool config_exe_dir(char *out, size_t n);
void config_resolve_paths(koboy_config *c);
#endif
