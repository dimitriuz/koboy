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

/* libretro RETRO_DEVICE_ID_JOYPAD_* as bits. The bit POSITION is the libretro
   id, which is what makes core.c's forwarding a plain shift (`latched >> id`)
   rather than a translation table -- do not renumber these.

   The Game Boy needs eight of them, which is all v1 ever defined. Y, X, L1 and
   R1 exist because a Game & Watch title's compat keymap binds whichever
   retropad buttons its author felt like: Mickey Mouse (Wide Screen) uses
   x = NORTHEAST and b = SOUTHEAST for its two diagonals and l1/r1 for its
   GAME A / GAME B switches, and a title whose action key is one koboy cannot
   name is a title nobody can play. Measured against the core's own on-screen
   overlay, which draws a SNES pad and labels the TOP button NORTHEAST -- see
   the LCD faceplate's diamond in chrome.c, whose geometry matches it so a
   user reading that overlay can find the same button on koboy. */
#define KOBOY_BTN_B      (1u << 0)
#define KOBOY_BTN_Y      (1u << 1)
#define KOBOY_BTN_SELECT (1u << 2)
#define KOBOY_BTN_START  (1u << 3)
#define KOBOY_BTN_UP     (1u << 4)
#define KOBOY_BTN_DOWN   (1u << 5)
#define KOBOY_BTN_LEFT   (1u << 6)
#define KOBOY_BTN_RIGHT  (1u << 7)
#define KOBOY_BTN_A      (1u << 8)
#define KOBOY_BTN_X      (1u << 9)
#define KOBOY_BTN_L1     (1u << 10)
#define KOBOY_BTN_R1     (1u << 11)

/* Which presentation the LOADED SYSTEM gets. Not a user preference and not a
   global: a .gb/.gbc gets DMG, a .mgw gets LCD, decided from the ROM's own
   extension the same way the core is (config_core_for_rom).

   DMG is the Game Boy faceplate v1 shipped: an INTEGER-scaled game rect
   centred in a drawn plastic case, with a d-pad, A/B, Start/Select and MENU
   drawn around it and hit-tested from the layout permille.

   LCD is for Game & Watch. Those titles draw their OWN buttons -- the
   direction pads, GAME A, GAME B, TIME, ACL -- into the artwork, at a
   different set of positions per title (Mickey Mouse needs NW/SW/NE/SE;
   Donkey Kong needs a full cross plus JUMP), so a fixed remap onto koboy's
   two drawn buttons cannot work and DID not: NE on Mickey Mouse is
   RETRO_DEVICE_ID_JOYPAD_X, which the DMG faceplate has no button for at
   all, and the device report was "only b button works".

   The answer is NOT to press those drawn buttons through a pointer. That was
   tried and measured: the core does query RETRO_DEVICE_POINTER, but the
   shipped .mgw files route through gwlua's compat init, which has no pointer
   handling at all -- a pointer press anywhere on the artwork changes ZERO
   pixels, a joypad press changes 211k. So LCD exposes the WHOLE retropad
   instead of guessing a subset: a full-width FRACTIONALLY scaled game rect
   above a bottom strip carrying a d-pad, the X/Y/A/B diamond, SELECT, START,
   L1, R1, MENU and the battery lamp. The pointer forwarding stays alongside
   it -- it is additive, and the newer .mgw format does read it. */
typedef enum { KOBOY_LAYOUT_DMG = 0, KOBOY_LAYOUT_LCD } koboy_layout_mode;

typedef struct { int x, y, w, h; } koboy_rect;   /* w == 0 means "empty" */
typedef struct { int x, y; bool down; } koboy_touch;

/* libretro-normalised pointer, as RETRO_DEVICE_POINTER reports it: -0x7fff at
   the left/top edge of the displayed frame, +0x7fff at the right/bottom.
   x/y deliberately KEEP their last value when pressed goes false -- that is
   what a real pointer device does, and the core reads PRESSED to decide
   whether a button is being held, not the coordinates. */
typedef struct { int16_t x, y; bool pressed; } koboy_pointer;

typedef struct {
    uint16_t     buttons;
    koboy_touch  touch[KOBOY_MAX_TOUCH];
    koboy_pointer pointer;
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
    /* koboy_layout_mode, resolved from koboy_config's own layout_mode by
       config_resolve_profile. It travels in the PROFILE rather than being
       read out of the config wherever it is needed because the profile is
       what video.c, chrome.c and input.c already carry -- exactly like
       scale/game_w/panel_w, which are resolved-once facts about this
       session's presentation rather than settings. */
    int      layout_mode;
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
