/* readlink() and PATH_MAX: config.c needs both for install-relative path
   resolution, and -std=c11 alone hides them behind __STRICT_ANSI__. */
#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include "chrome.h"          /* chrome_controls_top: the resolver must reserve
                                the control band, and chrome.c owns its geometry */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void config_defaults(koboy_config *c)
{
    memset(c, 0, sizeof *c);
    c->scale = 5;
    c->present_divisor = 3;
    /* Ghosting mitigation, DISABLED BY DEFAULT, and the history matters
       because "off" looks like an oversight otherwise.

       Both this and full_refresh_permille below were written for a forced-DU4
       pipeline. DU4 is a non-flashing waveform that cannot erase, so residue
       accumulated on every update and only a flashing GC16 cleared it; a
       periodic cleanup and a "large dirty area" promotion were the two ways of
       scheduling that flash. Then the fast waveform became AUTO, and AUTO hands
       the choice to the EPDC, which inspects the actual pixel transitions in
       each region and already picks something capable of erasing when a region
       is erasing. That made both mitigations redundant.

       MEASURED on the Libra 2, from a refresh trace: 35 AUTO refreshes on small
       rects, none of which flashed, against 21 GC16 flashes -- every single one
       of them a large region tripping the 450-permille threshold below. Zero
       scheduled cleanups fired in the whole run. So the threshold was causing
       all of the flashing the user complained about and the cleanup was doing
       nothing at all.

       With both off, a full game of Tetris played with no flashing whatever and
       only slight ghosting, which is the trade the user preferred. Raise
       cleanup_interval (presented frames) or cleanup_max_ms (wall clock) if you
       want the periodic flash back; the wall-clock ceiling exists because the
       dirty-rect pass suppresses unchanged frames, so a presented-frame counter
       is a poor clock -- 70s of measured gameplay presented only 45 frames. */
    c->cleanup_interval = 0;
    c->cleanup_max_ms = 0;
    /* 1000 permille, i.e. the promotion fires only when the dirty rect covers
       the game rect corner to corner. That is reachable -- the dirty rect is a
       single merged bounding box, so a full-screen wipe or a scene transition
       produces exactly it -- but normal gameplay essentially never does, and
       when the whole rect really has changed a flashing refresh is what you
       want anyway. Only *exceeding* the rect is impossible.
       See the note above: this threshold, not the cleanup, was the measured
       source of the flashing. Lower it (450 was the old default) to force a
       flash on large scene changes, which is worth having only if the driver's
       own choice leaves residue you can see. */
    c->full_refresh_permille = 1000;
    c->wfm_fast_policy = KOBOY_WFM_AUTO;
    c->grab_input = true;
    /* CROSS, because the faceplate chrome draws an absolute four-way cross and
       the drawn UI has to agree with the input model -- the drawing is the part
       a user trusts. Relative mode steers from wherever the finger first landed,
       which needs a drag that a drawn cross gives no hint of: the user could not
       steer at all in relative mode and could immediately in cross mode. Set
       dpad_mode = relative for the thumb-pad behaviour. */
    c->dpad_mode = KOBOY_DPAD_CROSS;
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

/* ------------------------------------------------- install-relative paths
 *
 * Why this exists, in full, because it cost a device round-trip to find.
 *
 * `core_path` defaults to a bare "gambatte_libretro.so", and core.c hands it
 * to dlopen(). Per POSIX and glibc, dlopen() given a name containing no slash
 * treats it as a library *name* and searches DT_RUNPATH, LD_LIBRARY_PATH,
 * /etc/ld.so.cache and the system library directories -- it NEVER looks in the
 * current working directory. So on the device it looked everywhere except the
 * one directory the file was actually in, and failed with "cannot open shared
 * object file: No such file or directory" while sitting next to it.
 *
 * No host test could have caught it: on the desktop the core always arrives as
 * a path with a slash in it (`--core build/stub_core.so`), which dlopen treats
 * as a filesystem path.
 *
 * The fix resolves against the directory containing the *executable*, not the
 * cwd. A "./" prefix would also have worked, but only when the cwd happens to
 * be the install directory -- it fails the moment koboy is launched from
 * NickelMenu or KFMon, which do not set one. /proc/self/exe is independent of
 * how the process was started.
 */

/* Split out from config_exe_dir and taking the directory as an argument so it
   is testable without depending on where the test binary happens to live.
   Returns false and leaves `out` untouched on truncation or bad input. */
bool config_join_sibling(char *out, size_t n, const char *name, const char *dir)
{
    if (!out || !n || !name || !name[0] || !dir || !dir[0]) return false;

    /* An explicit path -- absolute or containing a slash anywhere -- is the
       caller's own decision; honour it verbatim. */
    if (strchr(name, '/')) {
        if (strlen(name) >= n) return false;
        snprintf(out, n, "%s", name);
        return true;
    }

    char buf[PATH_MAX];
    int  len;
    /* "." means "the install directory", not "the install directory plus a
       stray /." component that then shows up in every error message. */
    if (!strcmp(name, "."))
        len = snprintf(buf, sizeof buf, "%s", dir);
    else if (!strcmp(dir, "/"))          /* avoid emitting "//name" */
        len = snprintf(buf, sizeof buf, "/%s", name);
    else
        len = snprintf(buf, sizeof buf, "%s/%s", dir, name);

    if (len < 0 || (size_t)len >= sizeof buf) return false;
    if ((size_t)len >= n) return false;
    snprintf(out, n, "%s", buf);
    return true;
}

/* The directory containing the running executable, with no trailing slash
   (except at the filesystem root, where it is exactly "/"). */
bool config_exe_dir(char *out, size_t n)
{
    if (!out || !n) return false;
    char    buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (len <= 0 || (size_t)len >= sizeof buf - 1) return false;
    buf[len] = '\0';

    char *slash = strrchr(buf, '/');
    if (!slash) return false;            /* not an absolute path: give up */
    if (slash == buf) buf[1] = '\0';     /* executable sits in "/" */
    else              *slash = '\0';
    if (strlen(buf) >= n) return false;
    snprintf(out, n, "%s", buf);
    return true;
}

/* Called once, after the ini and the command line have both been applied, so
   that a bare name from either source gets the same treatment. Deliberately
   NOT done inside config_load: the loader's job is to report what the file
   says, and folding resolution into it would make the parse tests assert on
   this machine's directory layout.

   All three paths get it, not just core_path. rom_path and save_dir reach
   fopen(), which does resolve relative to the cwd, so they are not broken the
   way core_path was -- but they fail the same way for the same reason when
   launched from a menu that sets no cwd, and save_dir is the worse of the two:
   the shipped default is ".", which under a menu launch would try to write
   save files to the read-only rootfs. Resolving all three keeps one rule to
   remember: no slash means "next to koboy". */
void config_resolve_paths(koboy_config *c)
{
    char dir[PATH_MAX];
    if (!config_exe_dir(dir, sizeof dir)) return;   /* leave paths as-is */

    char tmp[512];
    if (config_join_sibling(tmp, sizeof tmp, c->core_path, dir))
        snprintf(c->core_path, sizeof c->core_path, "%s", tmp);
    if (config_join_sibling(tmp, sizeof tmp, c->rom_path, dir))
        snprintf(c->rom_path, sizeof c->rom_path, "%s", tmp);
    if (config_join_sibling(tmp, sizeof tmp, c->save_dir, dir))
        snprintf(c->save_dir, sizeof c->save_dir, "%s", tmp);
}

static void trim(char *s)
{
    char *p = s; while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r' || s[n-1] == '\n')) s[--n] = 0;
}

static bool as_bool(const char *v) { return !(strcmp(v,"false")==0 || strcmp(v,"0")==0); }

/* The promotion test, extracted from the main loop so it can be tested.
   >= and not >, deliberately: a frame in which the entire game rect changed is
   precisely when a flashing refresh is wanted, so the boundary case belongs
   inside the promotion, not outside it. permille <= 0 disables the promotion
   outright -- without that guard 0 would make the comparison always true, which
   is the exact inverse of what "disabled" means. */
bool config_promote_full(const koboy_config *c, long dirty_px, long whole_px)
{
    if (!c || c->full_refresh_permille <= 0) return false;
    if (whole_px <= 0 || dirty_px <= 0)      return false;
    return dirty_px * 1000L >= whole_px * (long)c->full_refresh_permille;
}

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
        else if (!strcmp(k, "cleanup_max_ms"))   c->cleanup_max_ms = atoi(v);
        else if (!strcmp(k, "force_dither"))     c->force_dither = as_bool(v);
        else if (!strcmp(k, "grab_input"))       c->grab_input = as_bool(v);
        else if (!strcmp(k, "dpad_deadzone"))    c->dpad_deadzone = atoi(v);
        else if (!strcmp(k, "dpad_hysteresis"))  c->dpad_hysteresis = atoi(v);
        else if (!strcmp(k, "dpad_mode"))        c->dpad_mode = strcmp(v,"cross") ? KOBOY_DPAD_RELATIVE : KOBOY_DPAD_CROSS;
        else if (!strcmp(k, "key_a"))            c->key_a = (uint16_t)atoi(v);
        else if (!strcmp(k, "key_b"))            c->key_b = (uint16_t)atoi(v);
        else if (!strcmp(k, "rom"))              snprintf(c->rom_path,  sizeof c->rom_path,  "%s", v);
        else if (!strcmp(k, "full_refresh_permille")) c->full_refresh_permille = atoi(v);
        else if (!strcmp(k, "waveform_fast"))
            c->wfm_fast_policy = !strcmp(v, "du4") ? KOBOY_WFM_DU4 : KOBOY_WFM_AUTO;
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

    /* Two reservations, not one. KOBOY_CHROME_MARGIN keeps the bezel inside the
       buffer; ctrl_top keeps the game rect off the CONTROLS. Reserving only the
       bezel margin was the bug: at scale = 0 the fitted rect cleared the panel
       edges comfortably and still covered the drawn A button and d-pad on all
       four supported panels (on the Libra 2 it chose scale 7 and put 15,677
       chrome pixels inside the game rect). The touch zones stay live underneath
       a rect drawn over them, so tapping the lower playfield pressed A or a
       direction -- unplayable at the one setting whose whole purpose is a bigger
       picture. Shrinking the controls was the other way out and is explicitly
       not the fix: they are the only way to play on a device with no buttons.
       See chrome.h for why chrome.c owns the geometry. */
    int ctrl_top = chrome_controls_top(&c->layout, panel_w, panel_h);

    while (s > 1) {
        int game_w = KOBOY_GB_W * s;
        int game_h = KOBOY_GB_H * s;
        int game_x = (panel_w - game_w) / 2;
        int game_y = panel_h / 20;
        int left_margin   = game_x;
        int right_margin  = panel_w - game_x - game_w;
        int top_margin    = game_y;
        int bottom_margin = panel_h - game_y - game_h;

        if (left_margin >= KOBOY_CHROME_MARGIN &&
            right_margin >= KOBOY_CHROME_MARGIN &&
            top_margin >= KOBOY_CHROME_MARGIN &&
            bottom_margin >= KOBOY_CHROME_MARGIN &&
            game_y + game_h <= ctrl_top) {
            break;  /* margins sufficient AND clear of the control band */
        }
        s--;
    }
    /* The floor stays 1 rather than becoming a failure: at scale 1 the rect is
       160x144 and every panel spec §3 supports (>= 1072x1448) clears the control
       band by hundreds of pixels, so this is unreachable there -- and on some
       hypothetical tiny panel, running with a slightly overlapped control band
       still beats refusing to start. chrome_render clamps its own writes either
       way; it does not rely on this loop. */

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
    /* Read existing file, filtering out old key_a/key_b lines, then write
       atomically with new keys. This makes calibration idempotent: whether
       called for the first time or recalibration, the file ends with exactly
       one key_a line and one key_b line. */

    /* Create temp file in same directory as target to ensure same filesystem
       (so rename succeeds atomically, and so writes fail fast if disk full) */
    char temp_path[512];
    snprintf(temp_path, sizeof temp_path, "%s.tmp", path);

    FILE *out = fopen(temp_path, "w");
    if (!out) return false;

    /* Read existing file if it exists, filtering out key_a/key_b lines */
    FILE *in = fopen(path, "r");
    if (in) {
        char line[1024];
        while (fgets(line, sizeof line, in)) {
            /* Check if this is an assignment (has '=' before any '#').
               If so, check if it's key_a or key_b and skip if so.
               Pure comment lines (# ...) or blank lines pass through unchanged. */
            char *eq = strchr(line, '=');
            char *hash = strchr(line, '#');

            /* If there's an '=' and it comes before any '#', this is an assignment */
            if (eq && (!hash || eq < hash)) {
                /* Extract key name (everything before '=', trimmed) */
                char k[1024];
                snprintf(k, sizeof k, "%.*s", (int)(eq - line), line);
                trim(k);

                /* Skip old key_a and key_b lines */
                if (!strcmp(k, "key_a") || !strcmp(k, "key_b")) {
                    continue;
                }
            }

            /* Preserve this line (comments, blanks, other keys) */
            fputs(line, out);
        }
        fclose(in);
    }

    /* Write the new calibration */
    fprintf(out, "# written by first-run calibration\nkey_a = %u\nkey_b = %u\n",
            (unsigned)key_a, (unsigned)key_b);

    if (fclose(out) != 0) {
        /* fclose failed; temp file is left but out of sync, remove it */
        remove(temp_path);
        return false;
    }

    /* Atomically replace the target file */
    if (rename(temp_path, path) != 0) {
        remove(temp_path);
        return false;
    }

    return true;
}
