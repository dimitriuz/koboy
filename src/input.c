#include "input.h"
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
};

koboy_input *input_create(const koboy_config *c, const koboy_profile *p)
{
    koboy_input *in = calloc(1, sizeof *in);
    if (!in) return NULL;
    in->cfg = *c; in->prof = *p;
    in->raw_max_x = p->panel_w; in->raw_max_y = p->panel_h;
    return in;
}

void input_destroy(koboy_input *in) { free(in); }
const koboy_input_state *input_state(const koboy_input *in) { return &in->st; }

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

static void recompute(koboy_input *in)
{
    const koboy_layout *l = &in->cfg.layout;
    const int W = in->prof.panel_w, H = in->prof.panel_h;
    uint16_t b = in->key_bits;

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
    in->st.buttons = b;
}

void input_feed_key(koboy_input *in, uint16_t code, bool pressed)
{
    uint16_t bit = 0;
    if (in->cfg.key_a && code == in->cfg.key_a) bit = KOBOY_BTN_A;
    else if (in->cfg.key_b && code == in->cfg.key_b) bit = KOBOY_BTN_B;
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
