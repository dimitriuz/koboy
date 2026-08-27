#ifndef KOBOY_H
#define KOBOY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KOBOY_GB_W 160
#define KOBOY_GB_H 144
#define KOBOY_FRAME_US 16742          /* 1e6 / 59.7275 */

/* present_divisor: core frames run per frame sent to the panel. The core is
   paced at its own rate regardless (pacer_delay_us), so this trades PRESENTED
   frame rate for something the frame counter cannot see -- e-ink residue,
   which accumulates per panel UPDATE and not per second. Fewer updates, less
   smearing, choppier motion.

   3 is the shipped default and the only value a full game has been played at.
   MEASURED (docs/FOLLOWUPS.md #26, Darkwing Duck, 600 core frames): 3 -> 76
   presented, 2 -> 102, 1 -> 115, at an unchanged 10.24 s wall clock, and the
   owner judged 2 "the same or even worse" on the panel. Every value tried
   before this task was 3 or below; the direction the evidence points is UP,
   which is why the in-game FRAMES entry offers 4, 6 and 8.

   The MAX is a usability floor, not a technical one. At 8 the ceiling is 7.5
   presented frames per second on a 60 Hz core, which is already at the
   measured presented rate of the shipped default (7.4 fps) -- above it the
   divisor stops being what limits the picture and video_submit's ~17 ms does
   (docs/FOLLOWUPS.md #23), so raising it further costs motion and buys
   nothing. It is also what stops a hand-edited `present_divisor = 100000`
   from looking exactly like a hang: config_load rejects anything outside
   [1, MAX] and keeps the default. */
#define KOBOY_PRESENT_DIVISOR_DEFAULT 3
#define KOBOY_PRESENT_DIVISOR_MAX     8
/* 16.16 fixed point 1.0. The unit for every aspect ratio this project
   carries, and fixed point rather than float for the reason the scaler is:
   the numbers end up in the pixel path, and nothing in the pixel path
   touches a float. libretro hands aspect_ratio over as a `float`, so exactly
   one conversion happens -- in core.c, once per geometry announcement, well
   away from any per-pixel loop. */
#define KOBOY_ASPECT_ONE 65536u
#define KOBOY_MAX_TOUCH 10
#define KOBOY_CHROME_MARGIN 8         /* minimum clear border for bezel to stay in bounds */

typedef enum { KOBOY_REFRESH_FAST = 0, KOBOY_REFRESH_GRAY, KOBOY_REFRESH_FULL } koboy_refresh_mode;
typedef enum { KOBOY_PIXFMT_RGB565 = 0, KOBOY_PIXFMT_XRGB8888 } koboy_pixfmt;
typedef enum { KOBOY_DPAD_RELATIVE = 0, KOBOY_DPAD_CROSS } koboy_dpad_mode;

/* How a colour frame is reduced to the one grey value the four-level panel
   will quantise. Four of the six systems koboy runs are colour, and colour is
   the overwhelming majority of the shipped library, so this is not a detail --
   it is most of what the device shows. Rec.601 luma (KOBOY_GRAY_LUMA) is
   correct for an emissive display and wrong for this one: it weights blue at
   29/256, so a bright blue sky lands under video_quantise4's first threshold
   and renders BLACK. Measured examples, straight from the cores:
   Sonic Pocket Adventure's sky rgb(0,154,255) -> 119 -> level 1, and
   Castlevania's sky rgb(0,36,140) -> 36 -> level 0.

   The order is darkest-rendering to lightest, which is also the order the
   in-game MENU cycles them in, so "next" always means "brighter".

   Every one of these is identity on a neutral grey and none of them moves the
   Game Boy's four shades off the four levels they have always landed on --
   verified against the real gambatte palette, see tests/test_video_gray.c.
   That is why there is no per-system exemption here: an exemption keyed on
   160x144 geometry would also have caught the Game Gear, which is a COLOUR
   system and exactly the case this exists to fix. */
typedef enum {
    KOBOY_GRAY_LUMA = 0,   /* Rec.601 (77,150,29), no lift -- v1, byte for byte */
    KOBOY_GRAY_BRIGHT,     /* Rec.601 weights, shadow lift on */
    KOBOY_GRAY_BALANCED,   /* (81,118,57) + lift -- the shipped default */
    KOBOY_GRAY_EQUAL,      /* (85,85,86) + lift -- maximum blue lift */
    KOBOY_GRAY_VALUE,      /* max(R,G,B) -- "as bright as it reads", no lift */
    KOBOY_GRAY_COUNT
} koboy_gray_map;

/* Named once so config_defaults, the out-of-range fallback in video.c and the
   ini comment cannot drift apart. Chosen by measurement over 38 gameplay
   frames from 19 colour titles, not by taste -- see the report and the
   comment on KOBOY_GRAY_LIFT in src/video.c. */
#define KOBOY_GRAY_DEFAULT KOBOY_GRAY_BALANCED

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
       any single frame will report without a fresh load.

       WHICH ONE SIZES WHAT, because the two answers are no longer the same
       and the difference is worth a paragraph:

       - video's intermediate buffer is sized from MAX. That is memory
         safety -- video_pipeline_run accepts any frame up to max and writes
         it there -- and it has never been anything else.
       - game_w/game_h, the reserved rect, comes from BASE in
         KOBOY_LAYOUT_DMG and from MAX in KOBOY_LAYOUT_LCD.

       Max used to size the rect too, and the reason it did was real: a frame
       anywhere in [1, max] then fitted the reserved rect by construction,
       with no risk of spilling onto the chrome or a live touch control. It
       stopped being worth its price when cores arrived whose max is a mode
       they never enter -- snes9x2005 declares 512x512 and draws 256x224
       forever, so a SNES was presented at 46% of the Game Boy's area on the
       same panel. The defence moved rather than being dropped:
       video_fit_rect now shrinks a frame the rect cannot hold at 1:1 instead
       of writing past it.

       For the Game Boy, base and max are both always 160x144, so none of
       this generalisation reaches it -- which the chrome goldens are what
       actually prove. */
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

/* One EXTRA disc on the DMG faceplate: a control the Game Boy did not have,
   drawn only for the systems that do.

   `r == 0` means "this slot is empty", which is what every consumer --
   chrome_controls_top, the DMG renderer, input.c's hit test -- guards on, so
   a zero is never a degenerate zero-radius control sitting at the panel
   origin (in_circle uses <=, and (0,0) is a coordinate a real finger can
   produce).

   This started as one hardcoded c_cx/c_cy/c_r triple for the Pokemon Mini's
   third face button and became an array the moment a second system needed a
   different set: WonderSwan needs TWO, and they are not a "C". A table
   rather than a widening row of parallel fields, for the reason
   config_core_for_rom's table gives -- every extra button needs a position, a
   bit AND a label, and a struct keeps those three together where a reviewer
   can see that they agree.

   `bit` is a KOBOY_BTN_* mask and is read off the CORE's own input
   descriptors, never chosen: the Pokemon Mini core advertises its C as
   RETRO_DEVICE_ID_JOYPAD_R, and beetle-wswan advertises the WonderSwan's A
   and B as JOYPAD_L / JOYPAD_R in its rotated key map. `label` is what the
   disc says, which is not always what the bit is called -- a Pokemon Mini's
   R1 disc says "C" because that is what is moulded on the hardware. */
typedef struct {
    int      cx, cy, r;      /* permille, like every other control here */
    uint16_t bit;            /* KOBOY_BTN_*: what a press reports */
    char     label[4];       /* drawn inside the disc */
} koboy_extra_btn;

/* Two, because that is what the widest system so far (WonderSwan: L1 and R1)
   needs and because the DMG faceplate has room for exactly two more discs
   without moving the Game Boy's own controls or pushing chrome_controls_top
   up into the game rect. Raising it means finding more space, not just
   changing this number. */
#define KOBOY_MAX_EXTRA_BTNS 2

/* Control geometry in permille of the panel, so one layout fits every device.

   `extra` is filled from the ROM's extension by config_extra_buttons_for_rom,
   the same way the core and the layout mode are, and it is empty for the Game
   Boy -- so the DMG faceplate a Game Boy sees is unchanged pixel for pixel,
   which tests/test_chrome.c's golden image is what actually proves. */
typedef struct {
    int dpad_cx, dpad_cy, dpad_r;
    int a_cx, a_cy, a_r;
    int b_cx, b_cy, b_r;
    koboy_extra_btn extra[KOBOY_MAX_EXTRA_BTNS];
    int start_cx, start_cy, start_w, start_h;
    int select_cx, select_cy, select_w, select_h;
    int menu_cx, menu_cy, menu_w, menu_h;
} koboy_layout;
#endif
