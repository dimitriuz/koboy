#include "calib.h"
#include <string.h>

bool calib_needed(const koboy_config *c) { return c->key_a == 0 || c->key_b == 0; }

void calib_begin(koboy_calib *k, koboy_config *c)
{
    (void)c;
    memset(k, 0, sizeof *k);
}

const char *calib_prompt(const koboy_calib *k)
{
    switch (k->stage) {
    case 0:  return "Press the button you want as  A";
    case 1:  return "Press the button you want as  B";
    default: return "Calibration complete";
    }
}

/* Returns true once both buttons are captured. Rejects the power button, so
   calibration can never make the device impossible to sleep, and rejects a
   duplicate code, so A and B cannot collide. */
bool calib_feed_key(koboy_calib *k, uint16_t code)
{
    if (code == KOBOY_KEY_POWER) return false;
    if (k->stage == 0)      { k->key_a = code; k->stage = 1; return false; }
    if (k->stage == 1) {
        if (code == k->key_a) return false;
        k->key_b = code; k->stage = 2; return true;
    }
    return true;
}

bool calib_commit(const koboy_calib *k, koboy_config *c, const char *ini_path)
{
    if (k->stage < 2) return false;
    c->key_a = k->key_a;
    c->key_b = k->key_b;
    return config_save_keys(ini_path, k->key_a, k->key_b);
}
