#include "test.h"
#include "core.h"
#include <dlfcn.h>
#include <stdlib.h>

static const void *last_data; static unsigned last_w, last_h;
static void on_frame(void *ud, const void *d, unsigned w, unsigned h, size_t pitch)
{
    (void)ud; (void)pitch; last_data = d; last_w = w; last_h = h;
}
static uint16_t g_buttons;
static uint16_t get_buttons(void *ud) { (void)ud; return g_buttons; }

TEST_MAIN({
    char err[256] = {0};

    /* a missing core reports an error rather than crashing, because on the
       device a silent failure is indistinguishable from a crash */
    CHECK(core_open("build/definitely-absent.so", ".", err, sizeof err) == NULL);
    CHECK(err[0] != 0);

    koboy_core *c = core_open("build/stub_core.so", "build", err, sizeof err);
    CHECK(c != NULL);
    CHECK_EQ_INT(core_pixfmt(c), KOBOY_PIXFMT_RGB565);

    core_set_frame_cb(c, on_frame, NULL);
    core_set_input_fn(c, get_buttons, NULL);

    /* No ROM loaded yet -- retro_get_system_av_info is only meaningful once
       one is, so there is nothing honest to report and core_get_geometry
       must say so rather than hand back zeroed or stale ints that a caller
       could mistake for a real (if degenerate) answer. */
    int bw = -1, bh = -1, mw = -1, mh = -1;
    CHECK(!core_get_geometry(c, &bw, &bh, &mw, &mh));
    CHECK_EQ_INT(bw, -1); CHECK_EQ_INT(bh, -1);
    CHECK_EQ_INT(mw, -1); CHECK_EQ_INT(mh, -1);

    const char *rom_path = "build/fake.gb";
    FILE *f = fopen(rom_path, "wb"); fputc(0, f); fclose(f);
    CHECK(core_load_rom(c, rom_path, err, sizeof err));

    /* stub_core.c's retro_get_system_av_info reports base == max == 160x144
       (tests/stub_core.c). Checked as four distinct numbers, not just "some
       positive value": a core.c that swapped base for max, or width for
       height, would still pass a looser check on this symmetric stub. */
    CHECK(core_get_geometry(c, &bw, &bh, &mw, &mh));
    CHECK_EQ_INT(bw, 160); CHECK_EQ_INT(bh, 144);
    CHECK_EQ_INT(mw, 160); CHECK_EQ_INT(mh, 144);

    size_t sl = 0;
    CHECK(core_sram(c, &sl) != NULL);
    CHECK_EQ_INT(sl, 8);

    /* the frame callback fires with Game Boy geometry */
    g_buttons = 0;
    core_run_frame(c);
    CHECK(last_data != NULL);
    CHECK_EQ_INT(last_w, 160);
    CHECK_EQ_INT(last_h, 144);

    /* buttons reach the core: the stub echoes the bitmask into pixel 0 */
    g_buttons = KOBOY_BTN_A | KOBOY_BTN_RIGHT;
    core_run_frame(c);
    CHECK_EQ_INT(((const uint16_t *)last_data)[0], KOBOY_BTN_A | KOBOY_BTN_RIGHT);

    /* #7: the stub's observation flags were only readable under gdb, so the
       assertions they were written for were never actually made. Exported as
       real symbols -- and READ, not merely resolved, below: a test that
       checks the symbol resolves without ever reading its value repeats the
       exact failure #7 is about. */
    void *so = dlopen("build/stub_core.so", RTLD_NOW);
    CHECK(so != NULL);
    int *unloaded  = (int *)dlsym(so, "stub_observed_unload");
    int *was_reset = (int *)dlsym(so, "stub_observed_reset");
    int *ser_calls = (int *)dlsym(so, "stub_serialize_calls");
    CHECK(unloaded != NULL);
    CHECK(was_reset != NULL);
    CHECK(ser_calls != NULL);

    /* Save states round-trip through the core. Deltas, not absolute values:
       the stub's counters are process-wide statics, so an absolute == 1
       would be fragile against test order/reruns within the same binary. */
    size_t n = core_state_size(c);
    CHECK(n > 0);
    uint8_t *blob = malloc(n);
    CHECK(blob != NULL);
    int ser_before = *ser_calls;
    CHECK_EQ_INT(core_state_save(c, blob, n), 1);
    CHECK_EQ_INT(*ser_calls, ser_before + 1);
    CHECK_EQ_INT(core_state_load(c, blob, n), 1);

    /* A short buffer is refused rather than truncated: handing a core a
       partial state is how a running game gets corrupted. */
    CHECK_EQ_INT(core_state_save(c, blob, n - 1), 0);
    CHECK_EQ_INT(core_state_load(c, blob, n - 1), 0);
    free(blob);

    int reset_before = *was_reset;
    CHECK_EQ_INT(core_reset(c), 1);
    CHECK_EQ_INT(*was_reset, reset_before + 1);

    /* Symmetric with the double-unload guard: load_rom on an already-loaded
       core is refused, not silently re-entered, because an un-torn-down
       cartridge state being overwritten underneath a running game is the
       exact misuse this task exists to prevent. */
    CHECK_EQ_INT(core_load_rom(c, rom_path, err, sizeof err), 0);
    CHECK(err[0] != 0);

    /* Unload, then load a different ROM through the SAME handle. dlclose is
       never called mid-session; retro_unload_game plus retro_load_game is the
       libretro-sanctioned way and avoids cycling the shared object. */
    int unload_before = *unloaded;
    CHECK_EQ_INT(core_unload_rom(c), 1);
    CHECK_EQ_INT(*unloaded, unload_before + 1);

    /* Geometry answers "no ROM loaded" again after an unload -- the same
       question core.c re-asks retro_get_system_av_info for on the NEXT load,
       not a cached leftover from the one that just went away. */
    bw = bh = mw = mh = -1;
    CHECK(!core_get_geometry(c, &bw, &bh, &mw, &mh));

    CHECK_EQ_INT(core_load_rom(c, rom_path, err, sizeof err), 1);
    CHECK(core_get_geometry(c, &bw, &bh, &mw, &mh));
    CHECK_EQ_INT(bw, 160); CHECK_EQ_INT(bh, 144);
    CHECK_EQ_INT(mw, 160); CHECK_EQ_INT(mh, 144);

    /* The invariant load_rom_into (src/main.c) is built on: core_sram() must
       be re-read after every load, never cached across an unload/load cycle,
       because the pointer belongs to the freshly loaded cartridge. Pinned
       here, headlessly, rather than trusted by inspection alone. */
    size_t sl2 = 0;
    CHECK(core_sram(c, &sl2) != NULL);
    CHECK_EQ_INT(sl2, 8);

    /* Not every core is the Game Boy. Poke the stub's geometry (exported for
       exactly this, tests/stub_core.c) to something in the Game & Watch
       range the multi-system design doc measured, reload through the same
       handle, and confirm core_get_geometry reports base != max faithfully
       instead of secretly collapsing one into the other. */
    int *sbw = (int *)dlsym(so, "stub_base_w"), *sbh = (int *)dlsym(so, "stub_base_h");
    int *smw = (int *)dlsym(so, "stub_max_w"),  *smh = (int *)dlsym(so, "stub_max_h");
    CHECK(sbw != NULL && sbh != NULL && smw != NULL && smh != NULL);

    *sbw = 431; *sbh = 322; *smw = 692; *smh = 759;   /* measured G&W range */
    CHECK_EQ_INT(core_unload_rom(c), 1);
    CHECK_EQ_INT(core_load_rom(c, rom_path, err, sizeof err), 1);
    CHECK(core_get_geometry(c, &bw, &bh, &mw, &mh));
    CHECK_EQ_INT(bw, 431); CHECK_EQ_INT(bh, 322);
    CHECK_EQ_INT(mw, 692); CHECK_EQ_INT(mh, 759);

    /* max_width/max_height == 0 is the libretro convention for "never bigger
       than base", not a literal request for a zero-sized buffer -- core.c's
       fallback (the comment above c->max_w/c->max_h in core_load_rom) turns
       that convention into base's own numbers so nothing downstream has to
       know the convention exists. Distinguishing this from "base == max
       because the core said 431/322 twice" requires base and max to differ
       here too, which is why this uses a third pair of numbers rather than
       reusing 431x322 for both. */
    *sbw = 973; *sbh = 532; *smw = 0; *smh = 0;
    CHECK_EQ_INT(core_unload_rom(c), 1);
    CHECK_EQ_INT(core_load_rom(c, rom_path, err, sizeof err), 1);
    CHECK(core_get_geometry(c, &bw, &bh, &mw, &mh));
    CHECK_EQ_INT(bw, 973); CHECK_EQ_INT(bh, 532);
    CHECK_EQ_INT(mw, 973); CHECK_EQ_INT(mh, 532);

    /* The Game & Watch core's actual measured behaviour, not just its final
       numbers: retro_get_system_av_info answers a placeholder right after
       retro_load_game, and the real geometry only arrives from inside the
       first retro_run(), via an environment call. tests/stub_core.c's
       stub_late_geometry reproduces exactly that timing (see its own
       comment) so this exercises core.c's env_cb SET_GEOMETRY/
       SET_SYSTEM_AV_INFO handling and core_geometry_changed() the same way
       core_get_geometry alone (checked immediately after a load, above)
       cannot -- that only ever observed the final state, never the
       placeholder-then-announcement sequence a caller has to react to. */
    int *slg = (int *)dlsym(so, "stub_late_geometry");
    CHECK(slg != NULL);

    *sbw = 658; *sbh = 395; *smw = 658; *smh = 395;   /* a measured G&W title */
    *slg = 1;                                          /* via SET_GEOMETRY */
    CHECK_EQ_INT(core_unload_rom(c), 1);
    CHECK_EQ_INT(core_load_rom(c, rom_path, err, sizeof err), 1);

    /* Right after load: the placeholder, not the real answer -- and
       core_geometry_changed() is false, matching its documented contract
       that the initial query is not itself a "change" (core.h). A test that
       only checked core_get_geometry's FINAL value after everything settled
       would never notice if core_load_rom's initial query alone silently
       started trusting stub_base_w/h instead of what retro_get_system_av_info
       actually answered -- this is the check that would catch that. */
    CHECK(core_get_geometry(c, &bw, &bh, &mw, &mh));
    CHECK_EQ_INT(bw, 128); CHECK_EQ_INT(bh, 128);
    CHECK_EQ_INT(mw, 128); CHECK_EQ_INT(mh, 128);
    CHECK(!core_geometry_changed(c));

    /* One retro_run() -- the stub fires SET_GEOMETRY from inside it, exactly
       once, exactly like the real core. */
    g_buttons = 0;
    core_run_frame(c);
    CHECK(core_geometry_changed(c));       /* the change is now pending... */
    CHECK(!core_geometry_changed(c));      /* ...and read-and-clear: gone now */
    CHECK(core_get_geometry(c, &bw, &bh, &mw, &mh));
    CHECK_EQ_INT(bw, 658); CHECK_EQ_INT(bh, 395);
    CHECK_EQ_INT(mw, 658); CHECK_EQ_INT(mh, 395);
    /* The frame callback -- "the reliable source" regardless of which
       environ call told the frontend first -- delivered a frame at the SAME
       real size, not the placeholder. */
    CHECK_EQ_INT(last_w, 658); CHECK_EQ_INT(last_h, 395);

    /* A second retro_run() does not re-announce (the stub only fires once
       per load, matching "never again afterwards" in the measured core) --
       core_geometry_changed() stays false rather than firing spuriously on
       every frame. */
    core_run_frame(c);
    CHECK(!core_geometry_changed(c));

    /* Same story again, through SET_SYSTEM_AV_INFO instead of SET_GEOMETRY
       -- the other environ call core.c has to handle, exercised end to end
       rather than only via env_cb's mutant-verified unit behaviour. Distinct
       numbers (973x532, the widest measured title) from the SET_GEOMETRY
       case above, so a test that accidentally checked stale state from the
       first round could not pass by accident. */
    *sbw = 973; *sbh = 532; *smw = 973; *smh = 532;
    *slg = 2;                                          /* via SET_SYSTEM_AV_INFO */
    CHECK_EQ_INT(core_unload_rom(c), 1);
    CHECK_EQ_INT(core_load_rom(c, rom_path, err, sizeof err), 1);
    CHECK(!core_geometry_changed(c));
    CHECK(core_get_geometry(c, &bw, &bh, &mw, &mh));
    CHECK_EQ_INT(bw, 128); CHECK_EQ_INT(bh, 128);

    core_run_frame(c);
    CHECK(core_geometry_changed(c));
    CHECK(core_get_geometry(c, &bw, &bh, &mw, &mh));
    CHECK_EQ_INT(bw, 973); CHECK_EQ_INT(bh, 532);
    CHECK_EQ_INT(mw, 973); CHECK_EQ_INT(mh, 532);
    CHECK_EQ_INT(last_w, 973); CHECK_EQ_INT(last_h, 532);

    /* Restored, not left pointed at Game & Watch numbers / late-geometry
       mode: the .so this dlsym reached is the SAME loaded object core_open's
       own dlopen returned (one shared object per path, refcounted -- not a
       private copy), so leaving this poked would be a real (if currently
       harmless, this being the last use of `c` in this binary) footgun for
       whoever adds a test below it later. */
    *sbw = 160; *sbh = 144; *smw = 160; *smh = 144; *slg = 0;

    core_close(c);
})
