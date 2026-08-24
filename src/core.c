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
};

/* libretro's callbacks are plain C function pointers with no user data, so the
   active core is reachable through this single static. koboy runs exactly one
   core at a time, which makes that safe. */
static koboy_core *g_active;

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
}
static int16_t state_cb(unsigned port, unsigned dev, unsigned idx, unsigned id)
{
    (void)idx;
    if (port != 0 || dev != RETRO_DEVICE_JOYPAD || id > 15) return 0;
    return (g_active->latched >> id) & 1u;
}
/* Required even though audio is off: a core calling a NULL pointer crashes. */
static void   audio_sample_cb(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_batch_cb(const int16_t *d, size_t frames) { (void)d; return frames; }

/* Binds one symbol, writing a specific "missing symbol: NAME" message into
   err on the first failure so a bad core build fails loudly, not silently. */
static void *xdlsym(void *so, const char *name, char *err, size_t errlen)
{
    dlerror(); /* clear any pending error */
    void *sym = dlsym(so, name);
    const char *e = dlerror();
    if (e || !sym) {
        if (err && errlen) snprintf(err, errlen, "core missing symbol: %s", name);
        return NULL;
    }
    return sym;
}

/* g_active is not yet pointed at c during binding, so failure here must not
   touch g_active — doing so would wipe out a different core that is already
   active and mid-session if a second core_open() call fails partway through. */
#define BIND(field, name) \
    do { \
        *(void **)&c->field = xdlsym(so, name, err, errlen); \
        if (!c->field) { dlclose(so); free(c); return NULL; } \
    } while (0)

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
    return true;
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

void core_close(koboy_core *c)
{
    if (!c) return;
    c->unload_game();
    c->deinit();
    dlclose(c->so);
    free(c);
    g_active = NULL;
}
