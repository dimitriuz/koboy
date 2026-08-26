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

/* Runs exactly one emulated frame (retro_run()). */
void core_run_frame(koboy_core *c);

/* Returns the core's save RAM pointer and length, or NULL/0 if it has none. */
uint8_t *core_sram(koboy_core *c, size_t *len);

/* The pixel format the core settled on via SET_PIXEL_FORMAT. */
koboy_pixfmt core_pixfmt(const koboy_core *c);

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
