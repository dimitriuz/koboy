#ifndef KOBOY_CALIB_H
#define KOBOY_CALIB_H
#include "config.h"

/* KOBOY_KEY_POWER and the page-turn codes live in koboy.h, with the rest of the
   evdev mirror and the measurement they came from. */

typedef struct { int stage; uint16_t key_a, key_b; } koboy_calib;

bool        calib_needed(const koboy_config *c);
void        calib_begin(koboy_calib *k, koboy_config *c);
const char *calib_prompt(const koboy_calib *k);
const char *calib_escape_prompt(void);
bool        calib_feed_key(koboy_calib *k, uint16_t code);
bool        calib_commit(const koboy_calib *k, koboy_config *c, const char *ini_path);

/* Leave calibration without having captured anything, with a mapping that still
   works. The calibration loop can be escaped by a touch (see main.c), and it MUST
   be escapable: the loop only advances on a raw key press, so on a touch-only Kobo
   -- Clara family, Elipsa, spec §3 supports all of them -- there is no key to
   press and nothing but the power button did anything. Escaping must not leave the
   zero sentinel behind either, or input_feed_key would ignore every key for the
   rest of the session; it fills in the measured page-turn defaults and guarantees
   two distinct, non-power codes. */
void        calib_escape(koboy_config *c);
#endif
