#include "test.h"
#include "input.h"
#include "chrome.h"   /* chrome_lcd_layout: the strip's zones under test */
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
/* Press one finger and LEAVE IT DOWN, so state that only exists while a
   finger is on the panel can be read. touch_probe below lifts before it
   returns, which is right for reading st.buttons (recompute latches those)
   and WRONG for reading st.pointer.pressed -- that is cleared by the lift, so
   a check made after touch_probe returns passes whether or not the press ever
   happened. It did: the "a drawn control wins the touch" mutant survived that
   way until this helper existed. */
static void touch_hold(koboy_input *in, int x, int y)
{
    koboy_ev down[5] = {
        { KOBOY_EV_ABS, KOBOY_ABS_MT_SLOT,        0 },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, 1 },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_X,  x },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_Y,  y },
        { KOBOY_EV_SYN, 0, 0 },
    };
    input_feed(in, down, 5);
}

static void touch_lift(koboy_input *in)
{
    koboy_ev up[2] = {
        { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, -1 },
        { KOBOY_EV_SYN, 0, 0 },
    };
    input_feed(in, up, 2);
}

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
    koboy_profile p; config_resolve_profile(&p, &c, 1264, 1680, KOBOY_GB_W, KOBOY_GB_H, KOBOY_GB_W, KOBOY_GB_H);
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
        config_resolve_profile(&p, &c, 1264, 1680, KOBOY_GB_W, KOBOY_GB_H, KOBOY_GB_W, KOBOY_GB_H);

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
        config_resolve_profile(&p, &c, 1264, 1680, KOBOY_GB_W, KOBOY_GB_H, KOBOY_GB_W, KOBOY_GB_H);
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

    /* THE EXTRA DISCS, on the DMG faceplate. Two systems have controls the
       Game Boy did not, and each disc's BIT is read off its core's own input
       descriptors rather than chosen here: the Pokemon Mini's C is
       RETRO_DEVICE_ID_JOYPAD_R -- bit 11, KOBOY_BTN_R1
       (third_party/pokemini/libretro/libretro.c), and beetle-wswan's rotated
       key map puts the WonderSwan's A and B on JOYPAD_L / JOYPAD_R -- bits 10
       and 11 (third_party/wswan/libretro.c, map[1]). The whole button word is
       compared each time, not just the bit under test: a zone that also fires
       its neighbour is worse than one that fires nothing. */
    {
        /* One system per row, with the geometry its core really reports: the
           Pokemon Mini's 96x64 (internal video scale at 1x, measured through
           scripts/probe_core.c) and the WonderSwan's square 224x224 max. */
        static const struct { const char *rom; int n, gw, gh; uint16_t bits[2]; } sys[] = {
            { "/roms/Pokemon Tetris.min", 1,  96,  64, { KOBOY_BTN_R1, 0 } },
            { "/roms/GunPey.ws",          2, 224, 224, { KOBOY_BTN_L1, KOBOY_BTN_R1 } },
        };

        for (size_t si = 0; si < sizeof sys / sizeof sys[0]; si++) {
            koboy_config c; config_defaults(&c);
            config_extra_buttons_for_rom(&c.layout, sys[si].rom);
            koboy_profile p;
            CHECK(config_resolve_profile(&p, &c, 1264, 1680,
                                         sys[si].gw, sys[si].gh,
                                         sys[si].gw, sys[si].gh));
            const int W = p.panel_w, H = p.panel_h;

            koboy_input *ci = input_create(&c, &p);
            CHECK(ci != NULL);
            input_set_touch_transform(ci, W - 1, H - 1, false, false, false);

            for (int e = 0; e < sys[si].n; e++) {
                CHECK_EQ_INT(c.layout.extra[e].bit, sys[si].bits[e]);
                int ccx = c.layout.extra[e].cx * W / 1000;
                int ccy = c.layout.extra[e].cy * H / 1000;
                int cr  = c.layout.extra[e].r  * W / 1000;
                uint16_t want = sys[si].bits[e];

                /* The centre, and each of the four extremes one pixel inside
                   the rim. A centre-only probe passes against a zone hit-
                   tested from the wrong radius, or from A's centre with a
                   bigger radius -- the rim is what pins the geometry. */
                CHECK_EQ_INT(touch_probe(ci, ccx, ccy), want);
                CHECK_EQ_INT(touch_probe(ci, ccx - cr + 1, ccy), want);
                CHECK_EQ_INT(touch_probe(ci, ccx + cr - 1, ccy), want);
                CHECK_EQ_INT(touch_probe(ci, ccx, ccy - cr + 1), want);
                CHECK_EQ_INT(touch_probe(ci, ccx, ccy + cr - 1), want);
                /* Just outside is nothing at all -- not A, not B, not MENU,
                   and not the OTHER extra disc. */
                CHECK_EQ_INT(touch_probe(ci, ccx + cr + 2, ccy), 0);
            }

            /* A and B still answer A and B: an extra zone that swallowed a
               neighbour would be a regression dressed as a feature. */
            CHECK_EQ_INT(touch_probe(ci, c.layout.a_cx * W / 1000,
                                         c.layout.a_cy * H / 1000), KOBOY_BTN_A);
            CHECK_EQ_INT(touch_probe(ci, c.layout.b_cx * W / 1000,
                                         c.layout.b_cy * H / 1000), KOBOY_BTN_B);
            input_destroy(ci);

            /* THE CONTROL, and it is the one that matters most: on a system
               with no extra discs the SAME coordinates must report nothing. A
               zero-radius circle at (0,0) is not the answer either -- in_circle
               uses <=, so an unguarded zone would report its bit for a touch at
               exactly the panel origin, which is a real coordinate a real
               finger can produce. Both are checked. */
            koboy_config g; config_defaults(&g);
            config_extra_buttons_for_rom(&g.layout, "/roms/Metroid.nes");
            CHECK_EQ_INT(g.layout.extra[0].r, 0);
            CHECK_EQ_INT(g.layout.extra[1].r, 0);
            koboy_profile gp;
            CHECK(config_resolve_profile(&gp, &g, 1264, 1680,
                                         KOBOY_GB_W, KOBOY_GB_H,
                                         KOBOY_GB_W, KOBOY_GB_H));
            koboy_input *gi = input_create(&g, &gp);
            CHECK(gi != NULL);
            input_set_touch_transform(gi, W - 1, H - 1, false, false, false);
            for (int e = 0; e < sys[si].n; e++) {
                int ccx = c.layout.extra[e].cx * W / 1000;
                int ccy = c.layout.extra[e].cy * H / 1000;
                CHECK_EQ_INT(touch_probe(gi, ccx, ccy) & sys[si].bits[e], 0);
            }
            CHECK_EQ_INT(touch_probe(gi, 0, 0) & (KOBOY_BTN_L1 | KOBOY_BTN_R1), 0);
            input_destroy(gi);
        }

        /* r == 0 MEANS ABSENT, whatever else the slot says -- and this is the
           only place that contract can be broken on purpose. Now that each
           disc carries its own bit, an empty slot cleared by
           config_extra_buttons_for_rom also carries bit 0, so dropping the
           guard in input.c reports nothing anyway and the guard looks dead.
           It is not: a slot with a bit but no radius is one forgotten
           assignment away, and in_circle uses <= -- so a zero-radius zone at
           (0,0) would fire for a touch at exactly the panel origin, which is
           a real coordinate a real finger can produce. Built by hand here
           because no ROM extension can produce it. */
        koboy_config m; config_defaults(&m);
        config_extra_buttons_for_rom(&m.layout, "/roms/Metroid.nes");
        m.layout.extra[0].bit = KOBOY_BTN_R1;   /* bit set, radius left 0 */
        koboy_profile mp;
        CHECK(config_resolve_profile(&mp, &m, 1264, 1680,
                                     KOBOY_GB_W, KOBOY_GB_H,
                                     KOBOY_GB_W, KOBOY_GB_H));
        koboy_input *mi = input_create(&m, &mp);
        CHECK(mi != NULL);
        input_set_touch_transform(mi, mp.panel_w - 1, mp.panel_h - 1,
                                  false, false, false);
        CHECK_EQ_INT(touch_probe(mi, 0, 0), 0);
        input_destroy(mi);
    }

    /* The MENU zone: a tap reports once and then clears, and it is NOT a
       joypad bit. There is no libretro button for "menu", and borrowing an
       unused bit would forward it straight to the core. */
    {
        koboy_config c; config_defaults(&c);
        koboy_profile p;
        config_resolve_profile(&p, &c, 1264, 1680, KOBOY_GB_W, KOBOY_GB_H, KOBOY_GB_W, KOBOY_GB_H);
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
        config_resolve_profile(&p, &c, 1264, 1680, KOBOY_GB_W, KOBOY_GB_H, KOBOY_GB_W, KOBOY_GB_H);
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

    /* ================================ the LCD layout: touch becomes a pointer
     *
     * A Game & Watch unit draws its OWN buttons into its artwork, at
     * different positions per title, so koboy forwards the touch position
     * instead of synthesising joypad bits from a faceplate that is not there.
     * Everything below drives input_feed -- the real event stream -- rather
     * than poking koboy_input_state, so what is asserted is the chain the
     * device runs.
     */
    {
        koboy_config lc; config_defaults(&lc);
        lc.layout_mode = KOBOY_LAYOUT_LCD;
        koboy_profile lp;
        /* Mickey Mouse's measured 654x396 on the verified panel: a
           1264x765 rect at (0,397). */
        CHECK(config_resolve_profile(&lp, &lc, 1264, 1680, 654, 396, 654, 396));
        CHECK_EQ_INT(lp.game_w, 1264);
        CHECK_EQ_INT(lp.game_h, 765);

        koboy_input *li = input_create(&lc, &lp);
        CHECK(li != NULL);
        input_set_touch_transform(li, 1263, 1679, false, false, false);

        /* Nothing touched yet: not pressed. */
        CHECK(!input_state(li)->pointer.pressed);

        koboy_ev up[2] = {
            { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, -1 },
            { KOBOY_EV_SYN, 0, 0 },
        };

        /* THE EDGES, exactly. -0x7fff at the first pixel and +0x7fff at the
           last, because the core divides straight back by 65534 over the
           frame's own width (third_party/gw/gwlua/functions.c) -- a mapping
           that only got the left edge right would make the rightmost column
           of artwork, where several titles put a button, unreachable. */
        koboy_ev tl[5] = {
            { KOBOY_EV_ABS, KOBOY_ABS_MT_SLOT,        0 },
            { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, 1 },
            { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_X,  lp.game_x },
            { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_Y,  lp.game_y },
            { KOBOY_EV_SYN, 0, 0 },
        };
        input_feed(li, tl, 5);
        CHECK(input_state(li)->pointer.pressed);
        CHECK_EQ_INT(input_state(li)->pointer.x, -32767);
        CHECK_EQ_INT(input_state(li)->pointer.y, -32767);
        /* And no joypad bit was synthesised from it -- the faceplate's zones
           are dead in this layout. */
        CHECK_EQ_INT(input_state(li)->buttons, 0);
        input_feed(li, up, 2);

        /* THE PROBE THAT MAKES THE LINE ABOVE MEAN SOMETHING.
           (0,397) is the game rect's own corner: no DMG control lives there
           either, so `buttons == 0` holds whether or not the faceplate's
           zones are still live and the check cannot fail. Found by scanning
           the panel: 12864 points press a DMG control and nothing under LCD;
           (1032,1020) is one of them -- dead centre of the DMG A button
           (buttons 0x100 in the DMG layout), and under LCD it is the gap
           between the game rect's bottom edge (247+765 = 1012) and the top
           of the control strip (1260), where nothing at all is drawn.

           So this asserts what the comment above claims: feed the same touch
           under LCD and no joypad bit appears. Mutant: delete the
           KOBOY_LAYOUT_LCD branch in input.c's recompute() so LCD falls
           through to the faceplate's zones -- this line then reports 0x100
           and fails, while the corner probe above stays green. */
        koboy_ev dmg_a[5] = {
            { KOBOY_EV_ABS, KOBOY_ABS_MT_SLOT,        0 },
            { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, 1 },
            { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_X,  1032 },
            { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_Y,  1020 },
            { KOBOY_EV_SYN, 0, 0 },
        };
        input_feed(li, dmg_a, 5);
        CHECK_EQ_INT(input_state(li)->buttons, 0);
        /* Nor is it a pointer press: 1020 is below the game rect, so this
           point is inert under LCD in both channels, not merely silent in
           the one being asserted. */
        CHECK(!input_state(li)->pointer.pressed);
        input_feed(li, up, 2);

        koboy_ev br[5] = {
            { KOBOY_EV_ABS, KOBOY_ABS_MT_SLOT,        0 },
            { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, 1 },
            { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_X,  lp.game_x + lp.game_w - 1 },
            { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_Y,  lp.game_y + lp.game_h - 1 },
            { KOBOY_EV_SYN, 0, 0 },
        };
        input_feed(li, br, 5);
        CHECK(input_state(li)->pointer.pressed);
        CHECK_EQ_INT(input_state(li)->pointer.x, 32767);
        CHECK_EQ_INT(input_state(li)->pointer.y, 32767);

        /* RELEASE MUST CLEAR PRESSED, or the core holds the artwork's button
           down forever. The coordinates deliberately do NOT reset -- that is
           what a real pointer device reports, and the core reads PRESSED. */
        input_feed(li, up, 2);
        CHECK(!input_state(li)->pointer.pressed);
        CHECK_EQ_INT(input_state(li)->pointer.x, 32767);
        CHECK_EQ_INT(input_state(li)->pointer.y, 32767);

        /* The centre, to within the rounding of one 65534-step. This is the
           check that would catch an axis swap or a half-scale error that the
           two corners above cannot: they are symmetric. */
        {
            int cx = lp.game_x + lp.game_w / 4;         /* a quarter across... */
            int cy = lp.game_y + lp.game_h * 3 / 4;     /* ...three quarters down */
            koboy_ev q[5] = {
                { KOBOY_EV_ABS, KOBOY_ABS_MT_SLOT,        0 },
                { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, 1 },
                { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_X,  cx },
                { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_Y,  cy },
                { KOBOY_EV_SYN, 0, 0 },
            };
            input_feed(li, q, 5);
            int wantx = (int)((long)(lp.game_w / 4) * 65534 / (lp.game_w - 1)) - 32767;
            int wanty = (int)((long)(lp.game_h * 3 / 4) * 65534 / (lp.game_h - 1)) - 32767;
            CHECK_EQ_INT(input_state(li)->pointer.x, wantx);
            CHECK_EQ_INT(input_state(li)->pointer.y, wanty);
            /* Not symmetric, so an x/y swap really is visible here. */
            CHECK(wantx < 0 && wanty > 0);
            input_feed(li, up, 2);
        }

        /* A touch on the CASE -- above the artwork, inside the panel -- is
           not a pointer press at all. The core would otherwise see a click
           on artwork the user never touched. */
        {
            koboy_ev off[5] = {
                { KOBOY_EV_ABS, KOBOY_ABS_MT_SLOT,        0 },
                { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, 1 },
                { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_X,  600 },
                { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_Y,  lp.game_y - 20 },
                { KOBOY_EV_SYN, 0, 0 },
            };
            input_feed(li, off, 5);
            CHECK(!input_state(li)->pointer.pressed);
            input_feed(li, up, 2);
        }

        /* MENU WINS. A tap on the MENU zone opens koboy's menu and must NOT
           also reach the core as a pointer press -- the two would otherwise
           fire together on any layout where they overlap, and "MENU also
           pressed something in the game" is invisible until it corrupts a
           run. Asserted through the SAME zone chrome.c draws. */
        {
            koboy_rect m;
            chrome_lcd_menu_rect(&lp, &m);
            koboy_ev mt[5] = {
                { KOBOY_EV_ABS, KOBOY_ABS_MT_SLOT,        0 },
                { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, 1 },
                { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_X,  m.x + m.w / 2 },
                { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_Y,  m.y + m.h / 2 },
                { KOBOY_EV_SYN, 0, 0 },
            };
            CHECK_EQ_INT(input_take_menu_request(li), 0);
            input_feed(li, mt, 5);
            CHECK_EQ_INT(input_take_menu_request(li), 1);
            CHECK(!input_state(li)->pointer.pressed);
            CHECK_EQ_INT(input_state(li)->buttons, 0);
            /* Held: latches exactly once, same edge contract as the DMG
               layout's MENU. */
            koboy_ev syn1[1] = { { KOBOY_EV_SYN, 0, 0 } };
            input_feed(li, syn1, 1);
            CHECK_EQ_INT(input_take_menu_request(li), 0);
            input_feed(li, up, 2);

            /* AND WITH THE TWO ZONES OVERLAPPING. At the shipped geometry
               they cannot: MENU is in the bottom strip and the pointer rect
               is the artwork above it, so the containment test alone already
               rejects a MENU tap and the "MENU wins" ordering is unreachable
               -- confirmed by mutating that ordering away and watching this
               file stay green. Overlapping them here (through
               input_set_pointer_rect, the same public setter main.c drives
               every frame) is what makes the ordering an assertion instead of
               a comment. Restored immediately afterwards. */
            input_set_pointer_rect(li, m.x, m.y, m.w, m.h);
            input_feed(li, mt, 5);
            CHECK_EQ_INT(input_take_menu_request(li), 1);
            CHECK(!input_state(li)->pointer.pressed);
            input_feed(li, up, 2);
            input_set_pointer_rect(li, lp.game_x, lp.game_y, lp.game_w, lp.game_h);
        }

        /* ================== THE CONTROL STRIP: every zone, and only that zone
         *
         * The shipped .mgw titles are driven by per-title RETROPAD bindings
         * and ignore the pointer entirely (measured: a pointer press changes
         * 0 pixels, a joypad press 211k), so the strip exposes the whole
         * retropad. A zone that merely EXISTS is not evidence it is
         * reachable, and a zone that also fires its neighbour is worse than
         * one that fires nothing -- so each is tapped at its own centre and
         * the WHOLE button word is compared, not just the bit under test.
         */
        {
            chrome_lcd_controls c;
            memset(&c, 0, sizeof c);
            chrome_lcd_layout(&lp, &c);

            struct { int x, y; uint16_t want; const char *name; } tap[] = {
                { c.x_cx, c.x_cy, KOBOY_BTN_X, "X" },
                { c.y_cx, c.y_cy, KOBOY_BTN_Y, "Y" },
                { c.a_cx, c.a_cy, KOBOY_BTN_A, "A" },
                { c.b_cx, c.b_cy, KOBOY_BTN_B, "B" },
                { c.l1.x     + c.l1.w / 2,     c.l1.y     + c.l1.h / 2,     KOBOY_BTN_L1,     "L1" },
                { c.select.x + c.select.w / 2, c.select.y + c.select.h / 2, KOBOY_BTN_SELECT, "SELECT" },
                { c.start.x  + c.start.w / 2,  c.start.y  + c.start.h / 2,  KOBOY_BTN_START,  "START" },
                { c.r1.x     + c.r1.w / 2,     c.r1.y     + c.r1.h / 2,     KOBOY_BTN_R1,     "R1" },
            };
            for (size_t t = 0; t < sizeof tap / sizeof tap[0]; t++) {
                uint16_t got = touch_probe(li, tap[t].x, tap[t].y);
                if (got != tap[t].want)
                    fprintf(stderr, "  strip zone %s at (%d,%d): got 0x%04x want 0x%04x\n",
                            tap[t].name, tap[t].x, tap[t].y, got, tap[t].want);
                CHECK_EQ_INT(got, tap[t].want);
            }

            /* AND EVERY ZONE'S OWN EXTREMES, not just its centre. A zone
               hit-tested against the wrong rect can still contain the centre
               of the right one -- the two boxes need only overlap in the
               middle -- so the corners are what pin the geometry. Inset by
               one pixel because the right/bottom edges are exclusive. */
            const koboy_rect *box[4] = { &c.l1, &c.select, &c.start, &c.r1 };
            const uint16_t   bit[4] = { KOBOY_BTN_L1, KOBOY_BTN_SELECT,
                                        KOBOY_BTN_START, KOBOY_BTN_R1 };
            for (int i = 0; i < 4; i++) {
                CHECK_EQ_INT(touch_probe(li, box[i]->x, box[i]->y), bit[i]);
                CHECK_EQ_INT(touch_probe(li, box[i]->x + box[i]->w - 1,
                                             box[i]->y + box[i]->h - 1), bit[i]);
                /* One pixel outside is NOTHING -- neither this button nor the
                   next one along. */
                CHECK_EQ_INT(touch_probe(li, box[i]->x - 1, box[i]->y + box[i]->h / 2), 0);
                CHECK_EQ_INT(touch_probe(li, box[i]->x + box[i]->w,
                                             box[i]->y + box[i]->h / 2), 0);
                CHECK_EQ_INT(touch_probe(li, box[i]->x + box[i]->w / 2, box[i]->y - 1), 0);
                CHECK_EQ_INT(touch_probe(li, box[i]->x + box[i]->w / 2,
                                             box[i]->y + box[i]->h), 0);
            }

            /* THE FOUR DISCS ARE DISCS, not their bounding boxes: a bounding
               box corner is sqrt(2) * face_r from the centre and so belongs to
               no button. Tested at the corners pointing AWAY from the diamond
               (X's top-left, B's bottom-right) and not at the inner ones,
               which was the first draft and was wrong for a real reason worth
               recording: the inner corner of X's box is 0.85 * face_r from Y's
               CENTRE, i.e. genuinely inside the Y disc. The four boxes
               overlap; the four circles do not. */
            CHECK_EQ_INT(touch_probe(li, c.x_cx - c.face_r, c.x_cy - c.face_r), 0);
            CHECK_EQ_INT(touch_probe(li, c.b_cx + c.face_r, c.b_cy + c.face_r), 0);

            /* THE D-PAD STEERS. CROSS mode (the shipped default) takes its
               origin from the drawn centre, so a tap off-centre reports that
               direction and the hub reports none. The same decode the DMG
               faceplate uses -- one implementation, dpad_bits(). */
            CHECK_EQ_INT(touch_probe(li, c.dpad_cx, c.dpad_cy - c.dpad_r / 2), KOBOY_BTN_UP);
            CHECK_EQ_INT(touch_probe(li, c.dpad_cx, c.dpad_cy + c.dpad_r / 2), KOBOY_BTN_DOWN);
            CHECK_EQ_INT(touch_probe(li, c.dpad_cx - c.dpad_r / 2, c.dpad_cy), KOBOY_BTN_LEFT);
            CHECK_EQ_INT(touch_probe(li, c.dpad_cx + c.dpad_r / 2, c.dpad_cy), KOBOY_BTN_RIGHT);
            CHECK_EQ_INT(touch_probe(li, c.dpad_cx, c.dpad_cy), 0);
            /* Well outside the cross is nothing at all -- the pad must not
                claim the whole left half of the strip. */
            CHECK_EQ_INT(touch_probe(li, c.dpad_cx + 2 * c.dpad_r, c.dpad_cy), 0);

            /* BARE STRIP BETWEEN CONTROLS presses nothing. Without this every
               check above is satisfied just as well by a hit test that
               returns its zone's bit for the entire panel. */
            CHECK_EQ_INT(touch_probe(li, (c.l1.x + c.l1.w + c.select.x) / 2,
                                         c.l1.y + c.l1.h / 2), 0);
            CHECK_EQ_INT(touch_probe(li, (c.dpad_cx + c.dpad_r + c.menu.x) / 2,
                                         c.dpad_cy), 0);

            /* MENU presses NO game button, and is still edge-triggered. It is
               the only way back to the ROM browser; a MENU tap that also fired
               START would begin a round on the way out. */
            CHECK_EQ_INT(input_take_menu_request(li), 0);
            CHECK_EQ_INT(touch_probe(li, c.menu.x + c.menu.w / 2,
                                         c.menu.y + c.menu.h / 2), 0);
            CHECK_EQ_INT(input_take_menu_request(li), 1);

            /* A DRAWN CONTROL WINS THE TOUCH over the pointer. At the shipped
               geometry the two cannot overlap -- the controls are in the
               strip and the pointer rect is the artwork above it -- so the
               ordering is unreachable and untestable as shipped, exactly like
               the MENU case below it. Overlapping them deliberately, through
               input_set_pointer_rect (the same public setter main.c drives
               every frame), is what makes it an assertion rather than a
               comment. Restored immediately afterwards. */
            input_set_pointer_rect(li, c.strip.x, c.strip.y, c.strip.w, c.strip.h);
            touch_hold(li, c.a_cx, c.a_cy);
            CHECK_EQ_INT(input_state(li)->buttons, KOBOY_BTN_A);
            CHECK(!input_state(li)->pointer.pressed);   /* read WHILE down */
            touch_lift(li);
            touch_hold(li, c.start.x + c.start.w / 2, c.start.y + c.start.h / 2);
            CHECK_EQ_INT(input_state(li)->buttons, KOBOY_BTN_START);
            CHECK(!input_state(li)->pointer.pressed);
            touch_lift(li);
            /* ...while bare strip inside the same rect DOES reach the pointer,
               which is what proves the two checks above are about the control
               and not about the rect being ignored wholesale. */
            touch_hold(li, (c.l1.x + c.l1.w + c.select.x) / 2, c.l1.y + c.l1.h / 2);
            CHECK(input_state(li)->pointer.pressed);
            touch_lift(li);
            /* The D-PAD wins its touch too, and by a different route: it is
               claimed by dpad_bits before the loop runs, so what keeps it out
               of the pointer is the loop SKIPPING the pad's slot. Without this
               case that skip is unreachable -- the d-pad circle overlaps no
               other zone at the shipped geometry -- and deleting it changes
               nothing any other check can see. (Real mutant, confirmed.) */
            touch_hold(li, c.dpad_cx, c.dpad_cy - c.dpad_r / 2);
            CHECK_EQ_INT(input_state(li)->buttons, KOBOY_BTN_UP);
            CHECK(!input_state(li)->pointer.pressed);
            touch_lift(li);
            input_set_pointer_rect(li, lp.game_x, lp.game_y, lp.game_w, lp.game_h);

            /* A FINGER ON THE ARTWORK DOES NOT DISARM THE BUTTONS. The pointer
               is claimed by the first touch that lands inside the frame rect;
               the scan must carry on to the remaining slots afterwards, or a
               second finger on a control is silently dropped for as long as
               the first is down. Slot ORDER matters here: the artwork touch is
               slot 0 so that it is the one that claims the pointer first. */
            {
                koboy_ev two[10]; int n = 0;
                mt(two, &n, 0, 1, lp.game_x + lp.game_w / 2, lp.game_y + lp.game_h / 2);
                mt(two, &n, 1, 2, c.a_cx, c.a_cy);
                input_feed(li, two, (size_t)n);
                CHECK(input_state(li)->pointer.pressed);
                CHECK_EQ_INT(input_state(li)->buttons, KOBOY_BTN_A);
                int m = 0; koboy_ev off3[10];
                mt(off3, &m, 0, -1, 0, 0);
                mt(off3, &m, 1, -1, 0, 0);
                input_feed(li, off3, (size_t)m);
                CHECK(!input_state(li)->pointer.pressed);
                CHECK_EQ_INT(input_state(li)->buttons, 0);
            }

            /* TWO FINGERS AT ONCE: a d-pad direction and a face button
               together, which is what actually playing looks like. The pad
               owns its own slot and must not swallow the second one. */
            {
                koboy_ev two[10]; int n = 0;
                mt(two, &n, 0, 1, c.dpad_cx + c.dpad_r / 2, c.dpad_cy);
                mt(two, &n, 1, 2, c.b_cx, c.b_cy);
                input_feed(li, two, (size_t)n);
                CHECK_EQ_INT(input_state(li)->buttons,
                             KOBOY_BTN_RIGHT | KOBOY_BTN_B);
                int m = 0; koboy_ev off2[10];
                mt(off2, &m, 0, -1, 0, 0);
                mt(off2, &m, 1, -1, 0, 0);
                input_feed(li, off2, (size_t)m);
                CHECK_EQ_INT(input_state(li)->buttons, 0);
            }

            /* TWO FINGERS ON THE ARTWORK: the FIRST one owns the pointer.
               There is one touchscreen and one pointer to report, so which
               touch wins has to be decided, and "the first" is the only
               answer that does not make the reported position jump to
               whichever finger happens to occupy the highest slot. Asserted
               at the two opposite corners of the frame rect, so first-wins
               and last-wins give opposite signs on both axes. */
            {
                koboy_ev two[10]; int n = 0;
                mt(two, &n, 0, 1, lp.game_x, lp.game_y);
                mt(two, &n, 1, 2, lp.game_x + lp.game_w - 1, lp.game_y + lp.game_h - 1);
                input_feed(li, two, (size_t)n);
                CHECK(input_state(li)->pointer.pressed);
                CHECK_EQ_INT(input_state(li)->pointer.x, -32767);
                CHECK_EQ_INT(input_state(li)->pointer.y, -32767);
                int m = 0; koboy_ev off4[10];
                mt(off4, &m, 0, -1, 0, 0);
                mt(off4, &m, 1, -1, 0, 0);
                input_feed(li, off4, (size_t)m);
            }
        }

        /* THE DRAWN DMG CONTROLS ARE DEAD. Their permille zones do not stop
           existing just because nothing draws them, and a live d-pad under a
           full-width Game & Watch unit would eat exactly the touches meant
           for the artwork's own buttons. Probed at the d-pad centre and at
           the A and B discs, all three of which fall inside this layout's
           game rect on this panel. */
        {
            const int W = 1264, H = 1680;
            int dcx = lc.layout.dpad_cx * W / 1000, dcy = lc.layout.dpad_cy * H / 1000;
            int dr  = lc.layout.dpad_r  * W / 1000;
            int acx = lc.layout.a_cx * W / 1000,    acy = lc.layout.a_cy * H / 1000;
            int bcx = lc.layout.b_cx * W / 1000,    bcy = lc.layout.b_cy * H / 1000;
            /* Offset from the d-pad's centre, not on it: under CROSS the
               centre itself is the neutral position and reports no direction
               in EITHER layout, which would make this pair of checks pass
               without proving anything. */
            int dux = dcx, duy = dcy - dr / 2;
            CHECK_EQ_INT(touch_probe(li, dux, duy), 0);
            CHECK_EQ_INT(touch_probe(li, acx, acy), 0);
            CHECK_EQ_INT(touch_probe(li, bcx, bcy), 0);
            /* Control: the SAME coordinates in the DMG layout DO press
               something. Without this the three checks above would pass just
               as well against a probe that missed every zone. */
            koboy_config dc2; config_defaults(&dc2);
            koboy_profile dp2;
            config_resolve_profile(&dp2, &dc2, W, H, KOBOY_GB_W, KOBOY_GB_H,
                                   KOBOY_GB_W, KOBOY_GB_H);
            koboy_input *di = input_create(&dc2, &dp2);
            CHECK(di != NULL);
            input_set_touch_transform(di, W - 1, H - 1, false, false, false);
            CHECK_EQ_INT(touch_probe(di, dux, duy) & KOBOY_BTN_UP, KOBOY_BTN_UP);
            CHECK_EQ_INT(touch_probe(di, acx, acy) & KOBOY_BTN_A, KOBOY_BTN_A);
            CHECK_EQ_INT(touch_probe(di, bcx, bcy) & KOBOY_BTN_B, KOBOY_BTN_B);
            /* ...and the DMG layout NEVER produces a pointer press, however
               it is touched: gambatte does not read one, and forwarding one
               would be state nobody clears. */
            touch_probe(di, dp2.game_x + 10, dp2.game_y + 10);
            CHECK(!input_state(di)->pointer.pressed);
            input_destroy(di);
        }

        /* THE PAGE-TURN KEYS KEEP WORKING. A hardware button is the only
           control a Game & Watch unit's compat keymap can reach on a device
           with no touch, and dropping the faceplate must not drop them. */
        input_feed_key(li, KOBOY_KEY_PAGE_F23, true);
        CHECK_EQ_INT(input_state(li)->buttons & KOBOY_BTN_A, KOBOY_BTN_A);
        input_feed_key(li, KOBOY_KEY_PAGE_F23, false);
        CHECK_EQ_INT(input_state(li)->buttons & KOBOY_BTN_A, 0);
        input_feed_key(li, KOBOY_KEY_PAGE_F24, true);
        CHECK_EQ_INT(input_state(li)->buttons & KOBOY_BTN_B, KOBOY_BTN_B);
        input_feed_key(li, KOBOY_KEY_PAGE_F24, false);

        /* THE POINTER RECT FOLLOWS THE FRAME. main.c narrows it every
           presented frame to what video_frame_rect reports, because a Game &
           Watch title renders BELOW its max geometry -- the zoomed LCD-only
           view -- several times a second, and the core normalises the pointer
           it receives against whatever it is currently showing. Normalising
           against the reserved rect instead leaves every touch offset by the
           centring margin.

           305x191 fitted into the 1264x765 rect is 1221x765 at x = 21, which
           is the rect video_frame_rect reports for that frame. */
        {
            int fw = 305 * 765 / 191, fh = 765;
            int fx = lp.game_x + (lp.game_w - fw) / 2, fy = lp.game_y;
            input_set_pointer_rect(li, fx, fy, fw, fh);

            koboy_ev le[5] = {
                { KOBOY_EV_ABS, KOBOY_ABS_MT_SLOT,        0 },
                { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, 1 },
                { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_X,  fx },
                { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_Y,  fy },
                { KOBOY_EV_SYN, 0, 0 },
            };
            input_feed(li, le, 5);
            CHECK(input_state(li)->pointer.pressed);
            CHECK_EQ_INT(input_state(li)->pointer.x, -32767);
            input_feed(li, up, 2);

            /* The left edge of the RESERVED rect is now outside the frame,
               so it is no longer a press at all -- it is bezel. This is the
               half that distinguishes "the rect narrowed" from "the rect was
               ignored": against the reserved rect this coordinate would
               still read -32767 and press. */
            koboy_ev out[5] = {
                { KOBOY_EV_ABS, KOBOY_ABS_MT_SLOT,        0 },
                { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, 1 },
                { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_X,  lp.game_x },
                { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_Y,  lp.game_y + 10 },
                { KOBOY_EV_SYN, 0, 0 },
            };
            input_feed(li, out, 5);
            CHECK(!input_state(li)->pointer.pressed);
            input_feed(li, up, 2);

            /* LIVE GUARD: a degenerate rect is refused, leaving the previous
               one in force rather than dividing by zero on the next touch. */
            input_set_pointer_rect(li, 0, 0, 0, 10);
            input_set_pointer_rect(li, 0, 0, 10, 0);
            input_feed(li, le, 5);
            CHECK(input_state(li)->pointer.pressed);
            CHECK_EQ_INT(input_state(li)->pointer.x, -32767);
            input_feed(li, up, 2);
        }

        input_destroy(li);
    }
})
