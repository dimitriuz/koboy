#include "input.h"
#include "chrome.h"    /* chrome_lcd_layout: every control in the LCD layout's
                          bottom strip is a drawn box or disc, and chrome.c
                          owns where the drawn controls are -- see chrome.h.
                          Hit-testing them from a second, independently derived
                          copy of that arithmetic is precisely how a control
                          and its touch zone drift apart. */
#include <stdlib.h>
#include <string.h>

struct koboy_input {
    koboy_config      cfg;
    koboy_profile     prof;
    koboy_input_state st;
    int  slot;
    int  raw_max_x, raw_max_y;
    bool transpose, flip_x, flip_y;
    int  raw_x[KOBOY_MAX_TOUCH], raw_y[KOBOY_MAX_TOUCH];
    /* Whether this panel has ever spoken multitouch, which is what unlocks the
       single-touch ABS_X/ABS_Y position axes -- see input_feed_from. Latched,
       never cleared: a panel does not change protocol mid-session. */
    bool saw_mt;

    bool     pad_active;
    int      pad_slot, pad_ox, pad_oy;
    uint16_t held_dirs;      /* latched directions, for hysteresis */
    uint16_t key_bits;       /* from hardware keys */
    uint16_t hat_bits;       /* from a gamepad's ABS_HAT0X/Y d-pad -- see recompute() */

    bool menu_latched;      /* set on a MENU tap, cleared by the taker */
    bool menu_touching;     /* edge state, so a held finger latches once */

    koboy_rect ptr_rect;    /* see input_set_pointer_rect */
};

koboy_input *input_create(const koboy_config *c, const koboy_profile *p)
{
    koboy_input *in = calloc(1, sizeof *in);
    if (!in) return NULL;
    in->cfg = *c; in->prof = *p;
    in->raw_max_x = p->panel_w; in->raw_max_y = p->panel_h;
    /* The reserved game rect is the honest starting answer: it is where a
       frame will land, and until one has actually been submitted nobody knows
       any better. main.c narrows it per frame -- see input_set_pointer_rect. */
    in->ptr_rect.x = p->game_x; in->ptr_rect.y = p->game_y;
    in->ptr_rect.w = p->game_w; in->ptr_rect.h = p->game_h;
    return in;
}

void input_set_pointer_rect(koboy_input *in, int x, int y, int w, int h)
{
    if (!in) return;
    if (w < 1 || h < 1) return;   /* LIVE GUARD -- see input.h */
    in->ptr_rect.x = x; in->ptr_rect.y = y;
    in->ptr_rect.w = w; in->ptr_rect.h = h;
}

void input_destroy(koboy_input *in) { free(in); }
const koboy_input_state *input_state(const koboy_input *in) { return &in->st; }

/* Contract and the full rationale in input.h. key_bits is exactly the hardware
   half of what recompute() folds into st.buttons; the touch coordinates come
   over untouched so the list can still hit-test rows. */
void input_ui_state(const koboy_input *in, koboy_input_state *out)
{
    *out = in->st;
    out->buttons = in->key_bits;
}

void input_set_touch_transform(koboy_input *in, int raw_max_x, int raw_max_y,
                               bool transpose, bool flip_x, bool flip_y)
{
    in->raw_max_x = raw_max_x > 0 ? raw_max_x : 1;
    in->raw_max_y = raw_max_y > 0 ? raw_max_y : 1;
    in->transpose = transpose; in->flip_x = flip_x; in->flip_y = flip_y;
}

static int scale_axis(int v, int raw_max, int out)
{
    if (v < 0) v = 0;
    if (v > raw_max) v = raw_max;
    long r = (long)v * (out - 1) / raw_max;
    return (int)r;
}

static void apply_transform(koboy_input *in, int slot)
{
    int rx = in->raw_x[slot], ry = in->raw_y[slot];
    int px, py;
    if (in->transpose) {
        px = scale_axis(ry, in->raw_max_y, in->prof.panel_w);
        py = scale_axis(rx, in->raw_max_x, in->prof.panel_h);
    } else {
        px = scale_axis(rx, in->raw_max_x, in->prof.panel_w);
        py = scale_axis(ry, in->raw_max_y, in->prof.panel_h);
    }
    if (in->flip_x) px = in->prof.panel_w - 1 - px;
    if (in->flip_y) py = in->prof.panel_h - 1 - py;
    in->st.touch[slot].x = px;
    in->st.touch[slot].y = py;
}

static int perm(int v, int total) { return v * total / 1000; }

static bool in_circle(int x, int y, int cx, int cy, int r)
{
    long dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= (long)r * r;
}

static bool in_rect(int x, int y, int cx, int cy, int w, int h)
{
    return x >= cx - w / 2 && x <= cx + w / 2 && y >= cy - h / 2 && y <= cy + h / 2;
}

/* One axis of the thumb-pad. `held` carries the previous decision so release
   requires falling below (deadzone - hysteresis): without this the player gets
   boundary flicker with no timely visual cue to correct by. */
static uint16_t axis_bits(int delta, int dz, int hys, uint16_t neg, uint16_t pos,
                          uint16_t held)
{
    int on = (held & (neg | pos)) ? (dz - hys) : dz;
    if (on < 1) on = 1;
    if (delta >  on) return pos;
    if (delta < -on) return neg;
    return 0;
}

static bool in_rect_xywh(int x, int y, const koboy_rect *r)
{
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

/* One axis of the libretro pointer: a panel offset `d` inside a `span`-wide
   rect becomes -0x7fff at the first pixel and +0x7fff at the last.
   65534 and (span - 1), NOT 65536 and span, so BOTH ends are exact: the core
   divides straight back by 65534, so getting only the left edge right leaves
   the rightmost artwork column -- where several titles put a button --
   permanently unreachable. */
static int16_t pointer_axis(int d, int span)
{
    if (span < 2) return 0;      /* LIVE GUARD: a 1px rect has no gradient */
    if (d < 0) d = 0;
    if (d > span - 1) d = span - 1;
    return (int16_t)((long)d * 65534 / (span - 1) - 32767);
}

/* The touch d-pad, for a cross at (dcx, dcy) with arms of half-length dr. BOTH
   layouts call it -- the strip and the DMG faceplate draw the same cross
   (draw_dpad) -- so the deadzone, the hysteresis and the CROSS-vs-RELATIVE
   origin have ONE implementation. v1 learned that a drawn absolute cross must
   be steered absolutely; a second copy is a second chance to get it wrong.

   Owns pad_active/pad_slot/pad_ox/pad_oy/held_dirs and must be called exactly
   ONCE per recompute: claim-on-touch-down and release-on-lift are EDGE logic,
   not a query. */
static uint16_t dpad_bits(koboy_input *in, int dcx, int dcy, int dr)
{
    /* claim a pad slot on touch-down inside the pad region */
    if (!in->pad_active) {
        for (int s = 0; s < KOBOY_MAX_TOUCH; s++) {
            if (in->st.touch[s].down &&
                in_circle(in->st.touch[s].x, in->st.touch[s].y, dcx, dcy, dr)) {
                in->pad_active = true; in->pad_slot = s;
                in->pad_ox = in->st.touch[s].x; in->pad_oy = in->st.touch[s].y;
                break;
            }
        }
    } else if (!in->st.touch[in->pad_slot].down) {
        in->pad_active = false; in->held_dirs = 0;
    }
    if (!in->pad_active) return 0;

    const koboy_touch *t = &in->st.touch[in->pad_slot];
    int ox = in->pad_ox, oy = in->pad_oy;
    if (in->cfg.dpad_mode == KOBOY_DPAD_CROSS) { ox = dcx; oy = dcy; }
    uint16_t d = 0;
    d |= axis_bits(t->x - ox, in->cfg.dpad_deadzone, in->cfg.dpad_hysteresis,
                   KOBOY_BTN_LEFT, KOBOY_BTN_RIGHT, in->held_dirs);
    d |= axis_bits(t->y - oy, in->cfg.dpad_deadzone, in->cfg.dpad_hysteresis,
                   KOBOY_BTN_UP, KOBOY_BTN_DOWN, in->held_dirs);
    in->held_dirs = d;
    return d;
}

/* The LCD layout's input model: hardware keys, a FULL RETROPAD in the bottom
   strip, one MENU zone, and the pointer.

   The retropad is the correction this layout needed. Its first version
   hit-tested nothing but MENU, on the theory that a Game & Watch title exposes
   its own on-artwork buttons to a pointer. MEASURED false: the shipped .mgw
   files route through gwlua's compat init, which has no pointer handling -- a
   pointer press anywhere on the artwork changes ZERO pixels, a joypad press
   changes 211k. These titles use per-title retropad bindings koboy cannot know
   in advance, so the whole set is exposed.

   Every zone comes out of chrome_lcd_layout -- the SAME struct chrome.c draws
   from -- so a drawn control and its live zone cannot drift.

   ORDERING IS STRUCTURAL: MENU is tested FIRST and its slot excluded from
   everything after, and a slot that pressed any control is excluded from the
   pointer scan. At the shipped geometry none of these zones overlap, but that
   is a property of numbers in two files, and the consequence of it lapsing is
   a MENU tap that also fires a button. tests/test_input_touch.c overlaps the
   zones deliberately so the ordering is an assertion, not a comment. */
static void recompute_lcd(koboy_input *in, uint16_t b)
{
    chrome_lcd_controls c;
    chrome_lcd_layout(&in->prof, &c);

    int menu_slot = -1;
    for (int s = 0; s < KOBOY_MAX_TOUCH; s++) {
        if (!in->st.touch[s].down) continue;
        if (in_rect_xywh(in->st.touch[s].x, in->st.touch[s].y, &c.menu)) { menu_slot = s; break; }
    }
    /* Edge triggered, exactly as the DMG path: a held finger latches once. */
    if (menu_slot >= 0 && !in->menu_touching) in->menu_latched = true;
    in->menu_touching = (menu_slot >= 0);

    /* The d-pad, through the same decode the DMG faceplate uses. Called
       before the loop below and unconditionally, because it is edge logic:
       skipping it on a pass with no touches would never release the pad. */
    b |= dpad_bits(in, c.dpad_cx, c.dpad_cy, c.dpad_r);

    bool pressed = false;
    for (int s = 0; s < KOBOY_MAX_TOUCH; s++) {
        if (!in->st.touch[s].down) continue;
        if (s == menu_slot) continue;                      /* MENU wins */
        if (in->pad_active && s == in->pad_slot) continue; /* steering, not a button */
        int x = in->st.touch[s].x, y = in->st.touch[s].y;

        /* The face buttons. WHERE they sit is per system; WHICH BIT each
           reports is not -- c.x_cx is always the disc that sends KOBOY_BTN_X,
           whichever arrangement is drawn.

           The two SHOULDER bits are what moves: discs in the grid under ROWS6,
           pills in the lower band under the diamond. Tested from the same
           struct chrome.c drew from, and the unused form is inert twice over
           (chrome_lcd_layout zeroes the pills under ROWS6, and in_rect_xywh
           cannot match a zero-width rect). */
        uint16_t hit = 0;
        /* GATED ON face_n, NOT ON THE ZERO. PAIR2 leaves x_* and y_* at the
           origin, and unlike an absent PILL an absent DISC is NOT inert:
           in_circle(x, y, 0, 0, face_r) matches every touch within face_r of
           the panel's top-left corner, which is inside the game rect on every
           supported panel. */
        if (c.face_n >= 4) {
            if (in_circle(x, y, c.x_cx, c.x_cy, c.face_r)) hit |= KOBOY_BTN_X;
            if (in_circle(x, y, c.y_cx, c.y_cy, c.face_r)) hit |= KOBOY_BTN_Y;
        }
        if (in_circle(x, y, c.a_cx, c.a_cy, c.face_r)) hit |= KOBOY_BTN_A;
        if (in_circle(x, y, c.b_cx, c.b_cy, c.face_r)) hit |= KOBOY_BTN_B;
        if (c.face_n == 6) {
            if (in_circle(x, y, c.l1_cx, c.l1_cy, c.face_r)) hit |= KOBOY_BTN_L1;
            if (in_circle(x, y, c.r1_cx, c.r1_cy, c.face_r)) hit |= KOBOY_BTN_R1;
        } else {
            if (in_rect_xywh(x, y, &c.l1)) hit |= KOBOY_BTN_L1;
            if (in_rect_xywh(x, y, &c.r1)) hit |= KOBOY_BTN_R1;
        }
        if (in_rect_xywh(x, y, &c.select)) hit |= KOBOY_BTN_SELECT;
        if (in_rect_xywh(x, y, &c.start))  hit |= KOBOY_BTN_START;
        if (hit) { b |= hit; continue; }        /* a drawn control wins the touch */

        /* One condition rather than two early-outs so the loop ALWAYS reaches
           every remaining slot: an early `break` once the pointer is claimed
           reads as an optimisation, behaves identically with two fingers, and
           silently drops a third finger's button press. `!pressed` keeps the
           first live touch as the owner -- one touchscreen, one pointer. */
        if (!pressed && in_rect_xywh(x, y, &in->ptr_rect)) {
            in->st.pointer.x = pointer_axis(x - in->ptr_rect.x, in->ptr_rect.w);
            in->st.pointer.y = pointer_axis(y - in->ptr_rect.y, in->ptr_rect.h);
            pressed = true;
        }
    }
    /* MUST be assigned every pass, not only when something is down: a core
       that never sees PRESSED go false holds the artwork's button forever.
       The COORDINATES are deliberately left where they were -- what a real
       pointer device reports on release.

       The pointer stays even though the shipped titles ignore it: 18 of the
       games in third_party/gw/games/ use gwlua's NEW init path and read it.
       Not dead code -- code this collection's CONTENT happens not to
       exercise. */
    in->st.pointer.pressed = pressed;

    in->st.buttons = b;
}

static void recompute(koboy_input *in)
{
    const koboy_layout *l = &in->cfg.layout;
    const int W = in->prof.panel_w, H = in->prof.panel_h;
    /* hat_bits MERGES with the hardware keys here, the same way the touch
       d-pad and the on-screen A/B/Start/Select merge in below: a user may
       have a pad connected and still tap the drawn cross, or press a
       page-turn button, and none of those sources should shadow another. */
    uint16_t b = in->key_bits | in->hat_bits;

    /* The page-turn keys and a gamepad work in BOTH layouts -- folded in above
       the split, not inside the DMG branch: several Game & Watch titles bind
       ordinary retropad buttons too, and a device with hardware buttons should
       not lose them because the faceplate stopped drawing any. */
    if (in->prof.layout_mode == KOBOY_LAYOUT_LCD) { recompute_lcd(in, b); return; }

    b |= dpad_bits(in, perm(l->dpad_cx, W), perm(l->dpad_cy, H), perm(l->dpad_r, W));

    for (int s = 0; s < KOBOY_MAX_TOUCH; s++) {
        if (!in->st.touch[s].down) continue;
        if (in->pad_active && s == in->pad_slot) continue;
        int x = in->st.touch[s].x, y = in->st.touch[s].y;
        if (in_circle(x, y, perm(l->a_cx, W), perm(l->a_cy, H), perm(l->a_r, W))) b |= KOBOY_BTN_A;
        if (in_circle(x, y, perm(l->b_cx, W), perm(l->b_cy, H), perm(l->b_r, W))) b |= KOBOY_BTN_B;
        /* The extra discs. Which BIT each reports is the CORE's decision, so
           the bit travels in the layout beside the geometry (koboy.h) and this
           loop just reports it. LIVE GUARD on r, as chrome.c's draw has: an
           empty slot is a zero-radius circle at (0,0), and in_circle's <=
           would report a hit for a touch at exactly the panel origin -- a real
           coordinate a real finger can produce. */
        for (int e = 0; e < KOBOY_MAX_EXTRA_BTNS; e++)
            if (l->extra[e].r > 0 &&
                in_circle(x, y, perm(l->extra[e].cx, W), perm(l->extra[e].cy, H),
                          perm(l->extra[e].r, W)))
                b |= l->extra[e].bit;
        if (in_rect(x, y, perm(l->start_cx, W), perm(l->start_cy, H),
                    perm(l->start_w, W), perm(l->start_h, H))) b |= KOBOY_BTN_START;
        if (in_rect(x, y, perm(l->select_cx, W), perm(l->select_cy, H),
                    perm(l->select_w, W), perm(l->select_h, H))) b |= KOBOY_BTN_SELECT;
    }

    /* MENU is edge triggered for the same reason ui.c is: a held finger would
       otherwise re-open the menu on every poll. */
    bool menu_now = false;
    for (int s = 0; s < KOBOY_MAX_TOUCH; s++) {
        if (!in->st.touch[s].down) continue;
        if (in->pad_active && s == in->pad_slot) continue;
        if (in_rect(in->st.touch[s].x, in->st.touch[s].y,
                    perm(l->menu_cx, W), perm(l->menu_cy, H),
                    perm(l->menu_w, W), perm(l->menu_h, H)))
            menu_now = true;
    }
    if (menu_now && !in->menu_touching) in->menu_latched = true;
    in->menu_touching = menu_now;

    in->st.buttons = b;
}

void input_feed_key(koboy_input *in, uint16_t code, bool pressed)
{
    uint16_t bit = 0;
    if (in->cfg.key_a && code == in->cfg.key_a) bit = KOBOY_BTN_A;
    else if (in->cfg.key_b && code == in->cfg.key_b) bit = KOBOY_BTN_B;
    else if (in->cfg.key_start && code == in->cfg.key_start) bit = KOBOY_BTN_START;
    else if (in->cfg.key_select && code == in->cfg.key_select) bit = KOBOY_BTN_SELECT;
    if (!bit) return;
    if (pressed) in->key_bits |= bit; else in->key_bits &= (uint16_t)~bit;
    recompute(in);
}

/* Marks every contact lifted. BTN_TOUCH IS OUT-OF-BAND of the multitouch
   stream: FBInk's reader states the contract as "you won't get an
   EV_KEY:BTN_TOUCH:0 until *all* contact points have been lifted", so a zero
   there is a statement about the whole panel and not about in->slot. Getting
   that wrong the other way -- clearing only the current slot -- would leave a
   phantom finger on a panel that reports no slot at all. */
static void all_contacts_up(koboy_input *in)
{
    for (int s = 0; s < KOBOY_MAX_TOUCH; s++) in->st.touch[s].down = false;
}

/* Contract in input.h.
 *
 * THE LIFT IS THE WHOLE DIFFICULTY, and it is why this function takes a source.
 * A Kobo announces that a finger has left the panel in one of two ways, and
 * koboy shipped v2 understanding only the first:
 *
 *   ABS_MT_TRACKING_ID == -1   protocol B. The verified Libra 2 and everything
 *                              modern. Retires the contact by id.
 *   BTN_TOUCH == 0             everything else. The id STAYS NON-NEGATIVE
 *                              through the lift ("Phoenix": Aura H2O, Aura,
 *                              Aura SE r1, Glo HD, Touch 2.0, Nia, KA1) or is
 *                              repeated unchanged ("Snow"/Mk7: H2O2 r2, Clara
 *                              HD, Forma), or is never sent at all (the
 *                              pre-multitouch Touch A/B/C, Mini, Glo, Aura HD).
 *
 * Reading only the tracking id cost every tap after the first on three of those
 * four families -- ui.c is edge-triggered, so a contact that never falls never
 * rises again -- and on the emulator screen it held a joypad button down for
 * the rest of the session. github issue #1, reported on an Aura H2O.
 * tests/test_input_protocols.c replays all four streams.
 *
 * WHAT THIS DOES NOT FIX: `in->slot` is advanced by ABS_MT_SLOT alone, and the
 * Phoenix packet has none -- it separates contacts with SYN_MT_REPORT, which
 * this loop treats as an ordinary recompute boundary. So those panels track ONE
 * finger, and d-pad-plus-button is not available on them. `docs/FOLLOWUPS.md`
 * #108 has the two candidate demuxes and why neither should be guessed at
 * without a capture.
 *
 * WHY BTN_TOUCH AND NOT THE PRESSURE AXES. FBInk's own reader also treats
 * ABS_PRESSURE / ABS_MT_PRESSURE / ABS_MT_WIDTH_MAJOR going to zero as a lift,
 * and warns in the same breath that "getting a 0-pressure event on lift is
 * *not* a guarantee". They are not needed: every family above sends BTN_TOUCH,
 * which is a binary contact flag. And ABS_MT_TOUCH_MAJOR, which looks like the
 * obvious signal because it reads 0 on a lift, is a TRAP -- FBInk: "Oops, not
 * that one, it's always 0 on early Mk.7 devices", i.e. it reads 0 with a finger
 * DOWN, and believing it would make those panels untouchable. If a device ever
 * turns up that lifts with pressure alone, this is the paragraph to amend. */
void input_feed_from(koboy_input *in, koboy_ev_source src,
                     const koboy_ev *evs, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const koboy_ev *e = &evs[i];
        if (e->type == KOBOY_EV_KEY) {
            /* Not a button, and never bindable as one (koboy.h): the
               touchscreen's contact flag. Consumed HERE rather than falling
               through, so no key mapping can ever claim it. */
            if (src == KOBOY_EV_SRC_TOUCH && e->code == KOBOY_KEY_BTN_TOUCH) {
                if (e->value != 0) in->st.touch[in->slot].down = true;
                else               all_contacts_up(in);
                continue;
            }
            input_feed_key(in, e->code, e->value != 0);
            continue;
        }
        if (e->type == KOBOY_EV_SYN) {
            /* One recompute per SYN, not per event: a protocol B packet is
               only coherent at the SYN boundary. */
            recompute(in);
            continue;
        }
        if (e->type != KOBOY_EV_ABS) continue;
        switch (e->code) {
        /* The hat needs NO deadzone and NO hysteresis, unlike the touch
           thumb-pad: the kernel already reports a clean three-state axis
           (-1/0/+1, MEASURED on a real Xbox Wireless Controller, 2026-08-26),
           so copying axis_bits() onto it would only add latency a digital
           switch does not have. Updated immediately here; both this and touch
           position wait for recompute() at SYN to reach st.buttons. */
        case KOBOY_ABS_HAT0X:
            in->hat_bits &= (uint16_t)~(KOBOY_BTN_LEFT | KOBOY_BTN_RIGHT);
            if (e->value < 0) in->hat_bits |= KOBOY_BTN_LEFT;
            else if (e->value > 0) in->hat_bits |= KOBOY_BTN_RIGHT;
            break;
        case KOBOY_ABS_HAT0Y:
            in->hat_bits &= (uint16_t)~(KOBOY_BTN_UP | KOBOY_BTN_DOWN);
            if (e->value < 0) in->hat_bits |= KOBOY_BTN_UP;
            else if (e->value > 0) in->hat_bits |= KOBOY_BTN_DOWN;
            break;
        /* ABS_X/ABS_Y (0x00/0x01) are TWO THINGS AT ONCE, which is what `src`
           exists for. On a gamepad they are the analog stick, and decoding
           that is still refused: MEASURED, the stick streams at ~100 Hz even
           at rest (fuzz 255, flat 4095 around a centred 32768), so it would
           flood this loop with noise for no gain -- the hat is already the
           d-pad. Stick support, if ever added, needs the measured `flat` as a
           deadzone and must NOT reuse axis_bits() untouched, whose deadzone is
           in raw touch pixels, not 0..65535.
           On a TOUCHSCREEN they are a finger's position, and on the
           pre-multitouch Kobos they are the only position there is.
           `saw_mt` is a second gate on top of the source: a panel that speaks
           multitouch may ALSO emulate single-touch for legacy clients, and
           those coordinates track contact 0 -- writing them into whatever slot
           is current would corrupt a second finger's position. Every family in
           tests/test_input_protocols.c leads its first packet with an MT event,
           so on an MT panel this branch closes before it can be reached. */
        case KOBOY_ABS_X:
            if (src != KOBOY_EV_SRC_TOUCH || in->saw_mt) break;
            in->raw_x[in->slot] = e->value; apply_transform(in, in->slot); break;
        case KOBOY_ABS_Y:
            if (src != KOBOY_EV_SRC_TOUCH || in->saw_mt) break;
            in->raw_y[in->slot] = e->value; apply_transform(in, in->slot); break;
        case KOBOY_ABS_MT_SLOT:
            in->saw_mt = true;
            if (e->value >= 0 && e->value < KOBOY_MAX_TOUCH) in->slot = e->value;
            break;
        case KOBOY_ABS_MT_TRACKING_ID:
            in->saw_mt = true;
            /* A NEGATIVE id retires the contact; a non-negative one asserts it.
               NOT the whole story on its own -- three of the four Kobo touch
               protocols never send the negative, which is why BTN_TOUCH above
               is the other half. See input_feed_from's header. */
            in->st.touch[in->slot].down = (e->value >= 0);
            break;
        case KOBOY_ABS_MT_POSITION_X:
            in->saw_mt = true;
            in->raw_x[in->slot] = e->value; apply_transform(in, in->slot); break;
        case KOBOY_ABS_MT_POSITION_Y:
            in->saw_mt = true;
            in->raw_y[in->slot] = e->value; apply_transform(in, in->slot); break;
        default: break;
        }
    }
}

void input_feed(koboy_input *in, const koboy_ev *evs, size_t n)
{
    input_feed_from(in, KOBOY_EV_SRC_TOUCH, evs, n);
}

bool input_take_menu_request(koboy_input *in)
{
    bool v = in->menu_latched;
    in->menu_latched = false;
    return v;
}
