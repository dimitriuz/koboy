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

/* Unloads the game, deinitializes, and closes the shared library. */
void core_close(koboy_core *c);

#endif
