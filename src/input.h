#ifndef KOBOY_INPUT_H
#define KOBOY_INPUT_H
#include "koboy.h"
#include "config.h"

/* Mirrors the fields of linux input_event we care about, so input.c stays
   portable and testable on a host without linux/input.h. */
typedef struct { uint16_t type, code; int32_t value; } koboy_ev;

#define KOBOY_EV_SYN 0x00
#define KOBOY_EV_KEY 0x01
#define KOBOY_EV_ABS 0x03

#define KOBOY_ABS_MT_SLOT        0x2f
#define KOBOY_ABS_MT_POSITION_X  0x35
#define KOBOY_ABS_MT_POSITION_Y  0x36
#define KOBOY_ABS_MT_TRACKING_ID 0x39

/* The d-pad on a Bluetooth gamepad. MEASURED on a real Xbox Wireless
   Controller, 2026-08-26: it does NOT arrive as EV_KEY like the page-turn
   buttons -- it is a hat switch, reported as two absolute axes each taking
   exactly -1/0/+1. input.c decodes these directly into KOBOY_BTN_UP/DOWN/
   LEFT/RIGHT; see the long comment there for why that decode needs none of
   the touch thumb-pad's deadzone/hysteresis machinery.

   ABS_X/ABS_Y (the analog stick, codes 0x00/0x01) are deliberately NOT
   named or handled here -- see input.c's input_feed. */
#define KOBOY_ABS_HAT0X 0x10
#define KOBOY_ABS_HAT0Y 0x11

typedef struct koboy_input koboy_input;

koboy_input *input_create(const koboy_config *c, const koboy_profile *p);
void         input_destroy(koboy_input *in);
void         input_set_touch_transform(koboy_input *in, int raw_max_x, int raw_max_y,
                                       bool transpose, bool flip_x, bool flip_y);
void         input_feed(koboy_input *in, const koboy_ev *evs, size_t n);
void         input_feed_key(koboy_input *in, uint16_t code, bool pressed);
const koboy_input_state *input_state(const koboy_input *in);

/* The state a UI MODE must act on: the HARDWARE page-turn keys, plus the raw
   touch coordinates, and none of the faceplate's touch-synthesised joypad
   bits.

   This exists because the faceplate's zones do not stop being live when a
   full-panel list is drawn over them. recompute() hit-tests A and B against
   the layout permille unconditionally -- it has no notion of a UI mode -- and
   ui_list_feed consumes a rising A/B as page-previous/page-next BEFORE it
   reaches the row hit-test. On the verified 1264x1680 panel the A and B discs
   sit on browser rows 6, 7 and 8, each covering about 17% of the width, so a
   tap on those rows paged the list or did nothing instead of selecting. All
   panels, because the layout is permille.

   The bits are dropped HERE rather than in ui.c: a list has to keep honouring
   the real page-turn keys, which is the entire reason it reads A and B. And
   the composition is a function rather than a mode flag inside input.c
   because a flag would have to be set and cleared correctly on every entry to
   and exit from five run_list call sites reached through three wrappers --
   one missed clear leaves the game deaf. A pure read-only projection has no
   state to leak, and tests/test_ui.c can drive it through input_feed to prove
   the whole chain, which is what a synthetic koboy_input_state could not do. */
void input_ui_state(const koboy_input *in, koboy_input_state *out);

/* True exactly once per MENU tap, then clears.

   Deliberately NOT a joypad bit: there is no libretro button for "menu", and
   borrowing an unused RETRO_DEVICE_ID_JOYPAD_* bit would forward every menu
   tap straight into the running game. */
bool input_take_menu_request(koboy_input *in);
#endif
