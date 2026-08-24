#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void config_defaults(koboy_config *c)
{
    memset(c, 0, sizeof *c);
    c->scale = 5;
    c->present_divisor = 3;
    c->cleanup_interval = 200;
    c->grab_input = true;
    c->dpad_mode = KOBOY_DPAD_RELATIVE;
    c->dpad_deadzone = 24;
    c->dpad_hysteresis = 10;
    snprintf(c->core_path, sizeof c->core_path, "gambatte_libretro.so");
    snprintf(c->save_dir, sizeof c->save_dir, ".");
    /* Control geometry, permille of panel. Game rect occupies the top; the
       d-pad sits lower-left under the left thumb, A/B lower-right. */
    koboy_layout l = { .dpad_cx = 220, .dpad_cy = 720, .dpad_r = 150,
                       .a_cx = 830, .a_cy = 670, .a_r = 85,
                       .b_cx = 660, .b_cy = 760, .b_r = 85,
                       .start_cx = 610, .start_cy = 920, .start_w = 200, .start_h = 55,
                       .select_cx = 390, .select_cy = 920, .select_w = 200, .select_h = 55 };
    c->layout = l;
}

static void trim(char *s)
{
    char *p = s; while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r' || s[n-1] == '\n')) s[--n] = 0;
}

static bool as_bool(const char *v) { return !(strcmp(v,"false")==0 || strcmp(v,"0")==0); }

bool config_load(koboy_config *c, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return true;                 /* absent file: defaults stand */
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#'); if (hash) *hash = 0;
        char *eq = strchr(line, '=');    if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        trim(k); trim(v);
        if      (!strcmp(k, "scale"))            c->scale = atoi(v);
        else if (!strcmp(k, "present_divisor"))  c->present_divisor = atoi(v);
        else if (!strcmp(k, "cleanup_interval")) c->cleanup_interval = atoi(v);
        else if (!strcmp(k, "force_dither"))     c->force_dither = as_bool(v);
        else if (!strcmp(k, "grab_input"))       c->grab_input = as_bool(v);
        else if (!strcmp(k, "dpad_deadzone"))    c->dpad_deadzone = atoi(v);
        else if (!strcmp(k, "dpad_hysteresis"))  c->dpad_hysteresis = atoi(v);
        else if (!strcmp(k, "dpad_mode"))        c->dpad_mode = strcmp(v,"cross") ? KOBOY_DPAD_RELATIVE : KOBOY_DPAD_CROSS;
        else if (!strcmp(k, "key_a"))            c->key_a = (uint16_t)atoi(v);
        else if (!strcmp(k, "key_b"))            c->key_b = (uint16_t)atoi(v);
        else if (!strcmp(k, "rom"))              snprintf(c->rom_path,  sizeof c->rom_path,  "%s", v);
        else if (!strcmp(k, "core"))             snprintf(c->core_path, sizeof c->core_path, "%s", v);
        else if (!strcmp(k, "save_dir"))         snprintf(c->save_dir,  sizeof c->save_dir,  "%s", v);
        /* unknown keys ignored on purpose: forward compatibility */
    }
    fclose(f);
    return true;
}

bool config_resolve_profile(koboy_profile *p, const koboy_config *c,
                            int panel_w, int panel_h)
{
    memset(p, 0, sizeof *p);
    int fit_w = panel_w / KOBOY_GB_W;
    int fit_h = panel_h / KOBOY_GB_H;
    int max_fit = fit_w < fit_h ? fit_w : fit_h;
    if (max_fit < 1) return false;
    int s = c->scale > 0 ? c->scale : max_fit;
    if (s > max_fit) s = max_fit;        /* configured scale does not fit */
    p->scale   = s;
    p->panel_w = panel_w;
    p->panel_h = panel_h;
    p->game_w  = KOBOY_GB_W * s;
    p->game_h  = KOBOY_GB_H * s;
    p->game_x  = (panel_w - p->game_w) / 2;
    p->game_y  = panel_h / 20;           /* small top margin, chrome fills the rest */
    return true;
}

bool config_save_keys(const char *path, uint16_t key_a, uint16_t key_b)
{
    FILE *f = fopen(path, "a");
    if (!f) return false;
    fprintf(f, "\n# written by first-run calibration\nkey_a = %u\nkey_b = %u\n",
            (unsigned)key_a, (unsigned)key_b);
    fclose(f);
    return true;
}
