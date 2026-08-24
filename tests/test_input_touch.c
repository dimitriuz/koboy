#include "test.h"
#include "input.h"
#include "config.h"

/* Protocol B, as the Elan panel reports it: slot, tracking id, x, y, syn. */
static void mt(koboy_ev *e, int *n, int slot, int id, int x, int y)
{
    e[(*n)++] = (koboy_ev){ KOBOY_EV_ABS, KOBOY_ABS_MT_SLOT, slot };
    e[(*n)++] = (koboy_ev){ KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, id };
    if (id >= 0) {
        e[(*n)++] = (koboy_ev){ KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_X, x };
        e[(*n)++] = (koboy_ev){ KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_Y, y };
    }
    e[(*n)++] = (koboy_ev){ KOBOY_EV_SYN, 0, 0 };
}

TEST_MAIN({
    koboy_config c; config_defaults(&c);
    koboy_profile p; config_resolve_profile(&p, &c, 1264, 1680);
    koboy_input *in = input_create(&c, &p);
    CHECK(in != NULL);

    /* Measured Libra 2 geometry: raw X spans 1680 and raw Y spans 1264 on a
       1264x1680 panel, so the axes are transposed. */
    input_set_touch_transform(in, 1680, 1264, true, false, false);

    koboy_ev ev[32]; int n = 0;
    mt(ev, &n, 0, 100, 0, 0);
    input_feed(in, ev, (size_t)n);
    CHECK(input_state(in)->touch[0].down);
    CHECK_EQ_INT(input_state(in)->touch[0].x, 0);
    CHECK_EQ_INT(input_state(in)->touch[0].y, 0);

    /* raw far corner maps to the panel far corner */
    n = 0; mt(ev, &n, 0, 100, 1680, 1264);
    input_feed(in, ev, (size_t)n);
    CHECK_EQ_INT(input_state(in)->touch[0].x, 1263);
    CHECK_EQ_INT(input_state(in)->touch[0].y, 1679);

    /* transposition: raw x advances the panel's Y axis */
    n = 0; mt(ev, &n, 0, 100, 840, 0);
    input_feed(in, ev, (size_t)n);
    CHECK_EQ_INT(input_state(in)->touch[0].x, 0);
    CHECK(input_state(in)->touch[0].y > 800 && input_state(in)->touch[0].y < 880);

    /* a second slot is tracked independently */
    n = 0; mt(ev, &n, 1, 101, 100, 200);
    input_feed(in, ev, (size_t)n);
    CHECK(input_state(in)->touch[0].down);
    CHECK(input_state(in)->touch[1].down);

    /* tracking id -1 lifts that slot only */
    n = 0; mt(ev, &n, 0, -1, 0, 0);
    input_feed(in, ev, (size_t)n);
    CHECK(!input_state(in)->touch[0].down);
    CHECK(input_state(in)->touch[1].down);

    input_destroy(in);
})
