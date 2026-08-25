#include "libretro_min.h"
#include <string.h>
#include <stdlib.h>

static retro_environment_t   env_cb;
static retro_video_refresh_t video_cb;
static retro_input_poll_t    poll_cb;
static retro_input_state_t   state_cb;
static retro_audio_sample_batch_t batch_cb;
static uint16_t fb[160 * 144];
static uint8_t  sram[8];
int stub_saw_mix_frames_disabled = 0;
int stub_saw_can_dupe = 0;

/* Exported, not static: the test used to inspect these under gdb, which meant
   the assertions they existed for were never actually made. dlsym-able flags
   are real assertions. */
int stub_observed_unload = 0;
int stub_observed_reset  = 0;
int stub_serialize_calls = 0;

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
    i->geometry.base_width = 160; i->geometry.base_height = 144;
    i->geometry.max_width = 160;  i->geometry.max_height = 144;
    i->timing.fps = 59.7275; i->timing.sample_rate = 32768.0;
}
bool retro_load_game(const struct retro_game_info *g)
{
    if (!g) return false;
    memset(sram, 0, sizeof sram);
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
    if (video_cb) video_cb(fb, 160, 144, 160 * sizeof(uint16_t));
}
void  *retro_get_memory_data(unsigned id) { return id == RETRO_MEMORY_SAVE_RAM ? sram : NULL; }
size_t retro_get_memory_size(unsigned id) { return id == RETRO_MEMORY_SAVE_RAM ? sizeof sram : 0; }
