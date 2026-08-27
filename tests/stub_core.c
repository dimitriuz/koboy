#include "libretro_min.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

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
/* And the arcade one, recorded the same way and for a sharper reason: an
   arcade board has NO battery save at all, so FinalBurn Neo's hiscore.dat
   mechanism is the only thing that persists anything, and the code that reads
   this option leaves it OFF when the frontend refuses the query. "koboy said
   something" would not distinguish the two answers that matter. */
char stub_fbneo_hiscores[32] = "";
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

/* The other two things retro_get_system_av_info reports, poked the same way
   and for the same reason -- both are consumed for the first time by the task
   that gave koboy per-core pacing and non-square pixels.

   stub_aspect 0.0 is not a placeholder: it is libretro's "no answer, assume
   base_width/base_height", one shipped core (gearcoleco) really does report
   it, and it is what every OTHER test in this binary keeps seeing, so the
   fallback path in core_display_aspect is the default rather than a corner.

   stub_fps is also readable from the environment as KOBOY_STUB_FPS, which
   dlsym-poking cannot cover: tests/smoke_host.sh drives the real binary as a
   subprocess and times it, and a wall-clock check is the only thing in this
   project that can prove main.c actually PACES at the rate it resolved rather
   than merely logging it. */
double stub_aspect = 0.0;
double stub_fps    = 59.7275;
/* The rate a MID-RUN SET_SYSTEM_AV_INFO announces, when that differs from the
   rate the load-time query answered with. 0 means "announce stub_fps", i.e.
   no change -- which is what every test that does not care sees. Poked by
   test_core.c (through the stub_late_geometry == 2 path) and set from
   KOBOY_STUB_FPS_LATE by smoke_host.sh (through the tick-10 path below);
   the two exist because one drives core.c directly and the other has to drive
   the whole binary as a subprocess. */
double stub_fps_late = 0.0;
static int stub_fps_late_env = 0;

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
   frame 30, base moving with it. The pair exists so a test can assert both
   directions: base churn must not provoke a re-fit in the layout whose rect
   comes from max, a rect change MUST. Asserting only the first would pass
   against a frontend that ignored geometry entirely.

   KOBOY_STUB_MAXONLY=1 is the third case and the one neither of those can
   reach: max grows at frame 30 and BASE DOES NOT MOVE. In KOBOY_LAYOUT_DMG
   the rect is sized from base, so the presentation is identical either side
   of that announcement -- and a re-fit is still required, because
   video_create's intermediate buffer is sized from max and a frame the
   bounds guard now accepts would not fit the old one. It exists because a
   main.c that compared everything about the resolved profile EXCEPT max
   passed every other check in this project. */
static int stub_osc = -1, stub_maxgrow = -1, stub_maxonly = -1, stub_tick = 0;
#define STUB_PLACEHOLDER_W 128
#define STUB_PLACEHOLDER_H 128

#define STUB_STATE_BYTES 128
static unsigned char stub_state[STUB_STATE_BYTES];

/* Reproduces the measured Genesis Plus GX behaviour, which is the reason
   core_sram pins the save-RAM LENGTH at load time (see src/core.c): that core
   answers retro_get_memory_size(SAVE_RAM) with the buffer's real size before
   emulation starts and with "how much of it is worth writing" once it is
   running -- a SMALLER number, and a different one every session.

   Poked by test_core.c, and left 0 by default so every other test in this
   binary keeps seeing the flat 8 bytes it always has. Counts retro_run calls
   itself rather than reading a flag, because the whole point is that the
   answer changes between "loaded" and "running". */
int stub_sram_shrink_when_running = 0;
static int stub_ran_since_load = 0;
#define STUB_SRAM_SHRUNK 3

/* Rotation, and this stub reproduces the two DIFFERENT core behaviours that
   made it necessary, because they are different questions.

   stub_rotation >= 0 makes retro_load_game announce that many quarter turns
   through RETRO_ENVIRONMENT_SET_ROTATION, which is what FinalBurn Neo does
   (SetRotation, called from inside retro_load_game -- 3 for Galaga, 0 for
   Donkey Kong Jr., out of the same .so).

   stub_rotation_accepted records WHAT KOBOY ANSWERED, and that is the half
   that is not decoration: beetle-wswan asks this same question and REMEMBERS
   the answer (third_party/wswan/libretro.c, hw_rotate_enabled). On false it
   keeps rotating in software; on true it stops, and hands over frames that
   the frontend now owes a turn. A koboy that recorded the number but
   answered false would look correct in every geometry assertion and would
   present a WonderSwan sideways. Only the answer catches that.

   -1 (the default) never asks at all, which is every other core koboy ships
   and every other test in this binary. */
int stub_rotation = -1;
int stub_rotation_accepted = -1;

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
    struct retro_variable vh = { "fbneo-hiscores", NULL };
    if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &vh) && vh.value) {
        strncpy(stub_fbneo_hiscores, vh.value, sizeof stub_fbneo_hiscores - 1);
        stub_fbneo_hiscores[sizeof stub_fbneo_hiscores - 1] = 0;
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
    /* KOBOY_STUB_GEOM=WxH, read BEFORE the fields below are filled in -- which
       is the whole point, and the mistake the comment further down already
       records somebody making with the aspect knob. Set after, it changes a
       global nothing reads again and the run proves nothing.

       It exists so a subprocess run can present a geometry other than the
       Game Boy's: anything keyed on the SYSTEM rather than the core (the
       per-system scale ceiling, for one) is untestable while the stub is
       stuck at 160x144, because a .sfc and a .gb then resolve identically. */
    {
        const char *g = getenv("KOBOY_STUB_GEOM");
        if (g && *g) {
            int gw = 0, gh = 0;
            if (sscanf(g, "%dx%d", &gw, &gh) == 2 && gw > 0 && gh > 0) {
                stub_base_w = stub_max_w = gw;
                stub_base_h = stub_max_h = gh;
            }
        }
    }
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
    /* BEFORE the fields are filled in, not after -- which is where this block
       first went, and the run that was meant to prove main.c honours a
       non-square aspect reported a square one instead. */
    {
        const char *e = getenv("KOBOY_STUB_FPS");
        if (e && *e) stub_fps = atof(e);
        /* KOBOY_STUB_ASPECT, for the same reason KOBOY_STUB_FPS exists: only
           a subprocess run of the real binary can show that main.c carries
           the core's display aspect into the RECT, and a subprocess cannot be
           dlsym-poked. */
        e = getenv("KOBOY_STUB_ASPECT");
        if (e && *e) stub_aspect = atof(e);
    }
    i->geometry.aspect_ratio = (float)stub_aspect;
    i->timing.fps = stub_fps; i->timing.sample_rate = 32768.0;
}
bool retro_load_game(const struct retro_game_info *g)
{
    if (!g) return false;
    memset(sram, 0, sizeof sram);
    stub_late_fired = 0;   /* each load gets its own chance to announce late */
    stub_ran_since_load = 0;
    /* Announced from inside retro_load_game, where FBNeo announces it, and
       NOT from retro_set_environment: rotation is a property of the GAME, so
       a core that asked once at init could not answer differently for the
       next ROM through the same handle. */
    /* KOBOY_STUB_ROTATE, for the same reason KOBOY_STUB_OSCILLATE exists: the
       test that needs it (tests/smoke_host.sh) runs koboy as a SEPARATE
       PROCESS and cannot poke this binary's globals. It also sets the
       geometry to a deliberately NON-SQUARE 288x224, so a koboy that recorded
       the rotation but did not act on it reports the wrong shape. */
    {
        const char *e = getenv("KOBOY_STUB_ROTATE");
        if (e && *e) {
            stub_rotation = atoi(e);
            /* NON-SQUARE, and max == base, which is stricter than FinalBurn
               Neo (it reports a square max so both orientations fit one
               buffer) and deliberately so: with a square max a frontend that
               ignored the rotation entirely would still ACCEPT every frame
               and merely draw it sideways -- invisible from outside the
               process, which is all a smoke test can see. With max == base
               the un-rotated frame is 288 wide against a 224-wide reserved
               rect, video_pipeline_run's bound guard drops it, and "no rects
               were ever emitted" is observable in the run's own summary line.
               beetle-wswan reports its geometry this way for real. */
            stub_base_w = 288; stub_base_h = 224;
            stub_max_w  = 288; stub_max_h  = 224;
        }
    }
    if (stub_rotation >= 0 && env_cb) {
        unsigned r = (unsigned)stub_rotation;
        stub_rotation_accepted = env_cb(RETRO_ENVIRONMENT_SET_ROTATION, &r) ? 1 : 0;
    }
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
    stub_ran_since_load++;
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
        e = getenv("KOBOY_STUB_MAXONLY");
        stub_maxonly = (e && *e && *e != '0') ? 1 : 0;
        /* A MID-RUN TIMING CHANGE, which is the half of SET_SYSTEM_AV_INFO
           koboy ignored until per-core pacing existed. Announced from inside
           retro_run() because that is where a real core announces it, and
           through the full av_info (not SET_GEOMETRY) because that is the
           only command that carries timing at all. tests/smoke_host.sh times
           a run across the switch: nothing else in this project can prove
           main.c re-paces rather than merely re-fitting. */
        e = getenv("KOBOY_STUB_FPS_LATE");
        if (e && *e) { stub_fps_late = atof(e); stub_fps_late_env = 1; }
    }
    stub_tick++;
    if (stub_fps_late_env && stub_tick == 10) {
        struct retro_system_av_info av;
        memset(&av, 0, sizeof av);
        av.geometry.base_width  = (unsigned)stub_base_w;
        av.geometry.base_height = (unsigned)stub_base_h;
        av.geometry.max_width   = (unsigned)stub_max_w;
        av.geometry.max_height  = (unsigned)stub_max_h;
        av.geometry.aspect_ratio = (float)stub_aspect;
        av.timing.fps = stub_fps_late; av.timing.sample_rate = 32768.0;
        if (env_cb) env_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av);
    }
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
    if (stub_maxonly && stub_tick == 30) {
        /* Max only. Base stays at whatever it was, so in the DMG layout the
           reserved rect does not move by a pixel -- and the buffers still
           have to grow. Kept inside STUB_FB_MAX. */
        stub_max_w = 200; stub_max_h = 150;
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
            av.geometry.aspect_ratio = (float)stub_aspect;
            /* stub_fps_late, when set, is what makes this announcement differ
               from the load-time query -- without a difference, a test cannot
               tell whether core.c read `av->timing` here or is still echoing
               what retro_get_system_av_info said. */
            av.timing.fps = stub_fps_late > 0.0 ? stub_fps_late : stub_fps;
            av.timing.sample_rate = 32768.0;
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
size_t retro_get_memory_size(unsigned id)
{
    if (id != RETRO_MEMORY_SAVE_RAM) return 0;
    if (stub_sram_shrink_when_running && stub_ran_since_load > 0)
        return STUB_SRAM_SHRUNK;
    return sizeof sram;
}
