#define _POSIX_C_SOURCE 200809L
#include "shot.h"
#include "png.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

/* Contract and rationale: shot.h. */

/* Digits in the sequence part of a filename. Fixed-width so a directory
   listing sorts the way the shots were taken -- "shot-9" before "shot-10" is
   how an unpadded counter reads in every file manager and in the shell. */
#define SHOT_SEQ_DIGITS 3

static bool keep_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
}

void shot_stem_for_rom(char *out, size_t n, const char *rom_path)
{
    if (!out || n == 0) return;
    out[0] = '\0';
    if (!rom_path || !rom_path[0]) { snprintf(out, n, "game"); return; }

    const char *base = strrchr(rom_path, '/');
    base = base ? base + 1 : rom_path;

    /* The LAST dot, and only when it is not the first character: a file named
       ".hidden.gb" keeps its leading dot as part of the name, and one named
       "Sonic" with no extension at all must not lose its tail. */
    size_t len = strlen(base);
    const char *dot = strrchr(base, '.');
    if (dot && dot != base) len = (size_t)(dot - base);

    size_t o = 0;
    bool   pending_us = false;     /* a run of folded characters, not yet emitted */
    for (size_t i = 0; i < len && o + 1 < n; i++) {
        if (keep_char(base[i])) {
            /* Collapsed here rather than at the point of substitution, so
               "Zelda, The (USA)" becomes Zelda_The_USA and not
               Zelda__The__USA_ -- and so a trailing run is dropped entirely
               instead of leaving the name ending in an underscore. */
            if (pending_us && o > 0 && o + 2 < n) out[o++] = '_';
            pending_us = false;
            out[o++] = base[i];
        } else {
            pending_us = true;
        }
    }
    out[o] = '\0';
    /* A name made entirely of folded characters would otherwise be "". */
    if (o == 0) snprintf(out, n, "game");
}

/* True when `name` is exactly "<stem>-NNN.png" for this stem, and if so the
   number is written to *seq. Deliberately strict about the digit COUNT being
   at least SHOT_SEQ_DIGITS and the suffix being exactly ".png": anything
   else in the directory (a .srm, a hand-renamed favourite, another game's
   shots) must not move this game's counter. */
static bool parse_shot_name(const char *name, const char *stem, int *seq)
{
    size_t sl = strlen(stem);
    if (strncmp(name, stem, sl) != 0) return false;
    if (name[sl] != '-') return false;

    const char *d = name + sl + 1;
    int digits = 0;
    long v = 0;
    while (d[digits] >= '0' && d[digits] <= '9') {
        v = v * 10 + (d[digits] - '0');
        if (v > 1000000L) return false;         /* absurd; not ours */
        digits++;
    }
    if (digits < SHOT_SEQ_DIGITS) return false;
    if (strcmp(d + digits, ".png") != 0) return false;
    *seq = (int)v;
    return true;
}

int shot_last_seq(const char *dir, const char *stem)
{
    if (!dir || !stem || !stem[0]) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;                            /* no directory: nothing to clobber */

    int best = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        int seq;
        if (parse_shot_name(e->d_name, stem, &seq) && seq > best) best = seq;
    }
    closedir(d);
    return best;
}

bool shot_path(char *out, size_t n, const char *dir, const char *stem, int seq)
{
    if (!out || n == 0) return false;
    out[0] = '\0';
    if (!dir || !stem || !stem[0]) return false;
    if (seq < 1 || seq > KOBOY_SHOT_SEQ_MAX) return false;
    int len = snprintf(out, n, "%s/%s-%0*d.png", dir, stem, SHOT_SEQ_DIGITS, seq);
    if (len < 0 || (size_t)len >= n) { out[0] = '\0'; return false; }
    return true;
}

bool shot_compose(uint8_t *dst, int dst_stride,
                  const uint8_t *panel, int panel_stride, int pw, int ph,
                  const uint8_t *game, int game_stride, const koboy_rect *r)
{
    if (!dst || !panel || !game || !r) return false;
    if (pw <= 0 || ph <= 0 || dst_stride < pw || panel_stride < pw) return false;
    /* Refused, not clipped -- shot.h says why. A rect outside the panel means
       the caller's geometry and video's disagree, and there is no version of
       "half the picture" worth writing to a file that claims to be a
       screenshot. */
    if (r->w <= 0 || r->h <= 0) return false;
    if (r->x < 0 || r->y < 0 || r->x + r->w > pw || r->y + r->h > ph) return false;
    if (game_stride < r->w) return false;

    for (int y = 0; y < ph; y++)
        memcpy(dst + (size_t)y * dst_stride,
               panel + (size_t)y * panel_stride, (size_t)pw);
    for (int y = 0; y < r->h; y++)
        memcpy(dst + (size_t)(r->y + y) * dst_stride + r->x,
               game + (size_t)y * game_stride, (size_t)r->w);
    return true;
}

bool shot_capture(const char *dir, const char *rom_path,
                  const uint8_t *panel, int panel_stride, int pw, int ph,
                  const uint8_t *game, int game_stride, const koboy_rect *r,
                  char *out_path, size_t out_n, int *out_seq)
{
    if (out_path && out_n) out_path[0] = '\0';
    if (out_seq) *out_seq = 0;
    if (!dir || !dir[0] || pw <= 0 || ph <= 0) return false;

    /* Created on demand, and an existing directory is not an error. Not
       recursive on purpose: this is one directory beside the binary, and a
       parent that does not exist means the install itself is wrong -- which
       mkdir -p would paper over by writing screenshots somewhere nobody
       looks. 0777 is deliberate too: the card is FAT32 (no mode bits at all)
       and the umask decides on anything else. */
    if (mkdir(dir, 0777) != 0) {
        struct stat st;
        if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    }

    char stem[96];
    shot_stem_for_rom(stem, sizeof stem, rom_path);

    int seq = shot_last_seq(dir, stem) + 1;
    char path[512];
    if (!shot_path(path, sizeof path, dir, stem, seq)) return false;

    /* The scratch panel is allocated per capture rather than kept around: it
       is 2 MB on the shipped panel, a screenshot happens when a human asks
       for one, and holding it for the whole session would cost that memory on
       every run for the benefit of the runs that never take a shot. */
    size_t need = (size_t)pw * (size_t)ph;
    uint8_t *full = malloc(need);
    if (!full) return false;

    bool ok = shot_compose(full, pw, panel, panel_stride, pw, ph,
                           game, game_stride, r);
    if (ok) ok = png_write_gray8(path, full, pw, pw, ph);
    free(full);
    if (!ok) return false;

    if (out_path && out_n) snprintf(out_path, out_n, "%s", path);
    if (out_seq) *out_seq = seq;
    return true;
}
