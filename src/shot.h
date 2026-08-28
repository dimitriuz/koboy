#ifndef KOBOY_SHOT_H
#define KOBOY_SHOT_H
#include "koboy.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Screenshots: the whole panel, exactly as it is being shown, written as a
 * PNG the owner can put straight into a README.
 *
 * WHY KOBOY TAKES ITS OWN: every remote route is worse. Reading /dev/fb0 over
 * ssh races the compositor and returns torn or stale buffers, catches Nickel
 * repainting its clock, and cannot reach gameplay at all because nothing over
 * ssh can press a button. Nine of the captures in docs/screenshots/ survived
 * that method; the rest were thrown away.
 *
 * SEPARATE FROM main.c ON PURPOSE: main.c is not linked into any test binary
 * (the Makefile's SRC filter), so a capture assembled inline there could be
 * asserted against nothing. The naming, the counter, the compositing and the
 * encoding are each a function a test can call.
 */

/* Highest sequence number this many digits can name. Past it, shot_capture
   refuses rather than wrapping onto a file that already exists: a counter
   that silently starts overwriting is exactly the failure the directory scan
   below exists to prevent, and 1000 screenshots of one game is a signal that
   something is wrong rather than a case to accommodate. */
#define KOBOY_SHOT_SEQ_MAX 999

/* The filename stem for `rom_path`: its basename, extension removed, every
   character outside [A-Za-z0-9._-] folded to a single '_'.

   The folding is NOT tidiness: these names are quoted in READMEs, passed to
   shell commands over ssh and put in URLs, and a No-Intro collection names its
   files "Legend of Zelda, The (USA, Europe).gb". Truncated to fit, always
   NUL-terminated. An empty or nameless path yields "game", so a capture lands
   somewhere predictable instead of producing a file called ".png". */
void shot_stem_for_rom(char *out, size_t n, const char *rom_path);

/* Highest N among `<dir>/<stem>-NNN.png`, or 0 when there is none.

   SCANNING rather than an in-memory counter is the point: the owner takes
   several shots of one game looking for a good one, and a session counter
   starting at 1 would overwrite the last session's best on the next launch. A
   missing directory reads as 0 -- correct, nothing to overwrite. */
int  shot_last_seq(const char *dir, const char *stem);

/* `<dir>/<stem>-NNN.png`. False (and out untouched beyond a NUL) if the
   sequence is out of range or the path would be truncated -- a truncated
   path names a DIFFERENT file, which for something that then gets written is
   a data bug and not a cosmetic one. Same rule as state_path. */
bool shot_path(char *out, size_t n, const char *dir, const char *stem, int seq);

/* Copies the chrome panel into `dst` and paints the live game rect over it,
   which is what makes a capture the WHOLE panel.

   main.c NEVER composites the two: the faceplate lives in `panel`, the game in
   video.c's buffer, and each is blitted to the platform separately (the game
   as dirty rectangles, at an offset). Nothing holds the complete picture, so a
   screenshot has to build it.

   `game` is video_buffer()'s pixels at `game_stride`; `r` is where they belong
   in panel coordinates. A rect that does not fit inside the panel is REFUSED
   rather than clipped: it means the caller's geometry and video's disagree,
   and half a picture written as if it were whole is worse than no file. */
bool shot_compose(uint8_t *dst, int dst_stride,
                  const uint8_t *panel, int panel_stride, int pw, int ph,
                  const uint8_t *game, int game_stride, const koboy_rect *r);

/* The whole operation: make `dir`, pick the next unused name, composite, and
   write the PNG ATOMICALLY through safefile.c -- a device that loses power
   mid-write must not leave a truncated file wearing a .png name.

   On success fills `out_path` and `out_seq`. Returns false and writes nothing
   on any failure. */
bool shot_capture(const char *dir, const char *rom_path,
                  const uint8_t *panel, int panel_stride, int pw, int ph,
                  const uint8_t *game, int game_stride, const koboy_rect *r,
                  char *out_path, size_t out_n, int *out_seq);
#endif
