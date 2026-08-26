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

/* Linux evdev key codes, mirrored rather than #included, for the same reason
   input.h mirrors input_event: portable code in this project never pulls in
   <linux/input.h>, so the host tests build on any machine. Only the codes koboy
   itself names are here.

   MEASURED on the author's Libra 2 (design spec Appendix A, input-device table,
   and §12): the `gpio-keys` node advertises exactly KEY_F1(59), KEY_POWER(116),
   KEY_F23(193) and KEY_F24(194). F23/F24 are the two page-turn buttons, which is
   why they are the shipped default mapping for A and B; POWER is the quit key and
   must never become a game button (calib.c rejects it, and so does the escape
   path in calib_escape). */
#define KOBOY_KEY_POWER    116
#define KOBOY_KEY_PAGE_F23 193
#define KOBOY_KEY_PAGE_F24 194

/* Gamepad button codes, MEASURED on a real Xbox Wireless Controller
   (Bus=0005) paired over Bluetooth to the verified Libra 2, 2026-08-26 --
   see docs/superpowers/plans/2026-08-25-koboy-v2-bluetooth.md. key_a/key_b
   already have a working default above (the page-turn buttons, so a
   touch-only Kobo is never stuck on first run); key_start/key_select have no
   such page-turn equivalent to fall back on, so these two are what
   config_defaults uses for them. A GUESS at which physical buttons a user
   wants for Start/Select, not a measurement of correctness -- config
   overridable, and first-run calibration exists for exactly this reason. */
#define KOBOY_KEY_BTN_SOUTH 304   /* Xbox A -- what key_a becomes once calibrated with a pad */
#define KOBOY_KEY_BTN_EAST  305   /* Xbox B -- likewise for key_b */
#define KOBOY_KEY_BTN_TL    310   /* Xbox LB -- shipped default for key_start */
#define KOBOY_KEY_BTN_TR    311   /* Xbox RB -- shipped default for key_select */

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
    /* The core's geometry (retro_get_system_av_info, queried once at ROM
       load -- see core_get_geometry's comment in core.h). base_w/base_h is
       what the core is rendering right now; max_w/max_h is the upper bound
       any single frame will report without a fresh load, and is what
       game_w/game_h and video's intermediate buffer are actually sized
       against, precisely so a frame anywhere in [1, max] fits inside the
       reserved rect without ever spilling onto the chrome or the touch
       controls drawn around it. For the Game Boy, base and max are both
       always 160x144, which is why this generalisation changes nothing
       about existing Game Boy behaviour. */
    int      base_w, base_h, max_w, max_h;
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
    int menu_cx, menu_cy, menu_w, menu_h;
} koboy_layout;
#endif
