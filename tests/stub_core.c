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
/* What koboy answered when asked for the two Pokemon Mini options core.c
   overrides. Recorded as the ANSWER, not as a bool, so a test can assert the
   exact string: "koboy replied something" would pass against a frontend that
   handed back the core's own default and undo the whole point of the
   override (a 4x internally-upscaled, dot-matrix-filtered, 83%-black panel
   -- see core.c). Empty means the key was refused. */
char stub_pm_video_scale[32] = "";
char stub_pm_palette[64] = "";
int stub_saw_can_dupe = 0;
int stub_unknown_option_refused = 0;

/* What koboy answered for the two directory queries, and whether it answered
   at all. Recorded as the STRINGS for the same reason the option answers
   above are: a bool would pass against a frontend that returned true and left
   the pointer alone.

   This is not decoration. Neo Geo Pocket cartridges save into FLASH, not
   into RETRO_MEMORY_SAVE_RAM -- measured: retro_get_memory_size(SAVE_RAM) is
   0 for every one of the author's ten .ngp titles, on BOTH available cores --
   and RACE writes that flash itself, as "<rom>.ngf" in whatever directory the
   frontend answers GET_SAVE_DIRECTORY with. So for that whole system this
   answer IS the save path, and a frontend that refused the query would lose
   every save while looking exactly like one that worked. */
char stub_save_dir[256] = "";
char stub_system_dir[256] = "";
int  stub_save_dir_answered = 0;
int  stub_system_dir_answered = 0;

/* Exported, not static: the test used to inspect these under gdb, which meant
   the assertions they existed for were never actually made. dlsym-able flags
   are real assertions. */
int stub_observed_unload = 0;
int stub_observed_reset  = 0;
int stub_serialize_calls = 0;

/* What retro_run() got back when it asked for a POINTER, so test_core.c can
   assert what a core actually SEES rather than what koboy believes it sent.
   The Game & Watch core queries port 2 (third_party/gw/src/libretro.c) where
   the libretro convention is port 0, so both are polled here and recorded
   separately -- a core.c that hard-coded either port would answer one and
   return 0 for the other, and only a stub that asks both can tell.
   stub_ptr_idx1_x polls touch index 1, which koboy never reports: it must
   come back 0 however the pointer is placed. */
int stub_ptr_x = -1, stub_ptr_y = -1, stub_ptr_pressed = -1;
int stub_ptr_port0_x = -1, stub_ptr_port0_pressed = -1;
int stub_ptr_idx1_x = -1;
int stub_ptr_count = -1;

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

/* Base-only geometry churn, driven by the environment rather than by a global
   because the test that needs it (tests/smoke_host.sh) runs koboy as a
   SEPARATE PROCESS and cannot poke this binary's globals.

   KOBOY_STUB_OSCILLATE=1 alternates base between max and half of max every 10
   frames, leaving max alone -- the measured Game & Watch behaviour, where a
   title flips between showing the whole unit and the LCD alone several times
   a second. KOBOY_STUB_MAXGROW=1 instead announces a LARGER max once, at
   frame 30. The pair exists so a test can assert both directions: base churn
   must NOT provoke a re-fit, a max change MUST. Asserting only the first
   would pass against a frontend that ignored geometry entirely. */
static int stub_osc = -1, stub_maxgrow = -1, stub_tick = 0;
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

    /* The real PokeMini core reads these in retro_load_game; the stub asks
       here because that is where it already asks for its other variable, and
       WHERE koboy is asked has never been the question -- WHAT it answers is.
       Asked with the same struct shape and the same key strings the real core
       uses (third_party/pokemini/libretro/libretro.c). */
    struct retro_variable vs = { "pokemini_video_scale", NULL };
    if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &vs) && vs.value) {
        strncpy(stub_pm_video_scale, vs.value, sizeof stub_pm_video_scale - 1);
        stub_pm_video_scale[sizeof stub_pm_video_scale - 1] = 0;
    }
    struct retro_variable vp = { "pokemini_palette", NULL };
    if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &vp) && vp.value) {
        strncpy(stub_pm_palette, vp.value, sizeof stub_pm_palette - 1);
        stub_pm_palette[sizeof stub_pm_palette - 1] = 0;
    }

    /* The directory queries, asked exactly the way the real cores ask them.
       RACE asks in retro_init and beetle-ngp in retro_load_game; WHERE has
       never been the question, WHAT koboy answers is. */
    const char *sd = NULL;
    if (env_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &sd) && sd) {
        stub_save_dir_answered = 1;
        strncpy(stub_save_dir, sd, sizeof stub_save_dir - 1);
        stub_save_dir[sizeof stub_save_dir - 1] = 0;
    }
    const char *yd = NULL;
    if (env_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &yd) && yd) {
        stub_system_dir_answered = 1;
        strncpy(stub_system_dir, yd, sizeof stub_system_dir - 1);
        stub_system_dir[sizeof stub_system_dir - 1] = 0;
    }

    /* A key koboy has no opinion about must come back REFUSED, not
       answered: env_cb returning true with a stale v.value is how a core
       ends up acting on whatever happened to be in the struct. */
    struct retro_variable vu = { "stub_unknown_option", (const char *)0x1 };
    stub_unknown_option_refused = !env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &vu)
                                 && vu.value == NULL;
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
    /* A RECOGNISABLE SAVE-RAM SIGNATURE, written every frame. The size stays
       8 bytes (test_core.c pins that), but the CONTENT stops being all-zero,
       and that is what lets tests/smoke_host.sh assert a battery save really
       travelled core -> koboy -> disk: a .srm of eight zero bytes is
       indistinguishable from a file someone created with `: >`, whereas
       A0..A7 can only have come from here. retro_load_game still zeroes it,
       so "loaded, never run" remains distinguishable from "ran". */
    for (unsigned i = 0; i < sizeof sram; i++) sram[i] = (uint8_t)(0xA0 + i);
    uint16_t bits = 0;
    for (unsigned id = 0; id < 16; id++)
        if (state_cb && state_cb(0, RETRO_DEVICE_JOYPAD, 0, id)) bits |= (uint16_t)(1u << id);
    fb[0] = bits;
    if (state_cb) {
        stub_ptr_x       = state_cb(2, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
        stub_ptr_y       = state_cb(2, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
        stub_ptr_pressed = state_cb(2, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED);
        stub_ptr_count   = state_cb(2, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_COUNT);
        stub_ptr_port0_x       = state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
        stub_ptr_port0_pressed = state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED);
        stub_ptr_idx1_x  = state_cb(2, RETRO_DEVICE_POINTER, 1, RETRO_DEVICE_ID_POINTER_X);
    }
    int16_t silence[64] = {0};
    if (batch_cb) batch_cb(silence, 32);

    /* Fires exactly once per load, from inside retro_run() -- reproducing
       the measured Game & Watch core's timing, not just its final numbers.
       The struct is built from stub_base_w/h/max_w/h, the SAME numbers
       retro_get_system_av_info would have reported immediately if
       stub_late_geometry were 0, so a test comparing the "late" path against
       the "immediate" path is comparing the same target reached two
       different ways. */
    if (stub_osc < 0) {
        const char *e = getenv("KOBOY_STUB_OSCILLATE");
        stub_osc = (e && *e && *e != '0') ? 1 : 0;
        e = getenv("KOBOY_STUB_MAXGROW");
        stub_maxgrow = (e && *e && *e != '0') ? 1 : 0;
    }
    stub_tick++;
    if (stub_osc && stub_tick % 10 == 0) {
        /* Half of max, floored at 1, alternating with max itself. max is
           deliberately NOT touched. */
        int half_w = stub_max_w / 2, half_h = stub_max_h / 2;
        if (half_w < 1) half_w = 1;
        if (half_h < 1) half_h = 1;
        int want_w = (stub_base_w == stub_max_w) ? half_w : stub_max_w;
        int want_h = (stub_base_h == stub_max_h) ? half_h : stub_max_h;
        stub_base_w = want_w; stub_base_h = want_h;
        struct retro_game_geometry g;
        memset(&g, 0, sizeof g);
        g.base_width = (unsigned)stub_base_w; g.base_height = (unsigned)stub_base_h;
        g.max_width  = (unsigned)stub_max_w;  g.max_height  = (unsigned)stub_max_h;
        if (env_cb) env_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &g);
    }
    if (stub_maxgrow && stub_tick == 30) {
        /* Grow max (and base with it), once. Kept inside STUB_FB_MAX. */
        stub_max_w = stub_base_w = 200;
        stub_max_h = stub_base_h = 150;
        struct retro_game_geometry g;
        memset(&g, 0, sizeof g);
        g.base_width = (unsigned)stub_base_w; g.base_height = (unsigned)stub_base_h;
        g.max_width  = (unsigned)stub_max_w;  g.max_height  = (unsigned)stub_max_h;
        if (env_cb) env_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &g);
    }

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
