#ifndef KOBOY_H
#define KOBOY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KOBOY_GB_W 160
#define KOBOY_GB_H 144
#define KOBOY_FRAME_US 16742          /* 1e6 / 59.7275 */
#define KOBOY_MAX_TOUCH 10
#define KOBOY_CHROME_MARGIN 8         /* minimum clear border for bezel to stay in bounds */

typedef enum { KOBOY_REFRESH_FAST = 0, KOBOY_REFRESH_GRAY, KOBOY_REFRESH_FULL } koboy_refresh_mode;
typedef enum { KOBOY_PIXFMT_RGB565 = 0, KOBOY_PIXFMT_XRGB8888 } koboy_pixfmt;
typedef enum { KOBOY_DPAD_RELATIVE = 0, KOBOY_DPAD_CROSS } koboy_dpad_mode;

/* Which waveform KOBOY_REFRESH_FAST asks for.
   AUTO delegates the choice to the EPDC driver, which inspects the actual pixel
   transitions in the update region -- including whether anything is being
   erased -- and picks accordingly. DU4 forces the fast non-flashing waveform:
   quicker per refresh, but it cannot erase, so trails accumulate. */
typedef enum { KOBOY_WFM_AUTO = 0, KOBOY_WFM_DU4 } koboy_wfm_policy;

/* libretro RETRO_DEVICE_ID_JOYPAD_* as bits */
#define KOBOY_BTN_B      (1u << 0)
#define KOBOY_BTN_SELECT (1u << 2)
#define KOBOY_BTN_START  (1u << 3)
#define KOBOY_BTN_UP     (1u << 4)
#define KOBOY_BTN_DOWN   (1u << 5)
#define KOBOY_BTN_LEFT   (1u << 6)
#define KOBOY_BTN_RIGHT  (1u << 7)
#define KOBOY_BTN_A      (1u << 8)

typedef struct { int x, y, w, h; } koboy_rect;   /* w == 0 means "empty" */
typedef struct { int x, y; bool down; } koboy_touch;

typedef struct {
    uint16_t    buttons;
    koboy_touch touch[KOBOY_MAX_TOUCH];
} koboy_input_state;

typedef struct {
    int      scale;
    int      game_x, game_y, game_w, game_h;
    int      panel_w, panel_h;
    bool     has_hw_buttons;
    uint32_t wfm_fast, wfm_gray, wfm_full;
} koboy_profile;

/* Control geometry in permille of the panel, so one layout fits every device. */
typedef struct {
    int dpad_cx, dpad_cy, dpad_r;
    int a_cx, a_cy, a_r;
    int b_cx, b_cy, b_r;
    int start_cx, start_cy, start_w, start_h;
    int select_cx, select_cy, select_w, select_h;
} koboy_layout;
#endif
