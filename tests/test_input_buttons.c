#include "test.h"
#include "input.h"
#include "config.h"

static void touch_at(koboy_input *in, int slot, int id, int panel_x, int panel_y,
                     int panel_w, int panel_h)
{
    /* transform is identity for these tests, so raw == panel coords */
    koboy_ev e[5]; int n = 0;
    e[n++] = (koboy_ev){ KOBOY_EV_ABS, KOBOY_ABS_MT_SLOT, slot };
    e[n++] = (koboy_ev){ KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, id };
    if (id >= 0) {
        e[n++] = (koboy_ev){ KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_X, panel_x };
        e[n++] = (koboy_ev){ KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_Y, panel_y };
    }
    e[n++] = (koboy_ev){ KOBOY_EV_SYN, 0, 0 };
    input_feed(in, e, (size_t)n);
    (void)panel_w; (void)panel_h;
}

/* Drives a gamepad hat exactly the way a real Xbox Wireless Controller does
   (measured 2026-08-26): both axes as one packet, closed by a single SYN.
   Pass INT_MIN for an axis to omit it from the packet entirely (simulating a
   controller that only reports the axis that changed, which is normal
   evdev behaviour -- an unchanged axis is not re-sent). */
#include <limits.h>
static void hat_at(koboy_input *in, int x, int y)
{
    koboy_ev e[3]; int n = 0;
    if (x != INT_MIN) e[n++] = (koboy_ev){ KOBOY_EV_ABS, KOBOY_ABS_HAT0X, x };
    if (y != INT_MIN) e[n++] = (koboy_ev){ KOBOY_EV_ABS, KOBOY_ABS_HAT0Y, y };
    e[n++] = (koboy_ev){ KOBOY_EV_SYN, 0, 0 };
    input_feed(in, e, (size_t)n);
}

TEST_MAIN({
    koboy_config c; config_defaults(&c);
    c.key_a = 193; c.key_b = 194;
    koboy_profile p; config_resolve_profile(&p, &c, 1264, 1680);
    koboy_input *in = input_create(&c, &p);
    input_set_touch_transform(in, 1264, 1680, false, false, false);

    const int dcx = c.layout.dpad_cx * 1264 / 1000;
    const int dcy = c.layout.dpad_cy * 1680 / 1000;

    /* touching down inside the pad sets the origin and emits no direction */
    touch_at(in, 0, 200, dcx, dcy, 1264, 1680);
    CHECK_EQ_INT(input_state(in)->buttons, 0);

    /* moving past the dead zone to the right emits RIGHT only */
    touch_at(in, 0, 200, dcx + c.dpad_deadzone + 40, dcy, 1264, 1680);
    CHECK_EQ_INT(input_state(in)->buttons & KOBOY_BTN_RIGHT, KOBOY_BTN_RIGHT);
    CHECK_EQ_INT(input_state(in)->buttons & KOBOY_BTN_LEFT, 0);
    CHECK_EQ_INT(input_state(in)->buttons & KOBOY_BTN_UP, 0);

    /* inside the dead zone emits nothing */
    touch_at(in, 0, 200, dcx + 2, dcy + 2, 1264, 1680);
    CHECK_EQ_INT(input_state(in)->buttons, 0);

    /* diagonals emit two directions */
    int d = c.dpad_deadzone + 60;
    touch_at(in, 0, 200, dcx + d, dcy - d, 1264, 1680);
    CHECK(input_state(in)->buttons & KOBOY_BTN_RIGHT);
    CHECK(input_state(in)->buttons & KOBOY_BTN_UP);

    /* HYSTERESIS: once RIGHT is held, shrinking back to just inside the dead
       zone must not release until it falls below deadzone - hysteresis */
    touch_at(in, 0, 200, dcx + d, dcy, 1264, 1680);
    CHECK(input_state(in)->buttons & KOBOY_BTN_RIGHT);
    touch_at(in, 0, 200, dcx + c.dpad_deadzone - 2, dcy, 1264, 1680);
    CHECK(input_state(in)->buttons & KOBOY_BTN_RIGHT);
    touch_at(in, 0, 200, dcx + c.dpad_deadzone - c.dpad_hysteresis - 4, dcy, 1264, 1680);
    CHECK_EQ_INT(input_state(in)->buttons & KOBOY_BTN_RIGHT, 0);

    /* lifting clears directions */
    touch_at(in, 0, -1, 0, 0, 1264, 1680);
    CHECK_EQ_INT(input_state(in)->buttons, 0);

    /* on-screen A and B are absolute circular zones */
    int acx = c.layout.a_cx * 1264 / 1000, acy = c.layout.a_cy * 1680 / 1000;
    touch_at(in, 1, 300, acx, acy, 1264, 1680);
    CHECK(input_state(in)->buttons & KOBOY_BTN_A);
    touch_at(in, 1, -1, 0, 0, 1264, 1680);
    CHECK_EQ_INT(input_state(in)->buttons & KOBOY_BTN_A, 0);

    /* SIMULTANEITY: d-pad on one slot plus A on another */
    touch_at(in, 0, 400, dcx, dcy, 1264, 1680);
    touch_at(in, 0, 400, dcx + d, dcy, 1264, 1680);
    touch_at(in, 1, 401, acx, acy, 1264, 1680);
    CHECK(input_state(in)->buttons & KOBOY_BTN_RIGHT);
    CHECK(input_state(in)->buttons & KOBOY_BTN_A);

    /* calibrated hardware keys map to A and B */
    input_feed_key(in, 193, true);
    CHECK(input_state(in)->buttons & KOBOY_BTN_A);
    input_feed_key(in, 193, false);
    input_feed_key(in, 194, true);
    CHECK(input_state(in)->buttons & KOBOY_BTN_B);
    input_feed_key(in, 194, false);

    /* key_start/key_select route through input_feed_key exactly like key_a/
       key_b -- the mechanism the "a gamepad is just another key device"
       design leans on. c is untouched here, so these are the shipped
       defaults (310/311, BTN_TL/BTN_TR) config_defaults set, not values this
       test chose. */
    CHECK_EQ_INT(c.key_start, 310);
    CHECK_EQ_INT(c.key_select, 311);
    input_feed_key(in, 310, true);
    CHECK(input_state(in)->buttons & KOBOY_BTN_START);
    CHECK_EQ_INT(input_state(in)->buttons & KOBOY_BTN_SELECT, 0);
    input_feed_key(in, 310, false);
    input_feed_key(in, 311, true);
    CHECK(input_state(in)->buttons & KOBOY_BTN_SELECT);
    input_feed_key(in, 311, false);
    CHECK_EQ_INT(input_state(in)->buttons & (KOBOY_BTN_START | KOBOY_BTN_SELECT), 0);

    /* an uncalibrated code changes nothing */
    uint16_t before = input_state(in)->buttons;
    input_feed_key(in, 59, true);
    CHECK_EQ_INT(input_state(in)->buttons, before);

    /* ---------------------------------------------------------- gamepad hat
     *
     * Reset the touch and hardware-key state the tests above left held, so
     * the hat tests start from a known-clean baseline rather than an
     * accumulated one.
     */
    touch_at(in, 0, -1, 0, 0, 1264, 1680);
    touch_at(in, 1, -1, 0, 0, 1264, 1680);
    input_feed_key(in, 194, false);
    CHECK_EQ_INT(input_state(in)->buttons, 0);

    /* The four directions, each exactly the -1/0/+1 the kernel reports --
       NOT some larger magnitude past a deadzone. This is the test that would
       catch a mutant that copied the touch pad's axis_bits() deadzone onto
       the hat: with any deadzone >= 2 applied, a bare magnitude of 1 would
       fail to register and every one of these would fail. */
    hat_at(in, 1, 0);
    CHECK_EQ_INT(input_state(in)->buttons, KOBOY_BTN_RIGHT);
    hat_at(in, -1, 0);
    CHECK_EQ_INT(input_state(in)->buttons, KOBOY_BTN_LEFT);
    hat_at(in, 0, -1);
    CHECK_EQ_INT(input_state(in)->buttons, KOBOY_BTN_UP);
    hat_at(in, 0, 1);
    CHECK_EQ_INT(input_state(in)->buttons, KOBOY_BTN_DOWN);
    hat_at(in, 0, 0);
    CHECK_EQ_INT(input_state(in)->buttons, 0);

    /* Diagonals: both axes in the same packet produce both bits. */
    hat_at(in, 1, -1);
    CHECK(input_state(in)->buttons & KOBOY_BTN_RIGHT);
    CHECK(input_state(in)->buttons & KOBOY_BTN_UP);
    CHECK_EQ_INT(input_state(in)->buttons & (KOBOY_BTN_LEFT | KOBOY_BTN_DOWN), 0);
    hat_at(in, 0, 0);

    /* NO HYSTERESIS: unlike the touch pad's RIGHT-to-release test earlier in
       this file (which must pass back through deadzone - hysteresis before
       releasing), going straight from RIGHT to LEFT in one packet must land
       on LEFT with no trace of RIGHT -- there is no latched `held` state to
       fight. This is the test that catches a mutant that gave the hat its
       own held-direction latch. */
    hat_at(in, 1, 0);
    CHECK_EQ_INT(input_state(in)->buttons, KOBOY_BTN_RIGHT);
    hat_at(in, -1, 0);
    CHECK_EQ_INT(input_state(in)->buttons, KOBOY_BTN_LEFT);
    hat_at(in, 0, 0);
    CHECK_EQ_INT(input_state(in)->buttons, 0);

    /* MERGE, not replace: a user may have a pad connected and still tap the
       drawn cross, press a hardware/gamepad key, or tap the on-screen A --
       none of those sources may shadow another. This is the test that
       catches a mutant that assigned `b = in->hat_bits` instead of ORing it
       in at the top of recompute(). */
    hat_at(in, -1, 0);                                   /* hat: LEFT */
    touch_at(in, 0, 600, dcx + d, dcy, 1264, 1680);       /* touch d-pad: RIGHT */
    CHECK(input_state(in)->buttons & KOBOY_BTN_LEFT);
    CHECK(input_state(in)->buttons & KOBOY_BTN_RIGHT);
    touch_at(in, 0, -1, 0, 0, 1264, 1680);

    input_feed_key(in, 193, true);                       /* hardware/gamepad key: A */
    CHECK(input_state(in)->buttons & KOBOY_BTN_LEFT);     /* hat still there */
    CHECK(input_state(in)->buttons & KOBOY_BTN_A);
    input_feed_key(in, 193, false);

    touch_at(in, 1, 500, acx, acy, 1264, 1680);           /* on-screen A */
    CHECK(input_state(in)->buttons & KOBOY_BTN_LEFT);
    CHECK(input_state(in)->buttons & KOBOY_BTN_A);
    touch_at(in, 1, -1, 0, 0, 1264, 1680);
    hat_at(in, 0, 0);
    CHECK_EQ_INT(input_state(in)->buttons, 0);

    /* The analog stick (ABS_X/ABS_Y, raw codes 0x00/0x01) is deliberately
       ignored, even at a magnitude nowhere near the hat's -1/0/+1 -- it must
       not be mistaken for a fifth and sixth hat axis. Fed directly (not
       through hat_at, which only knows about the two HAT0 codes) to prove
       input_feed's switch really does fall through to `default` for them. */
    {
        koboy_ev e[3];
        e[0] = (koboy_ev){ KOBOY_EV_ABS, 0x00, 60000 };  /* ABS_X */
        e[1] = (koboy_ev){ KOBOY_EV_ABS, 0x01, 60000 };  /* ABS_Y */
        e[2] = (koboy_ev){ KOBOY_EV_SYN, 0, 0 };
        input_feed(in, e, 3);
        CHECK_EQ_INT(input_state(in)->buttons, 0);
    }

    input_destroy(in);
})
