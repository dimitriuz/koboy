/* mkdtemp() and system(), used by the blank-value/no-trailing-newline test
   below, are not declared under -std=c11 without this -- matches src/sram.c,
   src/config.c, tests/test_romlist.c and tests/test_uiscript.c. */
#define _DEFAULT_SOURCE
#include "test.h"
#include "chrome.h"          /* chrome_controls_top, for the new-geometry sweep below */
#include "config.h"
#include <stdio.h>
#include <stdlib.h>

TEST_MAIN({
    koboy_config c;
    config_defaults(&c);
    CHECK_EQ_INT(c.scale, 5);
    CHECK_EQ_INT(c.present_divisor, 3);
    /* The ghosting mitigations default OFF and the flash-promotion threshold
       defaults to firing only on a whole-rect change, which normal gameplay
       essentially never produces (and which is asserted below, not assumed). Both
       were written for forced DU4, which cannot erase; AUTO can, and measuring
       showed the threshold was the sole source of the flashing while the
       cleanup never fired. Asserted here so re-enabling them is a deliberate
       decision with a test to change, not a silent regression. */
    CHECK_EQ_INT(c.cleanup_interval, 0);
    CHECK_EQ_INT(c.full_refresh_permille, 1000);
    CHECK_EQ_INT(c.cleanup_max_ms, 0);
    CHECK_EQ_INT(c.wfm_fast_policy, KOBOY_WFM_AUTO);
    CHECK(c.grab_input);
    /* CROSS: the chrome draws an absolute cross, so the input model has to be
       the absolute one. */
    CHECK_EQ_INT(c.dpad_mode, KOBOY_DPAD_CROSS);
    /* The shipped button mapping, NOT the 0 "uncalibrated" sentinel. Asserted
       by value because the sentinel is what deadlocked first run on a Kobo with
       no page-turn buttons: see the note in config_defaults and test_calib.c.
       193/194 are KEY_F23/KEY_F24, measured on the Libra 2's gpio-keys node;
       116 is KEY_POWER and must never appear here. */
    CHECK_EQ_INT(c.key_a, KOBOY_KEY_PAGE_F23);
    CHECK_EQ_INT(c.key_b, KOBOY_KEY_PAGE_F24);
    CHECK_EQ_INT(c.key_a, 193);
    CHECK_EQ_INT(c.key_b, 194);
    CHECK(c.key_a != KOBOY_KEY_POWER && c.key_b != KOBOY_KEY_POWER);

    /* The default core, and -- separately -- the fact that NOBODY ASKED for
       it. main.c overrides an unasked-for core from the ROM's extension, so
       these two are different facts and both have to hold: the string alone
       cannot distinguish "the user configured gambatte" from "config_defaults
       always writes gambatte". */
    CHECK(strcmp(c.core_path, "gambatte_libretro.so") == 0);
    CHECK(!c.core_explicit);

    /* key_start/key_select: a GUESS (BTN_TL/BTN_TR, the Xbox pad's measured
       shoulder buttons), unlike key_a/key_b's measured page-turn default --
       asserted by value, same reasoning as key_a/key_b above, and never
       KOBOY_KEY_POWER for the same quit-key reason. */
    CHECK_EQ_INT(c.key_start, KOBOY_KEY_BTN_TL);
    CHECK_EQ_INT(c.key_select, KOBOY_KEY_BTN_TR);
    CHECK_EQ_INT(c.key_start, 310);
    CHECK_EQ_INT(c.key_select, 311);
    CHECK(c.key_start != KOBOY_KEY_POWER && c.key_select != KOBOY_KEY_POWER);

    /* The promotion DECISION, not just the stored threshold. Checking only that
       the default is 1000 would let a >= silently become a >, or the bounding
       box shrink by a pixel, and flashing would come back with every test still
       green. The game rect used here is the shipped 5x one, 800x720.

       At the default the promotion fires only on a dirty rect covering the whole
       game rect -- which IS reachable, since the dirty rect is a single merged
       bounding box and a full-screen wipe produces exactly it. That case is
       deliberately inside the promotion: a frame in which everything changed is
       when a flashing refresh is wanted. */
    const long whole = 800L * 720L;
    CHECK(config_promote_full(&c, whole, whole));            /* corner to corner */
    CHECK(!config_promote_full(&c, whole - 1, whole));        /* one pixel short */
    CHECK(!config_promote_full(&c, whole / 2, whole));        /* half the rect */
    CHECK(!config_promote_full(&c, 160L * 144L, whole));      /* a 1x-sized patch */

    /* A lowered threshold promotes proportionally: 450 permille means 45% of
       the rect, and the boundary is again inclusive. */
    c.full_refresh_permille = 450;
    CHECK(config_promote_full(&c, whole * 45 / 100, whole));
    CHECK(config_promote_full(&c, whole, whole));
    CHECK(!config_promote_full(&c, whole * 44 / 100, whole));

    /* <= 0 means disabled, and must not degenerate into "always": a plain
       comparison against 0 permille is true for every non-empty rect. */
    c.full_refresh_permille = 0;
    CHECK(!config_promote_full(&c, whole, whole));
    CHECK(!config_promote_full(&c, 1, whole));
    c.full_refresh_permille = -1;
    CHECK(!config_promote_full(&c, whole, whole));

    /* Degenerate geometry cannot promote, and must not divide or overflow. */
    c.full_refresh_permille = 1000;
    CHECK(!config_promote_full(&c, 0, whole));
    CHECK(!config_promote_full(&c, whole, 0));
    CHECK(!config_promote_full(NULL, whole, whole));

    config_defaults(&c);

    /* 5x fits every supported panel */
    koboy_profile p;
    CHECK(config_resolve_profile(&p, &c, 1264, 1680, KOBOY_GB_W, KOBOY_GB_H, KOBOY_GB_W, KOBOY_GB_H));
    CHECK_EQ_INT(p.scale, 5);
    CHECK_EQ_INT(p.game_x, (1264 - 800) / 2);
    CHECK(config_resolve_profile(&p, &c, 1072, 1448, KOBOY_GB_W, KOBOY_GB_H, KOBOY_GB_W, KOBOY_GB_H));
    CHECK_EQ_INT(p.scale, 5);
    CHECK_EQ_INT(p.game_x, (1072 - 800) / 2);

    /* An impossible configured scale falls back to the largest that fits -- and
       "fits" now means fits above the controls, not merely inside the panel.
       6x would clear the bezel margin on this panel with 176 px to spare and
       still bury the A button and the d-pad, so the answer is 5. */
    c.scale = 99;
    CHECK(config_resolve_profile(&p, &c, 1072, 1448, KOBOY_GB_W, KOBOY_GB_H, KOBOY_GB_W, KOBOY_GB_H));
    CHECK_EQ_INT(p.scale, 5);

    /* PINS the design spec's property that 5x fits every supported panel with
       room to spare, across all four target panel sizes (spec Section 3), with
       the shipped defaults untouched. Nothing else in this file swept every
       panel -- the two CHECK_EQ_INT above only ever covered Libra 2 and Clara
       -- and that gap is exactly how a MENU zone placed at 540 permille
       (mid-panel, not on the Start/Select row already reserved at 920) shipped
       once already: it silently knocked Clara from scale 5 to scale 4 and nothing
       caught it until config_resolve_profile was measured by hand across every
       panel. See the .menu_cx/.menu_cy comment in config_defaults for the
       measured chrome_controls_top values that made the difference. */
    {
        koboy_config dc; config_defaults(&dc);
        static const struct { int w, h; const char *name; } panels[] = {
            { 1072, 1448, "Clara"  },
            { 1264, 1680, "Libra2" },
            { 1404, 1872, "Elipsa" },
            { 1440, 1920, "Sage"   },
        };
        for (size_t i = 0; i < sizeof panels / sizeof panels[0]; i++) {
            koboy_profile pp;
            CHECK(config_resolve_profile(&pp, &dc, panels[i].w, panels[i].h, KOBOY_GB_W, KOBOY_GB_H, KOBOY_GB_W, KOBOY_GB_H));
            if (pp.scale != 5)
                fprintf(stderr, "  %s %dx%d: scale %d, expected 5\n",
                        panels[i].name, panels[i].w, panels[i].h, pp.scale);
            CHECK_EQ_INT(pp.scale, 5);
        }
    }

    /* Extends the sweep above past the one resolution the Game Boy core can
       ever report. A sweep that only ever exercised 160x144 would leave the
       entire point of this task -- deriving scale/the game rect from the
       CORE's geometry instead of KOBOY_GB_W/KOBOY_GB_H -- unguarded: every
       assertion above would pass identically whether or not
       config_resolve_profile's fitting loop actually reads base_w/max_w/h
       or silently still used the compiled-in Game Boy constants.

       Three of the four cases are REAL geometry, measured by running the
       actual gw-libretro core (build/gw_libretro_host.so) against titles
       from a real 59-title Game & Watch collection -- Parachute, Mario
       Bros., and Donkey Kong -- not values transcribed from the design doc.
       The fourth is a synthetic base != max case (a plausible "folds to a
       smaller view" shape for a multi-screen title, not a reproduction of
       any specific title's behaviour observed here) to exercise the one
       field pairing this sweep exists to distinguish: in KOBOY_LAYOUT_DMG
       game_w/game_h come from base_w/base_h, NOT max_w/max_h, which base ==
       max in the first three cases cannot tell apart.

       That pairing was the other way round until the rect started being
       sized from the frame a core actually draws (see the long note in
       config.c where rect_w is computed). The synthetic row is what moved:
       its scale went from a flat 1 on every panel to 2/2/3/3, because a
       431x322 rect fits where a 692x759 one did not.

       want_scale[] is MEASURED the same way the Game Boy's "5" above is --
       by running config_resolve_profile itself against the shipped
       defaults -- so a fitting-loop change that quietly picks a different
       (even if still valid) scale still fails this test, not just an
       out-of-range one. */
    {
        koboy_config dc; config_defaults(&dc);
        static const struct { int w, h; const char *name; } panels[] = {
            { 1072, 1448, "Clara"  },
            { 1264, 1680, "Libra2" },
            { 1404, 1872, "Elipsa" },
            { 1440, 1920, "Sage"   },
        };
        static const struct {
            int base_w, base_h, max_w, max_h;
            int want_scale[4];       /* Clara, Libra2, Elipsa, Sage */
            const char *name;
        } geoms[] = {
            { 658, 395, 658, 395, { 1, 1, 2, 2 }, "Parachute (measured)"   },
            { 973, 532, 973, 532, { 1, 1, 1, 1 }, "Mario Bros. (measured)" },
            { 606, 748, 606, 748, { 1, 1, 1, 1 }, "Donkey Kong (measured)" },
            { 431, 322, 692, 759, { 2, 2, 3, 3 }, "synthetic base!=max"    },
        };

        for (size_t g = 0; g < sizeof geoms / sizeof geoms[0]; g++) {
            for (size_t i = 0; i < sizeof panels / sizeof panels[0]; i++) {
                koboy_profile pp;
                CHECK(config_resolve_profile(&pp, &dc, panels[i].w, panels[i].h,
                                             geoms[g].base_w, geoms[g].base_h,
                                             geoms[g].max_w, geoms[g].max_h));
                if (pp.scale != geoms[g].want_scale[i])
                    fprintf(stderr, "  %s %s %dx%d: scale %d, expected %d\n",
                            geoms[g].name, panels[i].name, panels[i].w, panels[i].h,
                            pp.scale, geoms[g].want_scale[i]);
                CHECK_EQ_INT(pp.scale, geoms[g].want_scale[i]);

                /* game_w/game_h come from base_w/base_h, not max_w/max_h --
                   asserted directly, not just implied by the scale matching,
                   because a config_resolve_profile that silently sized the
                   rect off max while still reporting the base-derived scale
                   would pass a scale-only check. The first three geometries
                   (base == max) cannot distinguish this at all; only the
                   fourth actually exercises it, and it is the one row whose
                   expected scale moved when the rule did. */
                CHECK_EQ_INT(pp.game_w, geoms[g].base_w * pp.scale);
                CHECK_EQ_INT(pp.game_h, geoms[g].base_h * pp.scale);
                /* ...and it is not merely that max HAPPENS to agree: on the
                   synthetic row the two answers differ by hundreds of
                   pixels, so this is a real discrimination rather than a
                   restatement. */
                if (geoms[g].base_w != geoms[g].max_w)
                    CHECK(pp.game_w != geoms[g].max_w * pp.scale);
                CHECK_EQ_INT(pp.base_w, geoms[g].base_w);
                CHECK_EQ_INT(pp.base_h, geoms[g].base_h);
                CHECK_EQ_INT(pp.max_w,  geoms[g].max_w);
                CHECK_EQ_INT(pp.max_h,  geoms[g].max_h);

                /* The rect must clear the control band -- the exact
                   invariant config_resolve_profile's own comment says this
                   loop exists to keep, now checked against a rect shaped
                   nothing like the Game Boy's 800x720 (5x 160x144), so this
                   is not merely re-running the GB sweep's arithmetic under a
                   new label. */
                int ctrl_top = chrome_controls_top(dc.layout_mode, &dc.layout,
                                                  panels[i].w, panels[i].h);
                CHECK(pp.game_y + pp.game_h <= ctrl_top);

                /* And the bezel margin, the same four sides as the Sage
                   regression below, swept here across every geometry rather
                   than only the compiled-in default. */
                int left   = pp.game_x;
                int right  = panels[i].w - pp.game_x - pp.game_w;
                int top    = pp.game_y;
                int bottom = panels[i].h - pp.game_y - pp.game_h;
                CHECK(left   >= KOBOY_CHROME_MARGIN);
                CHECK(right  >= KOBOY_CHROME_MARGIN);
                CHECK(top    >= KOBOY_CHROME_MARGIN);
                CHECK(bottom >= KOBOY_CHROME_MARGIN);
            }
        }

        /* A degenerate geometry (a caller error, or a core that answered
           retro_get_system_av_info with nothing before this ever runs) must
           be refused, not fed to a division. */
        koboy_profile bad;
        CHECK(!config_resolve_profile(&bad, &dc, 1264, 1680, 0, 0, 0, 0));
        CHECK(!config_resolve_profile(&bad, &dc, 1264, 1680, 100, 100, -1, 50));
        /* BASE is degenerate now too, and it is a separate guard from max's:
           the DMG branch divides the panel by base, so a zero there is a
           division by zero rather than merely a rect nobody wants. Max is
           valid in both of these, so only the base check can refuse them. */
        CHECK(!config_resolve_profile(&bad, &dc, 1264, 1680, 0, 100, 200, 200));
        CHECK(!config_resolve_profile(&bad, &dc, 1264, 1680, 100, -3, 200, 200));
        /* ...and in the LCD layout too, which takes a completely separate
           branch and would otherwise divide by max_w in its own arithmetic. */
        koboy_config lbad; config_defaults(&lbad);
        lbad.layout_mode = KOBOY_LAYOUT_LCD;
        CHECK(!config_resolve_profile(&bad, &lbad, 1264, 1680, 0, 0, 0, 0));
        CHECK(!config_resolve_profile(&bad, &lbad, 1264, 1680, 100, 100, -1, 50));
    }

    /* --------------------------------------------- the LCD layout resolver
     *
     * WHICH LAYOUT, from the ROM's extension alone. Same predicate as
     * config_core_for_rom, deliberately a separate function -- see config.h.
     */
    {
        CHECK_EQ_INT(config_layout_for_rom("mickey.mgw"), KOBOY_LAYOUT_LCD);
        CHECK_EQ_INT(config_layout_for_rom("/a/b/MICKEY.MGW"), KOBOY_LAYOUT_LCD);
        CHECK_EQ_INT(config_layout_for_rom("mickey.MgW"), KOBOY_LAYOUT_LCD);
        CHECK_EQ_INT(config_layout_for_rom("tetris.gb"), KOBOY_LAYOUT_DMG);
        CHECK_EQ_INT(config_layout_for_rom("zelda.gbc"), KOBOY_LAYOUT_DMG);
        /* SNES AND MEGA DRIVE, the two systems whose pads outgrew the DMG
           faceplate's two spare pockets: a SNES is A B X Y L R, a six-button
           Mega Drive is A B C X Y Z, and the LCD strip is the only surface
           koboy has that carries either. Case-insensitive because the
           author's own SNES directory holds 47 files ending .smc and 11
           ending .SMC side by side on FAT32. */
        CHECK_EQ_INT(config_layout_for_rom("Star Fox.sfc"), KOBOY_LAYOUT_LCD);
        CHECK_EQ_INT(config_layout_for_rom("/r/SNES/STAR FOX.SFC"), KOBOY_LAYOUT_LCD);
        CHECK_EQ_INT(config_layout_for_rom("Super Metroid.smc"), KOBOY_LAYOUT_LCD);
        CHECK_EQ_INT(config_layout_for_rom("SUPER METROID.SMC"), KOBOY_LAYOUT_LCD);
        CHECK_EQ_INT(config_layout_for_rom("Sonic.md"), KOBOY_LAYOUT_LCD);
        CHECK_EQ_INT(config_layout_for_rom("SONIC.MD"), KOBOY_LAYOUT_LCD);

        /* THE THREE NEAR MISSES, and each is a real trap rather than a
           formality.

           .sms is a Master System, whose two-button pad fits the faceplate --
           and it shares Genesis Plus GX with .md, so a layout picker written
           against the CORE instead of the extension would move it too. .zip
           is arcade, evaluated for this layout and rejected in
           docs/FOLLOWUPS.md #74 (fractional scaling beats on arcade pixel
           art, and FBNeo's square max would band a vertical board). .pce is
           the system that looks most like it should move and least needs to:
           I, II, RUN and Select are four controls and the faceplate has
           four. */
        CHECK_EQ_INT(config_layout_for_rom("Sonic.sms"), KOBOY_LAYOUT_DMG);
        CHECK_EQ_INT(config_layout_for_rom("galaga.zip"), KOBOY_LAYOUT_DMG);
        CHECK_EQ_INT(config_layout_for_rom("Bonk.pce"), KOBOY_LAYOUT_DMG);
        /* ...and .sm, a strict prefix of both .sms and .smc, must match
           NEITHER -- the same collision ends_with_ext is written to survive
           and the reason .smc could not be a prefix test. */
        CHECK_EQ_INT(config_layout_for_rom("weird.sm"), KOBOY_LAYOUT_DMG);

        /* The systems added after the LCD layout existed and NOT moved to it.
           All are d-pad + face buttons + START/SELECT machines whose whole
           control set fits the DMG faceplate, and a layout picker that keyed
           off "not a Game Boy" rather than off the extension would fail
           exactly here. */
        CHECK_EQ_INT(config_layout_for_rom("metroid.nes"), KOBOY_LAYOUT_DMG);
        CHECK_EQ_INT(config_layout_for_rom("METROID.NES"), KOBOY_LAYOUT_DMG);
        CHECK_EQ_INT(config_layout_for_rom("tetris.min"), KOBOY_LAYOUT_DMG);
        CHECK_EQ_INT(config_layout_for_rom("TETRIS.MIN"), KOBOY_LAYOUT_DMG);
        /* Not a suffix match on "mgw" anywhere in the name, and not fooled by
           a name shorter than the extension -- the same two traps
           ends_with_mgw's own guard exists for. */
        CHECK_EQ_INT(config_layout_for_rom("mgw.gb"), KOBOY_LAYOUT_DMG);
        CHECK_EQ_INT(config_layout_for_rom("gw"), KOBOY_LAYOUT_DMG);
        CHECK_EQ_INT(config_layout_for_rom(""), KOBOY_LAYOUT_DMG);
        CHECK_EQ_INT(config_layout_for_rom(NULL), KOBOY_LAYOUT_DMG);
        /* config_defaults leaves it DMG, which is what every caller that
           never sets it -- including the placeholder profile main.c resolves
           before any ROM is chosen -- relies on. */
        koboy_config d0; config_defaults(&d0);
        CHECK_EQ_INT(d0.layout_mode, KOBOY_LAYOUT_DMG);
        CHECK_EQ_INT(d0.lcd_rect_from_max, false);

        /* WHICH GEOMETRY THE LCD RECT COMES FROM, which is now a per-system
           question and not a property of the layout. Game & Watch is the only
           true: gwlua alternates its frame several times a second and a rect
           following that would repaint the panel at the same rate. The two
           console systems take base, which is what lets the scale ceiling
           bite (asserted below) and what keeps snes9x2005's square 512x512
           max from putting a dead band under a 4:3 picture. */
        CHECK_EQ_INT(config_lcd_rect_from_max_for_rom("mickey.mgw"), true);
        CHECK_EQ_INT(config_lcd_rect_from_max_for_rom("/a/MICKEY.MGW"), true);
        CHECK_EQ_INT(config_lcd_rect_from_max_for_rom("Star Fox.sfc"), false);
        CHECK_EQ_INT(config_lcd_rect_from_max_for_rom("Metroid.smc"), false);
        CHECK_EQ_INT(config_lcd_rect_from_max_for_rom("Sonic.md"), false);
        CHECK_EQ_INT(config_lcd_rect_from_max_for_rom("tetris.gb"), false);
        CHECK_EQ_INT(config_lcd_rect_from_max_for_rom(""), false);
        CHECK_EQ_INT(config_lcd_rect_from_max_for_rom(NULL), false);
    }

    /* THE PER-SYSTEM SCALE CEILING SURVIVES THE MOVE TO THE LCD LAYOUT.
     *
     * This is the whole trap in moving SNES. The ceiling of 3 was added
     * because sizing the rect from the frame a core really draws quadrupled
     * SNES's picture and cost its heaviest titles measured device speed --
     * Star Fox 93% -> 67%, Kirby Super Star 96% -> 78%. The LCD layout fits
     * FRACTIONALLY and to the full panel width, so an uncapped .sfc would get
     * 1264x1106 on the verified panel: 2.3x the area the ceiling holds it to,
     * which is most of the way back to the cost the ceiling was added to
     * avoid.
     *
     * Asserted as a PAIR at identical geometry with only the ceiling
     * differing, which is the only shape that measures the ceiling rather
     * than the arithmetic: a test that asserted 897x672 alone would pass
     * against a resolver that had simply picked a smaller fit for some other
     * reason.
     *
     * 299x224 is the SNES's rect on the device -- 256 source columns widened
     * by snes9x2005's reported 4:3 display aspect (config_resolve_profile_par
     * does the widening; it is passed pre-widened here so this stays a test
     * of the cap). */
    {
        koboy_config sc; config_defaults(&sc);
        sc.layout_mode = KOBOY_LAYOUT_LCD;
        sc.lcd_rect_from_max = false;
        sc.scale_ceiling = config_scale_ceiling_for_rom("Star Fox.sfc");
        CHECK_EQ_INT(sc.scale_ceiling, 3);

        koboy_profile sp;
        CHECK(config_resolve_profile(&sp, &sc, 1264, 1680, 299, 224, 299, 224));
        CHECK_EQ_INT(sp.game_w, 299 * 3);
        CHECK_EQ_INT(sp.game_h, 224 * 3);
        CHECK_EQ_INT(sp.game_w, 897);
        CHECK_EQ_INT(sp.game_h, 672);
        /* Centred in what is left above the strip, and CLEAR of it -- the
           strip carries MENU, the only way back to the browser. */
        int ctop = chrome_controls_top(KOBOY_LAYOUT_LCD, &sc.layout, 1264, 1680);
        CHECK_EQ_INT(sp.game_x, (1264 - 897) / 2);
        CHECK_EQ_INT(sp.game_y, (ctop - 672) / 2);
        CHECK(sp.game_y + sp.game_h <= ctop);

        /* The control: the SAME resolver, the SAME geometry, no ceiling. */
        koboy_config uc = sc;
        uc.scale_ceiling = 0;
        koboy_profile up;
        CHECK(config_resolve_profile(&up, &uc, 1264, 1680, 299, 224, 299, 224));
        CHECK_EQ_INT(up.game_w, 1264);
        CHECK_EQ_INT(up.game_h, 224 * 1264 / 299);
        /* 1.98x, measured -- stated as "nearly twice" rather than a literal
           so a panel-independent claim is what is checked. */
        CHECK(up.game_w * up.game_h > sp.game_w * sp.game_h * 19 / 10);

        /* AND AN EXPLICIT `scale =` DOES NOT DISARM IT, which is where this
           very change nearly shipped a regression. The DMG branch lets an
           explicit scale override the ceiling, because down there the margin
           loop is a second limiter no setting can switch off. This branch has
           no backstop at all -- and the SHIPPED config/koboy.ini sets
           `scale = 5`, so scale_explicit is true on every real device. A cap
           gated on it would have been off everywhere it mattered. */
        koboy_config ec = sc;
        ec.scale_explicit = true;
        ec.scale = 5;
        koboy_profile ep;
        CHECK(config_resolve_profile(&ep, &ec, 1264, 1680, 299, 224, 299, 224));
        CHECK_EQ_INT(ep.game_w, 897);
        CHECK_EQ_INT(ep.game_h, 672);

        /* A system with no ceiling is untouched by any of this -- the Mega
           Drive fits fractionally to the panel width, which is the point of
           the layout. Same call, same resolver, different system. */
        koboy_config mc; config_defaults(&mc);
        mc.layout_mode = KOBOY_LAYOUT_LCD;
        CHECK_EQ_INT(config_scale_ceiling_for_rom("Sonic.md"), 0);
        mc.scale_ceiling = config_scale_ceiling_for_rom("Sonic.md");
        koboy_profile mp2;
        CHECK(config_resolve_profile(&mp2, &mc, 1264, 1680, 293, 224, 348, 240));
        CHECK_EQ_INT(mp2.game_w, 1264);
    }

    /* WHAT THE LCD STRIP'S CONTROLS SAY, per system. The bits do not change
       -- the strip's geometry is one thing for all three systems -- so this
       is entirely about the words, and the words are read off the cores'
       descriptor tables. test_chrome.c asserts they reach the panel; this
       asserts the table itself. */
    {
        koboy_layout l;
        koboy_config lc; config_defaults(&lc); l = lc.layout;

        /* THE MEGA DRIVE, where every one of these differs from the retropad
           name the strip would otherwise print. JOYPAD_A is the console's C,
           JOYPAD_Y its A, JOYPAD_X its Y, JOYPAD_L its X, JOYPAD_R its Z and
           JOYPAD_SELECT its MODE; only JOYPAD_B is B. A disc drawn "A" over
           JOYPAD_A is a lie a player acts on. */
        config_lcd_labels_for_rom(&l, "/roms/Streets of Rage 2 (USA).md");
        CHECK(strcmp(l.lcd.x, "Y") == 0);          /* diamond top    */
        CHECK(strcmp(l.lcd.y, "A") == 0);          /* diamond left   */
        CHECK(strcmp(l.lcd.a, "C") == 0);          /* diamond right  */
        CHECK(strcmp(l.lcd.b, "B") == 0);          /* diamond bottom */
        CHECK(strcmp(l.lcd.l1, "X") == 0);
        CHECK(strcmp(l.lcd.r1, "Z") == 0);
        CHECK(strcmp(l.lcd.select, "MODE") == 0);
        /* All six of a six-button pad are present and DISTINCT. A table that
           repeated a name would draw two discs the player cannot tell apart,
           and this catches it without naming the pair. */
        {
            const char *six[6] = { l.lcd.x, l.lcd.y, l.lcd.a, l.lcd.b,
                                   l.lcd.l1, l.lcd.r1 };
            for (int i = 0; i < 6; i++)
                for (int j = i + 1; j < 6; j++)
                    CHECK(strcmp(six[i], six[j]) != 0);
        }

        /* THE SNES, where the retropad IS the pad and only the shoulders are
           renamed: the console calls them L and R, and the "1" in the
           retropad's L1/R1 says there is a second pair to look for. */
        config_lcd_labels_for_rom(&l, "/roms/Star Fox (USA) (Rev 2).sfc");
        CHECK(strcmp(l.lcd.x, "X") == 0);
        CHECK(strcmp(l.lcd.y, "Y") == 0);
        CHECK(strcmp(l.lcd.a, "A") == 0);
        CHECK(strcmp(l.lcd.b, "B") == 0);
        CHECK(strcmp(l.lcd.l1, "L") == 0);
        CHECK(strcmp(l.lcd.r1, "R") == 0);
        CHECK(strcmp(l.lcd.select, "SELECT") == 0);
        /* .smc is the same cartridge behind a different dumping convention
           and must get the same table, byte for byte. */
        {
            koboy_layout l2 = lc.layout;
            config_lcd_labels_for_rom(&l2, "/roms/Super Metroid.smc");
            CHECK(memcmp(&l2.lcd, &l.lcd, sizeof l.lcd) == 0);
        }

        /* GAME & WATCH KEEPS THE RETROPAD NAMES, encoded as an EMPTY table --
           gw-libretro's own overlay speaks retropad, and a player consulting
           it has to find the same button here. Asserted after a .sfc, so this
           is also the clear-on-every-call contract: without the memset the
           strip would keep saying L and R on the next Game & Watch. */
        config_lcd_labels_for_rom(&l, "/roms/Donkey Kong.mgw");
        koboy_lcd_labels zero;
        memset(&zero, 0, sizeof zero);
        CHECK(memcmp(&l.lcd, &zero, sizeof zero) == 0);
        /* And so does everything that never reaches this layout, so a system
           moved here later starts from the retropad rather than from whatever
           the last ROM left behind. */
        config_lcd_labels_for_rom(&l, "/roms/Star Fox.sfc");
        config_lcd_labels_for_rom(&l, "/roms/Metroid.nes");
        CHECK(memcmp(&l.lcd, &zero, sizeof zero) == 0);
        config_lcd_labels_for_rom(&l, "/roms/Star Fox.sfc");
        config_lcd_labels_for_rom(&l, NULL);
        CHECK(memcmp(&l.lcd, &zero, sizeof zero) == 0);
        /* config_defaults leaves it empty too. */
        CHECK(memcmp(&lc.layout.lcd, &zero, sizeof zero) == 0);
    }

    /* The reserved rect in the LCD layout: full panel width, everything above
       the bottom strip, aspect preserved, FRACTIONAL. Asserted three ways --
       exact numbers on the verified panel, structural properties across every
       supported panel, and (the point of the whole layout) that the result is
       dramatically bigger than the integer fit the DMG branch would give.
       The last is what a "fractional fit" that silently floored to an integer
       multiple would fail; the first two would not catch it for every title. */
    {
        koboy_config lc; config_defaults(&lc);
        lc.layout_mode = KOBOY_LAYOUT_LCD;
        /* GAME & WATCH, which is the only system that sizes this rect from
           MAX -- see config_lcd_rect_from_max_for_rom. Set explicitly, because
           config_defaults leaves it false and every geometry below is a .mgw
           title's. The two console systems that share this layout take the
           other branch and are asserted separately. */
        lc.lcd_rect_from_max = true;
        koboy_profile lp;

        /* MEASURED geometries, from running the real gw-libretro core --
           the same three tests/test_config.c's DMG sweep already uses. The
           expected rects below are on the ONE verified panel (Libra 2,
           1264x1680), where the strip is 250 permille = 420 px and the rect
           therefore fits into 1264x1260. (It was 120 px until the strip had
           to carry a real retropad -- see chrome.h.) */
        CHECK(config_resolve_profile(&lp, &lc, 1264, 1680, 654, 396, 654, 396));
        CHECK_EQ_INT(lp.layout_mode, KOBOY_LAYOUT_LCD);
        CHECK_EQ_INT(lp.game_w, 1264);          /* Mickey Mouse: width binds */
        CHECK_EQ_INT(lp.game_h, 765);
        CHECK_EQ_INT(lp.game_x, 0);
        CHECK_EQ_INT(lp.game_y, (1260 - 765) / 2);

        /* Donkey Kong, 606x748 -- the TALLEST measured title, and the one the
           strip height costs the most: HEIGHT binds here, so it is the case
           that proves both axes are fitted rather than just the width, and
           the case that would notice if the strip ever grew again. Still
           1.68x, against the 1x the device reported before this layout
           existed, which is the trade the taller strip buys. */
        CHECK(config_resolve_profile(&lp, &lc, 1264, 1680, 606, 748, 606, 748));
        CHECK_EQ_INT(lp.game_h, 1260);
        CHECK_EQ_INT(lp.game_w, 606 * 1260 / 748);
        CHECK_EQ_INT(lp.game_y, 0);
        CHECK(lp.game_w > 606);                 /* bigger than 1:1, the whole point */

        /* Mario Bros., 973x532 -- the widest measured title. */
        CHECK(config_resolve_profile(&lp, &lc, 1264, 1680, 973, 532, 973, 532));
        CHECK_EQ_INT(lp.game_w, 1264);
        CHECK_EQ_INT(lp.game_h, 532 * 1264 / 973);

        /* base != max is carried through untouched, and the rect is sized off
           MAX -- the invariant that keeps a legitimately larger frame from
           spilling onto the strip. Only a base != max case can tell the two
           apart, exactly as in the DMG sweep above. This is Donkey Kong
           zoomed to its LCD alone (305x191 of a 654x396 unit), which is the
           real case: it happens several times a second, and the rect must not
           move when it does. */
        CHECK(config_resolve_profile(&lp, &lc, 1264, 1680, 305, 191, 654, 396));
        CHECK_EQ_INT(lp.base_w, 305);
        CHECK_EQ_INT(lp.base_h, 191);
        CHECK_EQ_INT(lp.max_w, 654);
        CHECK_EQ_INT(lp.max_h, 396);
        CHECK_EQ_INT(lp.game_w, 1264);          /* from 654x396, not 305x191 */
        CHECK_EQ_INT(lp.game_h, 765);

        /* AND THE OTHER SIDE OF THAT SWITCH, on the SAME geometry, so the
           only thing that differs between the two answers is the flag. A
           system that does NOT churn its base at video rate sizes the rect
           from what the core is drawing NOW, and 305x191 is a taller aspect
           than 654x396, so the height must come out DIFFERENT -- 791, not
           765. Asserting the changed number rather than "it still resolves"
           is what makes this fail if the flag is ignored; the previous
           version of this block passed with only one behaviour implemented,
           because the max-sized answer was also the only answer. */
        {
            koboy_config bc = lc;
            bc.lcd_rect_from_max = false;
            koboy_profile bp;
            CHECK(config_resolve_profile(&bp, &bc, 1264, 1680, 305, 191, 654, 396));
            CHECK_EQ_INT(bp.game_w, 1264);
            CHECK_EQ_INT(bp.game_h, 191 * 1264 / 305);
            CHECK_EQ_INT(bp.game_h, 791);
            CHECK(bp.game_h != lp.game_h);
        }

        static const struct { int w, h; const char *name; } panels[] = {
            { 1072, 1448, "Clara"  },
            { 1264, 1680, "Libra2" },
            { 1404, 1872, "Elipsa" },
            { 1440, 1920, "Sage"   },
        };
        static const struct { int w, h; const char *name; } geoms[] = {
            { 654, 396, "Mickey Mouse (measured)" },
            { 658, 395, "Parachute (measured)"    },
            { 973, 532, "Mario Bros. (measured)"  },
            { 606, 748, "Donkey Kong (measured)"  },
            { 128, 128, "the load-time placeholder" },
        };

        for (size_t g = 0; g < sizeof geoms / sizeof geoms[0]; g++) {
            for (size_t i = 0; i < sizeof panels / sizeof panels[0]; i++) {
                koboy_profile pp;
                CHECK(config_resolve_profile(&pp, &lc, panels[i].w, panels[i].h,
                                             geoms[g].w, geoms[g].h,
                                             geoms[g].w, geoms[g].h));
                int strip    = chrome_lcd_strip_h(panels[i].h);
                int ctrl_top = chrome_controls_top(KOBOY_LAYOUT_LCD, &lc.layout,
                                                   panels[i].w, panels[i].h);
                CHECK_EQ_INT(ctrl_top, panels[i].h - strip);

                /* CLEAR OF THE BOTTOM STRIP. The same invariant the DMG
                   branch keeps against the control band, and for the same
                   reason: MENU's touch zone lives down there and stays live
                   under anything drawn over it. */
                if (pp.game_y + pp.game_h > ctrl_top)
                    fprintf(stderr, "  %s on %s: rect %dx%d at (%d,%d) reaches %d,"
                            " strip starts at %d\n",
                            geoms[g].name, panels[i].name, pp.game_w, pp.game_h,
                            pp.game_x, pp.game_y, pp.game_y + pp.game_h, ctrl_top);
                CHECK(pp.game_y + pp.game_h <= ctrl_top);

                /* Inside the panel horizontally, and centred. */
                CHECK(pp.game_x >= 0);
                CHECK(pp.game_x + pp.game_w <= panels[i].w);
                CHECK_EQ_INT(pp.game_x, (panels[i].w - pp.game_w) / 2);
                CHECK_EQ_INT(pp.game_y, (ctrl_top - pp.game_h) / 2);

                /* One axis is filled EXACTLY -- the fit is a fit, not an
                   approximation that happens to land inside. */
                CHECK(pp.game_w == panels[i].w || pp.game_h == ctrl_top);

                /* Aspect preserved to within one pixel of truncation on the
                   non-binding axis. */
                long cross = (long)pp.game_w * geoms[g].h - (long)pp.game_h * geoms[g].w;
                long tol   = geoms[g].w > geoms[g].h ? geoms[g].w : geoms[g].h;
                if (cross > tol || cross < -tol)
                    fprintf(stderr, "  %s on %s: aspect error %ld > %ld\n",
                            geoms[g].name, panels[i].name, cross, tol);
                CHECK(cross <= tol && cross >= -tol);

                /* AND STRICTLY BIGGER THAN THE INTEGER FIT. This is the
                   reported problem -- "too small", a G&W unit using about
                   half the panel width at integer scale 1 -- so it is
                   asserted directly rather than inferred from the numbers
                   above. A fractional fit that floored to an integer
                   multiple would satisfy every other check here. */
                int ifit_w = panels[i].w / geoms[g].w;
                int ifit_h = ctrl_top / geoms[g].h;
                int ifit   = ifit_w < ifit_h ? ifit_w : ifit_h;
                if (ifit < 1) ifit = 1;
                if (pp.game_w < geoms[g].w * ifit)
                    fprintf(stderr, "  %s on %s: fractional %d < integer %d\n",
                            geoms[g].name, panels[i].name, pp.game_w, geoms[g].w * ifit);
                CHECK(pp.game_w >= geoms[g].w * ifit);
                /* At least one of the five geometries must be STRICTLY
                   better, or "fractional" bought nothing at all. Mickey
                   Mouse on the verified panel is 1264 wide against an
                   integer 654; asserted per-case for the ones that cannot
                   land on an exact multiple by luck. */
                if (geoms[g].w != 128)
                    CHECK(pp.game_w > geoms[g].w * ifit);
            }
        }

        /* The DMG layout is NOT affected by any of this: the same config
           with layout_mode back at DMG must still resolve the Game Boy to
           exactly scale 5, 800x720 at (232,84), on the verified panel. The
           four-panel DMG sweep above has been broken by a layout change
           before; this is the same pin restated right beside the new branch
           so the pair is read together. */
        koboy_config dmg; config_defaults(&dmg);
        koboy_profile dp;
        CHECK(config_resolve_profile(&dp, &dmg, 1264, 1680,
                                     KOBOY_GB_W, KOBOY_GB_H, KOBOY_GB_W, KOBOY_GB_H));
        CHECK_EQ_INT(dp.layout_mode, KOBOY_LAYOUT_DMG);
        CHECK_EQ_INT(dp.scale, 5);
        CHECK_EQ_INT(dp.game_w, 800);
        CHECK_EQ_INT(dp.game_h, 720);
        CHECK_EQ_INT(dp.game_x, 232);
        CHECK_EQ_INT(dp.game_y, 84);
    }

    /* ini overrides defaults; unknown keys are ignored, not fatal */
    FILE *f = fopen("build/t.ini", "w");
    fprintf(f, "# comment\nscale = 4\npresent_divisor=2\nrom = /x/y.gb\n"
               "grab_input = false\nnonsense_key = 1\n");
    fclose(f);
    config_defaults(&c);
    CHECK(config_load(&c, "build/t.ini"));
    CHECK_EQ_INT(c.scale, 4);
    CHECK_EQ_INT(c.present_divisor, 2);
    CHECK(strcmp(c.rom_path, "/x/y.gb") == 0);
    CHECK(!c.grab_input);

    /* key_start/key_select round-trip through the ini the same way key_a/
       key_b already do -- the distinguishing part is that the file's values
       (304/305, a real gamepad's A/B codes, deliberately DIFFERENT from the
       310/311 shipped default) are what come back, not the default surviving
       unnoticed. */
    f = fopen("build/t2.ini", "w");
    fprintf(f, "key_start = 304\nkey_select = 305\n");
    fclose(f);
    config_defaults(&c);
    CHECK_EQ_INT(c.key_start, 310);   /* default, before load */
    CHECK_EQ_INT(c.key_select, 311);
    CHECK(config_load(&c, "build/t2.ini"));
    CHECK_EQ_INT(c.key_start, 304);
    CHECK_EQ_INT(c.key_select, 305);

    /* a missing file is not an error: defaults stand */
    config_defaults(&c);
    CHECK(config_load(&c, "build/definitely-absent.ini"));
    CHECK_EQ_INT(c.scale, 5);

    /* config_save_keys is idempotent: calling it twice on the same file with
       different values should result in the file containing exactly one key_a
       line and one key_b line with the latest values. */
    CHECK(config_save_keys("build/keys1.ini", 111, 222));
    CHECK(config_save_keys("build/keys1.ini", 333, 444));

    /* Count occurrences of key_a and key_b in the file */
    FILE *count_f = fopen("build/keys1.ini", "r");
    CHECK(count_f);
    int key_a_count = 0, key_b_count = 0;
    char line[1024];
    while (fgets(line, sizeof line, count_f)) {
        if (strstr(line, "key_a =")) key_a_count++;
        if (strstr(line, "key_b =")) key_b_count++;
    }
    fclose(count_f);
    CHECK_EQ_INT(key_a_count, 1);
    CHECK_EQ_INT(key_b_count, 1);

    /* Verify the values are the latest ones */
    config_defaults(&c);
    CHECK(config_load(&c, "build/keys1.ini"));
    CHECK_EQ_INT(c.key_a, 333);
    CHECK_EQ_INT(c.key_b, 444);

    /* Preservation: seed a file with a comment, an unrelated key, and a REAL
       key_a line. Call config_save_keys. Verify the comment and unrelated key
       survive, and that the filter actually removed the old key_a line.
       The previous seed here was literally "old key_a = 999" -- a line the
       exact strcmp in config_save_keys' filter never matches, since the key
       name it extracts is "old key_a", not "key_a". That made the filter
       untested: the line survived whether or not the filter existed, and
       still would if the filter were deleted entirely. */
    FILE *seed_f = fopen("build/keys2.ini", "w");
    fprintf(seed_f, "# important comment\nkey_a = 99\nscale = 4\nkey_b = 98\n");
    fclose(seed_f);
    CHECK(config_save_keys("build/keys2.ini", 555, 666));

    /* Verify comment and scale survived */
    config_defaults(&c);
    CHECK(config_load(&c, "build/keys2.ini"));
    CHECK_EQ_INT(c.scale, 4);
    CHECK_EQ_INT(c.key_a, 555);
    CHECK_EQ_INT(c.key_b, 666);

    /* Verify the comment is actually in the file (not just parsed) */
    FILE *verify_f = fopen("build/keys2.ini", "r");
    bool found_comment = false;
    bool found_old_key_a = false;
    int keys2_key_a_count = 0, keys2_key_b_count = 0;
    while (fgets(line, sizeof line, verify_f)) {
        if (strstr(line, "important comment")) found_comment = true;
        if (strstr(line, "key_a = 99")) found_old_key_a = true;
        if (strstr(line, "key_a =")) keys2_key_a_count++;
        if (strstr(line, "key_b =")) keys2_key_b_count++;
    }
    fclose(verify_f);
    CHECK(found_comment);
    /* The old key_a = 99 line must be GONE -- this is the check that the
       previous "old key_a" seed could never fail, because it never matched
       the filter to begin with. */
    CHECK(!found_old_key_a);
    CHECK_EQ_INT(keys2_key_a_count, 1);
    CHECK_EQ_INT(keys2_key_b_count, 1);

    /* Failure path: attempt to save to a non-existent directory.
       Should return false and leave no temp file. */
    bool result = config_save_keys("/nonexistent/dir/keys.ini", 777, 888);
    CHECK(!result);
    CHECK(!fopen("/nonexistent/dir/keys.ini.tmp", "r"));  /* temp file cleaned up */

    /* Chrome margin regression: On a 1440x1920 panel (Kobo Sage), auto-scale
       and explicit scale=9 must both reserve at least KOBOY_CHROME_MARGIN pixels
       on all sides so the bezel doesn't write outside the buffer. */
    config_defaults(&c);
    c.scale = 0;  /* auto */
    CHECK(config_resolve_profile(&p, &c, 1440, 1920, KOBOY_GB_W, KOBOY_GB_H, KOBOY_GB_W, KOBOY_GB_H));
    int left   = p.game_x;
    int right  = 1440 - p.game_x - p.game_w;
    int top    = p.game_y;
    int bottom = 1920 - p.game_y - p.game_h;
    CHECK(left >= KOBOY_CHROME_MARGIN);
    CHECK(right >= KOBOY_CHROME_MARGIN);
    CHECK(top >= KOBOY_CHROME_MARGIN);
    CHECK(bottom >= KOBOY_CHROME_MARGIN);

    /* Same constraint with explicit scale=9 */
    c.scale = 9;
    CHECK(config_resolve_profile(&p, &c, 1440, 1920, KOBOY_GB_W, KOBOY_GB_H, KOBOY_GB_W, KOBOY_GB_H));
    left   = p.game_x;
    right  = 1440 - p.game_x - p.game_w;
    top    = p.game_y;
    bottom = 1920 - p.game_y - p.game_h;
    CHECK(left >= KOBOY_CHROME_MARGIN);
    CHECK(right >= KOBOY_CHROME_MARGIN);
    CHECK(top >= KOBOY_CHROME_MARGIN);
    CHECK(bottom >= KOBOY_CHROME_MARGIN);

    /* #8: a blanked value must not silently mean true. as_bool treated
       everything except "false" and "0" as true, including "", so
       `grab_input = ` turned the grab ON -- the opposite of what someone
       clearing a line intends, and unrecoverable without reading the source. */
    {
        char dir[] = "/tmp/koboy_cfg_XXXXXX";
        CHECK(mkdtemp(dir) != NULL);
        char path[512];
        snprintf(path, sizeof path, "%s/koboy.ini", dir);

        /* force_dither's built-in default is false, so blanking it and
           comparing against that default already distinguishes fixed from
           broken: the old as_bool("") returns true, which does not equal
           false. */
        FILE *f = fopen(path, "w");
        CHECK(f != NULL);
        fputs("force_dither =   \n", f);
        fclose(f);

        koboy_config c; config_defaults(&c);
        bool dither_default = c.force_dither;
        CHECK(config_load(&c, path));
        CHECK_EQ_INT(c.force_dither, dither_default);

        /* grab_input's built-in default is true, which the broken
           as_bool("") also returned -- so comparing a blanked value against
           the compiled-in default cannot fail either way, whichever as_bool
           is linked in. That was a vacuous assertion, precisely the defect
           class this task exists to remove. Fix it by making the assertion
           DISTINGUISHING: set grab_input false on an earlier line, then
           blank it on a later one. The fixed code must leave the prior
           (false) value alone; the broken code flips it back to true. */
        f = fopen(path, "w");
        CHECK(f != NULL);
        fputs("grab_input = false\n", f);
        fclose(f);
        config_defaults(&c);
        CHECK(config_load(&c, path));
        CHECK_EQ_INT(c.grab_input, 0);

        f = fopen(path, "w");
        CHECK(f != NULL);
        fputs("grab_input = false\ngrab_input = \n", f);
        fclose(f);
        config_defaults(&c);
        CHECK(config_load(&c, path));
        CHECK_EQ_INT(c.grab_input, 0);      /* blank did not resurrect it */

        /* Explicit values still work in both directions. */
        f = fopen(path, "w");
        CHECK(f != NULL);
        fputs("grab_input = false\nforce_dither = true\n", f);
        fclose(f);
        config_defaults(&c);
        CHECK(config_load(&c, path));
        CHECK_EQ_INT(c.grab_input, 0);
        CHECK_EQ_INT(c.force_dither, 1);

        /* #9: an ini with NO trailing newline must not have the calibration
           block concatenated onto its last line. It is harmless today only
           because config_load truncates at the resulting '#', but it silently
           rewrites an unrelated line, against the whole point of preserving
           everything else. */
        f = fopen(path, "w");
        CHECK(f != NULL);
        fputs("scale = 4", f);            /* deliberately no newline */
        fclose(f);
        CHECK(config_save_keys(path, 193, 194));

        f = fopen(path, "r");
        CHECK(f != NULL);
        char first[256] = {0};
        CHECK(fgets(first, sizeof first, f) != NULL);
        fclose(f);
        /* The first line must still be exactly the scale line. */
        CHECK(strncmp(first, "scale = 4", 9) == 0);
        CHECK_EQ_INT((int)strlen(first), 10);   /* "scale = 4" plus '\n' */

        /* And the keys survived the round trip. */
        config_defaults(&c);
        CHECK(config_load(&c, path));
        CHECK_EQ_INT(c.scale, 4);
        CHECK_EQ_INT(c.key_a, 193);
        CHECK_EQ_INT(c.key_b, 194);

        char cmd[1024];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
        if (system(cmd) != 0) fprintf(stderr, "NOTE: cleanup failed\n");
    }

    /* ----------------------------------------------- core by extension */
    {
        /* Game & Watch content goes to gw-libretro; everything else the
           browser lists goes to gambatte. Case-insensitive, matching the
           browser filter, because a collection copied off a PC has both
           cases in it. */
        CHECK(strcmp(config_core_for_rom("BALL.mgw"), "gw_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("BALL.MGW"), "gw_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("BALL.Mgw"), "gw_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("/mnt/onboard/.adds/koboy/roms/OCTOPUS.mgw"),
                     "gw_libretro.so") == 0);

        CHECK(strcmp(config_core_for_rom("ZELDA.gb"), "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("ZELDA.GB"), "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("KIRBY.gbc"), "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("KIRBY.GBC"), "gambatte_libretro.so") == 0);
        /* An extensionless name, and the empty/NULL rom_path a run that
           reaches core_open with nothing chosen would carry. Falling back to
           gambatte keeps that run failing exactly the way it used to, in
           core_open, rather than crashing here. */
        CHECK(strcmp(config_core_for_rom("SOMETHING"), "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom(""), "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom(NULL), "gambatte_libretro.so") == 0);
        /* Superstring and prefix, same trap as the browser filter. */
        CHECK(strcmp(config_core_for_rom("BALL.mgwx"), "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("BALL.mg"), "gambatte_libretro.so") == 0);

        /* .nes -> fceumm, .min -> PokeMini. The gambatte assertions above are
           what make these mean something: a table that answered the LAST
           matching entry for everything, or a fall-through that stopped
           falling through, would break one of the two halves. */
        CHECK(strcmp(config_core_for_rom("METROID.nes"), "fceumm_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("METROID.NES"), "fceumm_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("METROID.NeS"), "fceumm_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("/mnt/onboard/.adds/koboy/roms/NES/Metroid (USA).nes"),
                     "fceumm_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("TETRIS.min"), "pokemini_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("TETRIS.MIN"), "pokemini_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("TETRIS.MiN"), "pokemini_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("/roms/PokemonMini/Pokemon Tetris (Europe) (En,Ja,Fr).min"),
                     "pokemini_libretro.so") == 0);
        /* Superstring and prefix for both new extensions. */
        CHECK(strcmp(config_core_for_rom("METROID.nesx"), "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("METROID.ne"), "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("TETRIS.minx"), "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("TETRIS.mi"), "gambatte_libretro.so") == 0);
        /* .ws/.wsc -> beetle-wswan, .ngp/.ngc -> RACE. TWO extensions each,
           because both systems have a mono and a Color half that one core
           covers, and the pair has to answer the SAME file -- a table row
           copy-pasted with the wrong .so would still satisfy every
           per-extension assertion on its own. The uppercase variants are not
           decoration either: the device partition is FAT32 and the author's
           own collections carry mixed case. */
        CHECK(strcmp(config_core_for_rom("GUNPEY.ws"),
                     "mednafen_wswan_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("GUNPEY.WS"),
                     "mednafen_wswan_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("/roms/wonderswancolor/Final Fantasy (Japan).wsc"),
                     "mednafen_wswan_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("FF.WSC"),
                     "mednafen_wswan_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("GUNPEY.ws"), config_core_for_rom("FF.wsc")) == 0);

        CHECK(strcmp(config_core_for_rom("SONIC.ngp"), "race_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("SONIC.NGP"), "race_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("/roms/ngpc/Metal Slug - 1st Mission (World) (En,Ja).ngc"),
                     "race_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("MS.NGC"), "race_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.ngp"), config_core_for_rom("A.ngc")) == 0);

        /* Superstring and prefix for all four, same trap as above. `.ngc` and
           `.ngp` are one character apart from each other AND from `.ng`, and
           `.ws` is a strict prefix of `.wsc` -- the case a suffix matcher gets
           wrong by comparing too few characters. */
        CHECK(strcmp(config_core_for_rom("A.wsx"), "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.w"),   "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.wscx"), "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.ngpx"), "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.ng"),   "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.ngcx"), "gambatte_libretro.so") == 0);
        /* Not the WonderSwan's third extension, nor the Neo Geo Pocket's
           other two: the cores accept .pc2/.ngpc/.npc but no collection the
           author has uses them, and an extension the browser lists but nobody
           has ever loaded is an untested claim. Asserted so that adding one
           later is a deliberate act. */
        CHECK(strcmp(config_core_for_rom("A.pc2"),  "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.ngpc"), "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.npc"),  "gambatte_libretro.so") == 0);

        /* .a26 -> stella2014, .col -> Gearcoleco, .int -> FreeIntv, and
           .sms/.gg -> Genesis Plus GX. The Sega pair is the second family
           where one .so answers two extensions, and the first where the two
           are not a mono/colour pair, so the same "both rows point at the
           same file" check applies. Uppercase is not decoration here either:
           the author's Game Gear directory really does carry 38 .gg and 15
           .GG side by side. */
        CHECK(strcmp(config_core_for_rom("ADVENTURE.a26"),
                     "stella2014_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("ADVENTURE.A26"),
                     "stella2014_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("/roms/atari2600/Yars' Revenge (USA).a26"),
                     "stella2014_libretro.so") == 0);

        CHECK(strcmp(config_core_for_rom("BURGERTIME.col"),
                     "gearcoleco_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("BURGERTIME.COL"),
                     "gearcoleco_libretro.so") == 0);

        CHECK(strcmp(config_core_for_rom("ATLANTIS.int"),
                     "freeintv_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("ATLANTIS.INT"),
                     "freeintv_libretro.so") == 0);

        CHECK(strcmp(config_core_for_rom("ALEXKIDD.sms"),
                     "genesis_plus_gx_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("ALEXKIDD.SMS"),
                     "genesis_plus_gx_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("/roms/gamegear/Crystal Warriors (USA, Europe).gg"),
                     "genesis_plus_gx_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("Battletoads (USA).GG"),
                     "genesis_plus_gx_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.sms"), config_core_for_rom("A.gg")) == 0);

        /* Superstring and prefix for the five new extensions. `.gg` is the
           dangerous one: it is two characters, it is a strict SUFFIX of
           nothing here but a strict PREFIX of nothing either -- which is
           exactly why a matcher that compared too few bytes would route
           "A.g" or "A.ggx" to Genesis Plus GX and nobody would notice until
           a Game Boy title landed on a Sega core. */
        CHECK(strcmp(config_core_for_rom("A.a26x"), "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.a2"),   "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.colx"), "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.co"),   "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.intx"), "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.in"),   "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.smsx"), "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.sm"),   "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.ggx"),  "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.g"),    "gambatte_libretro.so") == 0);
        /* .md -> Genesis Plus GX, THE SAME .so as .sms and .gg. Genesis Plus
           GX was always natively a Mega Drive core; what changed is that one
           extension of the list it advertises is now claimed. The
           same-file assertion is the one that matters: a third row that
           named a separate Mega Drive core would satisfy every other check
           here and ship a .so that does not exist. */
        CHECK(strcmp(config_core_for_rom("SONIC.md"),
                     "genesis_plus_gx_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("SONIC.MD"),
                     "genesis_plus_gx_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("/roms/megadrive/Streets of Rage 2 (USA).md"),
                     "genesis_plus_gx_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.md"), config_core_for_rom("A.sms")) == 0);
        CHECK(strcmp(config_core_for_rom("A.md"), config_core_for_rom("A.gg")) == 0);

        /* THE MEGA DRIVE'S OTHER TWO EXTENSIONS ARE REFUSED, and this is the
           assertion that makes that a decision instead of a gap. The owner
           ruled: .md only. 31 .bin and 5 .gen files in their collection do
           not list and do not load.

           `.bin` is the load-bearing half. koboy picks a core from the
           extension and has NO other signal, and .bin was counted across the
           author's whole collection before this was settled: 723 TI-99/4A,
           234 Odyssey 2, 119 Atari 5200, 72 Arcadia 2001, 71 Vectrex, 68
           Astrocade, 56 VC 4000, 38 Jaguar -- and only then 36 Mega Drive.
           Two more of them are exec.bin and grom.bin, the Intellivision BIOS
           this project asks the owner to install by hand. Claiming .bin
           would route all of that to a 68000 emulator, so the exec.bin case
           below is not a curiosity: it is the file the rule exists for. */
        CHECK(strcmp(config_core_for_rom("A.bin"),    "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.BIN"),    "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("exec.bin"), "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("grom.bin"), "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.gen"),    "gambatte_libretro.so") == 0);
        /* Still not .sg (SG-1000), nor stella2014's .mvc, for the reason
           .pc2 and .ngpc are absent: no collection here has them. */
        CHECK(strcmp(config_core_for_rom("A.sg"),  "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.mvc"), "gambatte_libretro.so") == 0);

        /* .sfc/.smc -> snes9x2005. The system the v1 design spec ruled out on
           CPU grounds; see TESTED.md for the measurement that overturned it.
           Two extensions, one core -- the WonderSwan pattern -- so the
           same-file check applies here too. The mixed case is real: 47 .smc
           and 11 .SMC in one of the author's directories. */
        CHECK(strcmp(config_core_for_rom("MARIO.sfc"),
                     "snes9x2005_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("MARIO.SFC"),
                     "snes9x2005_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("/roms/snes/Super Metroid (Japan, USA) (En,Ja).sfc"),
                     "snes9x2005_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.smc"),
                     "snes9x2005_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.SMC"),
                     "snes9x2005_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.sfc"), config_core_for_rom("A.smc")) == 0);
        /* The other historical copier extensions are NOT claimed. */
        CHECK(strcmp(config_core_for_rom("A.fig"), "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.swc"), "gambatte_libretro.so") == 0);

        /* .pce -> beetle-pce-fast, CARTRIDGE PC ENGINE ONLY. */
        CHECK(strcmp(config_core_for_rom("BONK.pce"),
                     "mednafen_pce_fast_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("BONK.PCE"),
                     "mednafen_pce_fast_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("/roms/pcengine/R-Type (USA).pce"),
                     "mednafen_pce_fast_libretro.so") == 0);
        /* .sgx AND .chd ARE REFUSED, and both are decisions worth an
           assertion rather than silence. beetle-pce-fast implements neither
           the SuperGrafx's second VDC nor its priority mixer, so it would
           render an .sgx WRONGLY rather than refuse it -- the failure this
           project treats as worse than absence, and the reason 7 files stay
           invisible instead of appearing and misbehaving. .chd and the rest
           of the CD family need a system-card BIOS nobody ships. */
        CHECK(strcmp(config_core_for_rom("A.sgx"), "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.SGX"), "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.chd"), "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.toc"), "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.m3u"), "gambatte_libretro.so") == 0);

        /* Superstring and prefix for the four new extensions, the same guard
           every other row gets. `.md` is the dangerous one this time: two
           characters, and a matcher one byte short would route every file
           ending in a `d` -- .a26 does not, but a hypothetical ".d" would --
           to a Mega Drive core. */
        CHECK(strcmp(config_core_for_rom("A.mdx"), "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.m"),   "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.sfcx"),"gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.sf"),  "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.smcx"),"gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.pcex"),"gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.pc"),  "gambatte_libretro.so") == 0);
        /* .sm is a strict prefix of BOTH .sms and .smc now, which is exactly
           the collision a two-row table invites. Neither may claim it. */
        CHECK(strcmp(config_core_for_rom("A.sm"),  "gambatte_libretro.so") == 0);

        /* .zip -> FinalBurn Neo, the first extension in this table that names
           a CONTAINER rather than a system. Uppercase because the device
           partition is FAT32 and a set unpacked on Windows can be either.

           The path cases are the ones that matter for arcade specifically: an
           FBNeo romset is loaded BY PATH (need_fullpath), and its members --
           the parent set, the device zips a board needs beside it -- are
           found by FBNeo in the same directory. A name with spaces and a
           directory prefix has to route the same as a bare one. */
        CHECK(strcmp(config_core_for_rom("galaga.zip"),
                     "fbneo_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("GALAGA.ZIP"),
                     "fbneo_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("/roms/fbneo/mspacman.zip"),
                     "fbneo_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("/mnt/onboard/.adds/koboy/roms/arcade/dkong.Zip"),
                     "fbneo_libretro.so") == 0);
        /* Superstring and prefix, the same guard every other row gets. */
        CHECK(strcmp(config_core_for_rom("A.zipx"), "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.zi"),   "gambatte_libretro.so") == 0);
        /* FBNeo's OTHER advertised extensions, deliberately unclaimed. .7z is
           not merely unlisted: scripts/build-fbneo-core.sh compiles 7-Zip
           support OUT of the device build (lib7z does not build against glibc
           2.19's headers), so a .zip routed here can be opened and a .7z
           could not be even if this row existed. .cue/.ccd are Neo Geo CD,
           outside this batch's pre-1990 scope. */
        CHECK(strcmp(config_core_for_rom("A.7z"),  "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.cue"), "gambatte_libretro.so") == 0);
        CHECK(strcmp(config_core_for_rom("A.ccd"), "gambatte_libretro.so") == 0);

        /* The THIRTEEN cores are thirteen DISTINCT files. A table whose
           entries had been copy-pasted with one name left unchanged would
           satisfy every individual assertion above and still ship a .min to
           fceumm.
           .md is NOT in this list and its absence is deliberate rather than
           an oversight: the Mega Drive shares Genesis Plus GX with .sms, so
           it is the one extension here that MUST collide, and the two
           same-file assertions above are what check it instead. Adding "A.md"
           to this array would fail, correctly. */
        static const char *distinct[] = { "A.gb", "A.mgw", "A.nes", "A.min",
                                          "A.ws", "A.ngp", "A.a26", "A.col",
                                          "A.int", "A.sms", "A.zip",
                                          "A.sfc", "A.pce" };
        for (size_t i = 0; i < sizeof distinct / sizeof distinct[0]; i++)
            for (size_t j = i + 1; j < sizeof distinct / sizeof distinct[0]; j++)
                CHECK(strcmp(config_core_for_rom(distinct[i]),
                             config_core_for_rom(distinct[j])) != 0);
        for (size_t i = 0; i < sizeof distinct / sizeof distinct[0]; i++)
            CHECK(strchr(config_core_for_rom(distinct[i]), '/') == NULL);

        /* ---- the fourteen-system batch's control sets.

           MEGA DRIVE AND SNES HAVE NO EXTRA DISCS ANY MORE, and asserting it
           is the point rather than a formality. Both used to have two, and
           both outgrew them: a SNES pad is A B X Y L R and a six-button Mega
           Drive is A B C X Y Z, against two spare pockets. They are on the
           LCD strip now (config_layout_for_rom), whose labels are asserted
           further down. If either ever comes back here with discs, one of
           two things has happened -- somebody re-added a partial control set,
           or the layout switch was reverted -- and both should fail. */
        {
            koboy_layout l;
            koboy_config lc; config_defaults(&lc); l = lc.layout;

            /* Seeded with a system that DOES have discs first, so this also
               proves the clear-on-every-call contract rather than merely
               finding a struct that was already zero -- the failure mode
               where an assertion's expected value is also the default. */
            config_extra_buttons_for_rom(&l, "/roms/GunPey.ws");
            CHECK(l.extra[0].r > 0);
            config_extra_buttons_for_rom(&l, "/roms/Streets of Rage 2 (USA).md");
            CHECK_EQ_INT(l.extra[0].r, 0);
            CHECK_EQ_INT(l.extra[1].r, 0);
            CHECK_EQ_INT((int)l.extra[0].bit, 0);
            config_extra_buttons_for_rom(&l, "/roms/GunPey.ws");
            config_extra_buttons_for_rom(&l, "/roms/Super Metroid.sfc");
            CHECK_EQ_INT(l.extra[0].r, 0);
            CHECK_EQ_INT(l.extra[1].r, 0);
            config_extra_buttons_for_rom(&l, "/roms/GunPey.ws");
            config_extra_buttons_for_rom(&l, "/roms/Super Metroid.smc");
            CHECK_EQ_INT(l.extra[0].r, 0);
            CHECK_EQ_INT(l.extra[1].r, 0);

            /* PC ENGINE HAS NO EXTRA DISCS, and this is a deliberate empty
               case rather than a forgotten one -- the same shape as the
               Atari 2600's and the Master System's. A standard PC Engine pad
               is I (JOYPAD_A), II (JOYPAD_B), RUN (START) and Select, and the
               core defaults to "2 Buttons"; the faceplate carries all four
               already. Asserted because "no discs" and "we forgot this
               system" look identical at a glance, and because it also proves
               the clear-on-every-call contract: this runs straight after a
               SNES ROM that set both. */
            config_extra_buttons_for_rom(&l, "/roms/Bonk's Adventure.pce");
            CHECK_EQ_INT(l.extra[0].r, 0);
            CHECK_EQ_INT(l.extra[1].r, 0);
        }

        /* ---- config_min_rom_bytes: the floor that stops a CRASH.
           snes9x2005 raises SIGFPE inside retro_load_game for a .sfc/.smc
           under 8192 bytes (`% Memory.CalculatedSize` in LoROMMap, where
           CalculatedSize rounds the file down to whole 8 KB blocks and so is
           zero). Measured, not deduced: every size from 0 to 1024 kills the
           loader with exit 136 and 8192 does not.

           The floor is asserted to be SNES-ONLY, and that half matters as
           much: an Atari 2600 cartridge is legitimately 2048 or 4096 bytes,
           so a global floor would refuse real content to guard a crash that
           system does not have. */
        CHECK_EQ_INT((int)config_min_rom_bytes("A.sfc"), 8192);
        CHECK_EQ_INT((int)config_min_rom_bytes("A.smc"), 8192);
        CHECK_EQ_INT((int)config_min_rom_bytes("A.SFC"), 8192);
        /* The file this actually exists for: a 212-byte macOS AppleDouble
           stub that every FAT32 collection grows beside its real files, and
           that the browser lists as a game because it ends in .smc. */
        CHECK_EQ_INT((int)config_min_rom_bytes(
            "/roms/snes/._desire_d-zero_snes_pal_revision_2021.smc"), 8192);
        CHECK_EQ_INT((int)config_min_rom_bytes("A.a26"), 0);
        CHECK_EQ_INT((int)config_min_rom_bytes("A.gb"),  0);
        CHECK_EQ_INT((int)config_min_rom_bytes("A.md"),  0);
        CHECK_EQ_INT((int)config_min_rom_bytes("A.pce"), 0);
        CHECK_EQ_INT((int)config_min_rom_bytes("A.min"), 0);
        CHECK_EQ_INT((int)config_min_rom_bytes(NULL),    0);
        CHECK_EQ_INT((int)config_min_rom_bytes(""),      0);

        /* The result must stay SLASHLESS. config_join_sibling passes any name
           containing a slash through verbatim, so a "cores/gw_libretro.so"
           returned here would reach dlopen as a cwd-relative path and fail on
           a menu launch that sets no cwd -- the exact bug the sibling-join
           exists to prevent. */
        CHECK(strchr(config_core_for_rom("BALL.mgw"), '/') == NULL);
        CHECK(strchr(config_core_for_rom("ZELDA.gb"), '/') == NULL);
        CHECK(strchr(config_core_for_rom("galaga.zip"), '/') == NULL);

        /* The length guard in ends_with_mgw, made deterministic rather than
           ASan-only -- same construction as tests/test_romlist.c's short-name
           case, and for the same reason: with the guard removed the backward
           read for a 1-character name walks off the front of the string, and
           spelling ".mgw" into the bytes immediately before it turns that
           into an observable WRONG ANSWER (a file called "w" routed to the
           Game & Watch core) instead of undefined behaviour a test cannot
           legitimately observe. */
        {
            char pad[5] = { '.', 'm', 'g', 'w', '\0' };
            CHECK(strcmp(config_core_for_rom(&pad[3]),   /* pad[3..] is "w" */
                         "gambatte_libretro.so") == 0);
        }
    }

    /* THE SHIPPED ini must not name a core. `core =` is what marks the choice
       explicit, and an explicit choice turns OFF the .mgw -> gw-libretro
       routing -- so a config/koboy.ini that merely restated the gambatte
       default would pin every install to gambatte with every other test in
       this file still green. It did exactly that until this check was
       written. Read from the repo-relative path DEFAULT_INI uses, so `make
       test` (run from the repo root) checks the real shipped file rather
       than a copy. */
    {
        config_defaults(&c);
        CHECK(config_load(&c, "config/koboy.ini"));
        CHECK(!c.core_explicit);
    }

    /* ------------------------------------------- explicit core wins ---- */
    {
        char dir[] = "/tmp/koboy_core_ini_XXXXXX";
        CHECK(mkdtemp(dir) != NULL);
        char path[512];

        /* An ini that names a core marks the choice explicit, which is what
           stops main.c overriding it from the ROM's extension. */
        snprintf(path, sizeof path, "%s/explicit.ini", dir);
        FILE *f = fopen(path, "w");
        CHECK(f != NULL);
        fputs("core = my_libretro.so\n", f);
        fclose(f);
        config_defaults(&c);
        CHECK(config_load(&c, path));
        CHECK(strcmp(c.core_path, "my_libretro.so") == 0);
        CHECK(c.core_explicit);

        /* An ini that says nothing about the core leaves the flag alone, so
           the default stays overridable. Asserted separately because a
           config_load that set core_explicit unconditionally -- or that never
           set it -- would pass one of these two and fail the other. */
        snprintf(path, sizeof path, "%s/quiet.ini", dir);
        f = fopen(path, "w");
        CHECK(f != NULL);
        fputs("scale = 4\n", f);
        fclose(f);
        config_defaults(&c);
        CHECK(config_load(&c, path));
        CHECK_EQ_INT(c.scale, 4);
        CHECK(strcmp(c.core_path, "gambatte_libretro.so") == 0);
        CHECK(!c.core_explicit);

        /* THE UPGRADE CASE, and the reason this is not just a duplicate of
           the "explicit" case above. Every koboy.ini written before
           choice-by-extension existed carries a literal
           `core = gambatte_libretro.so`, because v1 shipped that line
           uncommented. It records what was packaged, not what anyone chose,
           so it must NOT pin the core -- otherwise every pre-existing install
           lists .mgw files and then refuses to load them, with nothing in the
           unit suite going red.

           core_path is stomped with a sentinel between config_defaults and
           config_load ON PURPOSE. config_defaults writes the legacy value
           itself, so asserting core_path == legacy after loading a
           `core = <legacy>` line would hold whether or not config_load read
           the key at all -- a check that cannot fail is not a check. The
           sentinel makes the assertion mean "the line was parsed and its
           value honoured", which is the half that must keep working while
           the flag half deliberately does not. */
        snprintf(path, sizeof path, "%s/legacy.ini", dir);
        f = fopen(path, "w");
        CHECK(f != NULL);
        fputs("core = " KOBOY_CORE_LEGACY_DEFAULT "\n", f);
        fclose(f);
        config_defaults(&c);
        snprintf(c.core_path, sizeof c.core_path, "sentinel_not_a_core.so");
        CHECK(config_load(&c, path));
        CHECK(strcmp(c.core_path, KOBOY_CORE_LEGACY_DEFAULT) == 0);
        CHECK(!c.core_explicit);

        char cmd[1024];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
        if (system(cmd) != 0) fprintf(stderr, "NOTE: cleanup failed\n");
    }

    /* ---- the configured scale is the GAME BOY's, not every system's ---- */
    {
        koboy_config c; config_defaults(&c);
        koboy_profile p;

        /* THE GAME BOY, and it must not move: 5 is measured, and auto-fitting
           it lands on 6 -- verified by mutating the is_game_boy branch off,
           which turns this line and both chrome goldens red. So 5 is a
           deliberate choice against the panel's maximum. */
        CHECK(config_resolve_profile(&p, &c, 1264, 1680, KOBOY_GB_W, KOBOY_GB_H,
                                     KOBOY_GB_W, KOBOY_GB_H));
        CHECK_EQ_INT(p.scale, 5);
        CHECK_EQ_INT(p.game_w, 800);
        CHECK_EQ_INT(p.game_h, 720);

        /* A POKEMON MINI, 96x64. Under the old rule this inherited the Game
           Boy's 5 and rendered 480x320 -- a postage stamp on this panel, and
           the same complaint the Game & Watch layout was rebuilt to answer.
           It must now fit itself instead, which is far larger than 5. */
        koboy_profile pm;
        CHECK(config_resolve_profile(&pm, &c, 1264, 1680, 96, 64, 96, 64));
        CHECK(pm.scale > 5);
        CHECK(pm.game_w > 480);
        /* Still inside the panel and still clear of the controls -- fitting
           bigger must not mean fitting wrong. */
        CHECK(pm.game_x >= 0 && pm.game_y >= 0);
        CHECK(pm.game_x + pm.game_w <= 1264);
        CHECK(pm.game_y + pm.game_h <= chrome_controls_top(c.layout_mode, &c.layout, 1264, 1680));

        /* AN EXPLICIT SCALE STILL WINS, for a non-Game-Boy system: the point
           is a better DEFAULT, not the loss of control. Asserted with a value
           that is not the legacy 5, so it marks intent. */
        koboy_config ec; config_defaults(&ec);
        ec.scale = 3; ec.scale_explicit = true;
        koboy_profile ep;
        CHECK(config_resolve_profile(&ep, &ec, 1264, 1680, 96, 64, 96, 64));
        CHECK_EQ_INT(ep.scale, 3);
        CHECK_EQ_INT(ep.game_w, 288);
    }

    /* ---- gray_map: one setting, reachable from the ini AND the menu ------ */
    {
        char dir[] = "/tmp/koboy_gray_XXXXXX";
        CHECK(mkdtemp(dir) != NULL);
        char path[512];
        snprintf(path, sizeof path, "%s/koboy.ini", dir);

        koboy_config c; config_defaults(&c);
        CHECK_EQ_INT(c.gray_map, (int)KOBOY_GRAY_DEFAULT);

        /* Every shipped name parses, and to a DIFFERENT value each time --
           asserting only that "equal" parses would pass with the parser
           hard-wired to return the default. */
        static const char *const names[] = { "luma", "bright", "balanced",
                                             "equal", "value" };
        for (int i = 0; i < 5; i++) {
            FILE *f = fopen(path, "w");
            CHECK(f != NULL);
            fprintf(f, "gray_map = %s\n", names[i]);
            fclose(f);
            config_defaults(&c);
            CHECK(config_load(&c, path));
            CHECK_EQ_INT(c.gray_map, i);
        }

        /* An unrecognised name KEEPS the previous value. It must not fall to
           entry 0, which is exactly the Rec.601 mapping this key exists to
           move away from -- a typo would silently reinstate the rendering the
           user was trying to change. Driven from a NON-default prior value so
           the check can tell "kept" from "reset to the default". */
        {
            FILE *f = fopen(path, "w");
            CHECK(f != NULL);
            fputs("gray_map = value\ngray_map = nonsense\n", f);
            fclose(f);
            config_defaults(&c);
            CHECK(config_load(&c, path));
            CHECK_EQ_INT(c.gray_map, (int)KOBOY_GRAY_VALUE);
        }

        /* config_save_gray_map: writes the key, and PRESERVES everything else
           in the file -- the whole reason it shares config_save_keys'
           implementation instead of rewriting the ini from defaults. */
        {
            FILE *f = fopen(path, "w");
            CHECK(f != NULL);
            fputs("# a comment\nscale = 4\ngray_map = luma\ngrab_input = false\n", f);
            fclose(f);
            CHECK(config_save_gray_map(path, KOBOY_GRAY_EQUAL));

            config_defaults(&c);
            CHECK(config_load(&c, path));
            CHECK_EQ_INT(c.gray_map, (int)KOBOY_GRAY_EQUAL);
            CHECK_EQ_INT(c.scale, 4);          /* the other keys survived */
            CHECK_EQ_INT(c.grab_input, 0);

            /* Exactly ONE gray_map line, so repeated menu presses cannot grow
               the file without bound -- the same idempotence config_save_keys
               was written for. The comment is still there too. */
            f = fopen(path, "r");
            CHECK(f != NULL);
            char line[1024]; int nkey = 0, ncomment = 0;
            while (fgets(line, sizeof line, f))
                { if (strstr(line, "gray_map")) nkey++;
                  if (strstr(line, "a comment")) ncomment++; }
            fclose(f);
            CHECK_EQ_INT(nkey, 1);
            CHECK_EQ_INT(ncomment, 1);

            /* And a second save replaces rather than appends. */
            CHECK(config_save_gray_map(path, KOBOY_GRAY_BRIGHT));
            f = fopen(path, "r");
            CHECK(f != NULL);
            nkey = 0;
            while (fgets(line, sizeof line, f)) if (strstr(line, "gray_map")) nkey++;
            fclose(f);
            CHECK_EQ_INT(nkey, 1);
            config_defaults(&c);
            CHECK(config_load(&c, path));
            CHECK_EQ_INT(c.gray_map, (int)KOBOY_GRAY_BRIGHT);
        }

        /* config_save_keys still works, and the two writers do not clobber
           each other's key -- they share one implementation, so a regression
           in the shared filter would show up as one erasing the other. */
        /* 304/305 (an Xbox pad's A and B), NOT the page-turn 193/194: those
           two ARE config_defaults' built-in values, so asserting them after a
           reload cannot tell "the line survived" from "the line was deleted
           and the default filled the field back in". That vacuous form was
           caught by a mutant -- config_save_gray_map dropping key_a/key_b as
           well turned NOTHING red -- which is the whole reason this note
           exists. The same trap is already noted at the key_start/key_select
           check above. */
        CHECK(config_save_keys(path, 304, 305));
        config_defaults(&c);
        CHECK(config_load(&c, path));
        CHECK_EQ_INT(c.gray_map, (int)KOBOY_GRAY_BRIGHT);
        CHECK_EQ_INT(c.key_a, 304);
        CHECK(config_save_gray_map(path, KOBOY_GRAY_LUMA));
        config_defaults(&c);
        CHECK(config_load(&c, path));
        CHECK_EQ_INT(c.key_a, 304);
        CHECK_EQ_INT(c.key_b, 305);
        CHECK_EQ_INT(c.gray_map, (int)KOBOY_GRAY_LUMA);

        remove(path); remove(dir);
    }

    /* ----------------------------------------- present_divisor as a setting
       The in-game FRAMES entry and the ini key are ONE setting, so what the
       menu can produce and what the loader will accept are tested together. */
    {
        char dir[] = "/tmp/koboy_div_XXXXXX";
        CHECK(mkdtemp(dir) != NULL);
        char path[512];
        snprintf(path, sizeof path, "%s/koboy.ini", dir);

        koboy_config c; config_defaults(&c);
        CHECK_EQ_INT(c.present_divisor, KOBOY_PRESENT_DIVISOR_DEFAULT);
        CHECK_EQ_INT(KOBOY_PRESENT_DIVISOR_DEFAULT, 3);   /* the shipped value */

        /* THE LADDER, walked once around. Written as an expected SEQUENCE
           rather than as a loop over the same table the implementation uses:
           a loop reading config.c's own array would agree with any array. */
        {
            static const int want[] = { 2, 3, 4, 6, 8, 1 };
            int d = 1;
            for (int i = 0; i < 6; i++) {
                d = config_next_present_divisor(d);
                CHECK_EQ_INT(d, want[i]);
            }
            /* Six steps is a full cycle: back where it started. */
            CHECK_EQ_INT(d, 1);
            /* The ladder's top IS the range's top. If these ever disagree the
               menu offers a value config_load throws away, or the ini permits
               one the menu can never return to. */
            CHECK_EQ_INT(config_next_present_divisor(KOBOY_PRESENT_DIVISOR_MAX - 1),
                         KOBOY_PRESENT_DIVISOR_MAX);
            CHECK_EQ_INT(config_next_present_divisor(KOBOY_PRESENT_DIVISOR_MAX), 1);

            /* Off-ladder but in range: cycles UP to the next rung, it does not
               snap sideways or stick. 5 and 7 are the only two such values. */
            CHECK_EQ_INT(config_next_present_divisor(5), 6);
            CHECK_EQ_INT(config_next_present_divisor(7), 8);
            /* And a value the clamp would have rejected still leaves the menu
               somewhere usable rather than looping on itself. */
            CHECK_EQ_INT(config_next_present_divisor(0), 1);
            CHECK_EQ_INT(config_next_present_divisor(-4), 1);
            CHECK_EQ_INT(config_next_present_divisor(99999), 1);
        }

        /* THE RANGE. */
        CHECK(config_present_divisor_ok(1));
        CHECK(config_present_divisor_ok(3));
        CHECK(config_present_divisor_ok(KOBOY_PRESENT_DIVISOR_MAX));
        CHECK(!config_present_divisor_ok(0));
        CHECK(!config_present_divisor_ok(-1));
        CHECK(!config_present_divisor_ok(KOBOY_PRESENT_DIVISOR_MAX + 1));

        /* Every in-range value round-trips through the file, INCLUDING the
           off-ladder 5 and 7: the ini is not restricted to what the menu
           cycles, and a loader that quietly snapped to the nearest rung would
           be a different setting from the one documented. Started from a
           fresh config each time so nothing carries over. */
        for (int d = 1; d <= KOBOY_PRESENT_DIVISOR_MAX; d++) {
            FILE *f = fopen(path, "w");
            CHECK(f != NULL);
            fprintf(f, "present_divisor = %d\n", d);
            fclose(f);
            config_defaults(&c);
            CHECK(config_load(&c, path));
            CHECK_EQ_INT(c.present_divisor, d);
        }

        /* THE REJECTION PATH, which is the one with teeth: 0 reaching
           pacer_tick is a division by zero, not a wrong picture.

           Each case is driven from a NON-DEFAULT prior value (a preceding
           `present_divisor = 6` line in the same file), so the assertion can
           tell "the bad value was rejected and 6 kept" from "the bad value
           was rejected and everything reset to the default 3" -- and, more
           to the point, from "the bad value was accepted", which asserting
           against the default 3 could not distinguish from a loader that
           simply never read the key at all. */
        {
            static const char *const bad[] = {
                "0", "-1", "-999", "9", "100000", "fast", "", "3.9",
            };
            /* 3.9 is here because atoi truncates it to 3, which IS in range:
               it must therefore be ACCEPTED as 3, not rejected. Listed with
               the rejects on purpose, so the expectation below has to name
               each outcome rather than assume one rule fits all eight. */
            static const int want[] = { 6, 6, 6, 6, 6, 6, 6, 3 };
            for (int i = 0; i < 8; i++) {
                FILE *f = fopen(path, "w");
                CHECK(f != NULL);
                fprintf(f, "present_divisor = 6\npresent_divisor = %s\n", bad[i]);
                fclose(f);
                config_defaults(&c);
                CHECK(config_load(&c, path));
                CHECK_EQ_INT(c.present_divisor, want[i]);
            }
        }

        /* config_save_present_divisor: writes the key, preserves the rest,
           and stays idempotent -- the same three properties config_save_keys
           and config_save_gray_map are held to, through the same rewrite_ini
           underneath. */
        {
            FILE *f = fopen(path, "w");
            CHECK(f != NULL);
            fputs("# a comment\nscale = 4\npresent_divisor = 3\n"
                  "grab_input = false\n", f);
            fclose(f);
            CHECK(config_save_present_divisor(path, 6));

            config_defaults(&c);
            CHECK(config_load(&c, path));
            CHECK_EQ_INT(c.present_divisor, 6);
            CHECK_EQ_INT(c.scale, 4);
            CHECK_EQ_INT(c.grab_input, 0);

            f = fopen(path, "r");
            CHECK(f != NULL);
            char line[1024]; int nkey = 0, ncomment = 0;
            while (fgets(line, sizeof line, f))
                { if (strstr(line, "present_divisor")) nkey++;
                  if (strstr(line, "a comment")) ncomment++; }
            fclose(f);
            CHECK_EQ_INT(nkey, 1);
            CHECK_EQ_INT(ncomment, 1);

            /* A second save replaces rather than appends, and lands on a
               value distinguishable from the first. */
            CHECK(config_save_present_divisor(path, 8));
            config_defaults(&c);
            CHECK(config_load(&c, path));
            CHECK_EQ_INT(c.present_divisor, 8);

            /* REFUSES a value config_load would then throw away, and leaves
               the file alone when it does. A writer that produced
               `present_divisor = 0` would give a menu whose choice silently
               did not survive the relaunch -- exactly the file/menu
               disagreement one shared key exists to prevent. */
            CHECK(!config_save_present_divisor(path, 0));
            CHECK(!config_save_present_divisor(path, -1));
            CHECK(!config_save_present_divisor(path, KOBOY_PRESENT_DIVISOR_MAX + 1));
            config_defaults(&c);
            CHECK(config_load(&c, path));
            CHECK_EQ_INT(c.present_divisor, 8);      /* the refused writes changed nothing */

            /* The two ini writers do not clobber each other's key. They share
               rewrite_ini, so a regression in the shared drop-filter shows up
               as one erasing the other -- the mutant that was actually caught
               when gray_map's writer was added. 304/305, not the 193/194
               defaults, for the reason spelled out at that check. */
            CHECK(config_save_keys(path, 304, 305));
            CHECK(config_save_gray_map(path, KOBOY_GRAY_VALUE));
            CHECK(config_save_present_divisor(path, 4));
            config_defaults(&c);
            CHECK(config_load(&c, path));
            CHECK_EQ_INT(c.present_divisor, 4);
            CHECK_EQ_INT(c.gray_map, (int)KOBOY_GRAY_VALUE);
            CHECK_EQ_INT(c.key_a, 304);
            CHECK_EQ_INT(c.key_b, 305);
        }

        remove(path); remove(dir);
    }
})
