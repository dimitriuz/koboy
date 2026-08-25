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

typedef struct koboy_input koboy_input;

koboy_input *input_create(const koboy_config *c, const koboy_profile *p);
void         input_destroy(koboy_input *in);
void         input_set_touch_transform(koboy_input *in, int raw_max_x, int raw_max_y,
                                       bool transpose, bool flip_x, bool flip_y);
void         input_feed(koboy_input *in, const koboy_ev *evs, size_t n);
void         input_feed_key(koboy_input *in, uint16_t code, bool pressed);
const koboy_input_state *input_state(const koboy_input *in);
#endif
