/* corebench.c -- how many microseconds does one retro_run() cost?
 *
 * The question this project keeps having to answer about a new system is not
 * "does the core build" (scripts/verify-core.sh) nor "what geometry does it
 * report" (scripts/probe_core.c) but "does a frame fit in the budget", and
 * until this file existed that was answered by hand each time. It is the
 * deliverable of the Mega Drive / SNES / PC Engine batch, where the whole
 * point was choosing a core by measured speed on a single-core Cortex-A9.
 *
 * THE BUDGET, which is why this tool prints a percentage and not just a mean.
 * koboy runs the core at full rate and presents every present_divisor-th
 * frame, so the per-CORE-frame budget at 100% speed is
 *
 *     1e6 / core_fps  -  (submit + blit + refresh) / present_divisor
 *
 * On the Libra 2 the presentation term is 15-25 ms depending on rect area and
 * the shipped divisor is 3, so roughly 10-12 ms of each 16.7 ms frame is left
 * for emulation. --budget-us states that figure explicitly rather than
 * hardcoding a device: the caller knows which panel it measured on.
 *
 * WHY WALL TIME AND NOT CPU TIME. The device runs this over ssh with Nickel
 * still up, so the number a player experiences includes whatever else the
 * machine is doing. CLOCK_MONOTONIC is what koboy's own src/stats.c times
 * with, so the figures here are directly comparable to a koboy.log `stages`
 * line -- which is the only reason a host figure can be scaled into a device
 * one at all.
 *
 * WHY A WARMUP. Every core measured here is slowest in its first frames: ROM
 * decompression, lazily-built tables, and on the SNES cores a cold APU. The
 * first frame of snes9x2005 on Super Mario World measured 8x the median.
 * Those frames are real but they are not the steady state a playability
 * judgement is about, so they are run and discarded, and --warmup makes the
 * discard visible instead of silent.
 *
 * WHY THE MAX MATTERS MORE THAN THE MEAN, and why p95 is printed between
 * them: a mean inside budget with a max at 3x budget is a game that hitches
 * at exactly the moments a player notices (a screen transition, a boss
 * spawning). But a single max is also the one figure a stray scheduler
 * decision can invent, which is what p95 is for -- if p95 is near the mean
 * and only max is high, the spike is one frame and probably not the core's.
 *
 * GEOMETRY CHANGES ARE COUNTED, not just reported, because PC Engine is in
 * this batch: it switches horizontal resolution mid-game, and koboy re-fits
 * on each change. MEASURED rather than taken from the folklore, which says
 * 256/336/512: this core reports 256 and 352, and Military Madness alternates
 * between them five times in 2500 frames (256 for its title and transitions,
 * 352 for the map). A title that never changes width and one that changes
 * every scene are different tests of the front-end, and "how many times, and
 * to what" is the only way to tell them apart from out here.
 *
 * Usage: corebench [options] <core.so> <content> [<content> ...]
 *   --frames N     frames to measure   (default 600)
 *   --warmup N     frames to discard   (default 60)
 *   --budget-us N  per-frame budget for the % column; 0 uses the core's own
 *                  reported fps, i.e. 100% means "real time with no
 *                  presentation cost at all" (default 0)
 *   --csv          one machine-readable line per title, for a report table
 *
 * Build:  cc -O2 -o corebench scripts/corebench.c -ldl
 * Cross:  arm-linux-gnueabihf-gcc -std=c11 -O2 -march=armv7-a -mfpu=neon \
 *             -mfloat-abi=hard -o corebench-arm scripts/corebench.c -ldl
 *
 * -std=c11 IS REQUIRED FOR THE CROSS BUILD and is not decoration: Linaro
 * 4.9.2 defaults to gnu89, which rejects a declaration inside a `for`. The
 * host compiler defaults to something newer and builds this file happily
 * without it, so the flag is exactly the kind of thing that looks redundant
 * until the day it is not.
 *
 * The libretro struct layouts below are transcribed from upstream libretro.h
 * (MIT-licensed) and match the subset scripts/probe_core.c already binds.
 * Deliberately independent of koboy's own src/core.c: a measurement tool that
 * shares code with the thing being measured cannot be used to check it.
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
#include <time.h>

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

#define RETRO_MEMORY_SAVE_RAM 0

enum retro_pixel_format {
    RETRO_PIXEL_FORMAT_0RGB1555 = 0,
    RETRO_PIXEL_FORMAT_XRGB8888 = 1,
    RETRO_PIXEL_FORMAT_RGB565   = 2,
    RETRO_PIXEL_FORMAT_UNKNOWN  = INT_MAX
};

struct retro_game_info { const char *path; const void *data; size_t size; const char *meta; };
struct retro_system_info {
    const char *library_name, *library_version, *valid_extensions;
    int need_fullpath, block_extract;
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

/* ---- state ---- */
static enum retro_pixel_format g_fmt = RETRO_PIXEL_FORMAT_UNKNOWN;
static int g_geom_calls = 0;
static struct retro_game_geometry g_last_geom;
static unsigned g_frame_w = 0, g_frame_h = 0;
static int g_frames_with_pixels = 0;
/* Distinct base widths seen, for the PC Engine resolution-switch case. Eight
   is more than any real system uses; the counter saturates rather than
   overflowing, and the printed count says how many were RECORDED. */
#define MAX_WIDTHS 8
static unsigned g_widths[MAX_WIDTHS];
static int g_nwidths = 0;

static void note_width(unsigned w)
{
    if (!w) return;
    for (int i = 0; i < g_nwidths; i++) if (g_widths[i] == w) return;
    if (g_nwidths < MAX_WIDTHS) g_widths[g_nwidths++] = w;
}

static int env_cb(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        g_fmt = *(const enum retro_pixel_format *)data; return 1;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
        *(int *)data = 1; return 1;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = "."; return 1;
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        g_last_geom = *(const struct retro_game_geometry *)data;
        note_width(g_last_geom.base_width);
        g_geom_calls++; return 1;
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: {
        const struct retro_system_av_info *avi = data;
        g_last_geom = avi->geometry;
        note_width(g_last_geom.base_width);
        g_geom_calls++; return 1;
    }
    /* Refused, exactly as koboy's own src/core.c refuses them. A core that
       behaves differently when the frontend answers these would be measured
       in a configuration the device never runs -- and one of them (the log
       interface) is what makes smsplus-gx segfault, so answering generously
       here would hide a crash rather than find it. */
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
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

/* The frame is looked at but NOT converted: this measures the CORE, and
   adding koboy's pixel pipeline here would measure video_submit instead --
   the stage that is already known to dominate. Counting frames that carried
   pixels is the cheap check that the core is really producing output and not
   returning early from a black screen, which is what a "fast" broken core
   looks like. */
static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
    (void)pitch;
    if (data) g_frames_with_pixels++;
    if (w) { g_frame_w = w; note_width(w); }
    if (h) g_frame_h = h;
}
static void audio_sample_cb(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_batch_cb(const int16_t *d, size_t n) { (void)d; return n; }
static void poll_cb(void) {}
static int16_t state_cb(unsigned p, unsigned d, unsigned i, unsigned id)
{ (void)p; (void)d; (void)i; (void)id; return 0; }

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)(ts.tv_nsec / 1000);
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static const char *fmt_name(enum retro_pixel_format f)
{
    switch (f) {
    case RETRO_PIXEL_FORMAT_0RGB1555: return "0RGB1555";
    case RETRO_PIXEL_FORMAT_XRGB8888: return "XRGB8888";
    case RETRO_PIXEL_FORMAT_RGB565:   return "RGB565";
    default: return "unset";
    }
}

/* The last path component, so a CSV row carries a title rather than a
   collection's directory tree. */
static const char *basename_of(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

int main(int argc, char **argv)
{
    int frames = 600, warmup = 60, csv = 0;
    long budget_us = 0;
    int argi = 1;

    for (; argi < argc; argi++) {
        if (!strcmp(argv[argi], "--frames") && argi + 1 < argc) frames = atoi(argv[++argi]);
        else if (!strcmp(argv[argi], "--warmup") && argi + 1 < argc) warmup = atoi(argv[++argi]);
        else if (!strcmp(argv[argi], "--budget-us") && argi + 1 < argc) budget_us = atol(argv[++argi]);
        else if (!strcmp(argv[argi], "--csv")) csv = 1;
        else break;
    }
    if (argc - argi < 2) {
        fprintf(stderr,
            "usage: %s [--frames N] [--warmup N] [--budget-us N] [--csv]"
            " <core.so> <content> [<content> ...]\n", argv[0]);
        return 2;
    }
    /* LIVE CLAMP: a zero or negative --frames would malloc(0) and then divide
       by zero computing the mean. Callers pass this from shell loops. */
    if (frames < 1) frames = 1;
    if (warmup < 0) warmup = 0;

    const char *so_path = argv[argi++];

    uint64_t *samples = malloc((size_t)frames * sizeof *samples);
    if (!samples) { fprintf(stderr, "out of memory for %d samples\n", frames); return 1; }

    if (csv)
        printf("title,core,base_w,base_h,fps,widths,geom_calls,"
               "mean_us,p50_us,p95_us,max_us,pct_of_budget\n");

    int any_failed = 0;

    for (; argi < argc; argi++) {
        const char *content_path = argv[argi];

        /* Fresh dlopen per title, for probe_core.c's reason: these cores keep
           state in statics and a stale table from the previous title would
           show up as a speed difference that is not real. */
        void *so = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
        if (!so) { fprintf(stderr, "dlopen %s: %s\n", so_path, dlerror()); free(samples); return 1; }

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
        size_t (*p_get_memory_size)(unsigned) = dlsym(so, "retro_get_memory_size");

        if (!p_set_environment || !p_get_system_info || !p_get_system_av_info ||
            !p_load_game || !p_run) {
            fprintf(stderr, "core %s missing required symbols\n", so_path);
            dlclose(so); free(samples); return 1;
        }

        g_fmt = RETRO_PIXEL_FORMAT_UNKNOWN;
        g_geom_calls = 0; g_nwidths = 0; g_frames_with_pixels = 0;
        g_frame_w = g_frame_h = 0;
        memset(&g_last_geom, 0, sizeof g_last_geom);

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

        struct stat st;
        if (stat(content_path, &st) != 0) {
            fprintf(stderr, "%s: cannot stat, skipped\n", content_path);
            if (p_deinit) p_deinit();
            dlclose(so); any_failed = 1; continue;
        }

        struct retro_game_info gi;
        memset(&gi, 0, sizeof gi);
        gi.path = content_path;
        void *buf = NULL;
        if (!sysinfo.need_fullpath) {
            buf = malloc((size_t)st.st_size);
            FILE *f = fopen(content_path, "rb");
            if (!f || !buf || fread(buf, 1, (size_t)st.st_size, f) != (size_t)st.st_size) {
                fprintf(stderr, "%s: read failed, skipped\n", content_path);
                if (f) fclose(f);
                free(buf);
                if (p_deinit) p_deinit();
                dlclose(so); any_failed = 1; continue;
            }
            fclose(f);
            gi.data = buf;
            gi.size = (size_t)st.st_size;
        }

        if (!p_load_game(&gi)) {
            fprintf(stderr, "%s: core rejected rom\n", content_path);
            free(buf);
            if (p_deinit) p_deinit();
            dlclose(so); any_failed = 1; continue;
        }

        struct retro_system_av_info avi;
        memset(&avi, 0, sizeof avi);
        p_get_system_av_info(&avi);
        note_width(avi.geometry.base_width);

        /* Save-RAM size at LOAD time, which is the moment koboy's core_sram
           pins it (src/core.c, commit 1fb3802 -- GPGX reports a SHRINKING
           size once running). Printed so a new system's save behaviour is
           established here rather than discovered on the device. */
        size_t sram_at_load = p_get_memory_size ? p_get_memory_size(RETRO_MEMORY_SAVE_RAM) : 0;

        for (int f = 0; f < warmup; f++) p_run();
        size_t sram_running = p_get_memory_size ? p_get_memory_size(RETRO_MEMORY_SAVE_RAM) : 0;

        for (int f = 0; f < frames; f++) {
            uint64_t t0 = now_us();
            p_run();
            samples[f] = now_us() - t0;
        }

        uint64_t total = 0, max = 0;
        for (int f = 0; f < frames; f++) { total += samples[f]; if (samples[f] > max) max = samples[f]; }
        double mean = (double)total / frames;
        qsort(samples, (size_t)frames, sizeof *samples, cmp_u64);
        uint64_t p50 = samples[frames / 2];
        /* p95 index is clamped because frames can be as low as 1. */
        int i95 = (int)((double)frames * 0.95);
        if (i95 >= frames) i95 = frames - 1;
        uint64_t p95 = samples[i95];

        double fps = avi.timing.fps > 0 ? avi.timing.fps : 60.0;
        double budget = budget_us > 0 ? (double)budget_us : 1e6 / fps;
        /* Percentage of full speed: how much of the budget a frame leaves.
           Over budget means under 100%, so it is budget/cost, not cost/budget. */
        double pct = mean > 0 ? 100.0 * budget / mean : 999.9;
        if (pct > 100.0) pct = 100.0;   /* a core faster than real time still runs at 100% */

        char wbuf[64]; wbuf[0] = 0;
        for (int i = 0; i < g_nwidths; i++) {
            char one[16];
            snprintf(one, sizeof one, "%s%u", i ? "|" : "", g_widths[i]);
            strncat(wbuf, one, sizeof wbuf - strlen(wbuf) - 1);
        }

        if (csv) {
            printf("\"%s\",\"%s\",%u,%u,%.4f,\"%s\",%d,%.1f,%llu,%llu,%llu,%.1f\n",
                   basename_of(content_path),
                   sysinfo.library_name ? sysinfo.library_name : "?",
                   avi.geometry.base_width, avi.geometry.base_height, fps,
                   wbuf, g_geom_calls, mean,
                   (unsigned long long)p50, (unsigned long long)p95,
                   (unsigned long long)max, pct);
        } else {
            printf("=== %s\n", basename_of(content_path));
            printf("  core           %s %s, %s\n",
                   sysinfo.library_name ? sysinfo.library_name : "?",
                   sysinfo.library_version ? sysinfo.library_version : "?",
                   fmt_name(g_fmt));
            printf("  geometry       %ux%u base, %ux%u max, aspect %.4f, %.4f fps\n",
                   avi.geometry.base_width, avi.geometry.base_height,
                   avi.geometry.max_width, avi.geometry.max_height,
                   (double)avi.geometry.aspect_ratio, fps);
            printf("  widths seen    %s (%d SET_GEOMETRY/SET_SYSTEM_AV_INFO calls)\n",
                   wbuf[0] ? wbuf : "-", g_geom_calls);
            printf("  save ram       %zu bytes at load, %zu running%s\n",
                   sram_at_load, sram_running,
                   sram_at_load != sram_running ? "   <-- SIZE MOVES, pin it at load" : "");
            printf("  frames         %d measured, %d warmup, %d carried pixels\n",
                   frames, warmup, g_frames_with_pixels);
            printf("  core us/frame  mean %.1f  p50 %llu  p95 %llu  max %llu\n",
                   mean, (unsigned long long)p50, (unsigned long long)p95,
                   (unsigned long long)max);
            printf("  budget %.0f us -> %.1f%% of full speed (worst frame %.1f%%)\n",
                   budget, pct, max > 0 ? (100.0 * budget / (double)max > 100.0
                                           ? 100.0 : 100.0 * budget / (double)max) : 100.0);
            printf("\n");
        }

        if (p_unload_game) p_unload_game();
        free(buf);
        if (p_deinit) p_deinit();
        dlclose(so);
    }

    free(samples);
    return any_failed ? 1 : 0;
}
