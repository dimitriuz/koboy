#ifndef KOBOY_CALIB_H
#define KOBOY_CALIB_H
#include "config.h"

#define KOBOY_KEY_POWER 116

typedef struct { int stage; uint16_t key_a, key_b; } koboy_calib;

bool        calib_needed(const koboy_config *c);
void        calib_begin(koboy_calib *k, koboy_config *c);
const char *calib_prompt(const koboy_calib *k);
bool        calib_feed_key(koboy_calib *k, uint16_t code);
bool        calib_commit(const koboy_calib *k, koboy_config *c, const char *ini_path);
#endif
