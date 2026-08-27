#ifndef KOBOY_SHOT_H
#define KOBOY_SHOT_H
#include "koboy.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Screenshots: the whole panel, exactly as it is being shown, written as a
 * PNG the owner can put straight into a README.
 *
 * WHY KOBOY TAKES ITS OWN. Every remote route is worse. Reading /dev/fb0 over
 * ssh races the compositor and returns torn or stale buffers; with Nickel up
 * it catches Nickel repainting its clock over the panel; and it cannot reach
 * actual gameplay at all, because nothing over ssh can press a button. Nine
 * of the captures in docs/screenshots/ survived that method and the rest were
 * thrown away. The owner asked for the tool instead.
 *
 * EVERYTHING HERE IS SEPARATE FROM main.c ON PURPOSE. main.c is not linked
 * into any test binary (the Makefile's SRC filter), so a capture assembled
 * inline there could be asserted against nothing -- which is exactly how this
 * project has previously shipped tests that passed whether or not the code
 * they guarded existed. The naming, the counter, the compositing and the
 * encoding are each a function a test can call.
 */

/* Highest sequence number this many digits can name. Past it, shot_capture
   refuses rather than wrapping onto a file that already exists: a counter
   that silently starts overwriting is exactly the failure the directory scan
   below exists to prevent, and 1000 screenshots of one game is a signal that
   something is wrong rather than a case to accommodate. */
#define KOBOY_SHOT_SEQ_MAX 999

/* The filename stem for `rom_path`: its basename with the extension removed
   and every character outside [A-Za-z0-9._-] folded to a single '_'.

   The folding is not tidiness. These filenames are quoted in READMEs, passed
   to shell commands over ssh and put in URLs, and a No-Intro collection names
   its files "Legend of Zelda, The (USA, Europe).gb" -- spaces, commas and
   parentheses in every one. Truncated to fit, always NUL-terminated. An empty
   or nameless path yields "game", so a capture still lands somewhere
   predictable instead of producing a file called ".png". */
void shot_stem_for_rom(char *out, size_t n, const char *rom_path);

/* Highest N among `<dir>/<stem>-NNN.png`, or 0 when there is none (so the
   next shot is always this + 1).

   SCANNING, rather than a counter in memory, is the whole point: koboy has no
   wall clock it trusts, the owner takes several shots of one game looking for
   a good one, and a session counter starting at 1 would overwrite the last
   session's best shot on the next launch. A missing directory reads as 0,
   which is correct -- nothing to overwrite. */
int  shot_last_seq(const char *dir, const char *stem);

/* `<dir>/<stem>-NNN.png`. False (and out untouched beyond a NUL) if the
   sequence is out of range or the path would be truncated -- a truncated
   path names a DIFFERENT file, which for something that then gets written is
   a data bug and not a cosmetic one. Same rule as state_path. */
bool shot_path(char *out, size_t n, const char *dir, const char *stem, int seq);

/* Copies the chrome panel into `dst` and paints the live game rect over it,
   which is what makes a capture the WHOLE panel.

   This is not the shape main.c's own drawing suggests, and the difference is
   the reason this function exists: main.c never composites the two. The
   faceplate lives in the `panel` buffer, the game lives in video.c's own
   buffer, and each is blitted to the platform separately -- the game as dirty
   rectangles, at a panel offset. Nothing anywhere holds the complete picture,
   so a screenshot has to build it.

   `game` is video_buffer()'s pixels at `game_stride`; `r` is where they
   belong in panel coordinates (prof.game_x/y and the reserved rect's size).
   A rect that does not fit inside the panel is refused rather than clipped:
   it would mean the caller's geometry and video's disagree, and half a
   picture written as if it were whole is worse than no file. */
bool shot_compose(uint8_t *dst, int dst_stride,
                  const uint8_t *panel, int panel_stride, int pw, int ph,
                  const uint8_t *game, int game_stride, const koboy_rect *r);

/* The whole operation: make `dir` if it is not there, pick the next unused
   name, composite, and write the PNG atomically (temp file, fsync, rename --
   src/safefile.c, the same discipline as a save, and for the same reason: a
   device that loses power mid-write must not leave a truncated file wearing a
   .png name).

   On success fills `out_path` (if given) with what was written and `out_seq`
   with its number. Returns false and writes nothing on any failure -- no
   directory, no memory, a full card. */
bool shot_capture(const char *dir, const char *rom_path,
                  const uint8_t *panel, int panel_stride, int pw, int ph,
                  const uint8_t *game, int game_stride, const koboy_rect *r,
                  char *out_path, size_t out_n, int *out_seq);
#endif
