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
    /* Set by core_load_rom, kept live by env_cb's SET_GEOMETRY /
       SET_SYSTEM_AV_INFO; 0 until a ROM loads. core.h says why this is not a
       load-once value. */
    int     base_w, base_h, max_w, max_h;
    /* See core_geometry_changed. Left false by the initial core_load_rom
       query on purpose -- only env_cb's two geometry commands set it. */
    bool    geom_dirty;
    /* aspect_ratio and fps exactly as the core reported them, kept live by
       the same three paths that keep base/max live. `aspect` of 0 is
       libretro's own "no answer", STORED rather than normalised here: the
       fallback needs base_w/base_h, and base can change after aspect was
       announced. */
    float   aspect;
    double  fps;
    /* Quarter turns COUNTER-CLOCKWISE from SET_ROTATION, 0..3. Announced per
       GAME, not per core (FBNeo asks 3 on Galaga, 0 on Donkey Kong Jr.), so
       it resets with the handle and is set again inside retro_load_game. */
    unsigned rot;
    /* The save-RAM region's LENGTH, captured once by core_load_rom and never
       re-asked -- core_sram says why. 0 before load and after unload. */
    size_t  sram_len;
};

/* libretro's callbacks are plain C function pointers with no user data, so the
   active core is reachable only through this static. Safe because koboy runs
   exactly one core at a time. */
static koboy_core *g_active;

/* Shared by core_load_rom's initial query and SET_SYSTEM_AV_INFO: both hand
   over a FULL retro_game_geometry (unlike SET_GEOMETRY's partial update), so
   both apply "0 means same as base" and trust the numbers outright rather
   than only ever growing them. */
static void apply_full_geometry(koboy_core *c, const struct retro_game_geometry *g)
{
    c->base_w = (int)g->base_width;
    c->base_h = (int)g->base_height;
    c->max_w  = g->max_width  ? (int)g->max_width  : c->base_w;
    c->max_h  = g->max_height ? (int)g->max_height : c->base_h;
    c->aspect = g->aspect_ratio;
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
        /* PokeMini renders its 96x64 panel through an INTERNAL upscaler,
           default 4x, and bakes a "Dot Matrix" LCD filter in at any scale
           above 1. Both are wrong here, MEASURED:
             - at 4x the core reports 384x256, which the DMG scale search
               multiplies AGAIN (scale 3 on 1264x1680: 1152x768 destination
               pixels, ~23 ms of video_submit). At 1x it reports 96x64 and
               koboy's integer scaler enlarges for free inside a block copy.
             - the dot-matrix grid is a one-pixel-period pattern; quantised to
               four greys it survives as full-rect high-frequency noise on
               every frame -- rendered and looked at, not guessed.
           The core disables the filter outright at 1x, so one answer fixes
           both.

           The PALETTE is the e-ink half: the default paints the panel 83%
           BLACK (mean luma 0.174; "Monochrome" 0.104). A mostly-black rect is
           unreadable on reflective paper and the worst case for the panel's
           waveforms. "Monochrome Vector" is the same two shades inverted, mean
           0.868 -- dark ink on white.

           pokemini_lcdmode is deliberately left at the core's default:
           "analog" mimics the real LCD's ghosting, which sounds like
           gambatte_mix_frames' problem, but measured against 3shades/2shades
           on a static screen it moved the changed-pixel count by 3 pixels in
           6144 (13.88% vs 13.83%). Not the mix_frames case. */
        if (!strcmp(v->key, "pokemini_video_scale")) {
            v->value = "1x"; return true;
        }
        if (!strcmp(v->key, "pokemini_palette")) {
            v->value = "Monochrome Vector"; return true;
        }

        /* AN ARCADE BOARD HAS NO BATTERY, so the high-score table is all it can
           persist: retro_get_memory_size(SAVE_RAM) is 0 on all 227 romsets
           (measured) and there is no .srm for any of them. FBNeo keeps scores
           through its own hiscore.dat, and this answer turns that on.

           NOT a preference. The core's own default IS "enabled", but the code
           reading it (retro_common.cpp, check_variables) leaves EnableHiscores
           at its BSS zero when the frontend REFUSES the query -- which is what
           the line below this block does for every unrecognised key. So saying
           nothing gets the OPPOSITE of the core's stated default.

           Safe without hiscore.dat installed: HiscoreInit opens
           <system_dir>/fbneo/hiscore.dat, finds nothing, and the feature is
           inert. With it, scores go to <save_dir>/fbneo/<board>.hi -- and
           koboy answers both directory queries with save_dir. */
        if (!strcmp(v->key, "fbneo-hiscores")) {
            v->value = "enabled"; return true;
        }
        v->value = NULL; return false;
    }
    case RETRO_ENVIRONMENT_SET_ROTATION: {
        /* ANSWERING TRUE IS A PROMISE, not an acknowledgement, and a shipped
           core reads it as one: libretro's contract is that a frontend
           returning true turns the picture itself. beetle-wswan asks at
           surface-init time and REMEMBERS THE ANSWER (hw_rotate_enabled) --
           on false it allocates a second buffer and rotates in software, on
           true it stops and reports geometry in the orientation it renders.
           So this cannot be a no-op that records a number: the moment it
           returns true, a WonderSwan's frames arrive un-rotated and koboy owes
           the rotation. video_pipeline_run pays it.

           Values outside 0..3 are MASKED rather than rejected -- a stray 4
           would index nothing sensible downstream.

           geom_dirty because core_get_geometry reports PRESENTED geometry and
           an odd rotation transposes it, so a rotation change alone still
           needs main.c to re-fit the rect. */
        unsigned r = *(const unsigned *)data & 3u;
        if (r != g_active->rot) {
            g_active->rot = r;
            g_active->geom_dirty = true;
        }
        return true;
    }
    case RETRO_ENVIRONMENT_SET_GEOMETRY: {
        /* A PARTIAL update by libretro convention: base and aspect change
           here, and max_width/max_height are conventionally 0 for
           "unchanged" -- unlike SET_SYSTEM_AV_INFO's full reset, a 0 here must
           NOT collapse an already-known max down to base. Non-zero values are
           TRUSTED, not merely floored: a shrink is as legitimate as a grow (a
           Multi Screen title folding down), and the guard belongs to whoever
           sizes a buffer against it. */
        const struct retro_game_geometry *g = data;
        g_active->base_w = (int)g->base_width;
        g_active->base_h = (int)g->base_height;
        /* aspect_ratio is one of this update's fields. Trusted like base, and
           a 0 means what it means at load time: no answer, fall back to
           base_width/base_height. */
        g_active->aspect = g->aspect_ratio;
        if (g->max_width)  g_active->max_w = (int)g->max_width;
        if (g->max_height) g_active->max_h = (int)g->max_height;
        /* INVARIANT: base can never legitimately exceed max, so a core that
           grew base without mentioning a bigger max gets max raised. */
        if (g_active->max_w < g_active->base_w) g_active->max_w = g_active->base_w;
        if (g_active->max_h < g_active->base_h) g_active->max_h = g_active->base_h;
        g_active->geom_dirty = true;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: {
        /* The FULL reset: geometry AND timing, both applied.

           geom_dirty is deliberately the ONE flag for both: a core cannot
           announce new timing without coming through here, and coming through
           always sets it, so main.c re-reads the frame time in the same branch
           it re-reads geometry. A separate timing_dirty would buy a second
           poll per frame to distinguish a case whose only response
           (pacer_set_frame_us, a no-op when unchanged) is already correct. */
        const struct retro_system_av_info *av = data;
        apply_full_geometry(g_active, &av->geometry);
        g_active->fps = av->timing.fps;
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
    /* Latched at poll time like the joypad bits, so every input_state_cb call
       within one retro_run() sees ONE coherent snapshot: the gw core makes
       three separate POINTER queries per frame (X, Y, PRESSED), and reading
       live state on each would let a finger lift between them. */
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
        /* PORT IS DELIBERATELY NOT CHECKED: the Game & Watch core asks on
           port 2 where the convention is port 0, and koboy has exactly ONE
           pointer regardless. Hard-coding 2 would silently return 0 for any
           conventional core; hard-coding 0 would break the one that exists.
           `idx` IS checked -- it is the touch INDEX, and only the first touch
           becomes a pointer. */
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

/* Binds one symbol, writing "core PATH is missing NAME" into err on the first
   failure so a bad core build fails loudly. The PATH is the useful half on a
   device where a photo of the panel is the only diagnostic. */
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

/* Optional: a missing symbol is a CAPABILITY answer, not a fatal error --
   refusing to start because a core cannot serialise would trade playing the
   game for a feature the user did not ask for. */
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

    /* Refuse rather than re-entering retro_load_game on a cartridge state
       that was never torn down. The caller must core_unload_rom first --
       chosen over an implicit auto-unload so switching ROMs is always the
       explicit two-call sequence libretro expects. */
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

    /* A STARTING point, not the final word. retro_get_system_av_info is only
       meaningful once a game is loaded, so this is the EARLIEST CORRECT moment
       -- but not a trustworthy one: gw-libretro answers this exact call with a
       128x128 placeholder on all 59 titles and reports the real canvas later,
       from inside its first retro_run(). That is why env_cb's two geometry
       commands, not just this query, keep base/max current.
       geom_dirty is deliberately NOT set here -- see core_geometry_changed. */
    struct retro_system_av_info av;
    c->get_system_av_info(&av);
    apply_full_geometry(c, &av.geometry);
    c->fps = av.timing.fps;
    c->geom_dirty = false;

    /* The save-RAM LENGTH, captured here and here only -- see core_sram. */
    c->sram_len = c->get_memory_size(RETRO_MEMORY_SAVE_RAM);
    return true;
}

bool core_unload_rom(koboy_core *c)
{
    if (!c || !c->game_loaded) return false;
    c->unload_game();
    c->game_loaded = false;
    /* Cleared with the game: retro_unload_game takes the buffer this length
       described, and the next cartridge's is its own. */
    c->sram_len = 0;
    /* Same reasoning: FBNeo asks for rotation PER GAME, so leaving Galaga's
       quarter turn behind would present the next board sideways -- and Donkey
       Kong Jr., asking for none, never sends a SET_ROTATION to correct it.
       Only core_close reaches this today, so the clear is belt and braces --
       KEPT, because a future caller reloading through one handle must not have
       to rediscover it. */
    c->rot = 0;
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
    /* All three required TOGETHER: a core exporting only some cannot
       round-trip, and a non-zero size would offer a Save that cannot be
       loaded. */
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
    /* THE POINTER IS ASKED FOR EVERY TIME; THE LENGTH IS NOT. The pointer
       belongs to the loaded cartridge and can legitimately move (main.c
       re-fetches it after a state load). The length is the REGION's size,
       fixed when the cartridge is inserted, so it is taken once by
       core_load_rom -- the one moment libretro guarantees it is meaningful.

       NOT tidiness. Genesis Plus GX answers retro_get_memory_size(SAVE_RAM)
       with TWO DIFFERENT THINGS: 0x10000, the buffer's real size, before
       emulation starts, and once running "the index of the highest byte that
       is not 0xFF, plus one". MEASURED across the author's Master System and
       Game Gear collection, that running answer is 285 to 32160 bytes, and 0
       for a cartridge nobody has saved on.

       Re-asking mid-session therefore SHRINKS the length, and a shrunk length
       is a TRUNCATED .srm on the next flush -- a 65536-byte save rewritten at
       8191, unreadable next launch, saving disabled, "your save is corrupt".
       That is docs/FOLLOWUPS.md #3's destructive truncation by another road.

       Growing is no hazard: the pinned length is the region's own size. */
    uint8_t *p = c->get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (len) *len = p ? c->sram_len : 0;
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
    /* TRANSPOSED for an odd rotation, and the swap belongs HERE rather than in
       each caller: every consumer -- the scale search, chrome's reserved rect,
       video_create's buffer sizing, main.c's log line -- wants the picture AS
       PRESENTED, not the buffer the core renders into (Galaga: a 288x224
       buffer presented as 224x288). One consumer forgetting to transpose is a
       rect laid out for the wrong shape.

       The frame callback still reports the core's UN-rotated w/h, deliberately:
       video_pipeline_run is the one place that sees both, and where the
       rotation happens. */
    bool swap = (c->rot & 1u) != 0;
    if (base_w) *base_w = swap ? c->base_h : c->base_w;
    if (base_h) *base_h = swap ? c->base_w : c->base_h;
    if (max_w)  *max_w  = swap ? c->max_h  : c->max_w;
    if (max_h)  *max_h  = swap ? c->max_w  : c->max_h;
    return true;
}

unsigned core_rotation(const koboy_core *c)
{
    return c ? c->rot : 0u;
}

/* Contract in core.h. */
uint32_t core_display_aspect(const koboy_core *c)
{
    if (!c || !c->game_loaded) return KOBOY_ASPECT_ONE;

    double a = (double)c->aspect;
    /* NEGATED range test, as in pacer_frame_us_from_fps: it rejects NaN, which
       `a <= 0.0 || a > 64.0` would let through. The 64 ceiling is not a
       plausibility bound (the widest real content is 4:3) -- it keeps the
       16.16 conversion inside uint32_t. */
    if (a >= (1.0 / 64.0) && a <= 64.0)
        return (uint32_t)(a * 65536.0 + 0.5);

    /* libretro's documented fallback: "if aspect_ratio is <= 0.0, an aspect
       ratio of base_width / base_height is assumed". Exact integer arithmetic,
       from the PRESENTED base so the answer is in the orientation the reported
       float would have been. For every core in reach whose aspect is 0 this
       comes out at exactly KOBOY_ASPECT_ONE -- the fallback must not be the
       thing that moves a picture. */
    int bw = 0, bh = 0;
    core_get_geometry(c, &bw, &bh, NULL, NULL);
    if (bw < 1 || bh < 1) return KOBOY_ASPECT_ONE;
    return (uint32_t)(((uint64_t)bw << 16) / (uint64_t)bh);
}

/* Contract in core.h: RAW, deliberately. Validation is pacing's business. */
double core_fps(const koboy_core *c)
{
    return (c && c->game_loaded) ? c->fps : 0.0;
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
