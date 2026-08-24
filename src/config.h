#ifndef KOBOY_CONFIG_H
#define KOBOY_CONFIG_H
#include "koboy.h"

typedef struct {
    int      scale;              /* 0 = pick the largest that fits */
    int      present_divisor;    /* core frames per presented frame */
    int      cleanup_interval;   /* presented frames between game-rect cleanups */
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
#endif
