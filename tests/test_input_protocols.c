/* Kobo touch protocols: the four event streams the shipped hardware speaks.
 *
 * WHY THIS FILE EXISTS. Every other touch test in this suite feeds the ONE
 * protocol the verified Libra 2 speaks -- slot, tracking id, position, and a
 * lift announced as ABS_MT_TRACKING_ID == -1. That is not the only protocol on
 * the Kobo line, and koboy shipped unable to see a finger LEAVE the panel on
 * most of it: github issue #1, a Kobo Aura H2O whose ROM browser could not be
 * tapped. One tap per koboy_input object worked and every tap after it was
 * dead, because ui.c is edge-triggered and the contact never came up.
 *
 * THE STREAMS BELOW ARE NOT INVENTED. Each is transcribed from FBInk's
 * generate_button_press() (third_party/fbink/fbink_button_scan.c), which exists
 * precisely to synthesise a tap the local panel's driver will accept and so
 * records what each family really sends. FBInk's own reader
 * (utils/finger_trace.c) is the matching authority for how a lift is detected.
 * Device families, in FBInk's words:
 *
 *   protocol B  the reference device and everything modern
 *   "Phoenix"   KA1, Aura H2O, Aura, Aura SE r1, Glo HD, Touch 2.0, Nia
 *   "Snow"/Mk7  H2O2 r2, Clara HD, Forma, (Aura SE r2?)
 *   non-MT      Touch A/B/C, Mini, Glo, Aura HD
 *
 * Three of those four keep ABS_MT_TRACKING_ID non-negative through the lift, or
 * never send it at all, which is the whole bug.
 *
 * BOTH HALVES OF THE PIPELINE ARE CHECKED FOR EVERY FAMILY, because they read
 * contact state differently and only one of them was in the bug report:
 *   - the UI screens (ui.c) are EDGE-triggered -- a tap is a rising contact,
 *     so a contact that never falls costs every tap after the first;
 *   - the emulator screen (input.c's recompute) is LEVEL-triggered -- a held
 *     finger is a held button, so a contact that never falls is a button
 *     STUCK DOWN for the rest of the session.
 * A fix that only cleared the edge state would leave the second one broken.
 *
 * AND A THIRD THING, which is neither: the SECOND FINGER. Phoenix has no
 * ABS_MT_SLOT and separates contacts with SYN_MT_REPORT, so until that was
 * counted a second contact overwrote the first and there was no
 * d-pad-plus-button -- a menu never needs two fingers and a platformer cannot
 * do without them. Section 6. */
#include "test.h"
#include "config.h"
#include "input.h"
#include "ui.h"

#include <stdlib.h>

/* The reporter's device: Kobo Aura H2O, 1080x1430, touch layer mounted in the
   panel's own orientation (no transpose). The geometry is only a stage for the
   protocol under test -- every family is driven at the same coordinates. */
#define PW 1080
#define PH 1430

/* Event codes koboy DELIBERATELY DOES NOT DECODE, defined here rather than in
   input.h for exactly that reason: they appear in the recorded streams below,
   so the test must be able to send them, and koboy must be able to ignore
   them. Naming them in input.h would suggest the decode wants them.
     ABS_MT_TOUCH_MAJOR is the trap. It reads 0 on a lift on the panels here and
   looks like the obvious lift signal, but FBInk's reader says of it: "Oops, not
   that one, it's always 0 on early Mk.7 devices" -- a device whose finger is
   DOWN reports 0 too, so believing it would make those panels untouchable.
     The pressure axes are excluded for the milder version of the same reason
   (FBInk: "getting a 0-pressure event on lift is *not* a guarantee"). They are
   not NEEDED: every family below announces the lift with BTN_TOUCH as well, and
   BTN_TOUCH is a binary contact flag with no "always 0" failure mode. */
#define EV_ABS_MT_TOUCH_MAJOR 0x30
#define EV_ABS_MT_TOUCH_MINOR 0x31
#define EV_ABS_MT_WIDTH_MAJOR 0x32
#define EV_ABS_MT_ORIENTATION 0x34
#define EV_ABS_MT_PRESSURE    0x3a
#define EV_ABS_MT_DISTANCE    0x3b
#define EV_ABS_PRESSURE       0x18
#define EV_KEY_BTN_TOOL_FINGER 0x145
#define EV_SYN_MT_REPORT      0x02

#define EVN(a) (sizeof (a) / sizeof *(a))

/* ------------------------------------------------------------ the streams
   protocol B: the verified Libra 2. The control arm of every check below --
   it must keep working, and it is what says a failure is the protocol and not
   the fixture. */
static void b_press(koboy_input *in, int x, int y)
{
    koboy_ev ev[] = {
        { KOBOY_EV_ABS, KOBOY_ABS_MT_SLOT,        0 },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, 1 },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_X,  x },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_Y,  y },
        { KOBOY_EV_SYN, 0,                        0 },
    };
    input_feed(in, ev, EVN(ev));
}
/* NAMES THE SLOT, unlike the minimal `TRACKING_ID -1` form the other touch
   tests use: protocol B retires a contact BY SLOT and the cursor is sticky, so
   after the two-finger block below has retired slot 1 a bare -1 would retire
   slot 1 twice and leave the d-pad finger down forever. A real driver emits
   the slot too whenever the cursor is not already there. */
static void b_lift(koboy_input *in, int x, int y)
{
    (void)x; (void)y;
    koboy_ev ev[] = {
        { KOBOY_EV_ABS, KOBOY_ABS_MT_SLOT,         0 },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, -1 },
        { KOBOY_EV_SYN, 0,                         0 },
    };
    input_feed(in, ev, EVN(ev));
}

/* "Phoenix". NOTE THE TRACKING ID ON THE LIFT: still 1, not -1. That single
   value is github issue #1. Also note there is no ABS_MT_SLOT at all, and the
   packet is closed by SYN_MT_REPORT before SYN_REPORT. */
static void phoenix_press(koboy_input *in, int x, int y)
{
    koboy_ev ev[] = {
        { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, 1 },
        { KOBOY_EV_ABS, EV_ABS_MT_TOUCH_MAJOR,    1 },
        { KOBOY_EV_ABS, EV_ABS_MT_WIDTH_MAJOR,    1 },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_X,  x },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_Y,  y },
        { KOBOY_EV_ABS, EV_ABS_PRESSURE,       1024 },
        { KOBOY_EV_KEY, KOBOY_KEY_BTN_TOUCH,      1 },
        { KOBOY_EV_SYN, EV_SYN_MT_REPORT,         0 },
        { KOBOY_EV_SYN, 0,                        0 },
    };
    input_feed(in, ev, EVN(ev));
}
static void phoenix_lift(koboy_input *in, int x, int y)
{
    koboy_ev ev[] = {
        { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, 1 },   /* NOT -1 */
        { KOBOY_EV_ABS, EV_ABS_MT_TOUCH_MAJOR,    0 },
        { KOBOY_EV_ABS, EV_ABS_MT_WIDTH_MAJOR,    0 },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_X,  x },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_Y,  y },
        { KOBOY_EV_ABS, EV_ABS_PRESSURE,          0 },
        { KOBOY_EV_KEY, KOBOY_KEY_BTN_TOUCH,      0 },   /* the lift */
        { KOBOY_EV_SYN, EV_SYN_MT_REPORT,         0 },
        { KOBOY_EV_SYN, 0,                        0 },
    };
    input_feed(in, ev, EVN(ev));
}

/* "Snow" / Mk7. Same defect from a different angle: the tracking id is 0 on
   the press and 0 AGAIN on the lift, so `>= 0` reads as "down" both times.
   BTN_TOUCH and BTN_TOOL_FINGER close the contact, in a packet of their own
   AFTER the final SYN_REPORT -- which is why the check below must poll once
   more after the lift rather than reading state mid-packet. */
static void snow_press(koboy_input *in, int x, int y)
{
    koboy_ev ev[] = {
        { KOBOY_EV_KEY, EV_KEY_BTN_TOOL_FINGER,   1 },
        { KOBOY_EV_KEY, KOBOY_KEY_BTN_TOUCH,      1 },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, 0 },
        { KOBOY_EV_ABS, EV_ABS_MT_DISTANCE,       0 },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_X,  x },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_Y,  y },
        { KOBOY_EV_ABS, EV_ABS_MT_PRESSURE,      20 },
        { KOBOY_EV_ABS, EV_ABS_MT_TOUCH_MAJOR,    0 },
        { KOBOY_EV_ABS, EV_ABS_MT_TOUCH_MINOR,    0 },
        { KOBOY_EV_ABS, EV_ABS_MT_ORIENTATION,    0 },
        { KOBOY_EV_SYN, EV_SYN_MT_REPORT,         0 },
        { KOBOY_EV_SYN, 0,                        0 },
    };
    input_feed(in, ev, EVN(ev));
}
static void snow_lift(koboy_input *in, int x, int y)
{
    koboy_ev ev[] = {
        { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, 0 },   /* NOT -1 */
        { KOBOY_EV_ABS, EV_ABS_MT_DISTANCE,       0 },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_X,  x },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_Y,  y },
        { KOBOY_EV_ABS, EV_ABS_MT_PRESSURE,       0 },
        { KOBOY_EV_ABS, EV_ABS_MT_TOUCH_MAJOR,    0 },
        { KOBOY_EV_ABS, EV_ABS_MT_TOUCH_MINOR,    0 },
        { KOBOY_EV_ABS, EV_ABS_MT_ORIENTATION,    0 },
        { KOBOY_EV_SYN, EV_SYN_MT_REPORT,         0 },
        { KOBOY_EV_SYN, 0,                        0 },
        { KOBOY_EV_KEY, KOBOY_KEY_BTN_TOUCH,      0 },   /* the lift */
        { KOBOY_EV_KEY, EV_KEY_BTN_TOOL_FINGER,   0 },
        { KOBOY_EV_SYN, 0,                        0 },
    };
    input_feed(in, ev, EVN(ev));
}

/* non-MT: no multitouch events whatsoever. Position arrives on ABS_X/ABS_Y and
   the contact on BTN_TOUCH, so this family needs BOTH halves of the fix -- a
   decode that only learned the lift would see a contact with no coordinates. */
static void nonmt_press(koboy_input *in, int x, int y)
{
    koboy_ev ev[] = {
        { KOBOY_EV_ABS, KOBOY_ABS_Y,           y },
        { KOBOY_EV_ABS, KOBOY_ABS_X,           x },
        { KOBOY_EV_ABS, EV_ABS_PRESSURE,     100 },
        { KOBOY_EV_KEY, KOBOY_KEY_BTN_TOUCH,   1 },
        { KOBOY_EV_SYN, 0,                     0 },
    };
    input_feed(in, ev, EVN(ev));
}
static void nonmt_lift(koboy_input *in, int x, int y)
{
    koboy_ev ev[] = {
        { KOBOY_EV_ABS, KOBOY_ABS_Y,           y },
        { KOBOY_EV_ABS, KOBOY_ABS_X,           x },
        { KOBOY_EV_ABS, EV_ABS_PRESSURE,       0 },
        { KOBOY_EV_KEY, KOBOY_KEY_BTN_TOUCH,   0 },   /* the lift */
        { KOBOY_EV_SYN, 0,                     0 },
    };
    input_feed(in, ev, EVN(ev));
}

/* TWO FINGERS AT ONCE, which is what a game needs and a menu never does.
   Phoenix has no ABS_MT_SLOT: it separates contacts with SYN_MT_REPORT, the
   protocol-A way, so the packet below is the single-contact one twice over
   inside one SYN_REPORT frame. BTN_TOUCH is sent once, in the first block --
   it is out-of-band and says "something is touching", not "this contact is". */
static void phoenix_press2(koboy_input *in, int x0, int y0, int x1, int y1)
{
    koboy_ev ev[] = {
        { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, 0 },
        { KOBOY_EV_ABS, EV_ABS_MT_TOUCH_MAJOR,    1 },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_X, x0 },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_Y, y0 },
        { KOBOY_EV_ABS, EV_ABS_PRESSURE,       1024 },
        { KOBOY_EV_KEY, KOBOY_KEY_BTN_TOUCH,      1 },
        { KOBOY_EV_SYN, EV_SYN_MT_REPORT,         0 },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, 1 },
        { KOBOY_EV_ABS, EV_ABS_MT_TOUCH_MAJOR,    1 },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_X, x1 },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_Y, y1 },
        { KOBOY_EV_ABS, EV_ABS_PRESSURE,       1024 },
        { KOBOY_EV_SYN, EV_SYN_MT_REPORT,         0 },
        { KOBOY_EV_SYN, 0,                        0 },
    };
    input_feed(in, ev, EVN(ev));
}

/* The SECOND finger leaving, in each protocol's own idiom -- and they are
   genuinely different, not two spellings of one thing. Protocol A has no
   contact identity: a frame lists what is touching NOW, so dropping a block is
   the whole message. Protocol B has identity, and a contact stays down until
   its slot is explicitly retired, so a frame that simply omits it means
   "unchanged", not "gone". A test that used one form for both would be
   asserting the wrong protocol at one of them. */
static void phoenix_lift_second(koboy_input *in, int x0, int y0)
{
    phoenix_press(in, x0, y0);          /* a one-block frame: only this remains */
}
static void b_lift_second(koboy_input *in, int x0, int y0)
{
    (void)x0; (void)y0;
    koboy_ev ev[] = {
        { KOBOY_EV_ABS, KOBOY_ABS_MT_SLOT,        1 },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, -1 },
        { KOBOY_EV_SYN, 0,                         0 },
    };
    input_feed(in, ev, EVN(ev));
}

/* Protocol B's equivalent, as the control. */
static void b_press2(koboy_input *in, int x0, int y0, int x1, int y1)
{
    koboy_ev ev[] = {
        { KOBOY_EV_ABS, KOBOY_ABS_MT_SLOT,        0 },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, 1 },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_X, x0 },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_Y, y0 },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_SLOT,        1 },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, 2 },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_X, x1 },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_Y, y1 },
        { KOBOY_EV_SYN, 0,                        0 },
    };
    input_feed(in, ev, EVN(ev));
}

/* ------------------------------------------------ zForce: a REAL CAPTURE
   Everything else in this file is transcribed from FBInk's button INJECTOR,
   which describes what a driver will ACCEPT. This one is a recording of what a
   driver SENDS: `trace_touch = true` on the Kobo Aura H2O of github issue #1,
   371 events, decoded and reduced to the two frames below. It is the only
   stream here that is evidence rather than inference, and it is the one that
   proved the inference wrong.

   WHAT THE CAPTURE SAYS, and every clause cost a release:
     - SEVEN codes, ever. No EV_KEY of any kind.
     - BTN_TOUCH is ADVERTISED in the node's capability bitmap (`btn_touch=1`
       in koboy's own caps line) and is NEVER SENT. v0.5.3 read the lift from
       BTN_TOUCH and shipped; it could not have worked here.
     - ABS_PRESSURE likewise: advertised, never sent.
     - ABS_MT_TRACKING_ID is 1 in every frame of all 371 events. Never -1.
     - The lift is ABS_MT_TOUCH_MAJOR going 1 -> 0, with WIDTH_MAJOR alongside.

   That last one is the field koboy deliberately refused, on FBInk's warning
   that it "is always 0 on early Mk.7 devices". The warning is real and is
   about Mk7; this is a Mk5, where the same field is a clean binary flag. A
   fixed choice of field cannot satisfy both, which is why input.c ARMS a field
   on first seeing it positive instead of trusting or distrusting it in
   advance. */
static void zforce_press(koboy_input *in, int x, int y)
{
    koboy_ev ev[] = {
        { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, 1 },
        { KOBOY_EV_ABS, EV_ABS_MT_TOUCH_MAJOR,    1 },
        { KOBOY_EV_ABS, EV_ABS_MT_WIDTH_MAJOR,    1 },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_X,  x },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_Y,  y },
        { KOBOY_EV_SYN, EV_SYN_MT_REPORT,         0 },
        { KOBOY_EV_SYN, 0,                        0 },
    };
    input_feed(in, ev, EVN(ev));
}
static void zforce_lift(koboy_input *in, int x, int y)
{
    koboy_ev ev[] = {
        { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, 1 },   /* still 1 */
        { KOBOY_EV_ABS, EV_ABS_MT_TOUCH_MAJOR,    0 },   /* THE lift, and the
                                                            only one there is */
        { KOBOY_EV_ABS, EV_ABS_MT_WIDTH_MAJOR,    0 },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_X,  x },
        { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_Y,  y },
        { KOBOY_EV_SYN, EV_SYN_MT_REPORT,         0 },
        { KOBOY_EV_SYN, 0,                        0 },
    };
    input_feed(in, ev, EVN(ev));
}

typedef void (*touch_fn)(koboy_input *, int, int);
typedef struct { const char *name; touch_fn press, lift; } family;

static const family FAMILIES[] = {
    { "protocol B (Libra 2, verified)", b_press,       b_lift       },
    { "Phoenix (Aura H2O, issue #1)",   phoenix_press, phoenix_lift },
    { "Snow / Mk7",                     snow_press,    snow_lift    },
    { "non-MT (Touch, Mini, Glo, HD)",  nonmt_press,   nonmt_lift   },
    { "zForce IR (Aura H2O, CAPTURED)", zforce_press,  zforce_lift  },
};
#define N_FAMILIES (sizeof FAMILIES / sizeof *FAMILIES)

static koboy_input *fresh(const koboy_config *c, const koboy_profile *p)
{
    koboy_input *in = input_create(c, p);
    /* Identity transform: raw range == panel - 1, so scale_axis is a no-op and
       every coordinate below is pixel-exact. What platform_kobo_setup_touch
       installs on a panel-oriented touch layer. */
    input_set_touch_transform(in, PW - 1, PH - 1, false, false, false);
    return in;
}

/* One poll of the UI state, the way screens.c's screen_list loop reads it:
   input_ui_state, NOT input_state. */
static ui_action ui_poll(koboy_input *in, koboy_ui_list *u, int *idx)
{
    koboy_input_state st;
    input_ui_state(in, &st);
    *idx = -1;
    return ui_list_feed(u, &st, idx);
}

TEST_MAIN({
    koboy_config c; config_defaults(&c);
    koboy_profile prof;
    CHECK(config_resolve_profile(&prof, &c, PW, PH,
                                 KOBOY_GB_W, KOBOY_GB_H, KOBOY_GB_W, KOBOY_GB_H));

    /* ---------------------------------------------- 1. the UI, edge-triggered
       Three taps on the SAME row of one list. Under the bug the first selects
       and the rest are swallowed, which is exactly what the reporter saw: the
       MAIN MENU answered one tap and the browser answered none. */
    for (size_t f = 0; f < N_FAMILIES; f++) {
        const family *fam = &FAMILIES[f];
        koboy_input *in = fresh(&c, &prof);
        CHECK(in != NULL);

        static const char *const items[3] = { "RECENT", "ALL GAMES", "QUIT" };
        koboy_ui_list u;
        ui_list_init(&u, "KOBOY", items, 3, KOBOY_CHROME_MARGIN, KOBOY_CHROME_MARGIN,
                     PW - 2 * KOBOY_CHROME_MARGIN, PH - 2 * KOBOY_CHROME_MARGIN);

        /* Row 1 ("ALL GAMES"): title row, then row 1, then its midpoint. Taken
           from the widget's own derived row_h rather than assumed, and CHECKed
           to be inside the list, so a geometry change cannot leave this
           asserting nothing. */
        int row = 1;
        int tx = PW / 2;
        int ty = u.y + u.row_h + row * u.row_h + u.row_h / 2;
        CHECK(ty > u.y && ty < u.y + u.h);
        CHECK(row < u.rows);

        int selects = 0;
        for (int tap = 0; tap < 3; tap++) {
            int idx;
            ui_poll(in, &u, &idx);              /* the settled state first */
            fam->press(in, tx, ty);
            if (ui_poll(in, &u, &idx) == UI_SELECT && idx == row) selects++;
            fam->lift(in, tx, ty);
            ui_poll(in, &u, &idx);
            /* THE ROOT CAUSE, asserted directly rather than only through its
               consequence: after a lift, no contact is down. */
            CHECK(!input_state(in)->touch[0].down);
        }
        if (selects != 3)
            fprintf(stderr, "  [%s] %d of 3 taps selected\n", fam->name, selects);
        CHECK_EQ_INT(selects, 3);

        input_destroy(in);
    }

    /* ------------------------------- 2. the reported flow, at full fidelity
       ONE koboy_input across TWO lists, which is what main.c does: `ui_in` is
       created once per MODE_MAIN pass and shared by screen_main_menu and the
       browser it opens. The bug is invisible to a single-list test that stops
       after one tap. */
    for (size_t f = 0; f < N_FAMILIES; f++) {
        const family *fam = &FAMILIES[f];
        koboy_input *in = fresh(&c, &prof);

        static const char *const menu[3] = { "RECENT", "ALL GAMES", "QUIT" };
        static const char *const roms[4] = { "Game and Watch", "TETRIS", "ZELDA", "SONIC" };
        koboy_ui_list a, b;
        ui_list_init(&a, "KOBOY", menu, 3, KOBOY_CHROME_MARGIN, KOBOY_CHROME_MARGIN,
                     PW - 2 * KOBOY_CHROME_MARGIN, PH - 2 * KOBOY_CHROME_MARGIN);
        ui_list_init(&b, "ALL GAMES", roms, 4, KOBOY_CHROME_MARGIN, KOBOY_CHROME_MARGIN,
                     PW - 2 * KOBOY_CHROME_MARGIN, PH - 2 * KOBOY_CHROME_MARGIN);

        int tx = PW / 2;
        int y1 = a.y + a.row_h + 1 * a.row_h + a.row_h / 2;   /* ALL GAMES */
        int y2 = b.y + b.row_h + 2 * b.row_h + b.row_h / 2;   /* ZELDA */
        int idx;

        ui_poll(in, &a, &idx);
        fam->press(in, tx, y1);
        CHECK_EQ_INT(ui_poll(in, &a, &idx), UI_SELECT);
        CHECK_EQ_INT(idx, 1);
        fam->lift(in, tx, y1);

        /* Into the browser, same input object, as main.c hands it over. */
        ui_poll(in, &b, &idx);
        fam->press(in, tx, y2);
        ui_action act = ui_poll(in, &b, &idx);
        if (act != UI_SELECT)
            fprintf(stderr, "  [%s] the browser answered no tap\n", fam->name);
        CHECK_EQ_INT(act, UI_SELECT);
        CHECK_EQ_INT(idx, 2);
        fam->lift(in, tx, y2);

        input_destroy(in);
    }

    /* ---------------------------- 3. the emulator screen, level-triggered
       The other half of the pipeline, and the half the bug report could not
       reach: a stuck contact here is not a lost tap, it is a joypad button
       held down for the rest of the session. Checked at the DMG faceplate's A
       disc, whose centre is computed from the layout (permille) and asserted
       to be on the panel -- a probe that missed the disc would synthesise no
       button at all and every family would "pass". */
    {
        int acx = c.layout.a_cx * PW / 1000;
        int acy = c.layout.a_cy * PH / 1000;
        int ar  = c.layout.a_r  * PW / 1000;
        CHECK(ar > 0);
        CHECK(acx - ar >= 0 && acx + ar < PW);
        CHECK(acy - ar >= 0 && acy + ar < PH);

        for (size_t f = 0; f < N_FAMILIES; f++) {
            const family *fam = &FAMILIES[f];
            koboy_input *in = fresh(&c, &prof);

            /* Nothing pressed to begin with. */
            CHECK_EQ_INT(input_state(in)->buttons & KOBOY_BTN_A, 0);

            fam->press(in, acx, acy);
            if (!(input_state(in)->buttons & KOBOY_BTN_A))
                fprintf(stderr, "  [%s] the A disc did not press\n", fam->name);
            CHECK(input_state(in)->buttons & KOBOY_BTN_A);

            /* PRESS AND HOLD. The emulator loop polls ~60 times a second and a
               finger resting on the disc sends nothing at all between packets,
               so the button must stay asserted with NO further events. This is
               the check that stops a fix from clearing contact state per poll
               instead of per lift -- a "fix" that made the menus work by
               forgetting the contact every frame would make a game unplayable,
               and it would pass every check above. */
            for (int poll = 0; poll < 4; poll++)
                CHECK(input_state(in)->buttons & KOBOY_BTN_A);

            fam->lift(in, acx, acy);
            if (input_state(in)->buttons & KOBOY_BTN_A)
                fprintf(stderr, "  [%s] the A button stayed DOWN after the lift\n",
                        fam->name);
            CHECK_EQ_INT(input_state(in)->buttons & KOBOY_BTN_A, 0);
            CHECK(!input_state(in)->touch[0].down);

            /* And it presses AGAIN -- a second press after a lift is what a
               player does continuously, and it is the thing the stuck contact
               made impossible. */
            fam->press(in, acx, acy);
            CHECK(input_state(in)->buttons & KOBOY_BTN_A);
            fam->lift(in, acx, acy);
            CHECK_EQ_INT(input_state(in)->buttons & KOBOY_BTN_A, 0);

            input_destroy(in);
        }
    }

    /* ------------------------------------------- 4. the source seam itself
       ABS_X/ABS_Y had to be decoded for the pre-multitouch panels, and those
       are the SAME TWO CODES a gamepad uses for its analog stick -- which
       streams at ~100 Hz even at rest. Nothing else in the suite would notice
       if the guard went: a stick is not a touchscreen, so no other test feeds
       one. Both halves are checked, because they fail differently -- a stick
       read as a finger MOVES the contact, and a pad's BTN_TOUCH (were one ever
       to send it) would CREATE one. */
    {
        /* THE FIXTURE PRESSES WITH THE non-MT STREAM, and that is the whole
           point of it: the source guard is load-bearing ONLY on a panel that
           has never spoken multitouch, because on an MT panel `saw_mt` closes
           ABS_X/ABS_Y first and the guard behind it is never reached. Pressing
           with protocol B here made this block pass with the guard DELETED --
           it was checking saw_mt twice and the seam not at all. A
           pre-multitouch Kobo with a Bluetooth gamepad paired is a real
           configuration, and the only one where both meanings of ABS_X/ABS_Y
           are live at the same time. */
        koboy_input *in = fresh(&c, &prof);

        /* A real finger, first, so there is a position for stick noise to
           corrupt -- and so this cannot pass by nothing ever being down. */
        nonmt_press(in, 300, 400);
        CHECK(input_state(in)->touch[0].down);
        CHECK_EQ_INT(input_state(in)->touch[0].x, 300);
        CHECK_EQ_INT(input_state(in)->touch[0].y, 400);

        /* Stick at rest, off the gamepad node: centred 32768 with fuzz, the
           measured idle stream. */
        koboy_ev stick[] = {
            { KOBOY_EV_ABS, KOBOY_ABS_X, 32768 },
            { KOBOY_EV_ABS, KOBOY_ABS_Y, 32900 },
            { KOBOY_EV_SYN, 0,               0 },
        };
        input_feed_from(in, KOBOY_EV_SRC_BUTTONS, stick, EVN(stick));
        CHECK_EQ_INT(input_state(in)->touch[0].x, 300);
        CHECK_EQ_INT(input_state(in)->touch[0].y, 400);
        CHECK(input_state(in)->touch[0].down);

        /* And the pad cannot retire the contact either. */
        koboy_ev pad_touch[] = {
            { KOBOY_EV_KEY, KOBOY_KEY_BTN_TOUCH, 0 },
            { KOBOY_EV_SYN, 0,                   0 },
        };
        input_feed_from(in, KOBOY_EV_SRC_BUTTONS, pad_touch, EVN(pad_touch));
        CHECK(input_state(in)->touch[0].down);

        /* `saw_mt` is private, so it is asserted through its consequence: the
           touchscreen's OWN ABS_X/ABS_Y must still be honoured. Without this
           the two checks above would also pass on an object that had latched
           saw_mt, which is the way this block was wrong the first time. */
        nonmt_press(in, 555, 666);
        CHECK_EQ_INT(input_state(in)->touch[0].x, 555);
        CHECK_EQ_INT(input_state(in)->touch[0].y, 666);

        nonmt_lift(in, 555, 666);
        CHECK(!input_state(in)->touch[0].down);

        /* The hat is still the d-pad on that node -- the guard must not have
           made the whole gamepad source inert. */
        koboy_ev hat[] = {
            { KOBOY_EV_ABS, KOBOY_ABS_HAT0X, 1 },
            { KOBOY_EV_SYN, 0,               0 },
        };
        input_feed_from(in, KOBOY_EV_SRC_BUTTONS, hat, EVN(hat));
        CHECK(input_state(in)->buttons & KOBOY_BTN_RIGHT);

        input_destroy(in);
    }

    /* --------------------------------- 5. a multitouch panel's legacy axes
       A panel that speaks MT may also emulate single-touch, whose ABS_X/ABS_Y
       track contact 0. Those must not land in whatever slot happens to be
       current, or a second finger's position is corrupted. `saw_mt` is what
       closes that door, and this is the only thing that opens it. */
    {
        koboy_input *in = fresh(&c, &prof);
        b_press(in, 300, 400);                       /* slot 0, and saw_mt latches */

        koboy_ev second[] = {
            { KOBOY_EV_ABS, KOBOY_ABS_MT_SLOT,        1 },
            { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, 2 },
            { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_X, 700 },
            { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_Y, 900 },
            /* The legacy mirror of contact 0, arriving while slot 1 is current. */
            { KOBOY_EV_ABS, KOBOY_ABS_X,            300 },
            { KOBOY_EV_ABS, KOBOY_ABS_Y,            400 },
            { KOBOY_EV_SYN, 0,                        0 },
        };
        input_feed(in, second, EVN(second));
        CHECK_EQ_INT(input_state(in)->touch[1].x, 700);
        CHECK_EQ_INT(input_state(in)->touch[1].y, 900);
        CHECK_EQ_INT(input_state(in)->touch[0].x, 300);
        CHECK_EQ_INT(input_state(in)->touch[0].y, 400);

        /* BTN_TOUCH IS OUT-OF-BAND: FBInk's contract is that a zero does not
           arrive until EVERY contact has lifted, so it clears them all and not
           just the current slot. Two fingers are down here and the current slot
           is 1, so a decode that cleared only in->slot would leave slot 0 stuck
           -- the exact defect this whole file is about, one finger along. */
        CHECK(input_state(in)->touch[0].down);
        CHECK(input_state(in)->touch[1].down);
        koboy_ev all_up[] = {
            { KOBOY_EV_KEY, KOBOY_KEY_BTN_TOUCH, 0 },
            { KOBOY_EV_SYN, 0,                   0 },
        };
        input_feed(in, all_up, EVN(all_up));
        CHECK(!input_state(in)->touch[0].down);
        CHECK(!input_state(in)->touch[1].down);

        input_destroy(in);
    }

    /* ------------------------- 6. two fingers, which is what a GAME needs
       A menu never wants more than one contact; the DMG faceplate wants a
       thumb on the d-pad and a thumb on A at the same moment, and without it
       nothing with a jump button is playable. Phoenix has no ABS_MT_SLOT, so
       koboy's slot cursor never moved and the second contact landed on top of
       the first -- the position would alternate between the pad and the
       button every frame. `docs/FOLLOWUPS.md` #113.

       ASSERTED AS THE PLAYER'S PROPERTY, not as slot bookkeeping: a direction
       bit and the A bit set at the same time. Slot bookkeeping is checked too,
       but on its own it would pass a decode that tracked both contacts and
       then let dpad_bits claim the wrong one. */
    {
        int acx = c.layout.a_cx * PW / 1000, acy = c.layout.a_cy * PH / 1000;
        int dcx = c.layout.dpad_cx * PW / 1000, dcy = c.layout.dpad_cy * PH / 1000;
        int dr  = c.layout.dpad_r * PW / 1000;
        /* Steer RIGHT: inside the pad, past the deadzone, and still on the
           panel. All three CHECKed -- a probe that fell outside the pad would
           synthesise no direction and the whole block would pass vacuously. */
        int steer_x = dcx + dr / 2, steer_y = dcy;
        CHECK(dr / 2 > c.dpad_deadzone);
        CHECK(steer_x < PW && steer_x > dcx);
        CHECK(acx != steer_x || acy != steer_y);

        struct { const char *name;
                 void (*press2)(koboy_input *, int, int, int, int);
                 touch_fn lift_second, lift; } two[] = {
            { "protocol B", b_press2,       b_lift_second,       b_lift       },
            { "Phoenix",    phoenix_press2, phoenix_lift_second, phoenix_lift },
        };

        for (size_t f = 0; f < sizeof two / sizeof *two; f++) {
            koboy_input *in = fresh(&c, &prof);

            two[f].press2(in, steer_x, steer_y, acx, acy);

            /* Both contacts are tracked, each at its OWN place. */
            CHECK(input_state(in)->touch[0].down);
            CHECK(input_state(in)->touch[1].down);
            CHECK_EQ_INT(input_state(in)->touch[0].x, steer_x);
            CHECK_EQ_INT(input_state(in)->touch[0].y, steer_y);
            CHECK_EQ_INT(input_state(in)->touch[1].x, acx);
            CHECK_EQ_INT(input_state(in)->touch[1].y, acy);

            /* And the thing a player actually needs. */
            uint16_t b = input_state(in)->buttons;
            if (!(b & KOBOY_BTN_A) || !(b & KOBOY_BTN_RIGHT))
                fprintf(stderr, "  [%s] d-pad + A together: A=%d RIGHT=%d\n",
                        two[f].name, (b & KOBOY_BTN_A) != 0,
                        (b & KOBOY_BTN_RIGHT) != 0);
            CHECK(b & KOBOY_BTN_A);
            CHECK(b & KOBOY_BTN_RIGHT);

            /* One finger leaves and the other keeps working: a frame that
               reports FEWER contacts retires the ones it no longer mentions.
               Protocol A re-indexes what is left from 0, so the survivor moves
               to slot 0 -- which is why `dpad_mode = cross` is the shipped
               default and this still steers. */
            two[f].lift_second(in, steer_x, steer_y);
            CHECK(input_state(in)->buttons & KOBOY_BTN_RIGHT);
            CHECK_EQ_INT(input_state(in)->buttons & KOBOY_BTN_A, 0);

            two[f].lift(in, steer_x, steer_y);
            CHECK_EQ_INT(input_state(in)->buttons & (KOBOY_BTN_A | KOBOY_BTN_RIGHT), 0);
            CHECK(!input_state(in)->touch[0].down);
            CHECK(!input_state(in)->touch[1].down);

            input_destroy(in);
        }
    }

    /* ---------------------- 7. protocol A's own way of saying "all up"
       An EMPTY block -- SYN_MT_REPORT with no contact data in front of it --
       is how protocol A reports that nothing is touching. It must not be
       counted as a contact, and it must retire the ones that were. Phoenix
       also has BTN_TOUCH for this, so nothing above would notice if the empty
       block were mishandled; this is the check that does. */
    {
        koboy_input *in = fresh(&c, &prof);
        phoenix_press(in, 300, 400);
        CHECK(input_state(in)->touch[0].down);

        koboy_ev empty[] = {
            { KOBOY_EV_SYN, EV_SYN_MT_REPORT, 0 },
            { KOBOY_EV_SYN, 0,                0 },
        };
        input_feed(in, empty, EVN(empty));
        CHECK(!input_state(in)->touch[0].down);

        input_destroy(in);
    }

    /* ------------- 7b. ...and the same thing said with even less
       A protocol-A driver reports the contacts that exist. When none do, some
       drivers send the empty block above and others send a BARE SYN_REPORT
       with nothing in front of it at all -- the frame is empty, so the lift is
       the absence of the contact rather than any event about it.

       koboy's first attempt at this only retired contacts on a frame that
       carried a SYN_MT_REPORT of its own, which reads the second form as "no
       touch information, change nothing" and leaves the finger down forever --
       the ORIGINAL bug, reintroduced one protocol deeper. Once a panel has
       shown it speaks protocol A, every SYN_REPORT is a complete statement of
       what is touching, including a silent one.

       Which form a real zForce panel uses is not known here (github issue #1
       is open on exactly that question), so koboy handles both. */
    {
        koboy_input *in = fresh(&c, &prof);
        phoenix_press(in, 300, 400);
        CHECK(input_state(in)->touch[0].down);

        koboy_ev bare[] = { { KOBOY_EV_SYN, 0, 0 } };
        input_feed(in, bare, EVN(bare));
        if (input_state(in)->touch[0].down)
            fprintf(stderr, "  [protocol A] a bare SYN_REPORT left the contact down\n");
        CHECK(!input_state(in)->touch[0].down);

        /* And a panel that has NEVER spoken protocol A is untouched by this:
           on protocol B a frame that mentions no slot means "unchanged", and
           reading it as "all up" would drop a finger every frame. Section 8
           checks the same property for a panel that speaks both. */
        koboy_input *b = fresh(&c, &prof);
        b_press(b, 300, 400);
        CHECK(input_state(b)->touch[0].down);
        input_feed(b, bare, EVN(bare));
        CHECK(input_state(b)->touch[0].down);
        input_destroy(b);

        input_destroy(in);
    }

    /* ------------- 7c. the axis that is stuck at zero must not veto a touch
       The other half of the arming rule, and the reason it is a rule rather
       than a choice of field. FBInk's reader refuses ABS_MT_TOUCH_MAJOR
       outright -- "Oops, not that one, it's always 0 on early Mk.7 devices" --
       and the zForce capture in section 1 is a panel where that same axis is
       the ONLY lift signal there is. Both are true.

       Here is the panel FBInk is warning about: TOUCH_MAJOR present and
       permanently 0, contact carried by BTN_TOUCH. Reading the axis at face
       value makes it untouchable -- every press arrives pre-lifted. Arming
       fixes it by never believing an axis that has not first shown it can be
       positive. */
    {
        koboy_input *in = fresh(&c, &prof);
        koboy_ev press[] = {
            { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, 3 },
            { KOBOY_EV_ABS, EV_ABS_MT_TOUCH_MAJOR,    0 },   /* stuck, always */
            { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_X, 300 },
            { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_Y, 400 },
            { KOBOY_EV_KEY, KOBOY_KEY_BTN_TOUCH,      1 },
            { KOBOY_EV_SYN, EV_SYN_MT_REPORT,         0 },
            { KOBOY_EV_SYN, 0,                        0 },
        };
        input_feed(in, press, EVN(press));
        if (!input_state(in)->touch[0].down)
            fprintf(stderr, "  [stuck TOUCH_MAJOR] the press never registered\n");
        CHECK(input_state(in)->touch[0].down);
        CHECK_EQ_INT(input_state(in)->touch[0].x, 300);

        koboy_ev lift[] = {
            { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, 3 },
            { KOBOY_EV_ABS, EV_ABS_MT_TOUCH_MAJOR,    0 },
            { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_X, 300 },
            { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_Y, 400 },
            { KOBOY_EV_KEY, KOBOY_KEY_BTN_TOUCH,      0 },
            { KOBOY_EV_SYN, EV_SYN_MT_REPORT,         0 },
            { KOBOY_EV_SYN, 0,                        0 },
        };
        input_feed(in, lift, EVN(lift));
        CHECK(!input_state(in)->touch[0].down);

        input_destroy(in);
    }

    /* ------- 7d. protocol B is not touched by any of this, which is a promise
       The strength axes are a FALLBACK for panels that never retire a contact
       with ABS_MT_TRACKING_ID == -1. A panel that names its slots does retire
       them, needs none of this, and is the one kind of panel anybody here has
       actually tested on -- so input.c confines the mechanism to `!saw_slot`
       and this is the check that the confinement is real.

       Plenty of protocol-B drivers report ABS_MT_TOUCH_MAJOR, and some report
       0 for a light contact that is very much still there. On protocol B the
       tracking id is the authority and a zero strength axis must not overrule
       it; letting it would trade a bug nobody has for one on the reference
       device. (MUTANT: drop `&& !in->saw_slot` from the four axis cases.) */
    {
        koboy_input *in = fresh(&c, &prof);
        koboy_ev press[] = {
            { KOBOY_EV_ABS, KOBOY_ABS_MT_SLOT,        0 },
            { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, 5 },
            { KOBOY_EV_ABS, EV_ABS_MT_TOUCH_MAJOR,   12 },
            { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_X, 300 },
            { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_Y, 400 },
            { KOBOY_EV_SYN, 0,                        0 },
        };
        input_feed(in, press, EVN(press));
        CHECK(input_state(in)->touch[0].down);

        /* A light touch: the area reads 0, the finger has not gone anywhere,
           and the driver says so by NOT retiring the tracking id. */
        koboy_ev light[] = {
            { KOBOY_EV_ABS, EV_ABS_MT_TOUCH_MAJOR,    0 },
            { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_X, 301 },
            { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_Y, 401 },
            { KOBOY_EV_SYN, 0,                        0 },
        };
        input_feed(in, light, EVN(light));
        if (!input_state(in)->touch[0].down)
            fprintf(stderr, "  [protocol B] a zero TOUCH_MAJOR dropped a live contact\n");
        CHECK(input_state(in)->touch[0].down);
        CHECK_EQ_INT(input_state(in)->touch[0].x, 301);

        /* And the tracking id still ends it, as it always did. */
        b_lift(in, 301, 401);
        CHECK(!input_state(in)->touch[0].down);
        input_destroy(in);
    }

    /* ------------------------------- 8. a panel that speaks BOTH dialects
       The protocol-A cursor is gated on "this panel has never named a slot",
       and that gate is the only thing standing between the two schemes. Some
       drivers emit a trailing SYN_MT_REPORT out of habit while still doing
       real protocol-B slot bookkeeping; if the cursor believed it, it would
       count contacts the slots had already placed and the two would fight.
       ABS_MT_SLOT WINS -- it is an explicit statement and SYN_MT_REPORT is an
       inference. Nothing else in this file would notice the gate going. */
    {
        koboy_input *in = fresh(&c, &prof);
        koboy_ev hybrid[] = {
            { KOBOY_EV_ABS, KOBOY_ABS_MT_SLOT,        1 },
            { KOBOY_EV_ABS, KOBOY_ABS_MT_TRACKING_ID, 7 },
            /* A REAL contact area, which arms the axis. The zero that
               matters comes in the next frame. */
            { KOBOY_EV_ABS, EV_ABS_MT_TOUCH_MAJOR,   12 },
            { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_X, 400 },
            { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_Y, 500 },
            { KOBOY_EV_SYN, EV_SYN_MT_REPORT,         0 },   /* the habit */
            { KOBOY_EV_SYN, 0,                        0 },
        };
        input_feed(in, hybrid, EVN(hybrid));

        /* The contact is where the SLOT said, and slot 0 was never touched. */
        CHECK(input_state(in)->touch[1].down);
        CHECK_EQ_INT(input_state(in)->touch[1].x, 400);
        CHECK_EQ_INT(input_state(in)->touch[1].y, 500);
        CHECK(!input_state(in)->touch[0].down);

        /* And the frame end did not retire it as an "unmentioned" contact:
           protocol B keeps a contact until its slot is retired. */
        koboy_ev nothing[] = { { KOBOY_EV_SYN, 0, 0 } };
        input_feed(in, nothing, EVN(nothing));
        CHECK(input_state(in)->touch[1].down);

        /* NOW the zero, on an axis this panel has already proved it can drive.
           THIS is the shape where the `!saw_slot` guard on the strength axes
           earns its place, and it is the only one: everywhere else the
           mechanism is confined by being resolved at SYN_MT_REPORT, which a
           protocol-B panel never sends, or by the axis never having been
           armed. Here the panel sends SYN_MT_REPORT *and* has armed the axis
           *and* names its slots -- so without the guard a light touch would
           retire a contact whose tracking id is still live.
           (MUTANT: drop `&& !in->saw_slot` from the four axis cases.) */
        koboy_ev light[] = {
            { KOBOY_EV_ABS, KOBOY_ABS_MT_SLOT,        1 },
            { KOBOY_EV_ABS, EV_ABS_MT_TOUCH_MAJOR,    0 },
            { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_X, 401 },
            { KOBOY_EV_ABS, KOBOY_ABS_MT_POSITION_Y, 501 },
            { KOBOY_EV_SYN, EV_SYN_MT_REPORT,         0 },
            { KOBOY_EV_SYN, 0,                        0 },
        };
        input_feed(in, light, EVN(light));
        if (!input_state(in)->touch[1].down)
            fprintf(stderr, "  [hybrid] a zero TOUCH_MAJOR retired a slotted contact\n");
        CHECK(input_state(in)->touch[1].down);

        input_destroy(in);
    }
})
