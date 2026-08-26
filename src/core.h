#ifndef KOBOY_CORE_H
#define KOBOY_CORE_H
#include "koboy.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct koboy_core koboy_core;

/* Loads a libretro core via dlopen, negotiates the environment, and calls
   retro_init(). save_dir is handed back to the core for
   GET_SAVE_DIRECTORY / GET_SYSTEM_DIRECTORY queries. On failure returns
   NULL and writes a specific message into err. */
koboy_core *core_open(const char *so_path, const char *save_dir,
                      char *err, size_t errlen);

/* Reads rom_path into memory and hands it to retro_load_game(). On failure
   returns false and writes a specific message into err. */
bool core_load_rom(koboy_core *c, const char *rom_path, char *err, size_t errlen);

/* Installs the callback the core's video_refresh will forward frames to. */
void core_set_frame_cb(koboy_core *c,
                       void (*cb)(void *ud, const void *data, unsigned w, unsigned h, size_t pitch),
                       void *ud);

/* Installs the function polled once per frame to obtain the joypad bitmask
   (KOBOY_BTN_* bits, matching libretro's RETRO_DEVICE_ID_JOYPAD_* by bit position). */
void core_set_input_fn(koboy_core *c, uint16_t (*fn)(void *ud), void *ud);

/* Installs the function polled once per frame, alongside the joypad one
   above, to obtain the libretro POINTER state a Game & Watch title needs: it
   draws its own buttons into its artwork, so a touch has to reach the core as
   a position rather than as a button. x/y are libretro-normalised (-0x7fff at
   the left/top edge of the displayed frame, +0x7fff at the right/bottom).

   Optional and additive: leave it unset and every POINTER query answers 0,
   which is exactly what a core that never asks (gambatte) already saw. It
   does not touch the joypad path in either direction. */
void core_set_pointer_fn(koboy_core *c,
                         void (*fn)(void *ud, int16_t *x, int16_t *y, bool *pressed),
                         void *ud);

/* Runs exactly one emulated frame (retro_run()). */
void core_run_frame(koboy_core *c);

/* Returns the core's save RAM pointer and length, or NULL/0 if it has none. */
uint8_t *core_sram(koboy_core *c, size_t *len);

/* The pixel format the core settled on via SET_PIXEL_FORMAT. */
koboy_pixfmt core_pixfmt(const koboy_core *c);

/* Geometry: base_width/height is what the core is rendering right now;
   max_width/height is the upper bound any single video_refresh frame will
   report without it calling RETRO_ENVIRONMENT_SET_GEOMETRY or
   RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO first (both handled in core.c's
   env_cb). Seeded from retro_get_system_av_info right after
   retro_load_game, then kept live by those two environ calls for as long as
   the ROM stays loaded -- NOT "queried once and trusted for the session,"
   which was this project's first answer to the open question in the
   multi-system design doc §3 and was WRONG, caught only once a real core
   was measured: the Game & Watch core reports a 128x128 placeholder from
   retro_get_system_av_info called immediately after retro_load_game, on
   every one of 59 measured titles, and only resolves the real canvas
   (Parachute 658x395, Mario Bros. 973x532, Donkey Kong 606x748, ...) from
   INSIDE its first retro_run(), via exactly the two environ calls above.
   So this function's answer can legitimately change between two calls with
   no retro_load_game in between -- core_geometry_changed(), below, is how a
   caller finds out without diffing four ints itself on every frame.
   Per-frame width/height *within* whatever the current max is can still
   vary frame to frame by the ordinary libretro contract even without a new
   environ call, which is why video_submit takes w/h from the frame callback
   itself rather than from this query for the actual pixel work -- this
   query exists to size the buffer and lay out the chrome BEFORE that frame
   arrives, not to duplicate what the frame callback already reports.
   Returns false, and leaves the outputs untouched, if no ROM has been loaded
   yet -- there is no honest geometry to report before retro_load_game has
   run. */
bool core_get_geometry(const koboy_core *c, int *base_w, int *base_h,
                       int *max_w, int *max_h);

/* True the first time this is called since a SET_GEOMETRY/SET_SYSTEM_AV_INFO
   environ call last changed core_get_geometry's answer, and false on every
   call in between (read-and-clear, so two callers polling independently is
   not a supported shape today; koboy has exactly one caller, main.c's
   per-frame loop). Deliberately NOT set for the initial, load-time query
   itself (see core_load_rom): the caller that wants that initial answer
   already calls
   core_get_geometry directly right after core_load_rom returns, and does
   not need a "changed" flag to tell it something it is about to read
   unconditionally anyway. What this exists for is everything AFTER that:
   main.c calls this once per retro_run() and, when it fires, re-resolves
   the game rect and rebuilds the video buffer against the real numbers --
   which is what makes "a core does not know its geometry at load time" a
   handled case instead of a silently-wrong 128x128 render. */
bool core_geometry_changed(koboy_core *c);

/* Unloads the current game but keeps the shared object open and initialised,
   so another ROM can be loaded through the same handle. dlclose/dlopen cycling
   of a C++ core mid-session is avoidable, so it is avoided. */
bool core_unload_rom(koboy_core *c);

/* retro_reset. */
bool core_reset(koboy_core *c);

/* Save-state support. core_state_size returns 0 when the core does not export
   the serialisation symbols -- a capability answer, not an error. The menu
   greys the entries out; the game still plays. */
size_t core_state_size(koboy_core *c);

/* Both refuse a buffer shorter than core_state_size rather than truncating:
   handing a core a partial state corrupts a running game. */
bool   core_state_save(koboy_core *c, void *buf, size_t n);
bool   core_state_load(koboy_core *c, const void *buf, size_t n);

/* Unloads the game, deinitializes, and closes the shared library. */
void core_close(koboy_core *c);

#endif
