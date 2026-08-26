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

/* Press one finger at a panel coordinate, read the resulting buttons, lift it
   again. Goes through input_feed rather than reimplementing the hit tests, so
   what is asserted is the zone geometry the emulator actually uses. The caller
   must have installed an identity touch transform (raw_max == panel - 1), which
   makes scale_axis a no-op and the probe pixel-exact. */
static uint16_t touch_probe(koboy_input *in, int x, int y)
{
    koboy_ev down[5] = {
        { KOBOY_EV_ABS, KOBOY_ABS_MT_SLOT,        0 },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, 1 },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_X,  x },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_Y,  y },
        { KOBOY_EV_SYN, 0, 0 },
    };
    koboy_ev up[2] = {
        { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, -1 },
        { KOBOY_EV_SYN, 0, 0 },
    };
    input_feed(in, down, 5);
    uint16_t b = input_state(in)->buttons;
    input_feed(in, up, 2);
    return b;
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

    /* #1: dpad_mode = cross is the SHIPPED DEFAULT and the behaviour that
       distinguishes it from RELATIVE had no coverage, because every existing
       touch test lands on the pad centre -- where the two modes are
       identical by construction.

       The distinction: CROSS derives direction from the drawn cross's fixed
       centre, so a tap anywhere in the pad steers. RELATIVE sets its origin at
       the touch point, so the same tap steers nowhere until the finger drags.
       That difference is why the d-pad was unusable in relative mode on the
       device: the chrome draws an absolute cross and users press its arms. */
    {
        koboy_config c; config_defaults(&c);
        koboy_profile p;
        config_resolve_profile(&p, &c, 1264, 1680);

        const int W = p.panel_w, H = p.panel_h;
        int dcx = c.layout.dpad_cx * W / 1000;
        int dcy = c.layout.dpad_cy * H / 1000;
        int dr  = c.layout.dpad_r  * W / 1000;

        /* Well past the deadzone, well inside the pad: the right-hand arm. */
        int off = c.dpad_deadzone + c.dpad_hysteresis + 20;
        CHECK(off < dr);
        int tx = dcx + off, ty = dcy;

        c.dpad_mode = KOBOY_DPAD_CROSS;
        koboy_input *cross = input_create(&c, &p);
        CHECK(cross != NULL);
        input_set_touch_transform(cross, W - 1, H - 1, false, false, false);
        CHECK_EQ_INT(touch_probe(cross, tx, ty), KOBOY_BTN_RIGHT);
        input_destroy(cross);

        c.dpad_mode = KOBOY_DPAD_RELATIVE;
        koboy_input *rel = input_create(&c, &p);
        CHECK(rel != NULL);
        input_set_touch_transform(rel, W - 1, H - 1, false, false, false);
        /* Same coordinate, and RELATIVE must report NO direction: the origin
           is the touch itself, so displacement is zero. */
        CHECK_EQ_INT(touch_probe(rel, tx, ty), 0);
        input_destroy(rel);
    }

    /* #2: flip_x / flip_y are wired to real per-device probe data in
       platform_kobo.c but no caller in the test suite ever passes true, so any
       Kobo needing a mirrored touch axis depends on an untested path.
       Irrelevant on the verified Libra 2, load-bearing on hardware nobody has
       tried. */
    {
        koboy_config c; config_defaults(&c);
        c.dpad_mode = KOBOY_DPAD_CROSS;
        koboy_profile p;
        config_resolve_profile(&p, &c, 1264, 1680);
        const int W = p.panel_w, H = p.panel_h;

        int acx = c.layout.a_cx * W / 1000;
        int acy = c.layout.a_cy * H / 1000;

        /* Unflipped: touching A's centre presses A. */
        koboy_input *plain = input_create(&c, &p);
        CHECK(plain != NULL);
        input_set_touch_transform(plain, W - 1, H - 1, false, false, false);
        CHECK_EQ_INT(touch_probe(plain, acx, acy) & KOBOY_BTN_A, KOBOY_BTN_A);
        input_destroy(plain);

        /* flip_x: the MIRRORED raw coordinate must now land on A, and A's own
           coordinate must not. */
        koboy_input *fx = input_create(&c, &p);
        CHECK(fx != NULL);
        input_set_touch_transform(fx, W - 1, H - 1, false, true, false);
        CHECK_EQ_INT(touch_probe(fx, W - 1 - acx, acy) & KOBOY_BTN_A, KOBOY_BTN_A);
        CHECK_EQ_INT(touch_probe(fx, acx, acy) & KOBOY_BTN_A, 0);
        input_destroy(fx);

        /* flip_y likewise. */
        koboy_input *fy = input_create(&c, &p);
        CHECK(fy != NULL);
        input_set_touch_transform(fy, W - 1, H - 1, false, false, true);
        CHECK_EQ_INT(touch_probe(fy, acx, H - 1 - acy) & KOBOY_BTN_A, KOBOY_BTN_A);
        input_destroy(fy);
    }

    /* The MENU zone: a tap reports once and then clears, and it is NOT a
       joypad bit. There is no libretro button for "menu", and borrowing an
       unused bit would forward it straight to the core. */
    {
        koboy_config c; config_defaults(&c);
        koboy_profile p;
        config_resolve_profile(&p, &c, 1264, 1680);
        const int W = p.panel_w, H = p.panel_h;

        koboy_input *in2 = input_create(&c, &p);
        CHECK(in2 != NULL);
        input_set_touch_transform(in2, W - 1, H - 1, false, false, false);

        CHECK_EQ_INT(input_take_menu_request(in2), 0);

        int mx = c.layout.menu_cx * W / 1000;
        int my = c.layout.menu_cy * H / 1000;
        uint16_t bits = touch_probe(in2, mx, my);
        CHECK_EQ_INT(bits, 0);                        /* no joypad bit */
        CHECK_EQ_INT(input_take_menu_request(in2), 1); /* latched once */
        CHECK_EQ_INT(input_take_menu_request(in2), 0); /* and cleared */

        input_destroy(in2);
    }

    /* Edge-trigger regression: mutating the latch condition from
       "menu_now && !in->menu_touching" to plain "menu_now" left the block
       above green, because touch_probe only performs one down/up cycle and
       a single tap latches once either way regardless of which form is
       used -- disclosed, not fixed, at the time that gap was found. The
       edge is the half that matters in play: a finger RESTING on the zone
       across several polls (no lift, no move -- exactly what a held touch
       reports on every SYN) must still latch exactly once, or the menu
       reopens on every frame the finger stays down. */
    {
        koboy_config c; config_defaults(&c);
        koboy_profile p;
        config_resolve_profile(&p, &c, 1264, 1680);
        const int W = p.panel_w, H = p.panel_h;

        koboy_input *in3 = input_create(&c, &p);
        CHECK(in3 != NULL);
        input_set_touch_transform(in3, W - 1, H - 1, false, false, false);

        int mx = c.layout.menu_cx * W / 1000;
        int my = c.layout.menu_cy * H / 1000;

        koboy_ev down[5] = {
            { KOBOY_EV_ABS, KOBOY_ABS_MT_SLOT,        0 },
            { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, 1 },
            { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_X,  mx },
            { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_Y,  my },
            { KOBOY_EV_SYN, 0, 0 },
        };
        input_feed(in3, down, 5);
        CHECK_EQ_INT(input_take_menu_request(in3), 1); /* the initial touch-down */

        /* Three more SYNs with the finger still down and unmoved -- the
           tracking id is never re-sent and there is no lift event, matching
           a resting touch on a real protocol-B stream. Taken once already,
           above, WHILE still touching: this is the case a same-poll re-latch
           can only be seen in, because a fresh take() after release always
           reads 1 regardless of which form of the condition is running. */
        koboy_ev syn_only[1] = { { KOBOY_EV_SYN, 0, 0 } };
        input_feed(in3, syn_only, 1);
        input_feed(in3, syn_only, 1);
        input_feed(in3, syn_only, 1);

        /* Still held, never re-latched: exactly the edge-triggered contract. */
        CHECK_EQ_INT(input_take_menu_request(in3), 0);

        koboy_ev up[2] = {
            { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, -1 },
            { KOBOY_EV_SYN, 0, 0 },
        };
        input_feed(in3, up, 2);
        CHECK_EQ_INT(input_take_menu_request(in3), 0); /* lift alone latches nothing */

        input_destroy(in3);
    }
})
