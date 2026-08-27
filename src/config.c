/* readlink() and PATH_MAX: config.c needs both for install-relative path
   resolution, and -std=c11 alone hides them behind __STRICT_ANSI__. */
#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include "chrome.h"          /* chrome_controls_top: the resolver must reserve
                                the control band, and chrome.c owns its geometry */
#include "video.h"           /* video_fit_frac: ONE definition of the aspect
                                fit, shared with the per-frame placement that
                                has to land inside the rect this resolves */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void config_defaults(koboy_config *c)
{
    memset(c, 0, sizeof *c);
    c->scale = 5;
    /* SHIPPED BUTTON DEFAULTS, and the history matters because a zero here looks
       harmless. calib_needed() treats 0 as "not calibrated", the memset above
       makes both keys 0, and the calibration loop only advances on a hardware key
       press -- so a first run on a touch-only Kobo (Clara family, Nia, Elipsa,
       all supported by spec §3) sat on "press the button you want as A" forever
       with nothing but the power button doing anything. Spec §7 mandates built-in
       defaults as a starting guess that calibration then overrides; this is that
       guess, and shipping the sentinel instead was the bug.
       The codes are the two page-turn buttons MEASURED on the Libra 2's gpio-keys
       node (spec §12 and the input-device table). Never default either of them to
       KOBOY_KEY_POWER: the buttons share that node with it and power is the quit
       key. */
    c->key_a = KOBOY_KEY_PAGE_F23;
    c->key_b = KOBOY_KEY_PAGE_F24;
    /* key_start/key_select have no page-turn-button equivalent to fall back
       on the way key_a/key_b do -- a Kobo has exactly two hardware buttons,
       already spent above. So unlike key_a/key_b, THIS default is a GUESS,
       not a measurement of correctness: BTN_TL/BTN_TR (Xbox LB/RB) are a
       reasonable pair because they are two buttons distinct from A/B/the
       d-pad on the one real pad this project has measured (spec Appendix A,
       2026-08-26), but nothing says every gamepad's user wants shoulder
       buttons for Start/Select. Config-overridable, and first-run
       calibration exists for exactly this reason -- see KOBOY_KEY_BTN_TL's
       comment in koboy.h. On a touch-only or two-button Kobo with no pad,
       these two codes simply never arrive and Start/Select stay reachable
       only through the drawn faceplate, same as they always were. */
    c->key_start = KOBOY_KEY_BTN_TL;
    c->key_select = KOBOY_KEY_BTN_TR;
    c->present_divisor = KOBOY_PRESENT_DIVISOR_DEFAULT;
    /* Ghosting mitigation, DISABLED BY DEFAULT, and the history matters
       because "off" looks like an oversight otherwise.

       Both this and full_refresh_permille below were written for a forced-DU4
       pipeline. DU4 is a non-flashing waveform that cannot erase, so residue
       accumulated on every update and only a flashing GC16 cleared it; a
       periodic cleanup and a "large dirty area" promotion were the two ways of
       scheduling that flash. Then the fast waveform became AUTO, and AUTO hands
       the choice to the EPDC, which inspects the actual pixel transitions in
       each region and already picks something capable of erasing when a region
       is erasing. That made both mitigations redundant.

       MEASURED on the Libra 2, from a refresh trace: 35 AUTO refreshes on small
       rects, none of which flashed, against 21 GC16 flashes -- every single one
       of them a large region tripping the 450-permille threshold below. Zero
       scheduled cleanups fired in the whole run. So the threshold was causing
       all of the flashing the user complained about and the cleanup was doing
       nothing at all.

       With both off, a full game of Tetris played with no flashing whatever and
       only slight ghosting, which is the trade the user preferred. Raise
       cleanup_interval (presented frames) or cleanup_max_ms (wall clock) if you
       want the periodic flash back; the wall-clock ceiling exists because the
       dirty-rect pass suppresses unchanged frames, so a presented-frame counter
       is a poor clock -- 70s of measured gameplay presented only 45 frames. */
    c->cleanup_interval = 0;
    c->cleanup_max_ms = 0;
    /* 1000 permille, i.e. the promotion fires only when the dirty rect covers
       the game rect corner to corner. That is reachable -- the dirty rect is a
       single merged bounding box, so a full-screen wipe or a scene transition
       produces exactly it -- but normal gameplay essentially never does, and
       when the whole rect really has changed a flashing refresh is what you
       want anyway. Only *exceeding* the rect is impossible.
       See the note above: this threshold, not the cleanup, was the measured
       source of the flashing. Lower it (450 was the old default) to force a
       flash on large scene changes, which is worth having only if the driver's
       own choice leaves residue you can see. */
    c->full_refresh_permille = 1000;
    /* UNVALIDATED ON HARDWARE: this default is a starting guess, not a
       measurement -- unlike full_refresh_permille above, no device run has
       tuned it yet (that is Task 13's deferred Step 10). Pick it from a
       koboy.log `stages` line and `rects` count at 20/40/80 the first time a
       device is available, and record the result in TESTED.md. */
    c->refresh_fixed_tiles = 40;
    c->wfm_fast_policy = KOBOY_WFM_AUTO;
    c->gray_map = KOBOY_GRAY_DEFAULT;
    c->grab_input = true;
    /* CROSS, because the faceplate chrome draws an absolute four-way cross and
       the drawn UI has to agree with the input model -- the drawing is the part
       a user trusts. Relative mode steers from wherever the finger first landed,
       which needs a drag that a drawn cross gives no hint of: the user could not
       steer at all in relative mode and could immediately in cross mode. Set
       dpad_mode = relative for the thumb-pad behaviour. */
    c->pixel_aspect = true;
    c->dpad_mode = KOBOY_DPAD_CROSS;
    c->dpad_deadzone = 24;
    c->dpad_hysteresis = 10;
    /* Written unconditionally, and core_explicit deliberately left false by
       the memset above: a run with no ini and no browser must still open
       gambatte exactly as it always has, while config_core_for_rom stays
       free to override a default nobody asked for. */
    snprintf(c->core_path, sizeof c->core_path, "gambatte_libretro.so");
    snprintf(c->save_dir, sizeof c->save_dir, ".");
    snprintf(c->rom_dir, sizeof c->rom_dir, "roms");
    /* Control geometry, permille of panel. Game rect occupies the top; the
       d-pad sits lower-left under the left thumb, A/B lower-right. */
    koboy_layout l = { .dpad_cx = 220, .dpad_cy = 720, .dpad_r = 150,
                       .a_cx = 830, .a_cy = 670, .a_r = 85,
                       .b_cx = 660, .b_cy = 760, .b_r = 85,
                       .start_cx = 610, .start_cy = 920, .start_w = 200, .start_h = 55,
                       .select_cx = 390, .select_cy = 920, .select_w = 200, .select_h = 55,
                       /* MENU sits on the Start/Select row, not in the open band
                          below the game rect -- that band LOOKS free but is not:
                          chrome_controls_top is bound by whichever control sits
                          highest, and a zone placed at 540 permille (mid-panel)
                          measured lower than Start/Select's 920 on every panel
                          shorter than the Libra 2, dragging the auto-fit scale
                          down with it. Measured: on the 1072x1448 Clara panel,
                          chrome_controls_top went from 879 to 742 with the zone
                          there, which knocked the shipped scale from 5 (800x720)
                          down to 4 (640x576) -- a 36% area loss, and not only at
                          scale = 0, because config_resolve_profile's fitting loop
                          demotes an explicit configured scale too. The design spec's
                          promise that 5x fits every supported panel depends on
                          nothing lowering chrome_controls_top below the scale-5
                          rect, so this is not a cosmetic choice.
                          The Start/Select row is already reserved by two other
                          controls, so a third one costs nothing there. Verified
                          clear of both at scale 5 on every panel, no overlap
                          with Start and clear of the B disc, right margin well
                          past KOBOY_CHROME_MARGIN:
                            Clara  1072x1448  x[804..1018] y[1293..1371]  gap-to-Start 44px  clear-of-B 102px  right-margin 54px
                            Libra2 1264x1680  x[948..1200] y[1499..1591]  gap 51  clear 116  right-margin 64
                            Elipsa 1404x1872  x[1053..1333] y[1671..1773] gap 57  clear 130  right-margin 71
                            Sage   1440x1920  x[1080..1368] y[1714..1818] gap 58  clear 133  right-margin 72 */
                       .menu_cx = 850, .menu_cy = 920, .menu_w = 200, .menu_h = 55 };
    c->layout = l;
}

/* ------------------------------------------------------ core by extension
 *
 * koboy ships thirteen cores for fourteen systems now (Genesis Plus GX
 * answers for three of them), and which one a file needs is knowable from
 * its name alone: gw-libretro eats .mgw, fceumm eats .nes, PokeMini eats
 * .min, beetle-wswan eats .ws/.wsc, RACE eats .ngp/.ngc, stella2014 eats
 * .a26, Gearcoleco eats .col, FreeIntv eats .int, Genesis Plus GX eats
 * .sms/.gg, FinalBurn Neo eats .zip, and gambatte eats everything else this
 * project lists. The browser hands
 * main.c a path long after the config was read, so this cannot live in
 * config_load -- it is a pure function of the ROM name, called at load time.
 *
 * Its own case-insensitive suffix match rather than strcasecmp, and rather
 * than borrowing romlist.c's: config is the lower layer of the two (romlist
 * includes nothing of config's and must keep it that way), and <strings.h>
 * is the kind of host-dependent header this project keeps out of portable
 * code. Six lines is cheaper than either coupling.
 */
static bool ends_with_ext(const char *s, const char *ext)
{
    size_t ls = strlen(s), lx = strlen(ext);
    /* LIVE GUARD: a name shorter than the suffix ("a.gb" is fine, "gb" is
       not) would make s + ls - lx read before the string. */
    if (lx > ls) return false;
    const char *tail = s + ls - lx;
    for (size_t i = 0; i < lx; i++) {
        char c = tail[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != ext[i]) return false;   /* ext is written lowercase below */
    }
    return true;
}

/* A table rather than a chain of ifs, because the chain is what silently
   grows a hole: every new system needs an entry in BOTH this map and
   romlist_is_rom, and a table makes the pair reviewable side by side.
   Extensions are lowercase because ends_with_ext lowercases only the
   candidate, not the pattern. */
/* `ceiling` caps the auto-fitted scale for this system, 0 meaning no cap.
   It exists because the rect is now sized from the frame a core really draws
   (commit ae03e76), which quadrupled SNES's picture and, MEASURED on the
   device, cost its heaviest titles real speed: Star Fox 93%->67%, Kirby Super
   Star 96%->78%, while Mario World and Zelda stayed at 98%. At scale 3 the
   picture is still 2.25x what it was and every measured title is back at 95%
   or better, so the cap buys universal playability with a quarter of the
   area. A per-system number rather than a global one because the global
   `scale` is shared with thirteen other systems that pay nothing for 4x, and
   an explicit `scale =` in the ini still overrides this. */
static const struct { const char *ext; const char *core; int ceiling; } g_core_by_ext[] = {
    { ".mgw", "gw_libretro.so", 0 },   /* Game & Watch, gw-libretro   */
    { ".nes", "fceumm_libretro.so", 0 },   /* NES, libretro-fceumm        */
    { ".min", "pokemini_libretro.so", 0 },   /* Pokemon Mini, libretro/PokeMini */
    /* One core per SYSTEM FAMILY, not per extension: beetle-wswan reports
       `ws|wsc|pc2` and RACE reports `ngp|ngc|ngpc|npc`, so the mono and the
       Color halves of each family are the same .so. Two rows each rather
       than a prefix match, because this table is the thing a reviewer reads
       against romlist_is_rom's list and a wildcard would break that
       correspondence. .pc2/.ngpc/.npc are left out for the same reason .fds
       is: no evidence anyone's collection uses them, and an extension the
       browser lists but nobody has ever loaded is an untested claim. */
    { ".ws",  "mednafen_wswan_libretro.so", 0 }, /* WonderSwan, beetle-wswan */
    { ".wsc", "mednafen_wswan_libretro.so", 0 }, /* WonderSwan Color         */
    { ".ngp", "race_libretro.so", 0 },  /* Neo Geo Pocket, libretro/RACE */
    { ".ngc", "race_libretro.so", 0 },  /* Neo Geo Pocket Color        */
    { ".a26", "stella2014_libretro.so", 0 },  /* Atari 2600, stella2014      */
    { ".col", "gearcoleco_libretro.so", 0 },  /* ColecoVision, drhelius/Gearcoleco */
    { ".int", "freeintv_libretro.so", 0 },  /* Intellivision, libretro/FreeIntv */
    /* The second family after WonderSwan and Neo Geo Pocket where one .so
       covers two systems, and the first where the two are not a mono/colour
       pair: a Master System and a Game Gear are the same VDP behind a
       different viewport, so Genesis Plus GX runs both from one binary. Two
       rows, not a wildcard, for the reason above. .sg (SG-1000, which this
       core also accepts) and the whole Mega Drive list it advertises are
       deliberately absent: nobody's collection here has them, and an
       extension the browser lists but nobody has loaded is an untested
       claim. */
    { ".sms", "genesis_plus_gx_libretro.so", 0 }, /* Master System, GPGX     */
    { ".gg",  "genesis_plus_gx_libretro.so", 0 }, /* Game Gear, same core    */
    /* MEGA DRIVE, and it is the SAME .so as the two rows above -- Genesis
       Plus GX is natively a Mega Drive core and always was; the comment
       above used to say the Mega Drive list was "deliberately absent"
       because nobody had loaded one. Somebody has now, so one extension of
       that list is claimed and the rest still are not.

       .MD ONLY. NOT .bin, NOT .gen, and that is the owner's decision rather
       than an oversight, so here is what it costs and why it is right.
       Their Mega Drive tree is 1736 .md, 31 .bin and 5 .gen; the 36 files
       this row does not claim are homebrew and demoscene releases, and they
       will not appear in the browser at all.

       .bin is refused because koboy picks the core FROM THE EXTENSION AND
       NOTHING ELSE, and .bin is the most contested extension in retro
       computing. Counted across the owner's own collection rather than
       argued from memory: 723 TI-99/4A, 234 Odyssey 2, 119 Atari 5200, 72
       Arcadia 2001, 71 Vectrex, 68 Astrocade, 56 VC 4000, 38 Jaguar, 36
       Mega Drive, 34 Channel F, 33 CreatiVision, 28 Intellivision. The Mega
       Drive is the NINTH largest claimant of .bin in this one tree, and two
       of the files ahead of it (exec.bin and grom.bin) are literally the
       BIOS koboy asks the owner to install by hand. A row here would route
       every one of those to a 68000 emulator.
       .gen is unambiguous but is five files, and it is left out to keep the
       rule a reader can hold in their head: one system, one extension.
       roms/README.txt says so on the device, which is where somebody
       wondering why a file is missing will actually look. */
    { ".md",  "genesis_plus_gx_libretro.so", 0 }, /* Mega Drive, same core   */
    /* SNES, and the interesting part is that the v1 design spec ruled this
       system OUT on CPU grounds. That judgement was re-tested rather than
       inherited -- see scripts/build-snes-core.sh for the three-core
       shootout and TESTED.md for the device figures.

       Two extensions, one core, and the pair is the WonderSwan pattern: an
       .sfc and an .smc are the same cartridge behind different dumping
       conventions (.smc historically carries a 512-byte copier header, which
       the core detects and skips). .fig, .swc and .bs are the other
       historical copier extensions and are NOT claimed, for the reason
       .pc2 and .ngpc are not: nobody's collection here has them.

       Case-insensitivity is load-bearing again and not hypothetically: the
       author's SNES directory holds 47 files ending .smc and 11 ending
       .SMC, side by side, on top of the device's FAT32. */
    { ".sfc", "snes9x2005_libretro.so", 3 },   /* SNES -- see `ceiling` above */
    { ".smc", "snes9x2005_libretro.so", 3 },   /* SNES, copier-header dump   */
    /* PC ENGINE / TurboGrafx-16, CARTRIDGE ONLY. The core advertises
       `pce|sgx|cue|ccd|chd|toc|m3u` and exactly one is claimed.
       .sgx (SuperGrafx, 7 files in the author's collection) is refused
       because beetle-pce-FAST implements neither the second VDC nor the
       priority mixer that system needs -- it would load an .sgx and render
       it WRONGLY rather than refuse, which is the failure mode this project
       treats as worse than absence. .chd and the other CD extensions (48
       titles) need a system-card BIOS that is not ours to ship. Both
       exclusions are argued at length in scripts/build-pce-core.sh. */
    { ".pce", "mednafen_pce_fast_libretro.so", 0 }, /* PC Engine, beetle-pce-fast */
    /* THE FIRST EXTENSION IN THIS TABLE THAT IS NOT A SYSTEM'S OWN, and the
       decision behind it is the interesting part of adding arcade.
       An arcade "ROM" is a ZIP of the individual EPROM dumps off one PCB,
       named and CRC-checked against the emulator's own database -- so the
       extension says "archive", not "Namco board", and .zip is a container
       anything could be in.

       .ZIP IS CLAIMED FOR THE ARCADE CORE OUTRIGHT, rather than routed by a
       subdirectory convention or by looking the name up in FBNeo's dat. The
       reason is that the alternative is not "less ambiguous", it is
       "ambiguous plus a second mechanism to get wrong": nothing else koboy
       ships can open a .zip AT ALL. Nine of the ten other cores set
       need_fullpath = false, so core.c hands them the file's BYTES (core.c,
       core_load_rom) -- a zipped .nes reaches fceumm as the literal bytes
       "PK\3\4...", which it rejects as not a NES header. There is no case
       where routing .zip somewhere else would have worked and this row breaks
       it.

       WHAT IT COSTS, stated rather than discovered: a user who drops a zipped
       Game Boy ROM into roms/ now gets FinalBurn Neo refusing it ("core
       rejected rom") instead of the browser ignoring the file. That is a
       worse-looking failure for a file that could never have run either way,
       and it is the price of not building a dat parser into a 40 KB
       front-end. The error names the core, so the diagnosis is one line of
       koboy.log away.

       FBNeo also advertises `7z`, `cue` and `ccd`. None is claimed. .7z is
       not merely unclaimed but UNBUILDABLE for the device -- lib7z does not
       compile against glibc 2.19's headers, so scripts/build-fbneo-core.sh
       switches it off (see the script) and the shipped core physically
       cannot open one. .cue/.ccd are Neo Geo CD, which is outside this
       batch's pre-1990 scope and wants a BIOS besides. */
    { ".zip", "fbneo_libretro.so", 0 }, /* arcade, FinalBurn Neo    */
};

const char *config_core_for_rom(const char *rom_path)
{
    /* No name at all still answers gambatte: the caller writes the result
       into core_path unconditionally, and a NULL return would be a crash
       where the old unconditional default was merely useless. Gambatte also
       stays the fall-through for an unlisted extension, for the same reason
       it was the unconditional answer before this table existed. */
    if (!rom_path || !*rom_path) return "gambatte_libretro.so";
    for (size_t i = 0; i < sizeof g_core_by_ext / sizeof *g_core_by_ext; i++)
        if (ends_with_ext(rom_path, g_core_by_ext[i].ext))
            return g_core_by_ext[i].core;
    return "gambatte_libretro.so";
}

/* The scale ceiling for this ROM's system, or 0 for none. Same table and the
   same lookup as config_core_for_rom, so a system's core and its ceiling
   cannot drift apart. */
int config_scale_ceiling_for_rom(const char *rom_path)
{
    for (size_t i = 0; i < sizeof g_core_by_ext / sizeof g_core_by_ext[0]; i++)
        if (ends_with_ext(rom_path, g_core_by_ext[i].ext))
            return g_core_by_ext[i].ceiling;
    return 0;
}

size_t config_min_rom_bytes(const char *rom_path)
{
    if (!rom_path || !*rom_path) return 0;
    /* 8192 == one SNES mapping block. snes9x2005's InitROM rounds the file
       down to whole blocks into Memory.CalculatedSize and LoROMMap then does
       `% Memory.CalculatedSize`, so anything under one block divides by zero
       and raises SIGFPE inside retro_load_game -- the process dies, koboy and
       all. MEASURED, not deduced from the source: every size from 0 to 1024
       kills the loader (exit 136) and 8192 does not, and the backtrace is
       retro_load_game -> LoadROM -> InitROM -> LoROMMap.

       This is not a hypothetical file. The author's collection contains
       exactly one, and it is the kind every FAT32-and-macOS collection grows:
       `._desire_d-zero_....smc`, a 212-byte AppleDouble resource-fork stub
       that macOS writes beside the real file and that the browser lists as a
       game because it ends in .smc. A partial download does the same thing.

       ONLY the two SNES extensions carry a floor, and the restraint is
       deliberate: an Atari 2600 cartridge really is 2048 or 4096 bytes, a
       Game & Watch .mgw and a Pokemon Mini .min are small too, and a floor
       applied to those would refuse real content to guard against a crash
       they do not have. Every other core in this project REFUSES a short
       file cleanly ("core rejected rom"), which is the behaviour this one
       should have had. Add a row here only for a core measured to do worse
       than refuse. */
    if (ends_with_ext(rom_path, ".sfc") || ends_with_ext(rom_path, ".smc"))
        return 8192;
    return 0;
}

int config_layout_for_rom(const char *rom_path)
{
    /* No name at all is the DMG faceplate, matching the sibling above: this
       is the answer for the placeholder profile main.c resolves before any
       ROM has been chosen, and it is also the layout every UI screen (MAIN
       MENU, RECENT, ALL GAMES) is drawn over.

       .mgw is still the ONLY LCD extension, and none of the systems added
       since is in that company: a NES pad, a Pokemon Mini, a WonderSwan, a
       Neo Geo Pocket, an Atari 2600 joystick, a ColecoVision controller, an
       Intellivision hand controller and a Master System / Game Gear pad are
       all stick-or-pad plus physical buttons, not buttons drawn into the
       artwork, which is exactly what the DMG faceplate already draws and
       hit-tests. LCD exists because a Game & Watch unit draws its own
       controls; none of these does.

       The Intellivision came CLOSEST to earning it and still does not.
       FreeIntv has an optional `freeintv_multiscreen_overlay` mode that
       widens the frame to 1074x600 and paints a photographic 12-key keypad
       beside the game, driven by RETRO_DEVICE_POINTER -- which is the Game &
       Watch situation exactly, and koboy's LCD layout already forwards
       touches as a pointer. It is not used, for two measured reasons: the
       composite costs 1.335 ms/frame against 0.106 without it (12x, on the
       host, before the Cortex-A9 multiplier), and the same 12 keys are
       already reachable on the DMG faceplate through the core's own mini
       keypad -- see the .int case in config_extra_buttons_for_rom. Paying
       12x for a second way to press the same keys is not a trade. */
    if (!rom_path || !*rom_path) return KOBOY_LAYOUT_DMG;
    return ends_with_ext(rom_path, ".mgw") ? KOBOY_LAYOUT_LCD : KOBOY_LAYOUT_DMG;
}

void config_extra_buttons_for_rom(koboy_layout *l, const char *rom_path)
{
    if (!l) return;
    /* Cleared, not left alone, on every call: this runs once per ROM load and
       the config it edits outlives a single game (MENU -> CHOOSE ROM reuses
       it), so "set them for .min" without "clear them for everything else"
       would leave a C button drawn and live on the next Game Boy. */
    memset(l->extra, 0, sizeof l->extra);
    if (!rom_path || !*rom_path) return;

    /* The two positions on the DMG faceplate that fit another disc, and they
       are tight enough to be worth writing down once here rather than twice
       in the tables below.

       SLOT R -- (905, 790) r 70, the pocket below A and right of B. A and B
       are fixed (they are the Game Boy's, and this must not move them), the
       Start/Select/MENU row owns everything from 892 permille down, and the
       panel needs KOBOY_CHROME_MARGIN clear on the right. That leaves this
       pocket, and only at a smaller radius than A/B's 85. Checked on all four
       supported panels -- gap to the A disc, gap to the MENU pill, right
       margin:
         Clara  1072x1448  A-gap 25px  MENU-gap 75px  right margin 27px
         Libra2 1264x1680  A-gap 28    MENU-gap 84    right margin 33
         Elipsa 1404x1872  A-gap 30    MENU-gap 95    right margin 36
         Sage   1440x1920  A-gap 32    MENU-gap 98    right margin 37

       SLOTS L/R-STACKED -- (470, 700) and (470, 830), both r 62, the column
       between the d-pad's right edge and B's left edge. Two discs need two
       places and the pocket above holds one, so the WonderSwan pair goes
       here instead of splitting across the faceplate. Clearances at the
       WORST panel of the four (1072x1448, the narrowest): 40px to the d-pad,
       47px to B, 56px between the two discs, 26px to the START row.

       Neither slot becomes chrome_controls_top's binding minimum on any of
       the four panels (the minimum stays 879 / 1018 / 1135 / 1164, set by the
       A disc), which is what lets these systems get the same game rect and
       resolved scale they would have got with no extra buttons at all.
       tests/test_chrome.c re-derives every one of those numbers rather than
       trusting this comment.

       Labels go INSIDE these discs, not below them like A and B: there is no
       case band under any of the three to put one in. chrome.c reuses the LCD
       strip's draw_face_button for exactly that reason. */

    /* A Pokemon Mini genuinely has an A, a B and a C, and the core binds C to
       RETRO_DEVICE_ID_JOYPAD_R -- bit 11, KOBOY_BTN_R1 -- and advertises it as
       "C" in its own input descriptors. The mapping is read off the core, not
       chosen. */
    if (ends_with_ext(rom_path, ".min")) {
        l->extra[0] = (koboy_extra_btn){ 905, 790, 70, KOBOY_BTN_R1, "C" };
        return;
    }

    /* A WonderSwan needs TWO, and it needs them because of the ROTATION, not
       because the hardware has six face buttons. The console has two 4-way
       cursors (X1-X4, Y1-Y4) plus A, B and START, and many titles are played
       with the unit turned on its side -- beetle-wswan's default
       `wswan_rotate_display = manual` toggles that on SELECT, which the DMG
       faceplate already has, and `wswan_rotate_keymap = auto` swaps the
       retropad map to match.

       In that ROTATED map (third_party/wswan/libretro.c, map[1]) the retropad
       d-pad drives the Y cursor -- the one under the thumb in that grip, so
       koboy's d-pad is right -- but the WonderSwan's own A and B move to
       JOYPAD_L and JOYPAD_R. MEASURED, not read: `Kaze no Klonoa - Moonlight
       Museum` in portrait responds to exactly two inputs, START and JOYPAD_L,
       and JOYPAD_L is the hardware's A button. Without these two discs that
       title cannot be started at all -- the same "a button that exists in the
       hardware and is unreachable on koboy" bug the Game & Watch layout and
       then the Pokemon Mini each spent a round on.

       L1 above R1 because that is where they sit once the console is turned:
       A ends up above B. What is still NOT reachable is the rotated map's
       X-cursor up/right, which land on JOYPAD_Y and JOYPAD_X -- there is no
       room for two more discs, and no title measured so far needs them. */
    if (ends_with_ext(rom_path, ".ws") || ends_with_ext(rom_path, ".wsc")) {
        l->extra[0] = (koboy_extra_btn){ 470, 700, 62, KOBOY_BTN_L1, "L1" };
        l->extra[1] = (koboy_extra_btn){ 470, 830, 62, KOBOY_BTN_R1, "R1" };
        return;
    }

    /* An Intellivision hand controller is the hardest control set this
       project has met and the two discs make ALL OF IT reachable, which was
       not obvious and is the whole reason this case is long.

       The hardware: a 16-direction disc, three action buttons (top,
       lower-left, lower-right -- the two upper side buttons are one signal),
       and a TWELVE-KEY TELEPHONE KEYPAD. FreeIntv gives the disc's four
       cardinals to the retropad d-pad, the lower two action buttons to
       JOYPAD_A / JOYPAD_B, and the top one to JOYPAD_Y (src/controller.c's
       getControllerState -- read off the code, not the input descriptors,
       which contradict the core's own on-screen help about which of A/B is
       which side). So the top button needs a disc: that is extra[1].

       THE KEYPAD, which no faceplate can draw and which several titles
       cannot be started without -- BurgerTime and Bump 'n' Jump both stop at
       "Select 1 or 2 Players", Diner says "then press enter". FreeIntv puts
       keypad 0 and 5 on the thumbsticks and Clear/Enter on the triggers,
       and 1-9 only on the RIGHT ANALOG STICK, which koboy has no source for
       (src/core.c answers RETRO_DEVICE_JOYPAD alone). That would have left
       nine keys dead.

       It does not, because the core has a second way in that its input
       descriptors call "Show Keypad": HOLD JOYPAD_L, and a 4x3 keypad is
       drawn into the corner of the frame, the D-PAD moves a cursor over it,
       and any face button presses the selected key (src/controller.c,
       getKeypadState + drawMiniKeypad; libretro.c:1267 makes it modal, so
       the disc is not steering the game while it is held). MEASURED, not
       read: holding L1 and tapping A on BurgerTime's player-count prompt put
       a "1" on the screen, and the mini keypad appeared in both bottom
       corners. Three simultaneous touches -- disc, d-pad, A -- and koboy
       tracks ten.

       So extra[0] is JOYPAD_L, labelled KEY rather than L1 because what it
       does is open the keypad and nothing on the hardware is called L1.

       What is still NOT reachable: the disc's twelve DIAGONAL positions.
       koboy's touch d-pad reports the four cardinals and their four
       diagonals; the 16-way disc has eight more between those, and
       FreeIntv only offers them on the left analog stick. Titles that steer
       finely (Astrosmash's ship, Auto Racing) are coarser here than on real
       hardware. JOYPAD_X ("last selected keypad button") and the L2/R2/L3/R3
       keypad shortcuts are also unreachable, and all six are redundant with
       the mini keypad above rather than lost. */
    if (ends_with_ext(rom_path, ".int")) {
        l->extra[0] = (koboy_extra_btn){ 470, 700, 62, KOBOY_BTN_L1, "KEY" };
        l->extra[1] = (koboy_extra_btn){ 470, 830, 62, KOBOY_BTN_Y,  "TOP" };
        return;
    }

    /* A ColecoVision controller ALSO has a twelve-key keypad, and unlike the
       Intellivision this core offers no on-screen way to reach it: Gearcoleco
       spreads the keys across the whole retropad (keypad 1 on JOYPAD_Y, 2 on
       X, 3-8 on the shoulders and sticks, * and # on START/SELECT, 9 and 0 on
       an analog axis -- platforms/libretro/libretro.cpp's descriptors). Only
       two of those can have a disc.

       1 and 2 are the two, because that is what the CONSOLE'S OWN BIOS asks
       for: every cartridge boots into an option screen whose first two lines
       are "1 = SKILL 1/ONE PLAYER" and "2 = SKILL 2/ONE PLAYER" -- rendered
       and read, not assumed. Without keypad 1 a ColecoVision title cannot be
       started at all, which is the same bug the Game & Watch layout, the
       Pokemon Mini and the WonderSwan each spent a round on.

       Labelled K1/K2 rather than 1/2 so a finger looking for "the keypad"
       finds them; the faceplate has no other numbers on it.

       Two things a reader should know rather than discover: koboy's START and
       SELECT are keypad * and # on this system (the core binds them there,
       and the faceplate's moulded labels lie about it), and keypad 3-9 and 0
       are unreachable. The games that need those are the ones with in-play
       menus -- Fortune Builder, the Super Action titles -- not the ones with
       a start screen. */
    if (ends_with_ext(rom_path, ".col")) {
        l->extra[0] = (koboy_extra_btn){ 470, 700, 62, KOBOY_BTN_Y, "K1" };
        l->extra[1] = (koboy_extra_btn){ 470, 830, 62, KOBOY_BTN_X, "K2" };
        return;
    }

    /* THE MEGA DRIVE IS THE FIFTH TIME THIS PROJECT HAS MET THE "a button
       that exists in the hardware and is unreachable on koboy" BUG, and the
       first time it was caught by reading the core BEFORE shipping rather
       than by a title that would not start.

       A three-button Mega Drive pad is A, B, C and START. That is one more
       face button than the DMG faceplate has, and -- this is the part that
       makes it a trap rather than a shortfall -- the one that falls off is
       NOT the one the retropad naming suggests. Genesis Plus GX maps
       (libretro/libretro.c, the port-0 descriptor block):

         JOYPAD_B      -> B          JOYPAD_X      -> Y
         JOYPAD_A      -> C          JOYPAD_L      -> X
         JOYPAD_Y      -> A          JOYPAD_R      -> Z
         JOYPAD_START  -> Start      JOYPAD_SELECT -> Mode

       So the faceplate's B disc is the Mega Drive's B and its A disc is the
       Mega Drive's **C**. The hardware's **A** is on JOYPAD_Y, which the DMG
       faceplate does not have. Without a disc for it, koboy would present a
       console whose A button does not exist -- and it would LOOK fine,
       because the faceplate has a disc moulded "A" that does something. That
       is the ColecoVision failure in reverse and it is worse, because
       nothing refuses to start: Sonic plays (A, B and C are all jump), and
       then Streets of Rage has no special attack and Golden Axe has no
       magic.

       extra[0] is therefore JOYPAD_Y, labelled "A" after the hardware's own
       moulding, and with it ALL THREE BUTTONS OF A 3-BUTTON PAD ARE
       REACHABLE, which covers the overwhelming majority of the library.

       extra[1] is JOYPAD_X, the six-button pad's "Y" -- the middle of its
       top row. The six-button pad is X, Y, Z above A, B, C; two of those
       three have nowhere to go and Y is the one chosen because it is the
       middle-strength attack in the fighters that are most of what needs six
       buttons at all. Labelled "Y" for the same reason the arcade discs are
       labelled 3 and 4 rather than C and D: on this system the label a
       player is looking for is the one printed on the real pad.

       WHAT IS STILL UNREACHABLE, stated rather than discovered: X
       (JOYPAD_L) and Z (JOYPAD_R). There is no seventh and eighth place on
       this faceplate. Every title that needs them is a six-button fighter.
       MODE (JOYPAD_SELECT) IS reachable -- it is the faceplate's SELECT
       pill, whatever that pill's moulded label says -- and that matters more
       than it sounds: holding Mode at power-on is how a real six-button pad
       pretends to be a three-button one for the titles that mis-detect it.

       The pad type is not forced here and does not need to be: the core
       defaults to `DEVICE_PAD2B | DEVICE_PAD3B | DEVICE_PAD6B` (libretro.c),
       i.e. auto-detect, and koboy answers no core options, so all six
       buttons are live and the game decides. */
    if (ends_with_ext(rom_path, ".md")) {
        l->extra[0] = (koboy_extra_btn){ 470, 700, 62, KOBOY_BTN_Y, "A" };
        l->extra[1] = (koboy_extra_btn){ 470, 830, 62, KOBOY_BTN_X, "Y" };
        return;
    }

    /* A SNES pad is the retropad -- literally, the retropad was modelled on
       it -- so snes9x2005's descriptors are the identity map: B is B, A is
       A, X is X, Y is Y, L is L, R is R, plus Start and Select. That makes
       this the first system koboy runs where the control set is not merely
       bigger than the faceplate but bigger by FOUR: eight buttons against
       the faceplate's A, B, START, SELECT and two spare discs.

       THE FOUR FACE BUTTONS WIN THE TWO SLOTS. extra[0] is Y and extra[1] is
       X, so B/A/Y/X are all present and the shoulders are not. The trade is
       not close: on this console Y is the run-and-fire button (Mario runs on
       Y, Samus shoots on Y, Link's sword is B and item is Y), so a SNES
       without Y is not a SNES with a missing extra -- it is one where the
       primary action of most of the library has no button. L and R are
       secondary almost everywhere: map, weapon cycle, page turn, camera
       nudge.

       WHAT IT COSTS, counted honestly rather than waved past. The titles
       that genuinely want a shoulder are playable but reduced, not broken:
       Super Metroid's L/R aim the diagonals (the d-pad diagonals still aim,
       so it plays), Yoshi's Island cycles items on the shoulders, Zelda has
       none. The ones that lose something they cannot get back are the ones
       where a shoulder is a distinct move -- the fighters' fierce attacks,
       Mario Kart's hop -- and there is no eighth place on this faceplate for
       them. Recorded so this reads as a decision, not a gap somebody forgot.

       Labelled "Y" and "X" after the pad's own moulding, which is also what
       every SNES title's own control screen calls them. */
    if (ends_with_ext(rom_path, ".sfc") || ends_with_ext(rom_path, ".smc")) {
        l->extra[0] = (koboy_extra_btn){ 470, 700, 62, KOBOY_BTN_Y, "Y" };
        l->extra[1] = (koboy_extra_btn){ 470, 830, 62, KOBOY_BTN_X, "X" };
        return;
    }

    /* An Atari 2600 joystick is four directions and ONE button, and the
       console adds Reset and Select. stella2014 binds fire to JOYPAD_B,
       Reset to JOYPAD_START and Select to JOYPAD_SELECT, so the DMG
       faceplate already carries every control a 2600 game has and NO extra
       disc is needed -- recorded as a deliberate empty case for the reason
       the Neo Geo Pocket one below is.

       Worth knowing anyway: the A DISC IS DEAD for this system. Fire is B,
       and JOYPAD_A only does anything for a Genesis pad or paddles, neither
       of which koboy can present (libretro.cxx's event mapping). The two
       difficulty switches and the colour/black-and-white switch are
       unreachable; they are set-once console switches, not controls, and no
       title in the author's 82 needs one to start. Paddle titles (Breakout,
       Warlords) play on the d-pad rather than by feel, because a paddle is
       an analog axis and koboy has none.

       A Master System and a Game Gear are likewise fully covered: two
       buttons on JOYPAD_B and JOYPAD_A, and PAUSE/START on JOYPAD_START
       (third_party/gpgx's DEVICE_PAD2B branch). Nothing left over. */

    /* ARCADE, and this is the first system where the extra discs are chosen
       from a POPULATION rather than from one console's control panel --
       FinalBurn Neo is 227 different boards in the author's set alone, with
       no single answer to "what does the hardware have".

       So it was counted. Every one of those 227 romsets was loaded and its
       retro_input_descriptors read (port 0, RETRO_DEVICE_JOYPAD), and the
       mapping FBNeo uses is flat and consistent: JOYPAD_B is always the
       board's "Button 1", JOYPAD_A its "Button 2", JOYPAD_Y its "Button 3",
       JOYPAD_X its "Button 4". Counts across the 227:

         B  208    Y  134    L1 45    L2 45    L3 26
         A  185    X   71    R1 48    R2 46    R3 14

       B and A the DMG faceplate already has. Y is bound by 134 boards --
       more than half -- and X by 71, so those two are the discs, and they
       take the L/R-stacked slots the WonderSwan pair uses. That covers every
       board with four or fewer fire buttons, which is all of the pre-1990
       era this batch is scoped to and most of what came after.

       Labelled 3 and 4, NOT C and D, and the labels are the honest part:
       on this system the faceplate's moulded B and A ARE buttons 1 and 2, so
       numbering the new pair continues a sequence the player can actually
       follow. (The ColecoVision case above had the same problem and answered
       it the same way with K1/K2.)

       WHAT IS STILL UNREACHABLE, counted rather than guessed: L1/R1 (45/48
       boards), L2/R2 (45/46) and L3/R3 (26/14). Those are the SIX-BUTTON
       layouts -- Street Fighter's strong punch and kick, a Neo Geo D button,
       Defender's "Reverse" -- and there is no seventh and eighth place on
       this faceplate for them. Every one of those boards is outside the
       pre-1990 scope this core was added for. Within that scope the only
       casualty measured is Defender, which puts Hyperspace on Y, Thrust on X
       and Reverse on R1: the first two are reachable through these discs,
       Reverse is not.

       COIN AND START need no disc and this is worth writing down because it
       is the thing a reader will check first: FBNeo binds "Coin 1" to
       JOYPAD_SELECT and "Start 1" to JOYPAD_START (retro_input.cpp,
       GameInpStandardOne), which are the faceplate's SELECT and START pills.
       An arcade board will not start without a coin, so a missing SELECT
       would have been the "button that exists and is unreachable" bug for a
       fourth time. It is reachable. Verified by playing: Galaga reaches
       STAGE 1 from SELECT then START. */
    if (ends_with_ext(rom_path, ".zip")) {
        l->extra[0] = (koboy_extra_btn){ 470, 700, 62, KOBOY_BTN_Y, "3" };
        l->extra[1] = (koboy_extra_btn){ 470, 830, 62, KOBOY_BTN_X, "4" };
        return;
    }

    /* A Neo Geo Pocket has a stick, A, B and OPTION and nothing else, so it
       needs NO extra disc -- recorded here as a deliberate empty case rather
       than left to the fall-through, because "we checked and it needs none"
       and "we never looked" are indistinguishable otherwise.

       RACE binds OPTION to JOYPAD_START, and the two face buttons CROSSED:
       NGP A -> JOYPAD_B, NGP B -> JOYPAD_A (third_party/race/libretro/
       libretro.c's descriptors say so in as many words). That reads like a
       bug and is not one. On the hardware A is the LEFT button and B the
       right; on koboy's faceplate the B disc is left of the A disc, exactly
       as on a Game Boy. So the crossing puts each label where the finger
       expects it, and undoing it would be the bug. */
}

/* ------------------------------------------------- install-relative paths
 *
 * Why this exists, in full, because it cost a device round-trip to find.
 *
 * `core_path` defaults to a bare "gambatte_libretro.so", and core.c hands it
 * to dlopen(). Per POSIX and glibc, dlopen() given a name containing no slash
 * treats it as a library *name* and searches DT_RUNPATH, LD_LIBRARY_PATH,
 * /etc/ld.so.cache and the system library directories -- it NEVER looks in the
 * current working directory. So on the device it looked everywhere except the
 * one directory the file was actually in, and failed with "cannot open shared
 * object file: No such file or directory" while sitting next to it.
 *
 * No host test could have caught it: on the desktop the core always arrives as
 * a path with a slash in it (`--core build/stub_core.so`), which dlopen treats
 * as a filesystem path.
 *
 * The fix resolves against the directory containing the *executable*, not the
 * cwd. A "./" prefix would also have worked, but only when the cwd happens to
 * be the install directory -- it fails the moment koboy is launched from
 * NickelMenu or KFMon, which do not set one. /proc/self/exe is independent of
 * how the process was started.
 */

/* Split out from config_exe_dir and taking the directory as an argument so it
   is testable without depending on where the test binary happens to live.
   Returns false and leaves `out` untouched on truncation or bad input. */
bool config_join_sibling(char *out, size_t n, const char *name, const char *dir)
{
    if (!out || !n || !name || !name[0] || !dir || !dir[0]) return false;

    /* An explicit path -- absolute or containing a slash anywhere -- is the
       caller's own decision; honour it verbatim. */
    if (strchr(name, '/')) {
        if (strlen(name) >= n) return false;
        snprintf(out, n, "%s", name);
        return true;
    }

    char buf[PATH_MAX];
    int  len;
    /* "." means "the install directory", not "the install directory plus a
       stray /." component that then shows up in every error message. */
    if (!strcmp(name, "."))
        len = snprintf(buf, sizeof buf, "%s", dir);
    else if (!strcmp(dir, "/"))          /* avoid emitting "//name" */
        len = snprintf(buf, sizeof buf, "/%s", name);
    else
        len = snprintf(buf, sizeof buf, "%s/%s", dir, name);

    if (len < 0 || (size_t)len >= sizeof buf) return false;
    if ((size_t)len >= n) return false;
    snprintf(out, n, "%s", buf);
    return true;
}

/* The directory containing the running executable, with no trailing slash
   (except at the filesystem root, where it is exactly "/"). */
bool config_exe_dir(char *out, size_t n)
{
    if (!out || !n) return false;
    char    buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (len <= 0 || (size_t)len >= sizeof buf - 1) return false;
    buf[len] = '\0';

    char *slash = strrchr(buf, '/');
    if (!slash) return false;            /* not an absolute path: give up */
    if (slash == buf) buf[1] = '\0';     /* executable sits in "/" */
    else              *slash = '\0';
    if (strlen(buf) >= n) return false;
    snprintf(out, n, "%s", buf);
    return true;
}

/* Called once, after the ini and the command line have both been applied, so
   that a bare name from either source gets the same treatment. Deliberately
   NOT done inside config_load: the loader's job is to report what the file
   says, and folding resolution into it would make the parse tests assert on
   this machine's directory layout.

   All three paths get it, not just core_path. rom_path and save_dir reach
   fopen(), which does resolve relative to the cwd, so they are not broken the
   way core_path was -- but they fail the same way for the same reason when
   launched from a menu that sets no cwd, and save_dir is the worse of the two:
   the shipped default is ".", which under a menu launch would try to write
   save files to the read-only rootfs. Resolving all three keeps one rule to
   remember: no slash means "next to koboy". */
void config_resolve_paths(koboy_config *c)
{
    char dir[PATH_MAX];
    if (!config_exe_dir(dir, sizeof dir)) return;   /* leave paths as-is */

    char tmp[512];
    if (config_join_sibling(tmp, sizeof tmp, c->core_path, dir))
        snprintf(c->core_path, sizeof c->core_path, "%s", tmp);
    if (config_join_sibling(tmp, sizeof tmp, c->rom_path, dir))
        snprintf(c->rom_path, sizeof c->rom_path, "%s", tmp);
    if (config_join_sibling(tmp, sizeof tmp, c->save_dir, dir))
        snprintf(c->save_dir, sizeof c->save_dir, "%s", tmp);
    if (config_join_sibling(tmp, sizeof tmp, c->rom_dir, dir))
        snprintf(c->rom_dir, sizeof c->rom_dir, "%s", tmp);
}

static void trim(char *s)
{
    char *p = s; while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r' || s[n-1] == '\n')) s[--n] = 0;
}

/* An EMPTY value leaves the default alone rather than meaning true. This
   treated everything except "false" and "0" as true, including "", so a
   blanked `grab_input = ` silently turned the grab on -- the opposite of what
   clearing a line means, and invisible without reading this function. `trim`
   has already run, so "" covers whitespace-only values too. */
static bool as_bool(const char *v, bool dflt) {
    if (!v || !v[0]) return dflt;
    return !(strcmp(v,"false")==0 || strcmp(v,"0")==0);
}

/* The promotion test, extracted from the main loop so it can be tested.
   >= and not >, deliberately: a frame in which the entire game rect changed is
   precisely when a flashing refresh is wanted, so the boundary case belongs
   inside the promotion, not outside it. permille <= 0 disables the promotion
   outright -- without that guard 0 would make the comparison always true, which
   is the exact inverse of what "disabled" means. */
bool config_promote_full(const koboy_config *c, long dirty_px, long whole_px)
{
    if (!c || c->full_refresh_permille <= 0) return false;
    if (whole_px <= 0 || dirty_px <= 0)      return false;
    return dirty_px * 1000L >= whole_px * (long)c->full_refresh_permille;
}

bool config_load(koboy_config *c, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return true;                 /* absent file: defaults stand */
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#'); if (hash) *hash = 0;
        char *eq = strchr(line, '=');    if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        trim(k); trim(v);
        /* See KOBOY_SCALE_LEGACY_DEFAULT: an ini naming exactly 5 is
           indistinguishable from one that was never edited, so it does not
           mark intent. Any other value does. */
        if      (!strcmp(k, "scale")) {
            c->scale = atoi(v);
            c->scale_explicit = (c->scale != KOBOY_SCALE_LEGACY_DEFAULT);
        }
        /* REJECTED, not clamped, and not accepted as written: see
           config_present_divisor_ok in config.h for why neither clamp
           direction is defensible. atoi gives 0 for a non-numeric value too,
           so `present_divisor = fast` lands here and keeps the default rather
           than dividing by zero in pacer_tick. */
        else if (!strcmp(k, "present_divisor")) {
            int d = atoi(v);
            if (config_present_divisor_ok(d)) c->present_divisor = d;
        }
        else if (!strcmp(k, "cleanup_interval")) c->cleanup_interval = atoi(v);
        else if (!strcmp(k, "cleanup_max_ms"))   c->cleanup_max_ms = atoi(v);
        else if (!strcmp(k, "force_dither"))     c->force_dither = as_bool(v, c->force_dither);
        else if (!strcmp(k, "pixel_aspect"))     c->pixel_aspect = as_bool(v, c->pixel_aspect);
        else if (!strcmp(k, "grab_input"))       c->grab_input   = as_bool(v, c->grab_input);
        else if (!strcmp(k, "dpad_deadzone"))    c->dpad_deadzone = atoi(v);
        else if (!strcmp(k, "dpad_hysteresis"))  c->dpad_hysteresis = atoi(v);
        else if (!strcmp(k, "dpad_mode"))        c->dpad_mode = strcmp(v,"cross") ? KOBOY_DPAD_RELATIVE : KOBOY_DPAD_CROSS;
        /* Unlike dpad_mode above, an unrecognised name KEEPS the current
           value instead of falling to entry 0. dpad_mode has two settings and
           either is usable; gray_map has five, and entry 0 is the Rec.601
           mapping this key exists to move away from -- so a typo'd or
           truncated name must not silently reinstate exactly the rendering
           the user was trying to change. */
        else if (!strcmp(k, "gray_map")) {
            koboy_gray_map gm;
            if (video_gray_map_parse(v, &gm)) c->gray_map = (int)gm;
        }
        else if (!strcmp(k, "key_a"))            c->key_a = (uint16_t)atoi(v);
        else if (!strcmp(k, "key_b"))            c->key_b = (uint16_t)atoi(v);
        else if (!strcmp(k, "key_start"))        c->key_start = (uint16_t)atoi(v);
        else if (!strcmp(k, "key_select"))       c->key_select = (uint16_t)atoi(v);
        else if (!strcmp(k, "rom"))              snprintf(c->rom_path,  sizeof c->rom_path,  "%s", v);
        else if (!strcmp(k, "rom_dir"))          snprintf(c->rom_dir,   sizeof c->rom_dir,   "%s", v);
        else if (!strcmp(k, "full_refresh_permille")) c->full_refresh_permille = atoi(v);
        else if (!strcmp(k, "refresh_fixed_tiles")) {
            /* Clamped to >= 0, not merely accepted. video_split_dirty's cost
               sum adds fixed_tiles once per candidate rect (src/video.c), so
               a negative value makes the split branch cheaper the MORE rects
               it emits -- the opposite of a fixed cost, and unbounded as the
               candidate count approaches KOBOY_MAX_RECTS. cleanup_interval and
               full_refresh_permille guard <= 0 above for the same reason (a
               bad config value must not invert the behaviour it controls);
               this one clamps instead of treating <= 0 as "off" because 0 is
               itself a real, meaningful value here (no fixed cost at all),
               not a sentinel. */
            int t = atoi(v);
            c->refresh_fixed_tiles = t < 0 ? 0 : t;
        }
        else if (!strcmp(k, "waveform_fast"))
            c->wfm_fast_policy = !strcmp(v, "du4") ? KOBOY_WFM_DU4 : KOBOY_WFM_AUTO;
        /* An ini `core=` is an explicit choice and outranks the ROM's
           extension -- see core_explicit in config.h. WITH ONE EXCEPTION,
           and it is not a special case so much as a dated one: every koboy.ini
           written before this feature existed carries a literal
           `core = gambatte_libretro.so`, because that is what v1 shipped
           uncommented when gambatte was the only core there was. That line
           records PACKAGING, not preference -- nobody chose it -- so honouring
           it as a pin would silently disable choice-by-extension for every
           existing install, and present as ".mgw files are listed but refuse
           to load". A redeploy overwrites koboy.ini and would fix it, but
           docs/device-workflow.md tells the user to carry values forward from
           their backup, which is exactly how the dead line comes back.
           So: the historical default value alone does not mark intent.
           `--core` on the command line is exempt from the exemption -- it is
           typed deliberately, now, and can mean nothing else. */
        else if (!strcmp(k, "core")) {
            snprintf(c->core_path, sizeof c->core_path, "%s", v);
            c->core_explicit = strcmp(v, KOBOY_CORE_LEGACY_DEFAULT) != 0;
        }
        else if (!strcmp(k, "save_dir"))         snprintf(c->save_dir,  sizeof c->save_dir,  "%s", v);
        else if (!strcmp(k, "menu_cx"))          c->layout.menu_cx = atoi(v);
        else if (!strcmp(k, "menu_cy"))          c->layout.menu_cy = atoi(v);
        else if (!strcmp(k, "menu_w"))           c->layout.menu_w  = atoi(v);
        else if (!strcmp(k, "menu_h"))           c->layout.menu_h  = atoi(v);
        /* unknown keys ignored on purpose: forward compatibility */
    }
    fclose(f);
    return true;
}

bool config_resolve_profile(koboy_profile *p, const koboy_config *c,
                            int panel_w, int panel_h,
                            int base_w, int base_h, int max_w, int max_h)
{
    return config_resolve_profile_par(p, c, panel_w, panel_h,
                                      base_w, base_h, max_w, max_h,
                                      KOBOY_ASPECT_ONE);
}

bool config_resolve_profile_par(koboy_profile *p, const koboy_config *c,
                                int panel_w, int panel_h,
                                int base_w, int base_h, int max_w, int max_h,
                                uint32_t par)
{
    memset(p, 0, sizeof *p);
    /* LIVE GUARD: a core (or a hand-built test profile) reporting a
       non-positive geometry has nothing this function can scale -- dividing
       by max_w/max_h below would be a division by zero or a negative divisor,
       not merely a "too big to fit" case like max_fit < 1 already handles.
       Refusing here keeps that division safe without every caller having to
       re-derive the same check. */
    if (max_w < 1 || max_h < 1) return false;
    /* LIVE GUARD, and it is new with the base-sized rect below: base_w/base_h
       used to be carried through untouched and could be anything, because
       nothing divided by them. The DMG branch now does. A caller that has not
       got a geometry yet (main.c's placeholder profile passes the Game Boy's,
       so it is not that one) would otherwise divide by zero. */
    if (base_w < 1 || base_h < 1) return false;

    /* Both layouts reserve the game rect clear of whatever controls their
       faceplate draws, and both ask the same function where those start --
       see chrome.h. Hoisted above the split because it is the one thing the
       two branches genuinely share. */
    int ctrl_top = chrome_controls_top(c->layout_mode, &c->layout, panel_w, panel_h);

    /* The reserved rect's WIDTH IN SOURCE PIXELS. max_w for square pixels --
       which is what this whole function used before non-square ones existed,
       and what it still computes bit for bit, since par == KOBOY_ASPECT_ONE
       makes the multiply exact and the round-up a no-op. See
       config_resolve_profile_par in config.h for why the rect and not just
       the per-frame fit has to know.

       ROUNDED UP, not to nearest: the rect has to HOLD the widened picture,
       and rounding down by one source pixel costs an entire integer step of
       scale when it makes the fit's own ceiling one pixel short. Measured:
       at nearest-rounding the NES rect came out 292 and the frame needed
       877.7 of the 876 that gave, so the fit dropped from 3x to 2x -- the
       exact failure this parameter exists to remove, reintroduced by a
       rounding mode. */
    if (par == 0) par = KOBOY_ASPECT_ONE;

    /* WHICH GEOMETRY THE RECT IS SIZED FROM, and the two layouts answer
       differently on purpose.

       LCD keeps MAX. Its fit is fractional, so a frame smaller than max costs
       nothing but margin, and a Game & Watch title changes base several times
       a second (654x396 <-> 305x191 on Donkey Kong): sizing that rect from
       base would resize the artwork, redraw the strip and repaint the panel
       at that rate. Nothing about the Game & Watch presentation changes here.

       DMG takes BASE -- what the core is drawing NOW -- and that is the
       change. Max was chosen when the only two cores had base == max, and it
       is wrong for a core whose max is a mode it never enters: snes9x2005
       declares 512x512 for an interlaced hi-res mode almost nothing uses and
       then draws 256x224 forever, and a 512-tall reservation cannot exceed
       scale 1 under chrome_controls_top. MEASURED on the verified 1264x1680
       panel: 597x448 presented, against the Game Boy's 800x720 -- 46% of the
       area, on a system with 1.8x the Game Boy's pixels. From base it is
       1196x896.

       What made max safe was that a frame anywhere in [1, max] could not
       spill out of the rect. That defence has MOVED rather than gone:
       video_fit_rect now falls back to the fractional fit for any frame the
       integer one cannot shrink to size (its 1x floor could not), so a frame
       larger than base is presented SMALLER inside the rect instead of
       overflowing it. Check that before believing this comment -- it is the
       whole safety argument, and it is asserted by sweep in
       tests/test_video_pipeline.c.

       The BUFFER is still allocated from max (video_create). That is memory
       safety and it did not move. */
    int rect_w = max_w, rect_h = max_h;
    if (c->layout_mode != KOBOY_LAYOUT_LCD) { rect_w = base_w; rect_h = base_h; }
    if (par != KOBOY_ASPECT_ONE) {
        rect_w = (int)((((uint64_t)rect_w * par) + 65535u) >> 16);
        if (rect_w < 1) rect_w = 1;
    }

    if (c->layout_mode == KOBOY_LAYOUT_LCD) {
        /* The LCD layout, in three lines, because it has no scale search to
           do: the rect is simply the largest aspect-preserving fit of the
           core's MAX geometry into the full panel width and everything above
           the bottom strip. Fractional, so a 654x396 Mickey Mouse unit fills
           1264 columns instead of the 654 an integer scale of 1 would leave
           it at -- the "too small" the device reported.

           No KOBOY_CHROME_MARGIN on the sides, deliberately, unlike the DMG
           branch below: the whole point of dropping the drawn controls is to
           give the artwork the panel, and a Game & Watch unit's own artwork
           already has a moulded border drawn into it. The vertical margin is
           whatever centring leaves over. */
        if (panel_w < 1 || ctrl_top < 1) return false;
        int gw = 0, gh = 0;
        video_fit_frac(rect_w, rect_h, panel_w, ctrl_top, &gw, &gh);
        if (gw < 1 || gh < 1) return false;

        p->panel_w = panel_w;
        p->panel_h = panel_h;
        p->base_w  = base_w;
        p->base_h  = base_h;
        p->max_w   = max_w;
        p->max_h   = max_h;
        p->layout_mode = KOBOY_LAYOUT_LCD;
        p->game_w  = gw;
        p->game_h  = gh;
        p->game_x  = (panel_w - gw) / 2;
        p->game_y  = (ctrl_top - gh) / 2;
        /* INFORMATIONAL ONLY in this layout, and said here because the field
           name promises more than it can deliver: the real fit is fractional
           and lives in game_w/game_h, which is what video.c and chrome.c both
           read. Nothing outside the startup log line and the tests consumes
           p->scale (checked: it has no other reader in src/). The integer
           part is reported rather than 0 or 1 so the log still says something
           true about how much bigger the picture got. */
        p->scale = gw / rect_w;
        if (p->scale < 1) p->scale = 1;
        return true;
    }

    int fit_w = panel_w / rect_w;
    int fit_h = panel_h / rect_h;
    int max_fit = fit_w < fit_h ? fit_w : fit_h;
    if (max_fit < 1) return false;
    /* The configured scale is the GAME BOY's scale unless the user said
       otherwise, and applying it to every system was wrong in a way only a
       second system could reveal. 5 was measured for 160x144: it is what makes
       800x720 sit inside the DMG faceplate, and the design spec explicitly
       rejected a full-width Game Boy in favour of it. Auto-fitting the Game
       Boy today lands on 6 (measured by mutating this very branch off: the
       chrome goldens and test_config's sweep both go red at 6), so the 5 is a
       deliberate choice against the panel's maximum, not a coincidence of the
       arithmetic.

       KEYED ON MAX, not on the base the rect is now sized from, and that is
       load-bearing rather than leftover: a Game Gear's BASE is 160x144 --
       byte for byte the Game Boy's -- while its max is 284x240, so keying
       this on base would hand the Game Gear the Game Boy's measured 5 and
       shrink it. Genesis Plus GX is the core that makes those two questions
       different. None of that reasoning transfers. A Pokemon Mini is 96x64, so scale 5 is
       480x320 -- a postage stamp on a 1264x1680 panel, and precisely the
       complaint the Game & Watch layout was rebuilt to answer.

       So: an explicitly configured scale still wins, for every system. Absent
       one, the Game Boy keeps its measured 5 and every other system fits
       itself to the panel. Keyed on the geometry rather than on the core,
       because 5 was measured for that geometry and nothing else. */
    bool is_game_boy = (max_w == KOBOY_GB_W && max_h == KOBOY_GB_H);
    int want = (c->scale_explicit || is_game_boy) ? c->scale : 0;
    int s = want > 0 ? want : max_fit;
    if (s > max_fit) s = max_fit;        /* configured scale does not fit */
    /* The per-system ceiling applies to an AUTO-fitted scale only: an
       explicit `scale =` is the owner overriding a default, and a default is
       all this is. See `ceiling` on g_core_by_ext for the measurement. */
    if (!c->scale_explicit && c->scale_ceiling > 0 && s > c->scale_ceiling)
        s = c->scale_ceiling;

    /* Two reservations, not one. KOBOY_CHROME_MARGIN keeps the bezel inside the
       buffer; ctrl_top keeps the game rect off the CONTROLS. Reserving only the
       bezel margin was the bug: at scale = 0 the fitted rect cleared the panel
       edges comfortably and still covered the drawn A button and d-pad on all
       four supported panels (on the Libra 2 it chose scale 7 and put 15,677
       chrome pixels inside the game rect). The touch zones stay live underneath
       a rect drawn over them, so tapping the lower playfield pressed A or a
       direction -- unplayable at the one setting whose whole purpose is a bigger
       picture. Shrinking the controls was the other way out and is explicitly
       not the fix: they are the only way to play on a device with no buttons.
       See chrome.h for why chrome.c owns the geometry, and the hoisted
       ctrl_top above for where it is now computed. */

    while (s > 1) {
        int game_w = rect_w * s;
        int game_h = rect_h * s;
        int game_x = (panel_w - game_w) / 2;
        int game_y = panel_h / 20;
        int left_margin   = game_x;
        int right_margin  = panel_w - game_x - game_w;
        int top_margin    = game_y;
        int bottom_margin = panel_h - game_y - game_h;

        if (left_margin >= KOBOY_CHROME_MARGIN &&
            right_margin >= KOBOY_CHROME_MARGIN &&
            top_margin >= KOBOY_CHROME_MARGIN &&
            bottom_margin >= KOBOY_CHROME_MARGIN &&
            game_y + game_h <= ctrl_top) {
            break;  /* margins sufficient AND clear of the control band */
        }
        s--;
    }
    /* The floor stays 1 rather than becoming a failure: at scale 1 the rect is
       rect_w x rect_h and every panel spec §3 supports (>= 1072x1448) clears
       the control band by hundreds of pixels for the shipped Game Boy core's
       160x144 (this is unreachable there) -- and on some hypothetical tiny
       panel, or a core whose max geometry is itself large, running with a
       slightly overlapped control band still beats refusing to start.
       chrome_render clamps its own writes either way; it does not rely on
       this loop. */

    p->scale   = s;
    p->panel_w = panel_w;
    p->panel_h = panel_h;
    p->base_w  = base_w;
    p->base_h  = base_h;
    p->max_w   = max_w;
    p->max_h   = max_h;
    /* game_w/game_h -- and therefore the reserved rect chrome lays out around
       -- come from rect_w/rect_h, which for this layout is the core's BASE
       geometry. See the long note where rect_w is computed for why that is
       now safe and what it bought; the short version is that the rect no
       longer has to hold every frame the core COULD send, because
       video_fit_rect shrinks the ones it cannot hold. video_create's buffer
       still comes from max, and that is the part that is memory safety. */
    p->game_w  = rect_w * s;
    p->game_h  = rect_h * s;
    p->game_x  = (panel_w - p->game_w) / 2;
    p->game_y  = panel_h / 20;           /* small top margin, chrome fills the rest */
    return true;
}

bool config_profile_presentation_same(const koboy_profile *a,
                                      const koboy_profile *b)
{
    if (!a || !b) return false;
    return a->game_x == b->game_x && a->game_y == b->game_y &&
           a->game_w == b->game_w && a->game_h == b->game_h &&
           a->scale  == b->scale  &&
           a->layout_mode == b->layout_mode &&
           a->max_w  == b->max_w  && a->max_h  == b->max_h &&
           a->panel_w == b->panel_w && a->panel_h == b->panel_h;
}

/* Rewrites `path`, dropping every assignment whose key is in `drop` and
   appending `block` verbatim. Everything else -- comments, blanks, other keys,
   even the file's ordering -- comes through untouched.

   ONE implementation for both writers. config_save_keys (first-run
   calibration) and config_save_gray_map (the in-game GREYSCALE entry) want
   exactly the same read-filter-append-rename discipline over the same file,
   and two copies of it would be two places to get the temp-file, the trailing
   newline or the atomic rename wrong. */
static bool rewrite_ini(const char *path, const char *const *drop, int ndrop,
                        const char *block)
{
    /* Create temp file in same directory as target to ensure same filesystem
       (so rename succeeds atomically, and so writes fail fast if disk full) */
    char temp_path[512];
    snprintf(temp_path, sizeof temp_path, "%s.tmp", path);

    FILE *out = fopen(temp_path, "w");
    if (!out) return false;

    FILE *in = fopen(path, "r");
    int last_char_written = 0;
    if (in) {
        char line[1024];
        while (fgets(line, sizeof line, in)) {
            /* Check if this is an assignment (has '=' before any '#').
               If so, check if its key is one of `drop` and skip it if so.
               Pure comment lines (# ...) or blank lines pass through unchanged. */
            char *eq = strchr(line, '=');
            char *hash = strchr(line, '#');

            /* If there's an '=' and it comes before any '#', this is an assignment */
            if (eq && (!hash || eq < hash)) {
                /* Extract key name (everything before '=', trimmed) */
                char k[1024];
                int  skip = 0;
                snprintf(k, sizeof k, "%.*s", (int)(eq - line), line);
                trim(k);
                for (int i = 0; i < ndrop; i++)
                    if (!strcmp(k, drop[i])) { skip = 1; break; }
                if (skip) continue;
            }

            /* Preserve this line (comments, blanks, other keys) */
            fputs(line, out);
            size_t llen = strlen(line);
            if (llen) last_char_written = line[llen - 1];
        }
        fclose(in);
    }

    /* A source ini with no trailing newline would otherwise have the appended
       block concatenated onto its final line. Harmless today only because
       config_load truncates at the resulting '#', but it silently rewrites an
       unrelated line -- against the "preserve everything else" intent this
       function exists to honour. */
    if (last_char_written && last_char_written != '\n') fputc('\n', out);

    fputs(block, out);

    if (fclose(out) != 0) {
        /* fclose failed; temp file is left but out of sync, remove it */
        remove(temp_path);
        return false;
    }

    /* Atomically replace the target file */
    if (rename(temp_path, path) != 0) {
        remove(temp_path);
        return false;
    }

    return true;
}

bool config_save_keys(const char *path, uint16_t key_a, uint16_t key_b)
{
    /* Filtering out the old key_a/key_b lines first is what makes calibration
       idempotent: whether called for the first time or on recalibration, the
       file ends with exactly one key_a line and one key_b line. */
    static const char *const drop[] = { "key_a", "key_b" };
    char block[128];
    snprintf(block, sizeof block,
             "# written by first-run calibration\nkey_a = %u\nkey_b = %u\n",
             (unsigned)key_a, (unsigned)key_b);
    return rewrite_ini(path, drop, 2, block);
}

bool config_save_gray_map(const char *path, koboy_gray_map map)
{
    /* video_gray_map_name never returns NULL, and for an out-of-range map it
       names the default -- so what lands in the file is always a name
       config_load will parse back to a real mapping, never a number or an
       empty value. */
    static const char *const drop[] = { "gray_map" };
    char block[128];
    snprintf(block, sizeof block,
             "# written by the in-game GREYSCALE menu entry\ngray_map = %s\n",
             video_gray_map_name(map));
    return rewrite_ini(path, drop, 1, block);
}

/* The values the in-game FRAMES entry cycles through, ascending. Contract and
   the reasoning for these six and not 1..8 are on config_next_present_divisor
   in config.h. Kept here, in the one file that both the loader and the menu's
   policy live in, so the ini's valid range and the menu's offer cannot drift:
   tests/test_config.c asserts the last entry is KOBOY_PRESENT_DIVISOR_MAX. */
static const int PRESENT_DIVISOR_LADDER[] = { 1, 2, 3, 4, 6, 8 };
#define PRESENT_DIVISOR_LADDER_N \
    ((int)(sizeof PRESENT_DIVISOR_LADDER / sizeof PRESENT_DIVISOR_LADDER[0]))

bool config_present_divisor_ok(int v)
{
    return v >= 1 && v <= KOBOY_PRESENT_DIVISOR_MAX;
}

int config_next_present_divisor(int cur)
{
    for (int i = 0; i < PRESENT_DIVISOR_LADDER_N; i++)
        if (PRESENT_DIVISOR_LADDER[i] > cur) return PRESENT_DIVISOR_LADDER[i];
    return PRESENT_DIVISOR_LADDER[0];
}

bool config_save_present_divisor(const char *path, int divisor)
{
    static const char *const drop[] = { "present_divisor" };
    char block[128];
    /* Checked before the write, not after: rewrite_ini would happily produce a
       file config_load then ignores, and a menu whose choice silently does not
       survive the relaunch is worse than one that reports it could not save. */
    if (!config_present_divisor_ok(divisor)) return false;
    snprintf(block, sizeof block,
             "# written by the in-game FRAMES menu entry\npresent_divisor = %d\n",
             divisor);
    return rewrite_ini(path, drop, 1, block);
}
