/*!
 * libretro.h is a simple API that allows for the creation of games and emulators.
 *
 * @file libretro.h
 * @version 1
 * @author libretro
 * @copyright Copyright (C) 2010-2024 The RetroArch team
 *
 * @paragraph LICENSE
 * The following license statement only applies to this libretro API header (libretro.h).
 *
 * Copyright (C) 2010-2024 The RetroArch team
 *
 * Permission is hereby granted, free of charge,
 * to any person obtaining a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/* libretro_min.h — a minimal subset of the upstream libretro.h, transcribed
   verbatim for the pieces koboy actually binds: environment negotiation,
   the five runtime callbacks, game loading, and save RAM access. Names and
   numeric values below match upstream exactly; nothing here is invented. */

#ifndef KOBOY_LIBRETRO_MIN_H
#define KOBOY_LIBRETRO_MIN_H

#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <stdbool.h>

#ifndef RETRO_CALLCONV
#  if defined(__GNUC__) && defined(__i386__) && !defined(__x86_64__)
#    define RETRO_CALLCONV __attribute__((cdecl))
#  elif defined(_MSC_VER) && defined(_M_X86) && !defined(_M_X64)
#    define RETRO_CALLCONV __cdecl
#  else
#    define RETRO_CALLCONV
#  endif
#endif

#define RETRO_ENVIRONMENT_EXPERIMENTAL 0x10000

/* SET_ROTATION is the FIRST environment call koboy answers that changes what
   a pixel means rather than how many there are, and it exists in this file
   because a whole class of content cannot be presented without it: a
   golden-age arcade board turned its MONITOR on its side, so FinalBurn Neo
   renders Galaga into a 288x224 LANDSCAPE buffer and asks the frontend to
   turn it a quarter turn. The value is a count of 90-degree COUNTER-CLOCKWISE
   steps (0..3); 3 is what Galaga, Dig Dug, Donkey Kong, Frogger and Ms.
   Pac-Man all ask for. Answering false -- which every unknown command still
   does -- is a legal answer and is what koboy did before FBNeo arrived, but
   for these boards it means the picture is played sideways. See core.c's
   handler and video.c's video_set_rotation. */
#define RETRO_ENVIRONMENT_SET_ROTATION            1
#define RETRO_ENVIRONMENT_GET_CAN_DUPE            3
#define RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY    9
#define RETRO_ENVIRONMENT_SET_PIXEL_FORMAT        10
#define RETRO_ENVIRONMENT_GET_VARIABLE            15
#define RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY      31
/* A core that does not know its real geometry at retro_load_game time (the
   Game & Watch core, measured: retro_get_system_av_info reports a 128x128
   placeholder right after load, on every title, and only resolves the real
   canvas from inside its first retro_run()) uses one of these two to
   announce it later. SET_SYSTEM_AV_INFO carries a full retro_system_av_info
   (geometry AND timing); SET_GEOMETRY carries just the geometry half. Both
   are handled in core.c's env_cb -- see core_get_geometry's comment in
   core.h for how koboy reacts. */
#define RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO      32
#define RETRO_ENVIRONMENT_SET_GEOMETRY            37
#define RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE  (47 | RETRO_ENVIRONMENT_EXPERIMENTAL)

#define RETRO_DEVICE_JOYPAD    1
/* The Game & Watch core polls this, on PORT 2, to let the player press the
   buttons its own artwork draws (third_party/gw/src/libretro.c). Coordinates
   are normalised across the displayed frame: -0x7fff at the left/top edge,
   +0x7fff at the right/bottom. */
#define RETRO_DEVICE_POINTER   6
#define RETRO_DEVICE_ID_POINTER_X        0
#define RETRO_DEVICE_ID_POINTER_Y        1
#define RETRO_DEVICE_ID_POINTER_PRESSED  2
#define RETRO_DEVICE_ID_POINTER_COUNT    3
#define RETRO_MEMORY_SAVE_RAM  0

enum retro_pixel_format
{
   RETRO_PIXEL_FORMAT_0RGB1555 = 0,
   RETRO_PIXEL_FORMAT_XRGB8888 = 1,
   RETRO_PIXEL_FORMAT_RGB565   = 2,
   RETRO_PIXEL_FORMAT_UNKNOWN  = INT_MAX
};

struct retro_variable
{
   const char *key;
   const char *value;
};

struct retro_game_info
{
   const char *path;
   const void *data;
   size_t      size;
   const char *meta;
};

struct retro_system_info
{
   const char *library_name;
   const char *library_version;
   const char *valid_extensions;
   bool        need_fullpath;
   bool        block_extract;
};

struct retro_game_geometry
{
   unsigned base_width;
   unsigned base_height;
   unsigned max_width;
   unsigned max_height;
   float    aspect_ratio;
};

struct retro_system_timing
{
   double fps;
   double sample_rate;
};

struct retro_system_av_info
{
   struct retro_game_geometry geometry;
   struct retro_system_timing timing;
};

typedef bool (RETRO_CALLCONV *retro_environment_t)(unsigned cmd, void *data);
typedef void (RETRO_CALLCONV *retro_video_refresh_t)(const void *data,
      unsigned width, unsigned height, size_t pitch);
typedef void (RETRO_CALLCONV *retro_audio_sample_t)(int16_t left, int16_t right);
typedef size_t (RETRO_CALLCONV *retro_audio_sample_batch_t)(const int16_t *data,
      size_t frames);
typedef void (RETRO_CALLCONV *retro_input_poll_t)(void);
typedef int16_t (RETRO_CALLCONV *retro_input_state_t)(unsigned port,
      unsigned device, unsigned index, unsigned id);

#endif
