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
static int16_t g_ptr_x, g_ptr_y; static bool g_ptr_down;
static void get_pointer(void *ud, int16_t *x, int16_t *y, bool *pressed)
{ (void)ud; *x = g_ptr_x; *y = g_ptr_y; *pressed = g_ptr_down; }

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

    /* ------------------------------------------------------- the POINTER
     *
     * A Game & Watch title draws its own buttons into its artwork and reads
     * them as a POINTER, on port 2. The stub now polls that every retro_run
     * and records what came back (tests/stub_core.c), so what is asserted
     * here is what a core SEES -- not what koboy believes it sent.
     *
     * Read through dlsym on the same handle the observation flags below use.
     */
    {
        void *pso = dlopen("build/stub_core.so", RTLD_NOW);
        CHECK(pso != NULL);
        int *px  = (int *)dlsym(pso, "stub_ptr_x");
        int *py  = (int *)dlsym(pso, "stub_ptr_y");
        int *pp  = (int *)dlsym(pso, "stub_ptr_pressed");
        int *pc  = (int *)dlsym(pso, "stub_ptr_count");
        int *p0x = (int *)dlsym(pso, "stub_ptr_port0_x");
        int *p0p = (int *)dlsym(pso, "stub_ptr_port0_pressed");
        int *pi1 = (int *)dlsym(pso, "stub_ptr_idx1_x");
        CHECK(px && py && pp && pc && p0x && p0p && pi1);

        if (px && py && pp && pc && p0x && p0p && pi1) {
            /* ADDITIVE: with no pointer function installed -- which is every
               Game Boy session -- a POINTER query answers 0. That is exactly
               what gambatte already saw, so this path cannot disturb it. */
            core_run_frame(c);
            CHECK_EQ_INT(*px, 0);
            CHECK_EQ_INT(*py, 0);
            CHECK_EQ_INT(*pp, 0);

            /* Installed: the values arrive, on port 2. */
            g_ptr_x = -12345; g_ptr_y = 6789; g_ptr_down = true;
            core_set_pointer_fn(c, get_pointer, NULL);
            core_run_frame(c);
            CHECK_EQ_INT(*px, -12345);
            CHECK_EQ_INT(*py, 6789);
            CHECK_EQ_INT(*pp, 1);
            CHECK_EQ_INT(*pc, 1);

            /* PORT IS NOT CHECKED, on purpose: the gw core asks on port 2
               and the libretro convention is port 0, and koboy has one
               touchscreen either way. Asserting port 0 answers too is what
               keeps a future "if (port != 2) return 0" from shipping. */
            CHECK_EQ_INT(*p0x, -12345);
            CHECK_EQ_INT(*p0p, 1);

            /* INDEX IS checked: only the first touch becomes a pointer, so a
               query for touch index 1 answers 0 even while index 0 is
               pressed. Without this, `idx` could be ignored entirely and a
               multi-touch core would see the same finger several times. */
            CHECK_EQ_INT(*pi1, 0);

            /* Release really reaches the core. A core that never sees
               PRESSED go false holds the artwork's button down forever. */
            g_ptr_down = false;
            core_run_frame(c);
            CHECK_EQ_INT(*pp, 0);
            CHECK_EQ_INT(*pc, 0);
            /* ...and the coordinates still come through, unchanged: a real
               pointer device reports where it was. */
            CHECK_EQ_INT(*px, -12345);

            /* THE JOYPAD PATH IS UNDISTURBED, checked in the same frame as a
               live pointer -- the whole point of "additive". */
            g_buttons = KOBOY_BTN_B | KOBOY_BTN_UP;
            g_ptr_down = true;
            core_run_frame(c);
            CHECK_EQ_INT(((const uint16_t *)last_data)[0], KOBOY_BTN_B | KOBOY_BTN_UP);
            CHECK_EQ_INT(*pp, 1);

            /* THE NEW RETROPAD BITS REACH THE IDS THEY CLAIM TO. koboy's
               KOBOY_BTN_* are RETRO_DEVICE_ID_JOYPAD_* by BIT POSITION, which
               is what makes core.c's forwarding a plain `latched >> id`, and
               a Game & Watch title's binding is chosen by id: Mickey Mouse
               (Wide Screen) puts NORTHEAST on id 9 and its GAME A / GAME B
               switches on ids 10 and 11. Renumber a bit and koboy silently
               presses a different button on every one of these titles.

               stub_core.c queries ids 0..15 one at a time and repacks each
               answer as 1u << id, so the LITERAL on the right-hand side below
               is the id the core actually asked about -- deliberately written
               as a shift and not as the KOBOY_BTN_* name, which would compare
               the constant with itself and pass whatever it was changed to. */
            {
                static const struct { uint16_t bit; unsigned id; const char *n; } abi[] = {
                    { KOBOY_BTN_B,      0,  "B"      },
                    { KOBOY_BTN_Y,      1,  "Y"      },
                    { KOBOY_BTN_SELECT, 2,  "SELECT" },
                    { KOBOY_BTN_START,  3,  "START"  },
                    { KOBOY_BTN_UP,     4,  "UP"     },
                    { KOBOY_BTN_DOWN,   5,  "DOWN"   },
                    { KOBOY_BTN_LEFT,   6,  "LEFT"   },
                    { KOBOY_BTN_RIGHT,  7,  "RIGHT"  },
                    { KOBOY_BTN_A,      8,  "A"      },
                    { KOBOY_BTN_X,      9,  "X"      },
                    { KOBOY_BTN_L1,     10, "L1"     },
                    { KOBOY_BTN_R1,     11, "R1"     },
                };
                g_ptr_down = false;
                for (size_t k = 0; k < sizeof abi / sizeof abi[0]; k++) {
                    g_buttons = abi[k].bit;
                    core_run_frame(c);
                    uint16_t got = ((const uint16_t *)last_data)[0];
                    if (got != (uint16_t)(1u << abi[k].id))
                        fprintf(stderr, "  %s: core saw 0x%04x, id %u expects 0x%04x\n",
                                abi[k].n, got, abi[k].id, 1u << abi[k].id);
                    CHECK_EQ_INT(got, (uint16_t)(1u << abi[k].id));
                }
                g_buttons = 0;
            }

            /* Uninstalling goes back to zeros, so the rest of this file runs
               against the same core state it always did. */
            core_set_pointer_fn(c, NULL, NULL);
            core_run_frame(c);
            CHECK_EQ_INT(*px, 0);
            CHECK_EQ_INT(*pp, 0);
            g_buttons = KOBOY_BTN_A | KOBOY_BTN_RIGHT;
            core_run_frame(c);
        }
        if (pso) dlclose(pso);
    }

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

    /* THE POKEMON MINI CORE OPTIONS koboy answers, read back as the strings
       the core actually received. Both are corrections for the medium, not
       taste, and both are argued at length in core.c's env_cb:
         video_scale 1x        -- the core's default 4x makes it report
                                  384x256 and bakes a dot-matrix LCD filter
                                  into every frame; quantised to four greys
                                  that filter is full-rect noise, and the
                                  upscaling is work koboy's own integer
                                  scaler does for free.
         palette Monochrome Vector -- the default paints the game rect 83%
                                  black (measured mean luma 0.174). E-ink is
                                  reflective paper.
       Asserted as exact strings, because these are matched by strcmp inside
       the real core: "video_scale = 1" or "monochrome vector" would be
       silently ignored and the core would keep its default, which is
       precisely the failure this pins. */
    const char *pm_scale   = (const char *)dlsym(so, "stub_pm_video_scale");
    const char *pm_palette = (const char *)dlsym(so, "stub_pm_palette");
    int *unknown_refused   = (int *)dlsym(so, "stub_unknown_option_refused");
    CHECK(pm_scale != NULL);
    CHECK(pm_palette != NULL);
    CHECK(unknown_refused != NULL);
    if (pm_scale)   CHECK(strcmp(pm_scale, "1x") == 0);
    if (pm_palette) CHECK(strcmp(pm_palette, "Monochrome Vector") == 0);
    /* And a key koboy has no opinion about is REFUSED with v.value cleared.
       Without this, "koboy answers the two keys above" is equally consistent
       with a frontend that answers every key with something. */
    if (unknown_refused) CHECK_EQ_INT(*unknown_refused, 1);

    /* THE ARCADE HIGH-SCORE SWITCH, and it is not a preference. An arcade
       board has no battery: retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) is 0
       on all 227 romsets measured, so there is no .srm for any of them and
       FinalBurn Neo's hiscore.dat mechanism is the ONLY thing that persists
       anything at all. The core's own stated default for `fbneo-hiscores` is
       "enabled" -- and the code that reads it leaves the flag at its BSS zero
       when the frontend REFUSES the query, which is what koboy does with
       every key it has no opinion about. So saying nothing gets the opposite
       of the documented default, and this assertion is what stops that
       reverting. Verified end to end on the real core: with it answered,
       Ms. Pac-Man writes fbneo/mspacman.hi on unload and its attract screen
       reads back HIGH SCORE 220 on the next launch; without it, nothing is
       written. Exact string, because the core matches it with strcmp. */
    const char *fb_hi = (const char *)dlsym(so, "stub_fbneo_hiscores");
    CHECK(fb_hi != NULL);
    if (fb_hi) CHECK(strcmp(fb_hi, "enabled") == 0);

    /* THE SAVE DIRECTORY, which for one whole system IS the save path.
       Neo Geo Pocket cartridges save into flash rather than into
       RETRO_MEMORY_SAVE_RAM -- measured with scripts/probe_core.c:
       retro_get_memory_size(SAVE_RAM) is 0 for every one of the author's ten
       .ngp titles, on both available cores -- and RACE writes that flash
       itself, into whatever directory this query answers. So koboy's answer
       has to be the save_dir it was opened with, exactly, and it has to be an
       answer rather than a refusal. Nothing else in this suite pins it: every
       other system reaches disk through sram.c, and a refused query here
       would lose every Neo Geo Pocket save while looking identical to a
       working build.

       The SYSTEM directory is asserted alongside it because core.c answers
       both from the same field, and a core that asks for one and not the
       other is common (RACE asks only for the save directory, beetle-ngp for
       both). "build" is the save_dir this test's core_open was given. */
    const char *sdir = (const char *)dlsym(so, "stub_save_dir");
    const char *ydir = (const char *)dlsym(so, "stub_system_dir");
    int *sdir_ans = (int *)dlsym(so, "stub_save_dir_answered");
    int *ydir_ans = (int *)dlsym(so, "stub_system_dir_answered");
    CHECK(sdir != NULL); CHECK(ydir != NULL);
    CHECK(sdir_ans != NULL); CHECK(ydir_ans != NULL);
    if (sdir_ans) CHECK_EQ_INT(*sdir_ans, 1);
    if (ydir_ans) CHECK_EQ_INT(*ydir_ans, 1);
    if (sdir) CHECK(strcmp(sdir, "build") == 0);
    if (ydir) CHECK(strcmp(ydir, "build") == 0);

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

    /* THE LENGTH IS LOAD-ONCE EVEN THOUGH THE POINTER IS NOT, and this is the
       assertion that says so. Genesis Plus GX -- the Master System / Game
       Gear core -- answers retro_get_memory_size(SAVE_RAM) with the buffer's
       real size before emulation starts and with a SMALLER "how much is worth
       writing" number once it is running. main.c calls core_sram() again
       mid-session (after a save-state load, where the pointer really can
       move), and if that call also picked up the shrunken length, the next
       flush would rewrite a full-size .srm at the short length and the launch
       after it could not read the file whole -- a destroyed save that
       announces itself as "Save file unreadable".

       stub_sram_shrink_when_running reproduces exactly that: 8 bytes at load,
       3 once retro_run has been called. What core_sram reports must not move.

       MUTANT-VERIFIED: with core.c's `*len = p ? c->sram_len : 0` changed
       back to a live `c->get_memory_size(RETRO_MEMORY_SAVE_RAM)`, the
       CHECK_EQ_INT below fails with 3 != 8. */
    {
        int *shrink = (int *)dlsym(so, "stub_sram_shrink_when_running");
        CHECK(shrink != NULL);
        *shrink = 1;
        CHECK_EQ_INT(core_unload_rom(c), 1);
        CHECK_EQ_INT(core_load_rom(c, rom_path, err, sizeof err), 1);

        size_t at_load = 0;
        CHECK(core_sram(c, &at_load) != NULL);
        CHECK_EQ_INT(at_load, 8);

        /* Run a few frames, which is what flips the stub's answer, exactly as
           starting emulation flips the real core's. */
        for (int i = 0; i < 3; i++) core_run_frame(c);

        size_t while_running = 0;
        CHECK(core_sram(c, &while_running) != NULL);
        CHECK_EQ_INT(while_running, at_load);

        /* And the stub really is lying, so the check above is not passing
           because nothing changed. Asked through the core's own symbol, the
           way koboy used to ask. */
        size_t (*raw_size)(unsigned) =
            (size_t (*)(unsigned))dlsym(so, "retro_get_memory_size");
        CHECK(raw_size != NULL);
        CHECK_EQ_INT(raw_size(0 /* RETRO_MEMORY_SAVE_RAM */), 3);

        /* A fresh load re-asks, so the next cartridge is not stuck with this
           one's number. */
        CHECK_EQ_INT(core_unload_rom(c), 1);
        *shrink = 0;
        CHECK_EQ_INT(core_load_rom(c, rom_path, err, sizeof err), 1);
        size_t after = 0;
        CHECK(core_sram(c, &after) != NULL);
        CHECK_EQ_INT(after, 8);
    }

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

    /* ------------------------------------------------------------ rotation
       A core may ask for its frames to be turned a quarter turn before they
       are shown. FinalBurn Neo does, from inside retro_load_game, and it
       answers differently PER GAME out of one .so -- 3 for Galaga, 0 for
       Donkey Kong Jr. */
    {
        int *rot     = (int *)dlsym(so, "stub_rotation");
        int *rot_ack = (int *)dlsym(so, "stub_rotation_accepted");
        CHECK(rot && rot_ack);
        /* Out of the late-geometry mode the block above left the stub in:
           this one is about rotation, and a 128x128 placeholder would make
           every geometry assertion below fail for an unrelated reason. */
        *slg = 0;

        /* Galaga's real numbers: a 288x224 buffer, a SQUARE 288x288 max (the
           core reports max = max(w,h) so both orientations fit one buffer),
           and 3 quarter turns. */
        *sbw = 288; *sbh = 224; *smw = 288; *smh = 288;
        *rot = 3; *rot_ack = -1;
        CHECK_EQ_INT(core_unload_rom(c), 1);
        CHECK_EQ_INT(core_load_rom(c, rom_path, err, sizeof err), 1);

        /* THE ANSWER, not just the number. libretro's contract is that a
           frontend returning true has promised to do the turn itself, and
           beetle-wswan reads it that way -- on false it keeps rotating in
           software, on true it stops. A koboy that recorded the rotation and
           answered false would present a WonderSwan sideways while passing
           every geometry assertion below. */
        CHECK_EQ_INT(*rot_ack, 1);
        CHECK_EQ_INT((int)core_rotation(c), 3);

        /* And the geometry comes back TRANSPOSED, because what every caller
           of this wants is the size of the picture as PRESENTED, not the size
           of the buffer the core renders into. Galaga is 224x288 on screen. */
        CHECK(core_get_geometry(c, &bw, &bh, &mw, &mh));
        CHECK_EQ_INT(bw, 224); CHECK_EQ_INT(bh, 288);
        CHECK_EQ_INT(mw, 288); CHECK_EQ_INT(mh, 288);

        /* An EVEN rotation does not transpose. 2 is a half turn: same shape,
           different pixels. A swap keyed on "rotation is non-zero" rather
           than on "rotation is odd" reports 224x288 here and lays out a rect
           of the wrong shape. */
        *rot = 2; *rot_ack = -1;
        CHECK_EQ_INT(core_unload_rom(c), 1);
        CHECK_EQ_INT(core_load_rom(c, rom_path, err, sizeof err), 1);
        CHECK_EQ_INT((int)core_rotation(c), 2);
        CHECK(core_get_geometry(c, &bw, &bh, &mw, &mh));
        CHECK_EQ_INT(bw, 288); CHECK_EQ_INT(bh, 224);

        /* THE ROTATION IS CLEARED WITH THE GAME. MENU -> CHOOSE ROM reuses
           one open core handle, and a board that asks for no turn sends NO
           SET_ROTATION at all -- so a rotation left behind from the previous
           game is never corrected, and the next board plays sideways. Driven
           exactly that way: stub_rotation = -1 means "never ask". */
        *rot = 1; *rot_ack = -1;
        CHECK_EQ_INT(core_unload_rom(c), 1);
        CHECK_EQ_INT(core_load_rom(c, rom_path, err, sizeof err), 1);
        CHECK_EQ_INT((int)core_rotation(c), 1);
        CHECK(core_get_geometry(c, &bw, &bh, &mw, &mh));
        CHECK_EQ_INT(bw, 224);                       /* transposed, as above */

        *rot = -1;                                   /* the next game asks nothing */
        CHECK_EQ_INT(core_unload_rom(c), 1);
        CHECK_EQ_INT(core_load_rom(c, rom_path, err, sizeof err), 1);
        CHECK_EQ_INT((int)core_rotation(c), 0);
        CHECK(core_get_geometry(c, &bw, &bh, &mw, &mh));
        CHECK_EQ_INT(bw, 288); CHECK_EQ_INT(bh, 224);

        /* A core that never asks at all -- which is nine of the eleven koboy
           ships -- gets rotation 0 and untouched geometry, which is what
           makes this whole addition invisible to them. Already covered by the
           line above, restated here as the property rather than as a step in
           a sequence. */
        CHECK_EQ_INT((int)core_rotation(NULL), 0);

        *rot = -1; *rot_ack = -1;
    }

    /* Restored, not left pointed at Game & Watch numbers / late-geometry
       mode: the .so this dlsym reached is the SAME loaded object core_open's
       own dlopen returned (one shared object per path, refcounted -- not a
       private copy), so leaving this poked would be a real (if currently
       harmless, this being the last use of `c` in this binary) footgun for
       whoever adds a test below it later. */
    *sbw = 160; *sbh = 144; *smw = 160; *smh = 144; *slg = 0;

    core_close(c);
})
