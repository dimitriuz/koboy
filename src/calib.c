#include "calib.h"
#include <string.h>

bool calib_needed(const koboy_config *c) { return c->key_a == 0 || c->key_b == 0; }

void calib_begin(koboy_calib *k, koboy_config *c)
{
    (void)c;
    memset(k, 0, sizeof *k);
}

/* Two lines, because the way OUT has to be on the panel too. The loop advances
   only on a hardware key, so a device with no page-turn buttons cannot answer the
   question at all; without the second line the user is looking at a prompt they
   have no way to satisfy. Uppercase-and-space only: main.c's 5x7 font has no
   punctuation and renders anything else as a blank. */
const char *calib_prompt(const koboy_calib *k)
{
    switch (k->stage) {
    case 0:  return "Press the button you want as  A";
    case 1:  return "Press the button you want as  B";
    default: return "Calibration complete";
    }
}

const char *calib_escape_prompt(void)
{
    return "Or tap the screen to keep the defaults";
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

/* Contract in calib.h. Deliberately unconditional about the RESULT rather than
   about the input: whatever the config held, afterwards key_a and key_b are two
   different non-zero codes and neither is the power button, so calib_needed() is
   false and input_feed_key() has something to match. Nothing is written to the
   ini: an escape is "carry on with the defaults for now", not a calibration, and
   the next run should offer the prompt again if the user had asked for it. */
void calib_escape(koboy_config *c)
{
    if (c->key_a == 0 || c->key_a == KOBOY_KEY_POWER)
        c->key_a = KOBOY_KEY_PAGE_F23;
    if (c->key_b == 0 || c->key_b == KOBOY_KEY_POWER || c->key_b == c->key_a)
        c->key_b = (c->key_a == KOBOY_KEY_PAGE_F24) ? KOBOY_KEY_PAGE_F23
                                                    : KOBOY_KEY_PAGE_F24;
}

bool calib_commit(const koboy_calib *k, koboy_config *c, const char *ini_path)
{
    if (k->stage < 2) return false;
    c->key_a = k->key_a;
    c->key_b = k->key_b;
    return config_save_keys(ini_path, k->key_a, k->key_b);
}
