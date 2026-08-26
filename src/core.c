#define _POSIX_C_SOURCE 200809L
#include "core.h"
#include "libretro_min.h"
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct koboy_core {
    void *so;
    char  save_dir[512];
    koboy_pixfmt fmt;
    bool  need_fullpath;
    void (*frame_cb)(void *, const void *, unsigned, unsigned, size_t);
    void *frame_ud;
    uint16_t (*input_fn)(void *);
    void *input_ud;
    uint16_t latched;
    void (*pointer_fn)(void *, int16_t *, int16_t *, bool *);
    void *pointer_ud;
    int16_t ptr_x, ptr_y;
    bool    ptr_pressed;
    /* bound symbols */
    unsigned (*api_version)(void);
    void (*set_environment)(retro_environment_t);
    void (*set_video_refresh)(retro_video_refresh_t);
    void (*set_audio_sample)(retro_audio_sample_t);
    void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*set_input_poll)(retro_input_poll_t);
    void (*set_input_state)(retro_input_state_t);
    void (*init)(void);
    void (*deinit)(void);
    void (*get_system_info)(struct retro_system_info *);
    void (*get_system_av_info)(struct retro_system_av_info *);
    bool (*load_game)(const struct retro_game_info *);
    void (*unload_game)(void);
    void (*run)(void);
    void *(*get_memory_data)(unsigned);
    size_t (*get_memory_size)(unsigned);
    void (*reset)(void);
    /* Optional: serialisation. NULL when the core does not export it. */
    size_t (*serialize_size)(void);
    bool   (*serialize)(void *, size_t);
    bool   (*unserialize)(const void *, size_t);
    bool    game_loaded;
    /* Set by core_load_rom from retro_get_system_av_info, and kept live
       afterwards by env_cb's SET_GEOMETRY/SET_SYSTEM_AV_INFO handling; 0
       until a ROM has loaded. See core_get_geometry's comment in core.h for
       why this is not a load-once value. */
    int     base_w, base_h, max_w, max_h;
    /* See core_geometry_changed. Left false by the initial core_load_rom
       query on purpose -- only env_cb's two geometry commands set it. */
    bool    geom_dirty;
};

/* libretro's callbacks are plain C function pointers with no user data, so the
   active core is reachable through this single static. koboy runs exactly one
   core at a time, which makes that safe. */
static koboy_core *g_active;

/* Shared by core_load_rom's initial query and the SET_SYSTEM_AV_INFO
   handler below: both hand over a FULL, authoritative retro_game_geometry
   (as opposed to SET_GEOMETRY's partial update, handled separately in
   env_cb), so both apply the same "0 means same as base" convention and
   trust the numbers outright rather than only ever growing them. */
static void apply_full_geometry(koboy_core *c, const struct retro_game_geometry *g)
{
    c->base_w = (int)g->base_width;
    c->base_h = (int)g->base_height;
    c->max_w  = g->max_width  ? (int)g->max_width  : c->base_w;
    c->max_h  = g->max_height ? (int)g->max_height : c->base_h;
}

static bool env_cb(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        enum retro_pixel_format f = *(const enum retro_pixel_format *)data;
        if (f == RETRO_PIXEL_FORMAT_RGB565)   { g_active->fmt = KOBOY_PIXFMT_RGB565;   return true; }
        if (f == RETRO_PIXEL_FORMAT_XRGB8888) { g_active->fmt = KOBOY_PIXFMT_XRGB8888; return true; }
        return false;                      /* refuse legacy 0RGB1555 */
    }
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;              /* a NULL frame means "unchanged" */
        return true;
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
        *(int *)data = 1;                  /* video on, audio off: v1 has no audio */
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = g_active->save_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = data;
        if (!strcmp(v->key, "gambatte_mix_frames")) {
            /* blending makes every pixel change every frame, destroying
               dirty-rect tracking and layering fake ghosting on real */
            v->value = "disabled"; return true;
        }
        if (!strcmp(v->key, "gambatte_gbc_color_correction")) {
            v->value = "disabled"; return true;
        }
        v->value = NULL; return false;
    }
    case RETRO_ENVIRONMENT_SET_GEOMETRY: {
        /* A PARTIAL update, by libretro convention: only base_width/height
           (and aspect_ratio, which koboy does not use) are meant to change
           here, and max_width/max_height in the payload are conventionally
           left 0 to mean "unchanged" -- unlike SET_SYSTEM_AV_INFO's full
           reset below, a 0 here must NOT collapse an already-known max down
           to base. Trusted (not merely floored) when non-zero: a shrink is
           as legitimate an announcement as a grow (a Multi Screen title
           folding back down, say), and the guard belongs to whoever is
           about to size a buffer against it (video.c's own bounds check),
           not to this callback second-guessing the core. */
        const struct retro_game_geometry *g = data;
        g_active->base_w = (int)g->base_width;
        g_active->base_h = (int)g->base_height;
        if (g->max_width)  g_active->max_w = (int)g->max_width;
        if (g->max_height) g_active->max_h = (int)g->max_height;
        /* Invariant regardless of source: base can never legitimately
           exceed max, so a core that grew base without also mentioning a
           bigger max here gets max raised to match rather than left to lie. */
        if (g_active->max_w < g_active->base_w) g_active->max_w = g_active->base_w;
        if (g_active->max_h < g_active->base_h) g_active->max_h = g_active->base_h;
        g_active->geom_dirty = true;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: {
        /* The FULL reset: geometry AND timing. Timing (fps/sample_rate) is
           read from `av->timing` by nothing in this codebase -- src/pacing.c
           still paces every core at the fixed KOBOY_FRAME_US the Game Boy
           measured (koboy.h), which is a real, deliberately out-of-scope gap
           for this task (video resolution, not frame timing): pacing a core
           whose fps this call changes is future work, tracked the same way
           the rest of this project tracks a known-but-deferred item rather
           than silently dropped. Geometry, which IS this task's scope, is
           applied in full via the same helper the initial load-time query
           uses. */
        const struct retro_system_av_info *av = data;
        apply_full_geometry(g_active, &av->geometry);
        g_active->geom_dirty = true;
        return true;
    }
    default:
        return false;                      /* unknown calls: false is correct */
    }
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
    if (g_active->frame_cb) g_active->frame_cb(g_active->frame_ud, data, w, h, pitch);
}
static void poll_cb(void)
{
    g_active->latched = g_active->input_fn ? g_active->input_fn(g_active->input_ud) : 0;
    /* Latched at poll time, exactly like the joypad bits, so every
       input_state_cb call within one retro_run() sees one coherent snapshot.
       The gw core makes three separate POINTER queries per frame (X, Y, then
       PRESSED); reading live state on each would let a finger lift between
       the coordinate reads and the press read. */
    if (g_active->pointer_fn) {
        g_active->pointer_fn(g_active->pointer_ud, &g_active->ptr_x,
                             &g_active->ptr_y, &g_active->ptr_pressed);
    } else {
        g_active->ptr_x = g_active->ptr_y = 0;
        g_active->ptr_pressed = false;
    }
}
static int16_t state_cb(unsigned port, unsigned dev, unsigned idx, unsigned id)
{
    if (dev == RETRO_DEVICE_POINTER) {
        /* PORT IS DELIBERATELY NOT CHECKED. The Game & Watch core asks on
           port 2 (third_party/gw/src/libretro.c) where the libretro
           convention is port 0, and koboy has exactly ONE pointer to report
           regardless -- a single touchscreen. Hard-coding 2 would answer the
           one core measured here and silently return 0 for any other core
           that follows the convention; hard-coding 0 would break the one core
           that actually exists. `idx` IS checked: it is the touch INDEX, and
           only the first touch becomes a pointer (see recompute_lcd). */
        if (idx != 0) return 0;
        switch (id) {
        case RETRO_DEVICE_ID_POINTER_X:       return g_active->ptr_x;
        case RETRO_DEVICE_ID_POINTER_Y:       return g_active->ptr_y;
        case RETRO_DEVICE_ID_POINTER_PRESSED: return g_active->ptr_pressed ? 1 : 0;
        case RETRO_DEVICE_ID_POINTER_COUNT:   return g_active->ptr_pressed ? 1 : 0;
        default: return 0;
        }
    }
    (void)idx;
    if (port != 0 || dev != RETRO_DEVICE_JOYPAD || id > 15) return 0;
    return (g_active->latched >> id) & 1u;
}
/* Required even though audio is off: a core calling a NULL pointer crashes. */
static void   audio_sample_cb(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_batch_cb(const int16_t *d, size_t frames) { (void)d; return frames; }

/* Binds one symbol, writing a specific "missing symbol: NAME" message into
   err on the first failure so a bad core build fails loudly, not silently.
   #11: the message also names the core's .so path, like its dlopen sibling
   below -- on a device where a photo of the panel is the only diagnostic,
   the path is the useful half of the message. */
static void *xdlsym(void *so, const char *name, const char *so_path,
                    char *err, size_t errlen)
{
    dlerror(); /* clear any pending error */
    void *sym = dlsym(so, name);
    const char *e = dlerror();
    if (e || !sym) {
        if (err && errlen) snprintf(err, errlen, "core %s is missing %s", so_path, name);
        return NULL;
    }
    return sym;
}

/* g_active is not yet pointed at c during binding, so failure here must not
   touch g_active — doing so would wipe out a different core that is already
   active and mid-session if a second core_open() call fails partway through. */
#define BIND(field, name) \
    do { \
        *(void **)&c->field = xdlsym(so, name, so_path, err, errlen); \
        if (!c->field) { dlclose(so); free(c); return NULL; } \
    } while (0)

/* Optional: a missing symbol is a capability answer, not a fatal error. The
   test stub is the immediate reason, but the rule is general -- refusing to
   start because a core cannot serialise would trade playing the game for a
   feature the user did not ask for. */
#define BIND_OPT(field, name) \
    do { *(void **)&c->field = dlsym(so, name); } while (0)

koboy_core *core_open(const char *so_path, const char *save_dir, char *err, size_t errlen)
{
    if (err && errlen) err[0] = 0;

    void *so = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
    if (!so) {
        if (err && errlen) snprintf(err, errlen, "cannot open core %s: %s", so_path, dlerror());
        return NULL;
    }

    koboy_core *c = calloc(1, sizeof *c);
    if (!c) {
        if (err && errlen) snprintf(err, errlen, "out of memory opening core %s", so_path);
        dlclose(so);
        return NULL;
    }
    c->so = so;
    snprintf(c->save_dir, sizeof c->save_dir, "%s", save_dir ? save_dir : ".");

    BIND(api_version,             "retro_api_version");
    BIND(set_environment,         "retro_set_environment");
    BIND(set_video_refresh,       "retro_set_video_refresh");
    BIND(set_audio_sample,        "retro_set_audio_sample");
    BIND(set_audio_sample_batch,  "retro_set_audio_sample_batch");
    BIND(set_input_poll,          "retro_set_input_poll");
    BIND(set_input_state,         "retro_set_input_state");
    BIND(init,                    "retro_init");
    BIND(deinit,                  "retro_deinit");
    BIND(get_system_info,         "retro_get_system_info");
    BIND(get_system_av_info,      "retro_get_system_av_info");
    BIND(load_game,               "retro_load_game");
    BIND(unload_game,             "retro_unload_game");
    BIND(run,                     "retro_run");
    BIND(get_memory_data,         "retro_get_memory_data");
    BIND(get_memory_size,         "retro_get_memory_size");
    BIND(reset,                   "retro_reset");

    BIND_OPT(serialize_size, "retro_serialize_size");
    BIND_OPT(serialize,      "retro_serialize");
    BIND_OPT(unserialize,    "retro_unserialize");

    if (c->api_version() != 1) {
        if (err && errlen) snprintf(err, errlen, "core %s: unsupported API version", so_path);
        dlclose(so);
        free(c);
        return NULL;
    }

    g_active = c;

    /* set_environment MUST precede init(): cores query options and decide
       their pixel format during init based on what the environment answers. */
    c->set_environment(env_cb);
    c->set_video_refresh(video_cb);
    c->set_audio_sample(audio_sample_cb);
    c->set_audio_sample_batch(audio_batch_cb);
    c->set_input_poll(poll_cb);
    c->set_input_state(state_cb);
    c->init();

    struct retro_system_info info;
    c->get_system_info(&info);
    c->need_fullpath = info.need_fullpath;

    return c;
}

bool core_load_rom(koboy_core *c, const char *rom_path, char *err, size_t errlen)
{
    if (err && errlen) err[0] = 0;

    /* Symmetric with the double-unload guard in core_unload_rom: refuse
       rather than silently re-entering retro_load_game on top of a cartridge
       state that was never torn down. The caller must call core_unload_rom
       first -- chosen over an implicit auto-unload so switching ROMs is
       always the explicit two-call sequence the libretro API expects, with
       no hidden state transition a caller could miss. */
    if (c->game_loaded) {
        if (err && errlen) snprintf(err, errlen, "core %s already has a ROM loaded; call core_unload_rom first", rom_path);
        return false;
    }

    struct retro_game_info info = {0};
    void *buf = NULL;

    if (c->need_fullpath) {
        info.path = rom_path;
    } else {
        struct stat st;
        if (stat(rom_path, &st) != 0) {
            if (err && errlen) snprintf(err, errlen, "cannot stat rom %s: %s", rom_path, strerror(errno));
            return false;
        }
        buf = malloc((size_t)st.st_size);
        if (!buf && st.st_size != 0) {
            if (err && errlen) snprintf(err, errlen, "out of memory reading rom %s", rom_path);
            return false;
        }
        FILE *f = fopen(rom_path, "rb");
        if (!f) {
            if (err && errlen) snprintf(err, errlen, "cannot open rom %s: %s", rom_path, strerror(errno));
            free(buf);
            return false;
        }
        size_t got = st.st_size ? fread(buf, 1, (size_t)st.st_size, f) : 0;
        fclose(f);
        if (got != (size_t)st.st_size) {
            if (err && errlen) snprintf(err, errlen, "short read on rom %s", rom_path);
            free(buf);
            return false;
        }
        info.path = rom_path;
        info.data = buf;
        info.size = (size_t)st.st_size;
    }

    bool ok = c->load_game(&info);
    free(buf);
    if (!ok) {
        if (err && errlen) snprintf(err, errlen, "core rejected rom %s", rom_path);
        return false;
    }
    c->game_loaded = true;

    /* Queried here, once per load, as a STARTING point -- not the final
       word: retro_get_system_av_info is only meaningful once a game is
       loaded (the libretro spec says so, and an unloaded gambatte answers
       with whatever it was last built with, which is not "no answer" the
       way a NULL would be), so this is the earliest correct moment to call
       it. But "earliest correct" is not "trustworthy": the Game & Watch core
       answers this exact call with a 128x128 placeholder on every one of 59
       measured titles, and only reports the real canvas later, from inside
       its first retro_run(), via env_cb's SET_GEOMETRY/SET_SYSTEM_AV_INFO
       handling above -- which is why those two, not just this query, keep
       base_w/base_h/max_w/max_h current for as long as the ROM stays
       loaded. geom_dirty is deliberately NOT set here: a caller that wants
       this initial (possibly-placeholder) answer calls core_get_geometry
       directly right after core_load_rom returns, unconditionally, and does
       not need a change flag to tell it something it is about to read
       anyway -- see core_geometry_changed's comment in core.h. */
    struct retro_system_av_info av;
    c->get_system_av_info(&av);
    apply_full_geometry(c, &av.geometry);
    c->geom_dirty = false;
    return true;
}

bool core_unload_rom(koboy_core *c)
{
    if (!c || !c->game_loaded) return false;
    c->unload_game();
    c->game_loaded = false;
    return true;
}

bool core_reset(koboy_core *c)
{
    if (!c || !c->reset) return false;
    c->reset();
    return true;
}

size_t core_state_size(koboy_core *c)
{
    /* All three are required together: a core exporting only some of them
       cannot round-trip, and reporting a non-zero size would offer the user a
       Save that silently cannot be loaded. */
    if (!c || !c->serialize_size || !c->serialize || !c->unserialize) return 0;
    return c->serialize_size();
}

bool core_state_save(koboy_core *c, void *buf, size_t n)
{
    size_t need = core_state_size(c);
    if (!need || n < need || !buf) return false;
    return c->serialize(buf, need);
}

bool core_state_load(koboy_core *c, const void *buf, size_t n)
{
    size_t need = core_state_size(c);
    if (!need || n < need || !buf) return false;
    return c->unserialize(buf, need);
}

void core_set_frame_cb(koboy_core *c,
                       void (*cb)(void *ud, const void *data, unsigned w, unsigned h, size_t pitch),
                       void *ud)
{
    c->frame_cb = cb;
    c->frame_ud = ud;
}

void core_set_input_fn(koboy_core *c, uint16_t (*fn)(void *ud), void *ud)
{
    c->input_fn = fn;
    c->input_ud = ud;
}

void core_set_pointer_fn(koboy_core *c,
                         void (*fn)(void *ud, int16_t *x, int16_t *y, bool *pressed),
                         void *ud)
{
    c->pointer_fn = fn;
    c->pointer_ud = ud;
}

void core_run_frame(koboy_core *c)
{
    c->run();
}

uint8_t *core_sram(koboy_core *c, size_t *len)
{
    uint8_t *p = c->get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t   l = c->get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (len) *len = p ? l : 0;
    return p;
}

koboy_pixfmt core_pixfmt(const koboy_core *c)
{
    return c->fmt;
}

bool core_get_geometry(const koboy_core *c, int *base_w, int *base_h,
                       int *max_w, int *max_h)
{
    if (!c || !c->game_loaded) return false;   /* nothing honest to report yet */
    if (base_w) *base_w = c->base_w;
    if (base_h) *base_h = c->base_h;
    if (max_w)  *max_w  = c->max_w;
    if (max_h)  *max_h  = c->max_h;
    return true;
}

bool core_geometry_changed(koboy_core *c)
{
    if (!c || !c->geom_dirty) return false;
    c->geom_dirty = false;   /* read-and-clear: each change reported once */
    return true;
}

void core_close(koboy_core *c)
{
    if (!c) return;
    core_unload_rom(c); /* no-op if no ROM is loaded -- a double unload is impossible */
    c->deinit();
    dlclose(c->so);
    free(c);
    g_active = NULL;
}
