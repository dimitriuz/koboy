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

/* Polled once per frame alongside the joypad function, for the libretro
   POINTER state a Game & Watch title needs: it draws its own buttons into its
   artwork, so a touch must reach the core as a position. x/y are
   libretro-normalised (-0x7fff at the left/top edge of the displayed frame,
   +0x7fff at the right/bottom).
   Optional and ADDITIVE: unset, every POINTER query answers 0, which is what a
   core that never asks already saw. */
void core_set_pointer_fn(koboy_core *c,
                         void (*fn)(void *ud, int16_t *x, int16_t *y, bool *pressed),
                         void *ud);

/* Runs exactly one emulated frame (retro_run()). */
void core_run_frame(koboy_core *c);

/* Returns the core's save RAM pointer and length, or NULL/0 if it has none. */
uint8_t *core_sram(koboy_core *c, size_t *len);

/* The pixel format the core settled on via SET_PIXEL_FORMAT. */
koboy_pixfmt core_pixfmt(const koboy_core *c);

/* Geometry: base is what the core renders NOW, max the upper bound any single
   video_refresh frame reports without first calling SET_GEOMETRY or
   SET_SYSTEM_AV_INFO. Seeded from retro_get_system_av_info right after
   retro_load_game, then kept LIVE by those two environ calls -- NOT "queried
   once and trusted", which was this project's first answer and was WRONG:
   gw-libretro reports a 128x128 placeholder from retro_get_system_av_info on
   all 59 titles and only resolves the real canvas (Parachute 658x395, Mario
   Bros. 973x532, Donkey Kong 606x748) from INSIDE its first retro_run().

   So this answer can change between two calls with no retro_load_game between
   -- core_geometry_changed() below is how a caller finds out without diffing
   four ints every frame. Per-frame w/h WITHIN the current max can still vary
   by the ordinary libretro contract, which is why video_submit takes w/h from
   the frame callback: this query exists to size buffers and lay out chrome
   BEFORE that frame arrives.

   Returns false and leaves the outputs untouched before retro_load_game.

   ROTATION IS ALREADY APPLIED: a core may ask through SET_ROTATION for a
   quarter turn -- every arcade board with a sideways monitor does, because
   FBNeo renders Galaga into a 288x224 buffer meant to be seen as 224x288.
   This reports the PRESENTED size, so nothing downstream has to remember the
   difference. core_rotation is how a caller learns one exists. */
bool core_get_geometry(const koboy_core *c, int *base_w, int *base_h,
                       int *max_w, int *max_h);

/* What the core says its frames should be SHOWN at, and how fast. Both come
   from retro_system_av_info.

   core_display_aspect() returns the DISPLAY aspect -- the width:height the
   whole picture wants, not the shape of one pixel -- in 16.16, because nothing
   downstream may touch a float. KOBOY_ASPECT_ONE is 1:1. Three cases, all
   MEASURED:
     - `aspect_ratio <= 0` is libretro's "assume base_width/base_height", and
       gearcoleco really does report 0. The fallback is computed here in exact
       integer arithmetic from the PRESENTED base geometry, so video.c never
       sees an absent aspect.
     - The value is ALREADY in presented orientation for a rotated board: FBNeo
       reports 0.75 for Galaga while rendering a 288x224 landscape buffer. So
       it is NOT transposed here, unlike core_get_geometry's base/max.
     - A nonsense float (0, negative, NaN, absurd) falls back rather than
       propagating into a cast.

   core_fps() returns the RAW reported rate, 0.0 with no ROM loaded, and is
   deliberately UNVALIDATED: what counts as plausible is pacing policy
   (pacer_frame_us_from_fps), and main.c logs the raw number beside the
   microseconds it resolved to so a wrong-speed report is diagnosable from
   koboy.log. */
uint32_t core_display_aspect(const koboy_core *c);
double   core_fps(const koboy_core *c);

/* Quarter turns counter-clockwise the core asked for, 0..3. Per GAME, not per
   core: FBNeo answers 3 for Galaga and 0 for Donkey Kong Jr. out of the same
   .so, and core_unload_rom clears it so a second ROM through one handle starts
   from none. main.c hands this to video_set_rotation; video.c does the turn. */
unsigned core_rotation(const koboy_core *c);

/* True the first time this is called since a SET_GEOMETRY/SET_SYSTEM_AV_INFO
   call changed core_get_geometry's answer; READ-AND-CLEAR, so two independent
   pollers is not a supported shape (koboy has one: main.c's per-frame loop).
   Deliberately NOT set for the initial load-time query -- that caller reads
   core_get_geometry unconditionally right after core_load_rom. This exists for
   everything AFTER, and is what makes "a core does not know its geometry at
   load time" a handled case instead of a silently-wrong 128x128 render. */
bool core_geometry_changed(koboy_core *c);

/* Unloads the game but keeps the .so open and initialised, so another ROM can
   load through the same handle. main.c does NOT use it that way: sharing one
   handle across games meant sharing everything else derived from the first
   ROM's extension -- layout, buttons, ceiling, .srm path -- and a Mega Drive
   ROM reaching the GBA core through it killed a device. A switch closes the
   core and opens the one the new extension asks for. core_close is the only
   path that reaches this today. */
bool core_unload_rom(koboy_core *c);

/* retro_reset. */
bool core_reset(koboy_core *c);

/* Save-state support. core_state_size returns 0 when the core does not export
   the serialisation symbols -- a CAPABILITY answer, not an error: the menu
   greys the entries out and the game still plays. */
size_t core_state_size(koboy_core *c);

/* Both refuse a buffer shorter than core_state_size rather than truncating:
   handing a core a partial state corrupts a running game. */
bool   core_state_save(koboy_core *c, void *buf, size_t n);
bool   core_state_load(koboy_core *c, const void *buf, size_t n);

/* Unloads the game, deinitializes, and closes the shared library. */
void core_close(koboy_core *c);

#endif
