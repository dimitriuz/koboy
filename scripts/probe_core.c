/* probe_core.c — dlopens a libretro core directly (no koboy involved) and
 * reports what it says about itself and a piece of content: retro_get_
 * system_info, retro_get_system_av_info (before and after the first
 * retro_run(), since some cores only resolve real geometry once content has
 * actually been interpreted — see the Game & Watch core, whose placeholder
 * is 128x128 until the first frame), the pixel format it requests, and
 * whether it calls SET_GEOMETRY / SET_SYSTEM_AV_INFO at runtime.
 *
 * This is a standalone measurement tool, deliberately independent of
 * koboy's own src/core.c (which is off limits while a second task is
 * mid-edit there) -- the struct layouts below are transcribed from
 * third_party/gw/src/libretro.h (upstream libretro.h, MIT-licensed) and
 * match the subset koboy's own src/libretro_min.h binds.
 *
 * Usage: probe_core <core.so> <content-file> [<content-file> ...]
 * Build:  cc -O2 -o probe_core scripts/probe_core.c -ldl
 */
#define _POSIX_C_SOURCE 200809L
#include <dlfcn.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <limits.h>

/* ---- minimal libretro ABI subset (values verified against
 * third_party/gw/src/libretro.h) ---- */

#define RETRO_ENVIRONMENT_EXPERIMENTAL 0x10000
#define RETRO_ENVIRONMENT_GET_CAN_DUPE            3
#define RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY    9
#define RETRO_ENVIRONMENT_SET_PIXEL_FORMAT        10
#define RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS   11
#define RETRO_ENVIRONMENT_GET_VARIABLE            15
#define RETRO_ENVIRONMENT_SET_VARIABLES           16
#define RETRO_ENVIRONMENT_GET_LOG_INTERFACE       27
#define RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY      31
#define RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO      32
#define RETRO_ENVIRONMENT_SET_CONTROLLER_INFO     35
#define RETRO_ENVIRONMENT_SET_GEOMETRY            37
#define RETRO_ENVIRONMENT_GET_INPUT_BITMASKS      (51 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE 65
#define RETRO_ENVIRONMENT_GET_GAME_INFO_EXT        66
#define RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE   (47 | RETRO_ENVIRONMENT_EXPERIMENTAL)

#define RETRO_DEVICE_JOYPAD 1

enum retro_pixel_format {
    RETRO_PIXEL_FORMAT_0RGB1555 = 0,
    RETRO_PIXEL_FORMAT_XRGB8888 = 1,
    RETRO_PIXEL_FORMAT_RGB565   = 2,
    RETRO_PIXEL_FORMAT_UNKNOWN  = INT_MAX
};

struct retro_game_info {
    const char *path;
    const void *data;
    size_t      size;
    const char *meta;
};

struct retro_system_info {
    const char *library_name;
    const char *library_version;
    const char *valid_extensions;
    int         need_fullpath;   /* bool */
    int         block_extract;   /* bool */
};

struct retro_game_geometry {
    unsigned base_width, base_height, max_width, max_height;
    float aspect_ratio;
};
struct retro_system_timing { double fps, sample_rate; };
struct retro_system_av_info {
    struct retro_game_geometry geometry;
    struct retro_system_timing timing;
};

typedef int  (*retro_environment_t)(unsigned, void *);
typedef void (*retro_video_refresh_t)(const void *, unsigned, unsigned, size_t);
typedef void (*retro_audio_sample_t)(int16_t, int16_t);
typedef size_t (*retro_audio_sample_batch_t)(const int16_t *, size_t);
typedef void (*retro_input_poll_t)(void);
typedef int16_t (*retro_input_state_t)(unsigned, unsigned, unsigned, unsigned);

/* ---- probe state ---- */

static enum retro_pixel_format g_fmt = RETRO_PIXEL_FORMAT_UNKNOWN;
static int g_fmt_seen = 0;
static int g_geom_set_calls = 0;
static struct retro_game_geometry g_last_geom;
static struct retro_system_timing g_last_timing;
static int g_asked_system_dir = 0, g_asked_save_dir = 0;
static int g_frames_run = 0;
static int g_frame_seen = 0;
static unsigned g_frame_w = 0, g_frame_h = 0;
static size_t g_frame_pitch = 0;
static unsigned long g_frame_nonzero16 = 0; /* count of non-zero RGB565 pixels in the last frame */

static int env_cb(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        g_fmt = *(const enum retro_pixel_format *)data;
        g_fmt_seen = 1;
        return 1;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(int *)data = 1;
        return 1;
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
        *(int *)data = 1;
        return 1;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        g_asked_system_dir = 1;
        *(const char **)data = ".";
        return 1;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        g_asked_save_dir = 1;
        *(const char **)data = ".";
        return 1;
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        g_last_geom = *(const struct retro_game_geometry *)data;
        g_geom_set_calls++;
        return 1;
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: {
        const struct retro_system_av_info *avi = data;
        g_last_geom = avi->geometry;
        g_last_timing = avi->timing;
        g_geom_set_calls++;
        return 1;
    }
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        return 0; /* not supported -- exercises the per-button poll path */
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
    case RETRO_ENVIRONMENT_GET_VARIABLE:
    case RETRO_ENVIRONMENT_GET_GAME_INFO_EXT:
        return 0;
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        return 1;
    default:
        return 0;
    }
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
    g_frame_seen = data != NULL || g_frame_seen; /* NULL = "unchanged", still a frame */
    g_frame_w = w; g_frame_h = h; g_frame_pitch = pitch;
    /* Sanity check that the frame isn't degenerate (all zero) -- assumes
     * RGB565 (2 bytes/pixel), which is what this core requests. */
    if (data) {
        unsigned long nz = 0;
        const uint8_t *row = data;
        for (unsigned y = 0; y < h; y++) {
            const uint16_t *px = (const uint16_t *)(row + y * pitch);
            for (unsigned x = 0; x < w; x++) if (px[x] != 0) nz++;
        }
        g_frame_nonzero16 = nz;
    }
}
static void audio_sample_cb(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_batch_cb(const int16_t *d, size_t n) { (void)d; return n; }
static void poll_cb(void) {}
static int16_t state_cb(unsigned port, unsigned dev, unsigned idx, unsigned id)
{ (void)port; (void)dev; (void)idx; (void)id; return 0; }

static const char *fmt_name(enum retro_pixel_format f)
{
    switch (f) {
    case RETRO_PIXEL_FORMAT_0RGB1555: return "0RGB1555";
    case RETRO_PIXEL_FORMAT_XRGB8888: return "XRGB8888";
    case RETRO_PIXEL_FORMAT_RGB565:   return "RGB565";
    default: return "UNKNOWN/never-set";
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <core.so> <content> [<content> ...]\n", argv[0]);
        return 2;
    }
    const char *so_path = argv[1];

    for (int ci = 2; ci < argc; ci++) {
        const char *content_path = argv[ci];

        /* Fresh dlopen per title: this core (like most) keeps state in
         * statics, so reusing one handle across loads risks carrying stale
         * state between titles -- a fresh process-local handle per title
         * is the conservative choice for a measurement tool. */
        void *so = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
        if (!so) { fprintf(stderr, "dlopen %s: %s\n", so_path, dlerror()); return 1; }

        void (*p_set_environment)(retro_environment_t) = dlsym(so, "retro_set_environment");
        void (*p_set_video_refresh)(retro_video_refresh_t) = dlsym(so, "retro_set_video_refresh");
        void (*p_set_audio_sample)(retro_audio_sample_t) = dlsym(so, "retro_set_audio_sample");
        void (*p_set_audio_sample_batch)(retro_audio_sample_batch_t) = dlsym(so, "retro_set_audio_sample_batch");
        void (*p_set_input_poll)(retro_input_poll_t) = dlsym(so, "retro_set_input_poll");
        void (*p_set_input_state)(retro_input_state_t) = dlsym(so, "retro_set_input_state");
        void (*p_init)(void) = dlsym(so, "retro_init");
        void (*p_deinit)(void) = dlsym(so, "retro_deinit");
        void (*p_get_system_info)(struct retro_system_info *) = dlsym(so, "retro_get_system_info");
        void (*p_get_system_av_info)(struct retro_system_av_info *) = dlsym(so, "retro_get_system_av_info");
        int  (*p_load_game)(const struct retro_game_info *) = dlsym(so, "retro_load_game");
        void (*p_unload_game)(void) = dlsym(so, "retro_unload_game");
        void (*p_run)(void) = dlsym(so, "retro_run");
        unsigned (*p_api_version)(void) = dlsym(so, "retro_api_version");

        if (!p_set_environment || !p_get_system_info || !p_get_system_av_info ||
            !p_load_game || !p_run || !p_api_version) {
            fprintf(stderr, "core %s missing required symbols\n", so_path);
            dlclose(so);
            return 1;
        }

        g_fmt = RETRO_PIXEL_FORMAT_UNKNOWN; g_fmt_seen = 0;
        g_geom_set_calls = 0;
        g_asked_system_dir = 0; g_asked_save_dir = 0;
        g_frame_seen = 0; g_frame_w = g_frame_h = 0; g_frame_pitch = 0; g_frame_nonzero16 = 0;
        memset(&g_last_geom, 0, sizeof g_last_geom);
        memset(&g_last_timing, 0, sizeof g_last_timing);

        printf("=== %s ===\n", content_path);
        printf("api_version: %u\n", p_api_version());

        p_set_environment(env_cb);
        if (p_set_video_refresh) p_set_video_refresh(video_cb);
        if (p_set_audio_sample) p_set_audio_sample(audio_sample_cb);
        if (p_set_audio_sample_batch) p_set_audio_sample_batch(audio_batch_cb);
        if (p_set_input_poll) p_set_input_poll(poll_cb);
        if (p_set_input_state) p_set_input_state(state_cb);
        if (p_init) p_init();

        struct retro_system_info sysinfo;
        memset(&sysinfo, 0, sizeof sysinfo);
        p_get_system_info(&sysinfo);
        printf("library_name: %s\n", sysinfo.library_name ? sysinfo.library_name : "(null)");
        printf("library_version: %s\n", sysinfo.library_version ? sysinfo.library_version : "(null)");
        printf("valid_extensions: %s\n", sysinfo.valid_extensions ? sysinfo.valid_extensions : "(null)");
        printf("need_fullpath: %s\n", sysinfo.need_fullpath ? "true" : "false");

        struct stat st;
        if (stat(content_path, &st) != 0) {
            fprintf(stderr, "stat %s: file missing, skipping load\n", content_path);
            if (p_deinit) p_deinit();
            dlclose(so);
            continue;
        }

        struct retro_game_info gi;
        memset(&gi, 0, sizeof gi);
        gi.path = content_path;
        void *buf = NULL;
        if (sysinfo.need_fullpath) {
            /* core reads the path itself */
        } else {
            buf = malloc((size_t)st.st_size);
            FILE *f = fopen(content_path, "rb");
            if (!f || !buf || fread(buf, 1, (size_t)st.st_size, f) != (size_t)st.st_size) {
                fprintf(stderr, "read %s failed\n", content_path);
                if (f) fclose(f);
                free(buf);
                if (p_deinit) p_deinit();
                dlclose(so);
                continue;
            }
            fclose(f);
            gi.data = buf;
            gi.size = (size_t)st.st_size;
        }

        int loaded = p_load_game(&gi);
        printf("load_game: %s\n", loaded ? "ok" : "FAILED");

        if (loaded) {
            struct retro_system_av_info avi;
            memset(&avi, 0, sizeof avi);
            p_get_system_av_info(&avi);
            printf("av_info BEFORE first retro_run: base=%ux%u max=%ux%u aspect=%g fps=%g sample_rate=%g\n",
                   avi.geometry.base_width, avi.geometry.base_height,
                   avi.geometry.max_width, avi.geometry.max_height,
                   (double)avi.geometry.aspect_ratio, avi.timing.fps, avi.timing.sample_rate);
            printf("pixel_format requested: %s\n", fmt_name(g_fmt));
            printf("asked for system_directory: %s, save_directory: %s\n",
                   g_asked_system_dir ? "yes" : "no", g_asked_save_dir ? "yes" : "no");

            /* Run a handful of frames: gw-libretro's own retro_run only
             * resolves real geometry (via gwlua_create + SET_GEOMETRY /
             * SET_SYSTEM_AV_INFO) on the FIRST call, so one frame should be
             * enough, but run a few to see if it moves again afterwards. */
            for (int f = 0; f < 5; f++) {
                p_run();
                g_frames_run++;
            }

            p_get_system_av_info(&avi);
            printf("av_info AFTER %d retro_run() calls: base=%ux%u max=%ux%u aspect=%g fps=%g sample_rate=%g\n",
                   g_frames_run, avi.geometry.base_width, avi.geometry.base_height,
                   avi.geometry.max_width, avi.geometry.max_height,
                   (double)avi.geometry.aspect_ratio, avi.timing.fps, avi.timing.sample_rate);
            printf("SET_GEOMETRY/SET_SYSTEM_AV_INFO env calls seen: %d (last: %ux%u)\n",
                   g_geom_set_calls, g_last_geom.base_width, g_last_geom.base_height);
            printf("video_refresh called: %s, last frame %ux%u pitch=%zu, non-zero px=%lu/%lu\n",
                   g_frame_seen ? "yes" : "no", g_frame_w, g_frame_h, g_frame_pitch,
                   g_frame_nonzero16, (unsigned long)g_frame_w * g_frame_h);

            if (p_unload_game) p_unload_game();
        }

        free(buf);
        if (p_deinit) p_deinit();
        dlclose(so);
        printf("\n");
    }

    return 0;
}
