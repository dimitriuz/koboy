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

void input_feed(koboy_input *in, const koboy_ev *evs, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const koboy_ev *e = &evs[i];
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
