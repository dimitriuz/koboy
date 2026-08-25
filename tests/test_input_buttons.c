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

    /* an uncalibrated code changes nothing */
    uint16_t before = input_state(in)->buttons;
    input_feed_key(in, 59, true);
    CHECK_EQ_INT(input_state(in)->buttons, before);

    input_destroy(in);
})
