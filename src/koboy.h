#ifndef KOBOY_H
#define KOBOY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KOBOY_GB_W 160
#define KOBOY_GB_H 144
#define KOBOY_FRAME_US 16742          /* 1e6 / 59.7275 */

/* present_divisor: core frames run per frame sent to the panel. The core is
   paced at its own rate regardless, so this trades PRESENTED frame rate for
   something the frame counter cannot see -- e-ink residue, which accumulates
   per panel UPDATE and not per second.

   3 is the shipped default and the only value a full game has been played at.
   MEASURED on a Libra 2, Darkwing Duck, 600 core frames per run, wall clock
   10.24-10.27 s in every case -- so this costs nothing in emulation speed:

     divisor    1     2     3     4     6     8    12
     presented 115   102    76    67    49    39    31
     fps      11.2   9.9   7.4   6.5   4.8   3.8   3.0

   1, 2 and 3 reproduce docs/FOLLOWUPS.md #26 to the frame. The owner judged 2
   "the same or even worse" than 3 on the panel, which is why the ladder goes
   UP.

   THE MAX IS 8 BECAUSE OF THE SHAPE OF THAT TABLE, not a limit: the delivered
   rate falls far more slowly than 1/divisor, because koboy suppresses
   unchanged frames and a wider gap means fewer duplicates. 8 -> 12 halves what
   is REQUESTED and removes only 8 presented frames in ten seconds. The bound
   also stops a hand-edited 100000 from looking exactly like a hang -- outside
   [1, MAX] config_load keeps the default. */
#define KOBOY_PRESENT_DIVISOR_DEFAULT 3
#define KOBOY_PRESENT_DIVISOR_MAX     8

/* AREA-AWARE PRESENT PACING -- the settle model's two measured constants, in
   ms. pacer_settle_us charges base + full * dirty/whole per presented frame
   and holds the next present until it elapses. present_divisor alone paces by
   FRAME COUNT, so a two-tile sprite move and a whole-screen scroll were paced
   identically although they cost this panel an order of magnitude apart: the
   scroll started a new full-area update several times faster than the panel
   could finish one, which is the "flashes white on fast scrolling" the owner
   reported against the 1-bit build.

   MEASURED 2026-08-27, koboy-probe --coexist on the Libra 2: a full 800x720
   rect under the waveform AUTO picks for 1-bit content takes 153.5 ms. The fit
   across five region sizes spanning 49x in area is 144.4 ms + 15.8 ns/px,
   reproduced to 0.1%. Table in config/koboy.ini, method in the v1 spec's
   Appendix E.

   SO WHY IS THE BASE 0 WHEN THE FIXED TERM IS 144 MS? Because the model does
   not predict how long an update takes -- it decides when starting the next
   one does VISIBLE harm. A two-tile rect caught mid-transition is two tiles of
   artefact nobody has reported; a full rect caught mid-transition is the
   washed-out screen in the owner's video. Charging 144 ms to everything pins
   the device to 6.5 fps on static screens too -- no better than the
   `present_divisor = 8` the owner can already set by hand. A KEY and not a
   constant so that verdict can be revisited on the panel without a rebuild.

   THE CEILING IS A HANG GUARD: no measured waveform comes within a factor of
   two of a second per update, and settle_full_ms = 100000 would present one
   frame every hundred seconds. */
#define KOBOY_SETTLE_MS_MAX 1000
#define KOBOY_SETTLE_BASE_MS_DEFAULT 0
/* 150 and not the measured 153.5: the third significant figure is below what
   this device reproduces between sessions (Appendix B records a 2.2x spread
   across instruments), and 150 lands the first post-hold frame one frame
   earlier at the shipped divisor. Rounding DOWN is safe for responsiveness and
   unsafe for flashing -- the trade only the owner can judge, hence the key. */
#define KOBOY_SETTLE_FULL_MS_DEFAULT 150
/* 16.16 fixed point 1.0 -- the unit for every aspect ratio here. Fixed point
   and not float for the reason the scaler is: these numbers end up in the
   pixel path, and nothing there touches a float. libretro hands aspect_ratio
   over as a `float`, so exactly one conversion happens, in core.c, once per
   geometry announcement. */
#define KOBOY_ASPECT_ONE 65536u
#define KOBOY_MAX_TOUCH 10
#define KOBOY_CHROME_MARGIN 8         /* minimum clear border for bezel to stay in bounds */

typedef enum { KOBOY_REFRESH_FAST = 0, KOBOY_REFRESH_GRAY, KOBOY_REFRESH_FULL } koboy_refresh_mode;
typedef enum { KOBOY_PIXFMT_RGB565 = 0, KOBOY_PIXFMT_XRGB8888 } koboy_pixfmt;
typedef enum { KOBOY_DPAD_RELATIVE = 0, KOBOY_DPAD_CROSS } koboy_dpad_mode;

/* How a colour frame is reduced to the one grey the four-level panel
   quantises. Most of the shipped library is colour, so this is most of what
   the device shows. Rec.601 luma (KOBOY_GRAY_LUMA) is correct for an emissive
   display and WRONG for this one: it weights blue at 29/256, so a bright blue
   sky lands under video_quantise4's first threshold and renders BLACK.
   Measured, straight from the cores: Sonic Pocket Adventure's sky
   rgb(0,154,255) -> 119 -> level 1, Castlevania's rgb(0,36,140) -> 36 ->
   level 0.

   Ordered darkest-rendering to lightest, which is the order the in-game MENU
   cycles them, so "next" always means "brighter".

   Every one is identity on neutral grey and none moves the Game Boy's four
   shades off their levels -- verified against the real gambatte palette in
   tests/test_video_gray.c. That is why there is NO per-system exemption: one
   keyed on 160x144 would also catch the Game Gear, a COLOUR system and exactly
   the case this exists to fix. */
typedef enum {
    KOBOY_GRAY_LUMA = 0,   /* Rec.601 (77,150,29), no lift -- v1, byte for byte */
    KOBOY_GRAY_BRIGHT,     /* Rec.601 weights, shadow lift on */
    KOBOY_GRAY_BALANCED,   /* (81,118,57) + lift -- the shipped default */
    KOBOY_GRAY_EQUAL,      /* (85,85,86) + lift -- maximum blue lift */
    KOBOY_GRAY_VALUE,      /* max(R,G,B) -- "as bright as it reads", no lift */
    KOBOY_GRAY_COUNT
} koboy_gray_map;

/* Named once so config_defaults, video.c's out-of-range fallback and the ini
   comment cannot drift. Chosen by MEASUREMENT over 38 gameplay frames from 19
   colour titles -- see KOBOY_GRAY_LIFT in src/video.c. */
#define KOBOY_GRAY_DEFAULT KOBOY_GRAY_BALANCED

/* Which waveform KOBOY_REFRESH_FAST asks for.

   AUTO delegates to the EPDC driver, which inspects the actual pixel
   transitions in the region -- including whether anything is being erased.
   DU4 forces the FOUR-level non-flashing waveform: quicker per refresh, but it
   ghosted badly in play on the reference panel, which is why
   `waveform_fast = auto` ships.

   DU IS NOT A SMALLER DU4, AND THE DISTINCTION IS WHY IT IS HERE. DU4 is
   four-level; DU is TWO-level -- FBInk's header says "from any to B&W" and,
   decisively, "on-screen pixels will be left as-is for new content that is
   *not* B&W". Against koboy's four-level output that means every pixel landing
   on one of the two MIDDLE levels is a pixel the panel does not touch, which
   is what "DU4 cannot erase" looked like from the inside.

   Genuinely 1-bit content (`force_dither`, and the in-game MOTION entry that
   turns it on together with this) asks DU only for transitions it can complete
   exactly. Neither half of the pair is expected to be worth anything alone --
   docs/FOLLOWUPS.md #25.

   No capability gate, unlike DU4: DU is in FBInk's "Common" block, so every
   mxcfb-era Kobo has it, sunxi included. */
typedef enum { KOBOY_WFM_AUTO = 0, KOBOY_WFM_DU4, KOBOY_WFM_DU, KOBOY_WFM_COUNT } koboy_wfm_policy;

/* Linux evdev key codes, MIRRORED rather than #included, for the reason
   input.h mirrors input_event: portable code here never pulls in
   <linux/input.h>, so the host tests build anywhere.

   MEASURED on the author's Libra 2 (spec Appendix A and §12): the `gpio-keys`
   node advertises exactly KEY_F1(59), KEY_POWER(116), KEY_F23(193) and
   KEY_F24(194). F23/F24 are the page-turn buttons, hence the shipped A/B
   default; POWER is the quit key and must NEVER become a game button (calib.c
   and calib_escape both reject it). */
#define KOBOY_KEY_POWER    116
#define KOBOY_KEY_PAGE_F23 193
#define KOBOY_KEY_PAGE_F24 194

/* Gamepad button codes, MEASURED on a real Xbox Wireless Controller
   (Bus=0005) paired over Bluetooth to the verified Libra 2, 2026-08-26.
   key_a/key_b already default to the page-turn buttons above;
   key_start/key_select have no such equivalent, so config_defaults uses these
   -- a GUESS at which buttons a user wants, not a measurement. Overridable,
   and calibration exists for exactly this. */
#define KOBOY_KEY_BTN_SOUTH 304   /* Xbox A -- what key_a becomes once calibrated with a pad */
#define KOBOY_KEY_BTN_EAST  305   /* Xbox B -- likewise for key_b */
#define KOBOY_KEY_BTN_TL    310   /* Xbox LB -- shipped default for key_start */
#define KOBOY_KEY_BTN_TR    311   /* Xbox RB -- shipped default for key_select */

/* libretro RETRO_DEVICE_ID_JOYPAD_* as bits. The bit POSITION is the libretro
   id, which is what makes core.c's forwarding a plain shift (`latched >> id`)
   rather than a translation table -- DO NOT RENUMBER THESE.

   The Game Boy needs eight, which is all v1 defined. Y, X, L1 and R1 exist
   because a Game & Watch title's compat keymap binds whichever retropad
   buttons its author felt like: Mickey Mouse (Wide Screen) uses x = NORTHEAST
   and b = SOUTHEAST for its diagonals and l1/r1 for GAME A / GAME B, and a
   title whose action key koboy cannot name is a title nobody can play.
   Measured against the core's own overlay, which draws a SNES pad and labels
   the TOP button NORTHEAST -- chrome.c's LCD diamond matches that geometry. */
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

/* Which presentation the LOADED SYSTEM gets -- not a user preference, decided
   from the ROM's extension the way the core is (config_layout_for_rom).

   DMG is the Game Boy faceplate v1 shipped: an INTEGER-scaled game rect
   centred in a drawn plastic case, with d-pad, A/B, Start/Select and MENU
   around it, hit-tested from the layout permille.

   LCD was built for Game & Watch, whose titles draw their OWN buttons into the
   artwork at different positions per title (Mickey Mouse needs NW/SW/NE/SE,
   Donkey Kong a full cross plus JUMP), so a fixed remap onto two drawn buttons
   cannot work and did not: NE on Mickey Mouse is JOYPAD_X, which the DMG
   faceplate has no button for at all -- "only b button works".

   PRESSING THOSE DRAWN BUTTONS THROUGH A POINTER DOES NOT WORK, and it was
   measured: the core does query RETRO_DEVICE_POINTER, but the shipped .mgw
   files route through gwlua's compat init, which has no pointer handling --
   a pointer press anywhere on the artwork changes ZERO pixels, a joypad press
   changes 211k. So LCD exposes the WHOLE retropad instead of guessing a
   subset: a full-width FRACTIONALLY scaled rect above a strip carrying a
   d-pad, the face buttons, SELECT, START, L1, R1, MENU and the battery lamp.
   Pointer forwarding stays alongside -- additive, and the newer .mgw format
   does read it. */
typedef enum { KOBOY_LAYOUT_DMG = 0, KOBOY_LAYOUT_LCD } koboy_layout_mode;

typedef struct { int x, y, w, h; } koboy_rect;   /* w == 0 means "empty" */
typedef struct { int x, y; bool down; } koboy_touch;

/* libretro-normalised pointer, as RETRO_DEVICE_POINTER reports it: -0x7fff at
   the left/top edge of the displayed frame, +0x7fff at the right/bottom.
   x/y deliberately KEEP their last value when pressed goes false -- what a
   real pointer device does, and the core reads PRESSED rather than the
   coordinates to decide whether a button is held. */
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
    /* The core's geometry (core_get_geometry, queried once at ROM load).
       base_w/base_h is what the core renders now; max_w/max_h is the upper
       bound any single frame reports without a fresh load.

       WHICH ONE SIZES WHAT:
       - video's intermediate buffer comes from MAX. That is memory safety --
         video_pipeline_run accepts any frame up to max and writes it there.
       - game_w/game_h, the reserved rect, comes from BASE in
         KOBOY_LAYOUT_DMG and from MAX in KOBOY_LAYOUT_LCD.

       Max used to size the rect too, for a real reason: any frame in [1, max]
       then fitted by construction, with no risk of spilling onto chrome or a
       live touch control. It stopped being worth the price when cores arrived
       whose max is a mode they never enter -- snes9x2005 declares 512x512 and
       draws 256x224 forever, presenting a SNES at 46% of the Game Boy's area.
       The defence MOVED rather than dropped: video_fit_rect shrinks a frame
       the rect cannot hold instead of writing past it.

       For the Game Boy base == max == 160x144, so none of this reaches it --
       which the chrome goldens prove. */
    int      base_w, base_h, max_w, max_h;
    /* koboy_layout_mode, resolved by config_resolve_profile. It travels in the
       PROFILE because that is what video.c, chrome.c and input.c already
       carry -- a resolved-once fact about this session, like scale. */
    int      layout_mode;
    /* KOBOY_LAYOUT_LCD only: the reserved rect was fitted from max rather than
       base. Travels in the profile for the reason layout_mode does.

       NOT a duplicate of "layout_mode == LCD", and the difference is
       LOAD-BEARING: video_fit_rect has a shortcut handing a frame the WHOLE
       reserved rect when it arrives at exactly max geometry, on the premise
       that the rect was fitted from that geometry. True for Game & Watch,
       false for the consoles that joined this layout (their rect comes from
       base and may be cut again by the scale ceiling), where the shortcut
       would stretch a frame to a rect it does not match. */
    bool     rect_from_max;
    /* KOBOY_LAYOUT_LCD only: koboy_lcd_face, copied from the config by
       config_resolve_profile. It rides here because chrome_lcd_layout takes
       ONLY a profile -- the one definition of the strip's geometry that
       chrome.c draws from, input.c hit-tests and config.c reserves clear of.
       Zero is DIAMOND, so a hand-built profile draws what it always drew. */
    int      lcd_face;
    bool     has_hw_buttons;
    uint32_t wfm_fast, wfm_gray, wfm_full;
} koboy_profile;

/* One EXTRA disc on the DMG faceplate: a control the Game Boy did not have,
   drawn only for the systems that do.

   LIVE GUARD: `r == 0` means "empty slot", and every consumer
   (chrome_controls_top, the DMG renderer, input.c's hit test) tests it -- so a
   zero is never a degenerate zero-radius control at the panel origin
   (in_circle uses <=, and (0,0) is a coordinate a real finger can produce).

   A table rather than parallel fields: every extra button needs a position, a
   bit AND a label, and a struct keeps the three together where a reviewer can
   see they agree.

   `bit` is a KOBOY_BTN_* mask READ OFF THE CORE's input descriptors, never
   chosen (PokeMini advertises its C as JOYPAD_R; beetle-wswan advertises the
   WonderSwan's A and B as JOYPAD_L/JOYPAD_R in its rotated map). `label` is
   what the disc SAYS, which is not always what the bit is called. */
typedef struct {
    int      cx, cy, r;      /* permille, like every other control here */
    uint16_t bit;            /* KOBOY_BTN_*: what a press reports */
    char     label[4];       /* drawn inside the disc */
} koboy_extra_btn;

/* Two: what the widest system needs, and exactly the room the DMG faceplate
   has without moving the Game Boy's own controls or pushing
   chrome_controls_top up into the game rect. RAISING IT MEANS FINDING MORE
   SPACE, not just changing this number. */
#define KOBOY_MAX_EXTRA_BTNS 2

/* WHAT THE LCD STRIP'S CONTROLS SAY AND WHERE THEY SIT, per system -- never
   WHICH BIT they report. The bit is fixed by the strip's geometry
   (chrome_lcd_layout, input.c's recompute_lcd) and is the same for every
   system; what varies is the name the console prints and how it lays them out
   under a thumb.

   The strip was built labelled X/Y/A/B/L1/R1 -- RETROPAD names -- which is
   right for a .mgw, because gw-libretro's overlay speaks retropad. It is
   actively WRONG for a Mega Drive: Genesis Plus GX maps JOYPAD_A to the
   console's C, and a disc moulded "A" that produces C is worse than an
   unlabelled one.

   EMPTY MEANS THE RETROPAD NAME: chrome.c falls back to X/Y/A/B/L1/R1/SELECT
   for any field left "" (config_lcd_pad_for_rom clears them for every system
   that wants the retropad's own names). That keeps the field purely additive
   -- a config built by config_defaults alone draws exactly the strip it drew
   before this struct existed, which tests/golden/chrome_lcd_1264x1680.pgm
   proves.

   NO `start`, deliberately: START is called START on every system that reaches
   this layout, so the field could only hold the fallback. A term that cannot
   vary is removed and replaced by the reason. SELECT does vary -- it is the
   Mega Drive's MODE -- so it has one. */

/* HOW THE STRIP ARRANGES ITS FACE BUTTONS, per system for the reason the
   labels are: someone who has held the real pad knows where its buttons are.

   DIAMOND is four discs -- X top, Y left, A right, B bottom -- right for two
   systems for two reasons: gw-libretro's overlay DRAWS a SNES pad in that
   arrangement, and a real SNES pad IS that diamond.

   ROWS6 is two rows of three, the six-button Mega Drive pad:

       X  Y  Z        JOYPAD_L  JOYPAD_X  JOYPAD_R
       A  B  C        JOYPAD_Y  JOYPAD_B  JOYPAD_A

   That console has no shoulders, so the two shoulder BITS become grid discs
   rather than pills and the lower band carries MODE and START alone. A
   shoulder pill for a bit that already has a disc would be two controls under
   one name -- the same defect as a mislabelled one.

   PAIR2 is TWO discs on a north-east/south-west diagonal -- a Game Boy
   Advance -- and it exists because the diamond is a LIE about that machine,
   not merely a poor fit. A GBA has exactly two face buttons; drawn as a
   diamond it grows two more that are not even inert, since gpSP binds
   JOYPAD_X/JOYPAD_Y to "Turbo A"/"Turbo B". A disc labelled X that fires A
   twenty times a second is a control the hardware does not have, wearing a
   name.

   THE DIAGONAL IS THE HARDWARE'S: on a real GBA B sits down and left of A,
   and every title's control screen says so. chrome_lcd_layout places them
   face_off/2 either side of the cluster centre on both axes, putting their
   centres face_off * sqrt(2) apart -- the SAME separation the diamond gives
   its adjacent pair, so one number still governs the lot.

   Both shoulder BITS keep their pills under PAIR2, unlike ROWS6: a GBA does
   have L and R, they are a left/right pair, and the lower band's outermost
   slots are the only left/right pair the strip has -- which is most of why
   this system is in this layout (config_layout_for_rom). */
typedef enum {
    KOBOY_LCD_FACE_DIAMOND = 0,
    KOBOY_LCD_FACE_ROWS6,
    KOBOY_LCD_FACE_PAIR2
} koboy_lcd_face;

typedef struct {
    /* koboy_lcd_face as an int, for the reason dpad_mode is: this struct is
       memset to zero by its owners, and DIAMOND being the zero is what makes
       an untouched layout draw the strip it always drew. */
    int  face;
    char x[8], y[8], a[8], b[8];   /* by the RETROPAD bit each disc reports */
    char l1[8], r1[8];             /* the shoulder bits: pills under DIAMOND,
                                      the grid's top-left and top-right discs
                                      under ROWS6 */
    char select[8];
} koboy_lcd_pad;

/* Control geometry in permille of the panel, so one layout fits every device.
   `extra` is filled from the ROM's extension by config_extra_buttons_for_rom
   and is empty for the Game Boy, so a Game Boy's DMG faceplate is unchanged
   pixel for pixel -- tests/test_chrome.c's golden proves it. */
typedef struct {
    int dpad_cx, dpad_cy, dpad_r;
    int a_cx, a_cy, a_r;
    int b_cx, b_cy, b_r;
    koboy_extra_btn extra[KOBOY_MAX_EXTRA_BTNS];
    /* LCD faceplate only, carried HERE rather than in a parallel struct
       because chrome_render and input.c are both already handed a koboy_layout
       and neither knows which faceplate it is about to draw. */
    koboy_lcd_pad lcd;
    int start_cx, start_cy, start_w, start_h;
    int select_cx, select_cy, select_w, select_h;
    int menu_cx, menu_cy, menu_w, menu_h;
} koboy_layout;
#endif
