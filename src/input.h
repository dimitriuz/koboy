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

/* The two EV_SYN codes that mean different things. SYN_REPORT (0) ends a
   FRAME; SYN_MT_REPORT (2) ends one CONTACT inside a protocol-A frame, and is
   the only thing that separates two fingers on a panel with no ABS_MT_SLOT.
   input.c decoded every EV_SYN as a frame boundary until it had to tell a
   second finger from the first. */
#define KOBOY_SYN_REPORT    0x00
#define KOBOY_SYN_MT_REPORT 0x02

#define KOBOY_ABS_MT_SLOT        0x2f
#define KOBOY_ABS_MT_POSITION_X  0x35
#define KOBOY_ABS_MT_POSITION_Y  0x36
#define KOBOY_ABS_MT_TRACKING_ID 0x39

/* The SINGLE-touch position axes, which the pre-multitouch Kobos (Touch A/B/C,
   Mini, Glo, Aura HD) report instead of the ABS_MT_ pair above. THE SAME TWO
   CODES ARE A GAMEPAD'S ANALOG STICK, which is why input_feed takes a source
   and only honours them on KOBOY_EV_SRC_TOUCH -- see input.c. */
#define KOBOY_ABS_X              0x00
#define KOBOY_ABS_Y              0x01

/* The d-pad on a Bluetooth gamepad. MEASURED on a real Xbox Wireless
   Controller, 2026-08-26: it does NOT arrive as EV_KEY like the page-turn
   buttons -- it is a HAT SWITCH, two absolute axes each taking exactly
   -1/0/+1. input.c decodes them straight into KOBOY_BTN_UP/DOWN/LEFT/RIGHT.
   ABS_X/ABS_Y (the analog stick, 0x00/0x01) are deliberately NOT named or
   handled -- see input.c's input_feed. */
#define KOBOY_ABS_HAT0X 0x10
#define KOBOY_ABS_HAT0Y 0x11

typedef struct koboy_input koboy_input;

koboy_input *input_create(const koboy_config *c, const koboy_profile *p);
void         input_destroy(koboy_input *in);

/* The PANEL rect a touch is normalised against to become a libretro pointer,
   in the LCD layout. Seeded from the profile's reserved rect at input_create,
   refined by main.c once per presented frame to the rect the frame ACTUALLY
   occupies (video_frame_rect) -- smaller whenever the core renders below max,
   as a Game & Watch title does several times a second.

   A SETTER rather than input.c reading the video object: input.c has no
   video.h dependency, and the profile it copied at create time goes stale on a
   mid-session re-fit anyway. Ignored in the DMG layout. LIVE GUARD: a
   degenerate rect (w or h < 1) is rejected rather than stored -- it would make
   the normalisation divide by zero. */
void         input_set_pointer_rect(koboy_input *in, int x, int y, int w, int h);
void         input_set_touch_transform(koboy_input *in, int raw_max_x, int raw_max_y,
                                       bool transpose, bool flip_x, bool flip_y);
/* WHICH NODE a batch of events came off, because two codes mean different
   things on different nodes and there is no way to tell from the event: a
   touchscreen's ABS_X/ABS_Y are a finger's position, a gamepad's are the analog
   stick, and BTN_TOUCH is a contact flag on the one and nothing at all on the
   other. The KEY nodes and a gamepad share KOBOY_EV_SRC_BUTTONS: neither
   carries touch, which is the only distinction the decode makes. */
typedef enum { KOBOY_EV_SRC_TOUCH = 0, KOBOY_EV_SRC_BUTTONS } koboy_ev_source;

void         input_feed_from(koboy_input *in, koboy_ev_source src,
                             const koboy_ev *evs, size_t n);
/* == input_feed_from(in, KOBOY_EV_SRC_TOUCH, ...). The touchscreen spelling is
   the bare one because it is what every caller but platform_kobo.c's key and
   gamepad nodes means; a NEW node kind must pick its source explicitly. */
void         input_feed(koboy_input *in, const koboy_ev *evs, size_t n);
void         input_feed_key(koboy_input *in, uint16_t code, bool pressed);
const koboy_input_state *input_state(const koboy_input *in);

/* The state a UI MODE must act on: the HARDWARE page-turn keys plus raw touch
   coordinates, and NONE of the faceplate's touch-synthesised joypad bits.

   The faceplate's zones do not stop being live when a full-panel list is drawn
   over them: recompute() hit-tests A and B against the layout permille
   unconditionally, and ui_list_feed consumes a rising A/B as page-prev/next
   BEFORE the row hit-test. On the verified 1264x1680 panel the A and B discs
   sit on browser rows 6, 7 and 8 covering ~17% of the width each, so a tap
   there paged the list or did nothing instead of selecting. All panels, because
   the layout is permille.

   Dropped HERE rather than in ui.c: a list must keep honouring the real
   page-turn keys, which is why it reads A and B at all. A FUNCTION rather than
   a mode flag in input.c, because a flag would have to be set and cleared
   correctly at five run_list call sites reached through three wrappers, and
   one missed clear leaves the game deaf. A read-only projection has no state
   to leak, and tests/test_ui.c can drive it through input_feed to prove the
   whole chain. */
void input_ui_state(const koboy_input *in, koboy_input_state *out);

/* True exactly once per MENU tap, then clears. Deliberately NOT a joypad bit:
   there is no libretro button for "menu", and borrowing an unused
   RETRO_DEVICE_ID_JOYPAD_* would forward every menu tap into the game. */
bool input_take_menu_request(koboy_input *in);
#endif
