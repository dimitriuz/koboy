#include "libretro_min.h"
#include <string.h>
#include <stdlib.h>

static retro_environment_t   env_cb;
static retro_video_refresh_t video_cb;
static retro_input_poll_t    poll_cb;
static retro_input_state_t   state_cb;
static retro_audio_sample_batch_t batch_cb;
/* Big enough for every geometry a test poked stub_base_w/h to, including the
   Game & Watch numbers test_core.c exercises (973x532) -- not just the
   Game Boy's 160x144 every OTHER test in this binary still uses by leaving
   stub_base_w/h at their defaults below. */
#define STUB_FB_MAX (1200 * 900)
static uint16_t fb[STUB_FB_MAX];
static uint8_t  sram[8];
int stub_saw_mix_frames_disabled = 0;
int stub_saw_can_dupe = 0;

/* Exported, not static: the test used to inspect these under gdb, which meant
   the assertions they existed for were never actually made. dlsym-able flags
   are real assertions. */
int stub_observed_unload = 0;
int stub_observed_reset  = 0;
int stub_serialize_calls = 0;

/* Geometry retro_get_system_av_info reports, dlsym-poked by test_core.c
   BEFORE a load so it can exercise core_get_geometry against something other
   than the Game Boy's fixed 160x144 -- including the max_width == 0
   "same as base" convention core_load_rom falls back on (src/core.c). The
   160x144 defaults keep every other test (which never touches these) seeing
   exactly what it always has. */
int stub_base_w = 160, stub_base_h = 144;
int stub_max_w  = 160, stub_max_h  = 144;

/* Reproduces the measured Game & Watch core behaviour (see core_get_geometry's
   comment, src/core.h): retro_get_system_av_info answers a placeholder right
   after retro_load_game, and the real geometry (stub_base_w/h/max_w/h above)
   is only announced from inside the first retro_run(), via an environment
   call -- SET_GEOMETRY when this is 1, SET_SYSTEM_AV_INFO when it is 2. 0
   (the default) reports stub_base_w/h/max_w/h immediately, as every other
   test in this binary needs. Reset to 0 by whichever test poked it, once it
   is done -- see the restore comment in test_core.c. */
int stub_late_geometry = 0;
static int stub_late_fired = 0;
#define STUB_PLACEHOLDER_W 128
#define STUB_PLACEHOLDER_H 128

#define STUB_STATE_BYTES 128
static unsigned char stub_state[STUB_STATE_BYTES];

unsigned retro_api_version(void) { return 1; }
void retro_set_environment(retro_environment_t cb)
{
    env_cb = cb;
    bool dupe = false;
    if (env_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &dupe) && dupe) stub_saw_can_dupe = 1;
    struct retro_variable v = { "gambatte_mix_frames", NULL };
    if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &v) && v.value &&
        strcmp(v.value, "disabled") == 0) stub_saw_mix_frames_disabled = 1;
}
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { state_cb = cb; }
void retro_init(void)
{
    enum retro_pixel_format f = RETRO_PIXEL_FORMAT_RGB565;
    env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &f);
}
void retro_deinit(void) {}
void retro_get_system_info(struct retro_system_info *i)
{
    memset(i, 0, sizeof *i);
    i->library_name = "stub"; i->library_version = "1";
    i->valid_extensions = "gb|gbc"; i->need_fullpath = false;
}
void retro_get_system_av_info(struct retro_system_av_info *i)
{
    memset(i, 0, sizeof *i);
    if (stub_late_geometry) {
        /* The placeholder this call answers with on the real core, on every
           title, until the first retro_run() resolves the truth. */
        i->geometry.base_width = i->geometry.max_width  = STUB_PLACEHOLDER_W;
        i->geometry.base_height = i->geometry.max_height = STUB_PLACEHOLDER_H;
    } else {
        i->geometry.base_width  = (unsigned)stub_base_w;
        i->geometry.base_height = (unsigned)stub_base_h;
        i->geometry.max_width   = (unsigned)stub_max_w;
        i->geometry.max_height  = (unsigned)stub_max_h;
    }
    i->timing.fps = 59.7275; i->timing.sample_rate = 32768.0;
}
bool retro_load_game(const struct retro_game_info *g)
{
    if (!g) return false;
    memset(sram, 0, sizeof sram);
    stub_late_fired = 0;   /* each load gets its own chance to announce late */
    return true;
}
void retro_unload_game(void) { stub_observed_unload++; }
void retro_reset(void)       { stub_observed_reset++; }

size_t retro_serialize_size(void) { return STUB_STATE_BYTES; }

bool retro_serialize(void *data, size_t size)
{
    if (size < STUB_STATE_BYTES) return false;
    stub_serialize_calls++;
    memcpy(data, stub_state, STUB_STATE_BYTES);
    return true;
}

bool retro_unserialize(const void *data, size_t size)
{
    if (size < STUB_STATE_BYTES) return false;
    memcpy(stub_state, data, STUB_STATE_BYTES);
    return true;
}
void retro_run(void)
{
    if (poll_cb) poll_cb();
    uint16_t bits = 0;
    for (unsigned id = 0; id < 16; id++)
        if (state_cb && state_cb(0, RETRO_DEVICE_JOYPAD, 0, id)) bits |= (uint16_t)(1u << id);
    fb[0] = bits;
    int16_t silence[64] = {0};
    if (batch_cb) batch_cb(silence, 32);

    /* Fires exactly once per load, from inside retro_run() -- reproducing
       the measured Game & Watch core's timing, not just its final numbers.
       The struct is built from stub_base_w/h/max_w/h, the SAME numbers
       retro_get_system_av_info would have reported immediately if
       stub_late_geometry were 0, so a test comparing the "late" path against
       the "immediate" path is comparing the same target reached two
       different ways. */
    if (stub_late_geometry && !stub_late_fired) {
        stub_late_fired = 1;
        if (stub_late_geometry == 1) {
            struct retro_game_geometry g;
            memset(&g, 0, sizeof g);
            g.base_width  = (unsigned)stub_base_w;  g.base_height = (unsigned)stub_base_h;
            g.max_width   = (unsigned)stub_max_w;   g.max_height  = (unsigned)stub_max_h;
            if (env_cb) env_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &g);
        } else {
            struct retro_system_av_info av;
            memset(&av, 0, sizeof av);
            av.geometry.base_width  = (unsigned)stub_base_w;
            av.geometry.base_height = (unsigned)stub_base_h;
            av.geometry.max_width   = (unsigned)stub_max_w;
            av.geometry.max_height  = (unsigned)stub_max_h;
            av.timing.fps = 59.7275; av.timing.sample_rate = 32768.0;
            if (env_cb) env_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av);
        }
    }

    /* The frame this tick produces is always sized to stub_base_w/h -- the
       CURRENT geometry, whether that came from the immediate query or the
       late announcement just above -- because the frame callback is meant
       to be the reliable source regardless of which path told the frontend
       first (see core_get_geometry's comment). stub_base_w/h defaulting to
       160x144 is what keeps every test that never touches stub_late_geometry
       seeing exactly 160x144 frames, unchanged. */
    if (video_cb)
        video_cb(fb, (unsigned)stub_base_w, (unsigned)stub_base_h,
                (size_t)stub_base_w * sizeof(uint16_t));
}
void  *retro_get_memory_data(unsigned id) { return id == RETRO_MEMORY_SAVE_RAM ? sram : NULL; }
size_t retro_get_memory_size(unsigned id) { return id == RETRO_MEMORY_SAVE_RAM ? sizeof sram : 0; }
