#include "input.h"
#include "chrome.h"    /* chrome_lcd_menu_rect: the MENU zone in the LCD
                          layout is a drawn box, and chrome.c owns where the
                          drawn controls are -- see chrome.h. Hit-testing it
                          from a second, independently derived copy of that
                          arithmetic is precisely how a control and its touch
                          zone drift apart. */
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

   65534, not 65536, and (span - 1), not span, so BOTH ends are exact: the
   core divides straight back by 65534 (third_party/gw/gwlua/functions.c), so
   a mapping that only got the left edge right would leave the rightmost
   column of artwork -- where several titles put a button -- permanently
   unreachable. */
static int16_t pointer_axis(int d, int span)
{
    if (span < 2) return 0;      /* LIVE GUARD: a 1px rect has no gradient */
    if (d < 0) d = 0;
    if (d > span - 1) d = span - 1;
    return (int16_t)((long)d * 65534 / (span - 1) - 32767);
}

/* The LCD layout's input model, in full: hardware keys, one MENU zone, and
   the pointer. No d-pad, no A/B, no Start/Select -- chrome_render_lcd draws
   none of them, and a live zone under nothing drawn is worse than useless
   (it eats touches meant for the artwork).

   MENU is tested FIRST and its slot is excluded from the pointer scan below.
   The two zones cannot overlap at the shipped geometry -- MENU is in the
   bottom strip and the game rect stops above it -- but "cannot overlap today"
   is a property of two numbers in two files, and the consequence of it
   lapsing is a MENU tap that also presses whatever the artwork draws under
   it. Ordering makes it structural instead. */
static void recompute_lcd(koboy_input *in, uint16_t b)
{
    koboy_rect m;
    chrome_lcd_menu_rect(&in->prof, &m);

    int menu_slot = -1;
    for (int s = 0; s < KOBOY_MAX_TOUCH; s++) {
        if (!in->st.touch[s].down) continue;
        if (in_rect_xywh(in->st.touch[s].x, in->st.touch[s].y, &m)) { menu_slot = s; break; }
    }
    /* Edge triggered, exactly as the DMG path: a held finger latches once. */
    if (menu_slot >= 0 && !in->menu_touching) in->menu_latched = true;
    in->menu_touching = (menu_slot >= 0);

    bool pressed = false;
    for (int s = 0; s < KOBOY_MAX_TOUCH; s++) {
        if (!in->st.touch[s].down) continue;
        if (s == menu_slot) continue;                      /* MENU wins */
        int x = in->st.touch[s].x, y = in->st.touch[s].y;
        if (!in_rect_xywh(x, y, &in->ptr_rect)) continue;  /* case, not artwork */
        in->st.pointer.x = pointer_axis(x - in->ptr_rect.x, in->ptr_rect.w);
        in->st.pointer.y = pointer_axis(y - in->ptr_rect.y, in->ptr_rect.h);
        pressed = true;
        break;                     /* one pointer; the first live touch owns it */
    }
    /* MUST be assigned every pass, not only when something is down: a core
       that never sees PRESSED go false holds the artwork's button forever.
       The COORDINATES are deliberately left where they were -- that is what a
       real pointer device reports on release, and the core reads PRESSED to
       decide a press, not the position. */
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

    /* The page-turn keys and a gamepad keep working in BOTH layouts -- they
       are folded in above the split, not inside the DMG branch. Several Game
       & Watch titles bind ordinary retropad buttons as well as their drawn
       ones, and a device with hardware buttons should not lose them just
       because the faceplate stopped drawing any. */
    if (in->prof.layout_mode == KOBOY_LAYOUT_LCD) { recompute_lcd(in, b); return; }

    int dcx = perm(l->dpad_cx, W), dcy = perm(l->dpad_cy, H), dr = perm(l->dpad_r, W);

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

    if (in->pad_active) {
        const koboy_touch *t = &in->st.touch[in->pad_slot];
        int ox = in->pad_ox, oy = in->pad_oy;
        if (in->cfg.dpad_mode == KOBOY_DPAD_CROSS) { ox = dcx; oy = dcy; }
        uint16_t d = 0;
        d |= axis_bits(t->x - ox, in->cfg.dpad_deadzone, in->cfg.dpad_hysteresis,
                       KOBOY_BTN_LEFT, KOBOY_BTN_RIGHT, in->held_dirs);
        d |= axis_bits(t->y - oy, in->cfg.dpad_deadzone, in->cfg.dpad_hysteresis,
                       KOBOY_BTN_UP, KOBOY_BTN_DOWN, in->held_dirs);
        in->held_dirs = d;
        b |= d;
    }

    for (int s = 0; s < KOBOY_MAX_TOUCH; s++) {
        if (!in->st.touch[s].down) continue;
        if (in->pad_active && s == in->pad_slot) continue;
        int x = in->st.touch[s].x, y = in->st.touch[s].y;
        if (in_circle(x, y, perm(l->a_cx, W), perm(l->a_cy, H), perm(l->a_r, W))) b |= KOBOY_BTN_A;
        if (in_circle(x, y, perm(l->b_cx, W), perm(l->b_cy, H), perm(l->b_r, W))) b |= KOBOY_BTN_B;
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

void input_feed(koboy_input *in, const koboy_ev *evs, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const koboy_ev *e = &evs[i];
        if (e->type == KOBOY_EV_KEY) {
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
        /* The hat needs no deadzone and no hysteresis, unlike the touch
           thumb-pad above: the kernel already reports a clean three-state
           axis (-1/0/+1, measured on a real Xbox Wireless Controller,
           2026-08-26), so there is nothing left to filter. Copying the touch
           pad's axis_bits() machinery onto it would only add latency a
           digital switch does not have. Updated here, immediately, the same
           way apply_transform() updates touch position immediately below --
           both wait for the shared recompute() at SYN to become visible in
           st.buttons. */
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
        /* ABS_X/ABS_Y (codes 0x00/0x01, the analog stick) are deliberately
           NOT handled -- and not even named as constants, the same rule
           in.h explains. MEASURED: the stick streams at ~100Hz even at rest
           (fuzz 255, flat 4095 around a centred 32768), so decoding it here
           would flood input_feed with noise for no gain -- the hat above is
           already the d-pad a Game Boy needs. They fall through to `default`
           and are silently ignored. Stick support, if ever added, needs the
           measured `flat` value applied as a deadzone; it must NOT reuse
           axis_bits() untouched, because that deadzone is in raw touch
           pixels, not the stick's 0..65535 range. */
        case KOBOY_ABS_MT_SLOT:
            if (e->value >= 0 && e->value < KOBOY_MAX_TOUCH) in->slot = e->value;
            break;
        case KOBOY_ABS_MT_TRACKING_ID:
            in->st.touch[in->slot].down = (e->value >= 0);
            break;
        case KOBOY_ABS_MT_POSITION_X:
            in->raw_x[in->slot] = e->value; apply_transform(in, in->slot); break;
        case KOBOY_ABS_MT_POSITION_Y:
            in->raw_y[in->slot] = e->value; apply_transform(in, in->slot); break;
        default: break;
        }
    }
}

bool input_take_menu_request(koboy_input *in)
{
    bool v = in->menu_latched;
    in->menu_latched = false;
    return v;
}
