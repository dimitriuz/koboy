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

/* One clamp for both settle keys, for the reason pacer_set_divisor is one
   function: two copies of a bound is how one of them eventually loses it. */
static int clamp_settle_ms(int v)
{
    if (v < 0) return 0;
    if (v > KOBOY_SETTLE_MS_MAX) return KOBOY_SETTLE_MS_MAX;
    return v;
}

void config_defaults(koboy_config *c)
{
    memset(c, 0, sizeof *c);
    c->scale = 5;
    /* SHIPPED BUTTON DEFAULTS, and a zero here looks harmless but is not:
       calib_needed() treats 0 as "not calibrated" and the calibration loop only
       advances on a hardware key press, so a first run on a touch-only Kobo
       (Clara family, Nia, Elipsa) sat on "press the button you want as A"
       forever. Spec §7 mandates built-in defaults as the starting guess
       calibration overrides. The codes are the Libra 2's two page-turn buttons,
       MEASURED off its gpio-keys node (spec §12). Never default either to
       KOBOY_KEY_POWER: they share that node with it and power is the quit key. */
    c->key_a = KOBOY_KEY_PAGE_F23;
    c->key_b = KOBOY_KEY_PAGE_F24;
    /* A GUESS, unlike key_a/key_b above: a Kobo has exactly two hardware
       buttons and they are already spent. BTN_TL/BTN_TR (Xbox LB/RB) are
       distinct from A/B/d-pad on the one pad this project has measured (spec
       Appendix A, 2026-08-26); nothing says every user wants shoulders for
       Start/Select. Overridable, and calibration exists for this -- see
       KOBOY_KEY_BTN_TL in koboy.h. With no pad these codes never arrive and
       Start/Select stay reachable through the drawn faceplate. */
    c->key_start = KOBOY_KEY_BTN_TL;
    c->key_select = KOBOY_KEY_BTN_TR;
    c->present_divisor = KOBOY_PRESENT_DIVISOR_DEFAULT;
    /* Ghosting mitigation, DISABLED BY DEFAULT -- not an oversight.
       This and full_refresh_permille below were written for forced DU4, which
       cannot erase, so only a periodic flashing GC16 cleared residue. AUTO
       hands that choice to the EPDC, which already picks an erasing waveform
       when a region erases, making both redundant. MEASURED on the Libra 2 from
       a refresh trace: 35 AUTO refreshes on small rects, none flashing, against
       21 GC16 flashes -- every one a large region tripping the old 450-permille
       threshold. Zero scheduled cleanups fired. So the threshold caused all the
       flashing the user complained about and the cleanup did nothing.
       With both off, a full game of Tetris played with no flashing and only
       slight ghosting -- the trade the user preferred. Raise cleanup_interval
       (presented frames) or cleanup_max_ms (wall clock) to get the periodic
       flash back; the wall-clock ceiling exists because the dirty-rect pass
       suppresses unchanged frames, so presented frames are a poor clock -- 70 s
       of measured gameplay presented only 45. */
    c->cleanup_interval = 0;
    c->cleanup_max_ms = 0;
    /* 1000 permille: the promotion fires only on a corner-to-corner dirty
       rect. Reachable (the dirty rect is one merged bounding box, so a
       full-screen wipe produces exactly it) but rare in play, and when the
       whole rect really changed a flash is wanted anyway. This threshold, not
       the cleanup, was the measured source of the flashing -- see above. 450
       was the old default. */
    c->full_refresh_permille = 1000;
    /* A STARTING GUESS, not a measurement. An on-device 20/40/80 sweep found
       the three behaviourally identical on real content; the in-process timer
       cannot see the panel-side cost this is meant to amortise. See
       docs/FOLLOWUPS.md #24 and TESTED.md. */
    c->refresh_fixed_tiles = 40;
    /* AREA-AWARE PRESENT PACING -- MEASURED. Numbers in config/koboy.ini,
       method in the v1 design spec's Appendix D. BOTH 0 restores pure
       present_divisor pacing. */
    c->settle_base_ms = KOBOY_SETTLE_BASE_MS_DEFAULT;
    c->settle_full_ms = KOBOY_SETTLE_FULL_MS_DEFAULT;
    /* 1-BIT OUTPUT ON BY DEFAULT -- a deliberate reversal. It shipped off
       until someone judged it on a panel; it fixes the motion smearing that
       was the project's oldest open defect, because the fast waveforms are
       two-level and four-level content asks them for states they cannot reach,
       landing between and leaving a ghost plus overshoot. Confirmed by the
       owner on NES Super Mario Bros., 2026-08-27 (TESTED.md).
       AUTO stays the waveform: on 1-bit content it already selects DU to
       within 0.5 ms at three region sizes, so forcing DU buys nothing and DU4
       is the four-level variant this fix exists to avoid. */
    c->force_dither = true;
    c->wfm_fast_policy = KOBOY_WFM_AUTO;
    c->gray_map = KOBOY_GRAY_DEFAULT;
    c->grab_input = true;
    c->pixel_aspect = true;
    /* CROSS, because the faceplate draws an absolute four-way cross and the
       drawing is what a user trusts. Relative mode steers from wherever the
       finger first landed, which needs a drag a drawn cross gives no hint of:
       the user could not steer in relative mode and could immediately in
       cross. `dpad_mode = relative` for the thumb-pad behaviour. */
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
    snprintf(c->shot_dir, sizeof c->shot_dir, "screenshots");
    /* Control geometry, permille of panel. Game rect occupies the top; the
       d-pad sits lower-left under the left thumb, A/B lower-right. */
    koboy_layout l = { .dpad_cx = 220, .dpad_cy = 720, .dpad_r = 150,
                       .a_cx = 830, .a_cy = 670, .a_r = 85,
                       .b_cx = 660, .b_cy = 760, .b_r = 85,
                       .start_cx = 610, .start_cy = 920, .start_w = 200, .start_h = 55,
                       .select_cx = 390, .select_cy = 920, .select_w = 200, .select_h = 55,
                       /* MENU sits on the Start/Select row, NOT in the open
                          band below the game rect. That band looks free and is
                          not: chrome_controls_top is bound by the highest
                          control, and a zone at 540 permille measured lower
                          than Start/Select's 920 on every panel shorter than
                          the Libra 2. Measured on Clara 1072x1448,
                          chrome_controls_top 879 -> 742, knocking the shipped
                          scale from 5 (800x720) to 4 (640x576), a 36% area
                          loss -- and not only at scale = 0, because
                          config_resolve_profile's fitting loop demotes an
                          explicit scale too. The spec's promise that 5x fits
                          every supported panel depends on nothing lowering
                          chrome_controls_top below the scale-5 rect.
                          The Start/Select row is already reserved by two
                          controls, so a third costs nothing. Verified at scale
                          5 on every panel -- gap to Start, clearance of B,
                          right margin past KOBOY_CHROME_MARGIN:
                            Clara  1072x1448  x[804..1018] y[1293..1371]  gap-to-Start 44px  clear-of-B 102px  right-margin 54px
                            Libra2 1264x1680  x[948..1200] y[1499..1591]  gap 51  clear 116  right-margin 64
                            Elipsa 1404x1872  x[1053..1333] y[1671..1773] gap 57  clear 130  right-margin 71
                            Sage   1440x1920  x[1080..1368] y[1714..1818] gap 58  clear 133  right-margin 72 */
                       .menu_cx = 850, .menu_cy = 920, .menu_w = 200, .menu_h = 55 };
    c->layout = l;
}

/* ------------------------------------------------------ core by extension
 *
 * Which core a file needs is knowable from its name alone; the table below is
 * the whole mapping. The browser hands main.c a path long after the config was
 * read, so this cannot live in config_load -- it is a pure function of the ROM
 * name, called at load time.
 *
 * Its own case-insensitive suffix match rather than strcasecmp or romlist.c's:
 * config is the LOWER layer (romlist includes nothing of config's and must
 * keep it that way), and <strings.h> is a host-dependent header this project
 * keeps out of portable code.
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

/* A table rather than a chain of ifs: every new system needs an entry in BOTH
   this map and romlist_is_rom, and a table makes the pair reviewable side by
   side. Extensions are lowercase because ends_with_ext lowercases only the
   candidate, not the pattern.

   `ceiling` caps the auto-fitted scale for this system, 0 = no cap. In
   KOBOY_LAYOUT_LCD, where the fit is fractional, it caps the picture at
   `ceiling` times the source instead -- see config_resolve_profile_par.

   IT EXISTS BECAUSE A BIGGER PICTURE COSTS REAL, MEASURED SPEED, and
   video_submit is this project's bottleneck on every system. Sizing the rect
   from the frame a core really draws (ae03e76) quadrupled SNES's picture and
   cost its heaviest titles: Star Fox 93%->67%, Kirby Super Star 96%->78%,
   while Mario World and Zelda stayed at 98%.

   FOUR SYSTEMS CARRY ONE, every number a device measurement: Kobo Libra 2,
   1264x1680, `--frames 900` against an ideal 15,024 ms, ten seconds idle
   between runs, presented-frame count identical within each sweep:

     .sfc/.smc  SNES         3   Star Fox 67% -> 79%, Kirby 78% -> 95%
     .sms       Master Sys.  3   Sonic Chaos 1172x768 83% -> 879x576 98%
     .gg        Game Gear    5   Sonic Chaos 1152x864 79% -> 960x720
     .md        Mega Drive   3   Sonic 1264x966 -> 879x672

   THE THREE LARGEST RECTS koboy PRODUCES WERE ALL SEGA, all over 900k pixels,
   all reported slow in play -- 1.6x and 1.7x the Game Boy's 800x720 on frames
   the same size or smaller.

   THE GAME GEAR IS THE INSTRUCTIVE ONE: its frame is 160x144, byte for byte
   the Game Boy's, and TESTED.md recorded for months that it "needs no
   exemption" because it lands on exactly 800x720 by arithmetic. True until
   `pixel_aspect` made the rect 192 columns wide, the auto-fit went 5 -> 6 and
   the picture became 1152x864 with nothing watching. Its ceiling is 5 because
   5 is the number MEASURED for a 160x144 frame on this panel; 960x720 is that
   picture with the added pixel aspect. Sub-scale steps do not exist, so the
   only alternative is 4 (768x576, smaller than the Game Boy).

   NOBODY HAS MEASURED THE OTHER NINE -- docs/FOLLOWUPS.md #73. A row with no
   number beside it here is a guess.

   Per-system rather than global because the global `scale` is shared with
   systems that pay nothing for their fit. An explicit `scale =` overrides
   this; the shipped ini's `scale = 5` is NOT explicit (see
   KOBOY_SCALE_LEGACY_DEFAULT), so these ceilings are what a device runs. */
static const struct { const char *ext; const char *core; int ceiling; } g_core_by_ext[] = {
    { ".mgw", "gw_libretro.so", 0 },   /* Game & Watch, gw-libretro   */
    { ".nes", "fceumm_libretro.so", 0 },   /* NES, libretro-fceumm        */
    /* Pokemon Mini caps at 8, and the number is the Game Boy's own size. Its
       96x64 frame is the smallest koboy runs, so auto-fit reaches scale 13 --
       1248x832, edge to edge; the owner called it "huge and slow". Measured,
       300 frames each:

           auto (13)  1248x832   submit 22.3 ms
           12         1152x768          20.0
           10          960x640          15.7
           8           768x512          11.9   <- this row
           6           576x384           9.0

       The Game Boy at its verified scale 5 is 800x720 for 15.4 ms, so scale 8
       gives the same presence on the panel for three quarters of the cost.
       Chosen for the presence, not the timing. */
    { ".min", "pokemini_libretro.so", 8 },   /* Pokemon Mini, libretro/PokeMini */
    /* One core per SYSTEM FAMILY: beetle-wswan reports `ws|wsc|pc2`, RACE
       reports `ngp|ngc|ngpc|npc`, so mono and Color are the same .so. Two rows
       each rather than a prefix match, because a reviewer reads this table
       against romlist_is_rom's list and a wildcard breaks that correspondence.
       .pc2/.ngpc/.npc are left out for the reason .fds is: nobody's collection
       here uses them, and an extension nobody has loaded is an untested
       claim. */
    { ".ws",  "mednafen_wswan_libretro.so", 0 }, /* WonderSwan, beetle-wswan */
    { ".wsc", "mednafen_wswan_libretro.so", 0 }, /* WonderSwan Color         */
    { ".ngp", "race_libretro.so", 0 },  /* Neo Geo Pocket, libretro/RACE */
    { ".ngc", "race_libretro.so", 0 },  /* Neo Geo Pocket Color        */
    { ".a26", "stella2014_libretro.so", 0 },  /* Atari 2600, stella2014      */
    { ".col", "gearcoleco_libretro.so", 0 },  /* ColecoVision, drhelius/Gearcoleco */
    { ".int", "freeintv_libretro.so", 0 },  /* Intellivision, libretro/FreeIntv */
    /* Master System and Game Gear are the same VDP behind a different
       viewport, so Genesis Plus GX runs both from one binary. Two rows, not a
       wildcard, for the reason above. .sg (SG-1000, also accepted) is absent:
       nobody's collection here has it. */
    { ".sms", "genesis_plus_gx_libretro.so", 3 }, /* Master System -- ceiling above */
    { ".gg",  "genesis_plus_gx_libretro.so", 5 }, /* Game Gear, same core, own cap */
    /* MEGA DRIVE -- the SAME .so as the two rows above; Genesis Plus GX is
       natively a Mega Drive core.

       .MD ONLY. NOT .bin, NOT .gen, deliberately. The owner's tree is 1736
       .md, 31 .bin, 5 .gen; the 36 unclaimed files are homebrew and demoscene.

       .bin is refused because koboy picks the core FROM THE EXTENSION AND
       NOTHING ELSE, and .bin is the most contested extension in retro
       computing. COUNTED across the owner's collection: 723 TI-99/4A, 234
       Odyssey 2, 119 Atari 5200, 72 Arcadia 2001, 71 Vectrex, 68 Astrocade,
       56 VC 4000, 38 Jaguar, 36 Mega Drive, 34 Channel F, 33 CreatiVision, 28
       Intellivision. Mega Drive is the NINTH largest claimant here, and two of
       the files ahead of it (exec.bin, grom.bin) are the BIOS koboy asks the
       owner to install. A row here would route all of those to a 68000
       emulator.
       .gen is unambiguous but is five files, left out to keep the rule
       holdable: one system, one extension. roms/README.txt says so on the
       device. */
    { ".md",  "genesis_plus_gx_libretro.so", 3 }, /* Mega Drive -- ceiling above */
    /* SNES. The v1 design spec ruled this system OUT on CPU grounds; that was
       re-tested rather than inherited -- scripts/build-snes-core.sh has the
       three-core shootout, TESTED.md the device figures.

       .sfc and .smc are the same cartridge behind different dumping
       conventions (.smc historically carries a 512-byte copier header the core
       detects and skips). .fig/.swc/.bs are NOT claimed: nobody's collection
       here has them.

       Case-insensitivity is load-bearing, not hypothetically: the author's
       SNES directory holds 47 .smc and 11 .SMC side by side on FAT32. */
    { ".sfc", "snes9x2005_libretro.so", 3 },   /* SNES -- see `ceiling` above */
    { ".smc", "snes9x2005_libretro.so", 3 },   /* SNES, copier-header dump   */
    /* PC ENGINE / TurboGrafx-16, CARTRIDGE ONLY. The core advertises
       `pce|sgx|cue|ccd|chd|toc|m3u`; one is claimed. .sgx (SuperGrafx, 7 files
       here) is refused because beetle-pce-FAST implements neither the second
       VDC nor the priority mixer, so it would render WRONGLY rather than
       refuse -- worse than absence. .chd and the CD extensions (48 titles)
       need a system-card BIOS not ours to ship. See
       scripts/build-pce-core.sh. */
    { ".pce", "mednafen_pce_fast_libretro.so", 0 }, /* PC Engine, beetle-pce-fast */
    /* GAME BOY ADVANCE -- the SECOND system the v1 spec ruled out on CPU
       grounds that turned out playable (SNES was the first, which is why this
       was re-tested). scripts/build-gba-core.sh has the three-core shootout,
       TESTED.md the device figures.

       ONE EXTENSION: .gba has never meant anything else -- the owner's 1693
       files all end .gba, no .agb, no .bin, no copier convention.

       CEILING 4, AND IT IS NOT A HEADROOM CAP -- IT IS WHAT MAKES THE SYSTEM
       EXIST. A GBA frame is 240x160 square, the smallest koboy scales, so an
       uncapped auto-fit reaches the LCD strip's full width: 1264x842,
       1,064,288 px. MEASURED (koboy-arm --frames 900, Advance Wars 2, scale
       pinned; table in TESTED.md):

           scale 3  720x480   submit 11.3 ms  pipeline 14.8 ms
           scale 4  960x640   submit 18.1 ms  pipeline 24.9 ms   <- this row
           scale 6 1264x842   submit 30.3 ms  pipeline 40.7 ms

       At present_divisor = 2 the per-core-frame budget is 16742 - pipeline/2:
       4,316 us at scale 4, and every measured title fits. At the uncapped fit
       it is NEGATIVE -- 20.3 ms of presentation against a 16.7 ms frame.

       Do not raise this without re-measuring. The one title ON the budget
       rather than inside it is Metroid Fusion scrolling (4,467 us, 99.1% of
       full speed) -- docs/FOLLOWUPS.md #87, whose remedy is the divisor, not a
       smaller picture for the whole library. */
    { ".gba", "gpsp_libretro.so", 4 }, /* Game Boy Advance, gpSP -- ceiling above */
    /* THE FIRST EXTENSION HERE THAT IS NOT A SYSTEM'S OWN. An arcade "ROM" is
       a ZIP of one PCB's EPROM dumps, CRC-checked against the emulator's dat,
       so .zip says "archive", not "Namco board".

       .ZIP IS CLAIMED FOR THE ARCADE CORE OUTRIGHT rather than routed by
       subdirectory or dat lookup, because nothing else koboy ships can open a
       .zip AT ALL: nine of the ten other cores set need_fullpath = false, so
       core_load_rom hands them the file's BYTES and a zipped .nes reaches
       fceumm as literal "PK\3\4...", which it rejects. No routing this row
       breaks would have worked.

       WHAT IT COSTS: a zipped Game Boy ROM in roms/ now gets FinalBurn Neo
       refusing it ("core rejected rom") instead of the browser ignoring the
       file -- a worse-looking failure for a file that could never have run,
       and the price of not building a dat parser into a 40 KB front-end. The
       error names the core.

       FBNeo also advertises `7z`, `cue`, `ccd`; none claimed. .7z is
       UNBUILDABLE here -- lib7z does not compile against glibc 2.19's headers,
       so scripts/build-fbneo-core.sh switches it off and the shipped core
       physically cannot open one. .cue/.ccd are Neo Geo CD: outside the
       pre-1990 scope and wanting a BIOS. */
    { ".zip", "fbneo_libretro.so", 0 }, /* arcade, FinalBurn Neo    */
};

const char *config_core_for_rom(const char *rom_path)
{
    /* No name still answers gambatte: the caller writes the result into
       core_path unconditionally, so a NULL return would be a crash where the
       old unconditional default was merely useless. Gambatte is also the
       fall-through for an unlisted extension. */
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
       `% Memory.CalculatedSize`, so anything under one block divides by zero:
       SIGFPE inside retro_load_game, taking koboy with it. MEASURED, not
       deduced: every size 0..1024 kills the loader (exit 136), 8192 does not;
       backtrace retro_load_game -> LoadROM -> InitROM -> LoROMMap.

       Not hypothetical: the author's collection has one, a 212-byte
       `._desire_d-zero_....smc` AppleDouble stub of the kind every
       FAT32-and-macOS collection grows. A partial download does the same.

       ONLY the two SNES extensions carry a floor, deliberately: a 2600
       cartridge really is 2048 or 4096 bytes, and .mgw/.min are small too, so
       a floor there would refuse real content to guard a crash they do not
       have. Every other core REFUSES a short file cleanly. Add a row only for
       a core MEASURED to do worse than refuse. */
    if (ends_with_ext(rom_path, ".sfc") || ends_with_ext(rom_path, ".smc"))
        return 8192;
    return 0;
}

int config_layout_for_rom(const char *rom_path)
{
    /* No name is the DMG faceplate: the answer for the placeholder profile
       main.c resolves before a ROM is chosen, and the layout every UI screen
       (MAIN MENU, RECENT, ALL GAMES) is drawn over.

       THE TWO CONSOLES THAT JOINED .mgw DID SO BECAUSE THEIR PADS DO NOT FIT
       THE DMG FACEPLATE, not because of drawn-on artwork. That faceplate
       carries A, B, START, SELECT, MENU and at most two more discs
       (KOBOY_MAX_EXTRA_BTNS -- room, not a number anyone chose; see
       config_extra_buttons_for_rom). A SNES pad is A B X Y L R, so L and R
       fell off; a six-button Mega Drive pad is A B C X Y Z, so X and Z did.
       Neither was fixable there -- there is no seventh and eighth pocket.

       The LCD strip has a d-pad, a four-button DIAMOND, L1, R1, SELECT and
       START: EXACTLY a SNES pad, and a superset of a six-button Mega Drive's.
       Cost of moving is the moulded look and nothing else -- the scale ceiling
       still applies here (config_resolve_profile_par), so SNES presents at the
       same 897x672 as on the DMG side.

       ARCADE IS NOT HERE, evaluated rather than skipped (docs/FOLLOWUPS.md
       #74): FBNeo's square 512x512 max would put ~135 px of permanent white
       band down each side of a vertical board, and its DMG set (stick + four
       fire + coin + start) already covers the pre-1990 scope.

       THE REMAINING ELEVEN all fit the DMG faceplate with its two spare discs;
       moving them would be churn. The Intellivision came closest: FreeIntv has
       a `freeintv_multiscreen_overlay` mode that widens the frame to 1074x600
       and paints a 12-key keypad driven by RETRO_DEVICE_POINTER, which the LCD
       layout could forward. Refused on two measurements -- the composite costs
       1.335 ms/frame against 0.106 without it (12x, on the host, before the
       Cortex-A9 multiplier), and the same 12 keys are already reachable on the
       DMG faceplate through the core's own mini keypad (see the .int case in
       config_extra_buttons_for_rom). */
    if (!rom_path || !*rom_path) return KOBOY_LAYOUT_DMG;
    if (ends_with_ext(rom_path, ".mgw")) return KOBOY_LAYOUT_LCD;
    if (ends_with_ext(rom_path, ".sfc")) return KOBOY_LAYOUT_LCD;
    if (ends_with_ext(rom_path, ".smc")) return KOBOY_LAYOUT_LCD;
    if (ends_with_ext(rom_path, ".md"))  return KOBOY_LAYOUT_LCD;
    /* THE GBA IS HERE FOR A DIFFERENT REASON: its pad FITS the DMG faceplate
       (d-pad, A, B, START, SELECT and two shoulders, against A, B, START,
       SELECT, MENU and two spare pockets), so the rule above does not send it.

       WHAT SENDS IT HERE IS WHERE THE TWO SPARE POCKETS ARE: FACE pockets --
       (905, 790) and the stacked column (470, 700)/(470, 830). A GBA's L and R
       are a LEFT one and a RIGHT one at opposite ends of the machine, and the
       faceplate has no position saying "left shoulder" or "right". A disc in
       the middle of the case saying L is a control in the wrong PLACE -- a
       real cost here, already written down once for the Mega Drive's X and Z
       (config_lcd_pad_for_rom).

       The LCD strip has exactly one left/right pair (the outermost two slots
       of its lower band, drawn narrower than SELECT and START so they read as
       shoulders). Its face arrangement is per system, so a GBA gets TWO discs
       rather than the diamond's four -- KOBOY_LCD_FACE_PAIR2 in koboy.h.

       AND IT COSTS NOTHING IN PICTURE. Measured on 1264x1680, a 240x160 frame
       through config_resolve_profile_par at this system's ceiling of 4: DMG
       960x640 at (152, 84), LCD 960x640 at (152, 310). Same rect, same scale
       -- the ceiling binds before either layout's own limit, so the only
       difference is vertical centring. */
    if (ends_with_ext(rom_path, ".gba")) return KOBOY_LAYOUT_LCD;
    return KOBOY_LAYOUT_DMG;
}

/* LCD RECT FROM THE CORE'S MAX GEOMETRY, or from what it draws now?
   Meaningless outside KOBOY_LAYOUT_LCD -- the DMG branch has taken base since
   ae03e76.

   TRUE FOR GAME & WATCH ONLY, and the reason is a RATE, not a shape: a .mgw
   title alternates between the whole unit and the LCD alone SEVERAL TIMES A
   SECOND (Donkey Kong 654x396 <-> 305x191), so a base-sized rect would resize
   the artwork, redraw the faceplate and repaint the panel at that rate. That
   is why main.c's geometry check compares the resolved presentation, not the
   inputs.

   FALSE FOR SNES AND MEGA DRIVE. Their base does move (snes9x2005 into a
   512-wide hi-res mode, GPGX 256 <-> 320) but at SCENE boundaries, the same
   cost the DMG layout paid for them all along. Max would cost more:
   snes9x2005 declares a SQUARE 512x512 for an interlaced mode almost nothing
   enters, so the rect is a square recess around a 4:3 picture (~100 px of dead
   band) and -- worse -- the per-system scale ceiling has nothing to bite on,
   since 3 x 512 exceeds the panel. That ceiling is why SNES is playable;
   disarming it puts Star Fox back at 67%. */
bool config_lcd_rect_from_max_for_rom(const char *rom_path)
{
    if (!rom_path || !*rom_path) return false;
    return ends_with_ext(rom_path, ".mgw");
}

/* WHAT THE LCD STRIP'S CONTROLS SAY AND HOW THEY ARE ARRANGED. Contract, why
   empty means "the retropad's own name", and the arrangements: koboy_lcd_pad
   in koboy.h.

   Cleared on every call: this runs once per ROM load into a config that
   outlives one game (MENU -> CHOOSE ROM reuses it), so setting without
   clearing would leave a strip saying C on the next Game & Watch. */
void config_lcd_pad_for_rom(koboy_layout *l, const char *rom_path)
{
    if (!l) return;
    memset(&l->lcd, 0, sizeof l->lcd);
    if (!rom_path || !*rom_path) return;

    /* THE MEGA DRIVE. All six labels are READ OFF THE CORE, not chosen:
       Genesis Plus GX's port-0 descriptor block (libretro/libretro.c) says:

         JOYPAD_B -> B     JOYPAD_Y -> A     JOYPAD_L -> X
         JOYPAD_A -> C     JOYPAD_X -> Y     JOYPAD_R -> Z
         JOYPAD_START -> Start   JOYPAD_SELECT -> Mode

       The strip's diamond is TOP=JOYPAD_X, LEFT=JOYPAD_Y, RIGHT=JOYPAD_A,
       BOTTOM=JOYPAD_B (chrome_lcd_layout; input.c hit-tests the same struct),
       so the labels below are that table transposed. On the panel that reads A
       left, B bottom, C right -- the hardware's own A-B-C arc -- with Y
       directly above B, where a real six-button pad puts it.

       THE ARRANGEMENT IS THE CONSOLE'S OWN, not the strip's default diamond:
       KOBOY_LCD_FACE_ROWS6 puts X Y Z above A B C, as moulded. Same principle
       as the labels, applied to POSITION -- somebody who has held a Genesis pad
       knows where C is. It also gives X and Z real face discs rather than
       shoulder pills, which is right twice: the console has no shoulders, and
       the lower band then carries MODE and START alone. (On the DMG faceplate
       X and Z did not exist at all; that shortfall moved this system here.)

       SELECT SAYS MODE. A Mega Drive has no Select; JOYPAD_SELECT is the pad's
       Mode button, held at power-on so a six-button pad can pretend to be a
       three-button one. A pill saying SELECT that produces Mode is the same
       lie as a disc saying A that produces C.

       ALL SIX ARE LIVE WITHOUT A CORE OPTION, checked in the core: libretro.c
       :1039 sets config.input[].padtype to
       DEVICE_PAD2B|DEVICE_PAD3B|DEVICE_PAD6B, which input.c's SYSTEM_GAMEPAD
       case reads as "auto" and replaces with what the ROM HEADER declares --
       DEVICE_PAD6B iff the cartridge's I/O-support field contains '6'
       (core/loadrom.c's peripheralinfo table, bit 1). koboy answers no core
       options and never calls retro_set_controller_port_device, so that
       auto-detect stands. A three-button title reads nothing from X/Y/Z, as on
       real hardware. */
    if (ends_with_ext(rom_path, ".md")) {
        l->lcd.face = KOBOY_LCD_FACE_ROWS6;
        snprintf(l->lcd.x, sizeof l->lcd.x, "%s", "Y");
        snprintf(l->lcd.y, sizeof l->lcd.y, "%s", "A");
        snprintf(l->lcd.a, sizeof l->lcd.a, "%s", "C");
        snprintf(l->lcd.b, sizeof l->lcd.b, "%s", "B");
        snprintf(l->lcd.l1, sizeof l->lcd.l1, "%s", "X");
        snprintf(l->lcd.r1, sizeof l->lcd.r1, "%s", "Z");
        snprintf(l->lcd.select, sizeof l->lcd.select, "%s", "MODE");
        return;
    }

    /* THE SNES, where the labels are almost the retropad's own -- the
       retropad was modelled on this pad, so snes9x2005's descriptors are the
       identity map and the diamond is already in the hardware's arrangement:
       X on top, Y left, A right, B bottom.

       Only the shoulders are relabelled, and not cosmetically: the console
       calls them L and R, and "L1"/"R1" is a DualShock word that arrived
       through the retropad, whose "1" implies a second pair to look for.

       THE FOUR FACE LABELS ARE SET EXPLICITLY though identical to the
       fallback, so the whole map reads in one place and a test can assert a
       .sfc really was recognised here -- an empty field is indistinguishable
       from "no case matched". */
    if (ends_with_ext(rom_path, ".sfc") || ends_with_ext(rom_path, ".smc")) {
        /* The DIAMOND is left as the zero: a SNES pad IS this diamond -- X
           top, Y left, A right, B bottom -- so there is nothing to override. */
        snprintf(l->lcd.x, sizeof l->lcd.x, "%s", "X");
        snprintf(l->lcd.y, sizeof l->lcd.y, "%s", "Y");
        snprintf(l->lcd.a, sizeof l->lcd.a, "%s", "A");
        snprintf(l->lcd.b, sizeof l->lcd.b, "%s", "B");
        snprintf(l->lcd.l1, sizeof l->lcd.l1, "%s", "L");
        snprintf(l->lcd.r1, sizeof l->lcd.r1, "%s", "R");
        snprintf(l->lcd.select, sizeof l->lcd.select, "%s", "SELECT");
        return;
    }

    /* THE GAME BOY ADVANCE, whose whole control set is A, B, L, R, START and
       SELECT -- and whose retropad map is the identity, read off gpSP's own
       descriptor block (libretro/libretro.c, set_input_descriptors):

         JOYPAD_B -> B     JOYPAD_A -> A     JOYPAD_L -> L     JOYPAD_R -> R
         JOYPAD_START -> Start        JOYPAD_SELECT -> Select

       Nothing to transpose and nothing to shorten. Set explicitly anyway, for
       the reason the SNES's are: a test can assert a .gba was recognised here.

       WHAT THE OTHER RETROPAD BITS DO, so nobody hunts for discs: gpSP binds
       JOYPAD_X/JOYPAD_Y to "Turbo A"/"Turbo B" and JOYPAD_R2 to fast-forward.
       All three are front-end conveniences no GBA has, and none is reachable
       here -- deliberately. That is the argument for KOBOY_LCD_FACE_PAIR2
       (koboy.h): a disc labelled X that fires A twenty times a second is a
       control the hardware does not have. */
    if (ends_with_ext(rom_path, ".gba")) {
        l->lcd.face = KOBOY_LCD_FACE_PAIR2;
        snprintf(l->lcd.a, sizeof l->lcd.a, "%s", "A");
        snprintf(l->lcd.b, sizeof l->lcd.b, "%s", "B");
        snprintf(l->lcd.l1, sizeof l->lcd.l1, "%s", "L");
        snprintf(l->lcd.r1, sizeof l->lcd.r1, "%s", "R");
        snprintf(l->lcd.select, sizeof l->lcd.select, "%s", "SELECT");
        /* x and y left cleared, not omitted: PAIR2 draws no disc for either
           bit, so a label could never appear. */
        return;
    }

    /* GAME & WATCH KEEPS THE RETROPAD NAMES -- a deliberate empty case.
       gw-libretro's own overlay (START with no cursor active) draws a SNES pad
       in retropad names and quotes per-title bindings in them (Mickey Mouse:
       up/down/x/b for diagonals, l1/r1 for GAME A / GAME B). Relabelling would
       break the correspondence the labels exist to keep. chrome.c fills in
       X/Y/A/B/L1/R1/SELECT. */
}

void config_extra_buttons_for_rom(koboy_layout *l, const char *rom_path)
{
    if (!l) return;
    /* Cleared on every call: this runs once per ROM load into a config that
       outlives one game (MENU -> CHOOSE ROM reuses it), so setting without
       clearing leaves a C button drawn and live on the next Game Boy. */
    memset(l->extra, 0, sizeof l->extra);
    if (!rom_path || !*rom_path) return;

    /* The two positions on the DMG faceplate that fit another disc; both are
       tight enough to write down once.

       SLOT R -- (905, 790) r 70, the pocket below A and right of B. A and B
       are the Game Boy's and must not move, the Start/Select/MENU row owns
       everything from 892 permille down, and the right needs
       KOBOY_CHROME_MARGIN clear. That leaves this pocket, at a smaller radius
       than A/B's 85. Checked on all four panels -- A-gap, MENU-gap, right
       margin:
         Clara  1072x1448  A-gap 25px  MENU-gap 75px  right margin 27px
         Libra2 1264x1680  A-gap 28    MENU-gap 84    right margin 33
         Elipsa 1404x1872  A-gap 30    MENU-gap 95    right margin 36
         Sage   1440x1920  A-gap 32    MENU-gap 98    right margin 37

       SLOTS L/R-STACKED -- (470, 700) and (470, 830), both r 62, the column
       between the d-pad's right edge and B's left. Two discs need two places
       and the pocket above holds one, so a pair goes here rather than
       splitting across the faceplate. Clearances at the narrowest panel
       (1072x1448): 40px to the d-pad, 47px to B, 56px between the discs, 26px
       to the START row.

       LIVE CONSTRAINT: neither slot becomes chrome_controls_top's binding
       minimum on any of the four panels (it stays 879 / 1018 / 1135 / 1164,
       set by the A disc), which is what lets these systems keep the game rect
       and scale they would have had with no extra buttons.
       tests/test_chrome.c re-derives every number rather than trusting this.

       Labels go INSIDE these discs, not below like A and B: there is no case
       band under any of the three. chrome.c reuses the LCD strip's
       draw_face_button for that. */

    /* A Pokemon Mini has A, B and C; the core binds C to
       RETRO_DEVICE_ID_JOYPAD_R (bit 11, KOBOY_BTN_R1) and advertises it as "C"
       in its input descriptors. Read off the core, not chosen. */
    if (ends_with_ext(rom_path, ".min")) {
        l->extra[0] = (koboy_extra_btn){ 905, 790, 70, KOBOY_BTN_R1, "C" };
        return;
    }

    /* A WonderSwan needs TWO, because of the ROTATION rather than six face
       buttons. The console has two 4-way cursors (X1-X4, Y1-Y4) plus A, B and
       START, and many titles are played with the unit on its side --
       beetle-wswan's `wswan_rotate_display = manual` toggles that on SELECT
       and `wswan_rotate_keymap = auto` swaps the retropad map.

       In the ROTATED map (third_party/wswan/libretro.c, map[1]) the d-pad
       drives the Y cursor (the one under the thumb in that grip) but the
       WonderSwan's own A and B move to JOYPAD_L and JOYPAD_R. MEASURED: `Kaze
       no Klonoa - Moonlight Museum` in portrait responds to exactly START and
       JOYPAD_L, and JOYPAD_L is the hardware's A. Without these discs that
       title cannot be started at all -- the same unreachable-button bug the
       Game & Watch layout and the Pokemon Mini each spent a round on.

       L1 above R1 because that is where they sit once the console is turned.
       Still NOT reachable: the rotated map's X-cursor up/right on JOYPAD_Y and
       JOYPAD_X -- no room, and no measured title needs them. */
    if (ends_with_ext(rom_path, ".ws") || ends_with_ext(rom_path, ".wsc")) {
        l->extra[0] = (koboy_extra_btn){ 470, 700, 62, KOBOY_BTN_L1, "L1" };
        l->extra[1] = (koboy_extra_btn){ 470, 830, 62, KOBOY_BTN_R1, "R1" };
        return;
    }

    /* An Intellivision hand controller is the hardest control set this
       project has met, and these two discs make ALL of it reachable.

       The hardware: a 16-direction disc, three action buttons (top,
       lower-left, lower-right -- the two upper side buttons are one signal),
       and a TWELVE-KEY TELEPHONE KEYPAD. FreeIntv gives the four cardinals to
       the d-pad, the lower two actions to JOYPAD_A/JOYPAD_B and the top one to
       JOYPAD_Y (src/controller.c getControllerState -- read off the CODE; the
       input descriptors contradict the core's own on-screen help about which
       of A/B is which side). So the top button needs extra[1].

       THE KEYPAD, which no faceplate can draw and several titles cannot start
       without (BurgerTime and Bump 'n' Jump stop at "Select 1 or 2 Players",
       Diner says "then press enter"). FreeIntv puts 0 and 5 on the thumbsticks,
       Clear/Enter on the triggers, and 1-9 ONLY on the right analog stick,
       which koboy has no source for (core.c answers RETRO_DEVICE_JOYPAD
       alone) -- nine dead keys.

       The way in is what the descriptors call "Show Keypad": HOLD JOYPAD_L and
       a 4x3 keypad is drawn into the frame corner, the D-PAD moves a cursor
       and any face button presses the selected key (controller.c,
       getKeypadState + drawMiniKeypad; libretro.c:1267 makes it modal, so the
       disc is not steering while held). MEASURED: holding L1 and tapping A on
       BurgerTime's player-count prompt put a "1" on screen. Three simultaneous
       touches; koboy tracks ten.

       extra[0] is JOYPAD_L, labelled KEY rather than L1 because it opens the
       keypad and nothing on the hardware is called L1.

       Still NOT reachable: the disc's twelve DIAGONAL positions (koboy's touch
       d-pad gives four cardinals + four diagonals; the eight between are on
       FreeIntv's left analog stick only), so finely-steered titles
       (Astrosmash, Auto Racing) are coarser than on hardware. JOYPAD_X ("last
       selected keypad button") and the L2/R2/L3/R3 shortcuts are unreachable
       and redundant with the mini keypad. */
    if (ends_with_ext(rom_path, ".int")) {
        l->extra[0] = (koboy_extra_btn){ 470, 700, 62, KOBOY_BTN_L1, "KEY" };
        l->extra[1] = (koboy_extra_btn){ 470, 830, 62, KOBOY_BTN_Y,  "TOP" };
        return;
    }

    /* A ColecoVision controller ALSO has a twelve-key keypad, and unlike
       FreeIntv this core offers no on-screen way in: Gearcoleco spreads the
       keys across the whole retropad (1 on JOYPAD_Y, 2 on X, 3-8 on shoulders
       and sticks, * and # on START/SELECT, 9 and 0 on an analog axis --
       platforms/libretro/libretro.cpp). Only two can have a disc.

       1 and 2, because that is what the CONSOLE'S OWN BIOS asks for: every
       cartridge boots to an option screen reading "1 = SKILL 1/ONE PLAYER" /
       "2 = SKILL 2/ONE PLAYER" -- rendered and read, not assumed. Without
       keypad 1 no ColecoVision title starts at all.

       Labelled K1/K2 rather than 1/2 so a finger hunting "the keypad" finds
       them; the faceplate has no other numbers.

       Also: koboy's START and SELECT ARE keypad * and # here (the core binds
       them there and the moulded labels lie about it), and keypad 3-9 and 0
       are unreachable -- needed only by in-play-menu titles (Fortune Builder,
       the Super Action ones), not by start screens. */
    if (ends_with_ext(rom_path, ".col")) {
        l->extra[0] = (koboy_extra_btn){ 470, 700, 62, KOBOY_BTN_Y, "K1" };
        l->extra[1] = (koboy_extra_btn){ 470, 830, 62, KOBOY_BTN_X, "K2" };
        return;
    }

    /* THE MEGA DRIVE AND THE SNES HAVE NO DISCS HERE -- a deliberate empty
       case for two systems that used to have two each. config_layout_for_rom
       sends .md/.sfc/.smc to KOBOY_LAYOUT_LCD instead, whose strip carries a
       d-pad, a four-button diamond, L1, R1, SELECT and START.

       They moved because THE DISCS WERE NOT ENOUGH. Two spare pockets (see the
       SLOT notes above -- room, not policy), and: */

    /*   SNES needs A B X Y L R. The two discs went to Y and X (Y is the
         run-and-fire button: Mario runs on Y, Samus shoots on Y, Link's item
         is Y), leaving L and R unreachable -- no hop in Mario Kart, no fierce
         in the fighters, no aim-diagonal in Super Metroid.

         MEGA DRIVE needs A B C, or A B C X Y Z on a six-button pad. GPGX maps
         JOYPAD_B -> B, JOYPAD_A -> C, JOYPAD_Y -> A, so the faceplate's own A
         disc is the console's C and the hardware's A had to take extra[0] or
         not exist. One pocket left for three six-button keys; Y took it, X and
         Z were unreachable.

       The core mappings did not move -- they are read off the same descriptor
       tables and now live in config_lcd_pad_for_rom. Kept as an empty case
       because "needs no disc" and "somebody forgot this system" look identical
       at a glance. */

    /* An Atari 2600 joystick is four directions and ONE button, plus Reset
       and Select on the console. stella2014 binds fire to JOYPAD_B, Reset to
       JOYPAD_START, Select to JOYPAD_SELECT, so the DMG faceplate covers
       everything and NO extra disc is needed -- a deliberate empty case.

       THE A DISC IS DEAD for this system: fire is B, and JOYPAD_A only does
       anything for a Genesis pad or paddles, neither of which koboy can
       present (libretro.cxx). The two difficulty switches and the
       colour/B&W switch are unreachable set-once console switches; none of the
       author's 82 titles needs one to start. Paddle titles (Breakout,
       Warlords) play on the d-pad -- a paddle is an analog axis, and koboy has
       none.

       Master System and Game Gear are likewise fully covered: two buttons on
       JOYPAD_B/JOYPAD_A and PAUSE/START on JOYPAD_START (gpgx's DEVICE_PAD2B
       branch). */

    /* ARCADE -- the first system whose extra discs are chosen from a
       POPULATION rather than one console's control panel: FBNeo is 227
       different boards in the author's set, with no single answer to "what
       does the hardware have".

       So it was COUNTED. All 227 romsets were loaded and their
       retro_input_descriptors read (port 0, RETRO_DEVICE_JOYPAD). FBNeo's map
       is flat: JOYPAD_B = "Button 1", JOYPAD_A = 2, JOYPAD_Y = 3, JOYPAD_X = 4.
       Counts across the 227:

         B  208    Y  134    L1 45    L2 45    L3 26
         A  185    X   71    R1 48    R2 46    R3 14

       B and A the faceplate already has. Y is bound by 134 boards -- more than
       half -- and X by 71, so those are the discs, in the L/R-stacked slots.
       That covers every board with four or fewer fire buttons.

       Labelled 3 and 4, NOT C and D: on this system the faceplate's moulded B
       and A ARE buttons 1 and 2, so the numbering continues a sequence the
       player can follow. (Same problem the ColecoVision answered with K1/K2.)

       STILL UNREACHABLE, counted: L1/R1 (45/48 boards), L2/R2 (45/46), L3/R3
       (26/14) -- the SIX-BUTTON layouts (Street Fighter's strong punch and
       kick, a Neo Geo D, Defender's "Reverse"), all outside the pre-1990 scope
       this core was added for. The only measured casualty within scope is
       Defender, whose Reverse is on R1 (Hyperspace on Y and Thrust on X are
       reachable).

       COIN AND START NEED NO DISC, which is the first thing a reader will
       check: FBNeo binds "Coin 1" to JOYPAD_SELECT and "Start 1" to
       JOYPAD_START (retro_input.cpp, GameInpStandardOne) -- the faceplate's
       own pills. A board will not start without a coin, so a missing SELECT
       would have been the unreachable-button bug a fourth time. Verified by
       playing: Galaga reaches STAGE 1 from SELECT then START. */
    if (ends_with_ext(rom_path, ".zip")) {
        l->extra[0] = (koboy_extra_btn){ 470, 700, 62, KOBOY_BTN_Y, "3" };
        l->extra[1] = (koboy_extra_btn){ 470, 830, 62, KOBOY_BTN_X, "4" };
        return;
    }

    /* A Neo Geo Pocket has a stick, A, B and OPTION, so it needs NO extra
       disc -- a deliberate empty case, because "checked, needs none" and
       "never looked" are otherwise indistinguishable.

       RACE binds OPTION to JOYPAD_START and the face buttons CROSSED: NGP A ->
       JOYPAD_B, NGP B -> JOYPAD_A (race/libretro/libretro.c's descriptors).
       That reads like a bug and is not: on the hardware A is the LEFT button,
       and on koboy's faceplate the B disc is left of the A disc, as on a Game
       Boy. The crossing puts each label where the finger expects it; undoing
       it would be the bug. */
}

/* ------------------------------------------------- install-relative paths
 *
 * Cost a device round-trip to find. dlopen() given a slashless name treats it
 * as a library *name* and searches DT_RUNPATH, LD_LIBRARY_PATH,
 * /etc/ld.so.cache and the system directories -- NEVER the cwd. So a bare
 * "gambatte_libretro.so" failed with "cannot open shared object file" while
 * sitting next to the binary. No host test could catch it: on the desktop the
 * core always arrives with a slash (`--core build/stub_core.so`).
 *
 * The fix resolves against the directory containing the EXECUTABLE. "./" would
 * work only when the cwd happens to be the install directory, and NickelMenu
 * and KFMon set none; /proc/self/exe is independent of how the process
 * started.
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

/* Called once, after both the ini and the command line, so a bare name from
   either gets the same treatment. NOT inside config_load: the loader reports
   what the file says, and folding resolution in would make the parse tests
   assert on this machine's directory layout.

   Every path, not just core_path. rom_path and save_dir reach fopen(), which
   does resolve against the cwd, so they are not broken the way core_path was
   -- but they fail the same way under a menu launch that sets no cwd, and
   save_dir's shipped "." would try to write saves to the read-only rootfs.
   One rule: no slash means "next to koboy". */
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
    /* Resolved for the reason save_dir is: under a NickelMenu launch a bare
       name resolves against the read-only rootfs, and a screenshot that
       silently went nowhere looks like a feature that does not work. */
    if (config_join_sibling(tmp, sizeof tmp, c->shot_dir, dir))
        snprintf(c->shot_dir, sizeof c->shot_dir, "%s", tmp);
}

static void trim(char *s)
{
    char *p = s; while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r' || s[n-1] == '\n')) s[--n] = 0;
}

/* An EMPTY value leaves the default alone rather than meaning true: this once
   treated everything but "false"/"0" as true, so a blanked `grab_input = `
   silently turned the grab ON -- the opposite of what clearing a line means.
   `trim` has run, so "" covers whitespace-only values. */
static bool as_bool(const char *v, bool dflt) {
    if (!v || !v[0]) return dflt;
    return !(strcmp(v,"false")==0 || strcmp(v,"0")==0);
}

/* Extracted from the main loop so it can be tested. >= and not >: a frame in
   which the ENTIRE game rect changed is exactly when a flash is wanted.
   LIVE GUARD: permille <= 0 disables outright -- without it, 0 makes the
   comparison always true, the exact inverse of "disabled". */
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
           indistinguishable from one never edited, so it marks no intent. */
        if      (!strcmp(k, "scale")) {
            c->scale = atoi(v);
            c->scale_explicit = (c->scale != KOBOY_SCALE_LEGACY_DEFAULT);
        }
        /* REJECTED, not clamped -- config_present_divisor_ok in config.h says
           why neither clamp direction is defensible. atoi gives 0 for junk too,
           so `present_divisor = fast` keeps the default rather than dividing by
           zero in pacer_tick. */
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
           value rather than falling to entry 0: entry 0 is the Rec.601 mapping
           this key exists to move away from, so a typo must not silently
           reinstate the rendering the user was changing. */
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
        /* LIVE CLAMP, both directions, to [0, KOBOY_SETTLE_MS_MAX]. Negative
           makes pacer_settle_us' uint32_t arguments enormous -- a freeze, the
           one failure mode a pacing key must not have. The ceiling: a
           hand-edited 100000 looks exactly like a hang, and no measured panel
           is within an order of magnitude of a second per update. */
        else if (!strcmp(k, "settle_base_ms")) c->settle_base_ms = clamp_settle_ms(atoi(v));
        else if (!strcmp(k, "settle_full_ms")) c->settle_full_ms = clamp_settle_ms(atoi(v));
        else if (!strcmp(k, "refresh_fixed_tiles")) {
            /* LIVE CLAMP to >= 0. video_split_dirty adds fixed_tiles once per
               candidate rect, so a negative value makes the split branch
               cheaper the MORE rects it emits -- unbounded as the count
               approaches KOBOY_MAX_RECTS. Clamped rather than treated as "off"
               like cleanup_interval, because 0 is a real value here (no fixed
               cost), not a sentinel. */
            int t = atoi(v);
            c->refresh_fixed_tiles = t < 0 ? 0 : t;
        }
        else if (!strcmp(k, "waveform_fast")) {
            /* Unparseable keeps AUTO rather than the previous value: this
               selects a PANEL behaviour, and "let the driver decide" is the
               one safe answer on an unmeasured device. */
            koboy_wfm_policy wp = KOBOY_WFM_AUTO;
            config_wfm_policy_parse(v, &wp);
            c->wfm_fast_policy = wp;
        }
        /* An ini `core=` outranks the ROM's extension (core_explicit in
           config.h) WITH ONE DATED EXCEPTION: every koboy.ini written before
           choice-by-extension carries a literal `core = gambatte_libretro.so`,
           which v1 shipped uncommented. That records PACKAGING, not preference,
           so honouring it as a pin would silently disable choice-by-extension
           on every existing install and present as ".mgw files are listed but
           refuse to load". A redeploy would fix it, but
           docs/device-workflow.md tells the user to carry values forward from
           their backup -- which is how the dead line comes back.
           `--core` is exempt from the exemption: typed deliberately, now. */
        else if (!strcmp(k, "core")) {
            snprintf(c->core_path, sizeof c->core_path, "%s", v);
            c->core_explicit = strcmp(v, KOBOY_CORE_LEGACY_DEFAULT) != 0;
        }
        else if (!strcmp(k, "shot_dir"))         snprintf(c->shot_dir,  sizeof c->shot_dir,  "%s", v);
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
    /* LIVE GUARD: a non-positive geometry would divide by zero or by a
       negative below, not merely fail to fit like max_fit < 1 does. */
    if (max_w < 1 || max_h < 1) return false;
    /* LIVE GUARD, new with the base-sized rect below: base_w/base_h used to be
       carried through untouched because nothing divided by them. The DMG
       branch now does. */
    if (base_w < 1 || base_h < 1) return false;

    /* Both layouts reserve the game rect clear of their faceplate's controls
       and ask the same function where those start (chrome.h). Hoisted above
       the split as the one thing the branches share. */
    int ctrl_top = chrome_controls_top(c->layout_mode, &c->layout, panel_w, panel_h);

    /* The reserved rect's WIDTH IN SOURCE PIXELS. par == KOBOY_ASPECT_ONE
       makes the multiply exact and the round-up a no-op, so square pixels
       compute bit for bit what this function did before.

       ROUNDED UP, not to nearest: the rect must HOLD the widened picture, and
       rounding down by one source pixel costs a whole integer step of scale.
       MEASURED: at nearest-rounding the NES rect came out 292 and the frame
       needed 877.7 of the 876 that gave, dropping the fit 3x -> 2x. */
    if (par == 0) par = KOBOY_ASPECT_ONE;

    /* WHICH GEOMETRY THE RECT IS SIZED FROM -- the layouts differ on purpose.

       LCD ASKS THE SYSTEM (`lcd_rect_from_max`, set from the extension; that
       function explains the split). Game & Watch keeps MAX: its fit is
       fractional so a smaller frame costs only margin, and a .mgw title
       changes base several times a second (654x396 <-> 305x191 on Donkey
       Kong). SNES and Mega Drive take BASE, as on the DMG side.

       DMG TAKES BASE -- what the core draws NOW. Max was chosen when both
       cores had base == max and is wrong for a core whose max is a mode it
       never enters: snes9x2005 declares 512x512 for an interlaced hi-res mode
       almost nothing uses and then draws 256x224 forever, and a 512-tall
       reservation cannot exceed scale 1 under chrome_controls_top. MEASURED on
       the verified 1264x1680 panel: 597x448 presented against the Game Boy's
       800x720 -- 46% of the area on a system with 1.8x its pixels. From base
       it is 1196x896.

       Max's safety property was that any frame in [1, max] fitted the rect.
       That defence MOVED rather than went: video_fit_rect falls back to the
       fractional fit for any frame the integer one cannot shrink (its 1x floor
       could not), so an oversized frame is presented SMALLER inside the rect
       instead of overflowing. That is the whole safety argument and it is
       asserted by sweep in tests/test_video_pipeline.c -- check it before
       believing this comment.

       The BUFFER is still allocated from max (video_create): memory safety,
       unchanged. */
    int rect_w = max_w, rect_h = max_h;
    if (c->layout_mode != KOBOY_LAYOUT_LCD || !c->lcd_rect_from_max) {
        rect_w = base_w; rect_h = base_h;
    }
    if (par != KOBOY_ASPECT_ONE) {
        rect_w = (int)((((uint64_t)rect_w * par) + 65535u) >> 16);
        if (rect_w < 1) rect_w = 1;
    }

    if (c->layout_mode == KOBOY_LAYOUT_LCD) {
        /* No scale search here: the rect is the largest aspect-preserving fit
           into the full panel width and everything above the bottom strip.
           Fractional, so a 654x396 Mickey Mouse unit fills 1264 columns rather
           than the 654 an integer 1x would leave -- the "too small" the device
           reported.

           No KOBOY_CHROME_MARGIN on the sides, unlike the DMG branch: dropping
           the drawn controls is meant to give the artwork the panel, and a
           Game & Watch unit's artwork has its own moulded border. The vertical
           margin is whatever centring leaves. */
        if (panel_w < 1 || ctrl_top < 1) return false;
        int gw = 0, gh = 0;
        video_fit_frac(rect_w, rect_h, panel_w, ctrl_top, &gw, &gh);
        if (gw < 1 || gh < 1) return false;

        /* THE PER-SYSTEM SCALE CEILING, live here for the reason it is live
           in the DMG branch: it is a measured cap on PICTURE AREA, and
           video_submit does not care which faceplate is drawn around the
           result. Without it, moving SNES here would hand it a 1264x1106
           fractional fit -- 2.3x the capped area -- and put Star Fox back at
           the 67% that bought the ceiling (see `ceiling` on g_core_by_ext and
           TESTED.md's rect-sizing table).

           The cap is ceiling x rect, the same picture the DMG scale search
           gives: a .sfc is 897x672 in both layouts, to the pixel.

           ONE TEST DECIDES BOTH DIMENSIONS: video_fit_frac preserves the
           rect's aspect and the cap is proportional to it, so taking both from
           the cap keeps the ratio exact.

           AN EXPLICIT `scale =` REPLACES the ceiling rather than switching it
           off -- the honest translation of a scale into a fractional fit is a
           cap of N times the source, not "no cap", which would make
           `scale = 2` produce a BIGGER picture than the ceiling did.
           `scale = 0` is the ini's word for auto and falls through, which is
           why the > 0 test is not folded into scale_explicit.

           The shipped config/koboy.ini says `scale = 5` and that does NOT
           count as explicit (KOBOY_SCALE_LEGACY_DEFAULT), so the ceiling is
           what a device runs -- and it must be, because this fit is fractional
           and full-width with no margin loop under it. Measured: a .sfc is
           897x672 with the cap and 1264x946 (1.98x the area) without. */
        int cap = 0;
        if (c->scale_explicit && c->scale > 0) cap = c->scale;
        else if (c->scale_ceiling > 0)         cap = c->scale_ceiling;
        if (cap > 0) {
            int cap_w = rect_w * cap;
            int cap_h = rect_h * cap;
            if (gw > cap_w) { gw = cap_w; gh = cap_h; }
        }

        p->panel_w = panel_w;
        p->panel_h = panel_h;
        p->base_w  = base_w;
        p->base_h  = base_h;
        p->max_w   = max_w;
        p->max_h   = max_h;
        p->layout_mode = KOBOY_LAYOUT_LCD;
        /* Carried into the profile because video.c decides from it and only
           has the profile. */
        p->rect_from_max = c->lcd_rect_from_max;
        /* And the strip's face arrangement, for the same reason --
           chrome_lcd_layout takes only a profile. */
        p->lcd_face = c->layout.lcd.face;
        p->game_w  = gw;
        p->game_h  = gh;
        p->game_x  = (panel_w - gw) / 2;
        p->game_y  = (ctrl_top - gh) / 2;
        /* INFORMATIONAL ONLY here -- the field name promises more than it can
           deliver. The real fit is fractional and lives in game_w/game_h, which
           video.c and chrome.c read; nothing but the startup log line and the
           tests consumes p->scale (checked: no other reader in src/). The
           integer part is reported rather than 0 or 1 so the log says
           something true. */
        p->scale = gw / rect_w;
        if (p->scale < 1) p->scale = 1;
        return true;
    }

    int fit_w = panel_w / rect_w;
    int fit_h = panel_h / rect_h;
    int max_fit = fit_w < fit_h ? fit_w : fit_h;
    if (max_fit < 1) return false;
    /* The configured scale is the GAME BOY's unless the user said otherwise;
       applying it to every system was wrong in a way only a second system
       could reveal. 5 was MEASURED for 160x144 -- it is what makes 800x720 sit
       inside the DMG faceplate, and the spec explicitly rejected a full-width
       Game Boy in favour of it. Auto-fitting the Game Boy today lands on 6
       (mutating this branch off turns the chrome goldens and test_config's
       sweep red at 6), so 5 is a deliberate choice against the panel maximum.
       A Pokemon Mini at scale 5 is 480x320 -- a postage stamp on 1264x1680.

       KEYED ON MAX, not on the base the rect is now sized from, and that is
       load-bearing: a Game Gear's BASE is 160x144, byte for byte the Game
       Boy's, while its max is 284x240, so keying on base would hand the Game
       Gear the Game Boy's measured 5 and shrink it. Genesis Plus GX is what
       makes those two questions different.

       So: an explicit scale wins for every system; absent one the Game Boy
       keeps its measured 5 and everything else fits itself to the panel. */
    bool is_game_boy = (max_w == KOBOY_GB_W && max_h == KOBOY_GB_H);
    int want = (c->scale_explicit || is_game_boy) ? c->scale : 0;
    int s = want > 0 ? want : max_fit;
    if (s > max_fit) s = max_fit;        /* configured scale does not fit */
    /* The per-system ceiling applies to an AUTO-fitted scale only: an explicit
       `scale =` is the owner overriding a default. See `ceiling` above. */
    if (!c->scale_explicit && c->scale_ceiling > 0 && s > c->scale_ceiling)
        s = c->scale_ceiling;

    /* TWO reservations, not one: KOBOY_CHROME_MARGIN keeps the bezel inside
       the buffer, ctrl_top keeps the game rect off the CONTROLS. Reserving
       only the bezel was the bug -- at scale = 0 the fitted rect cleared the
       panel edges and still covered the drawn A button and d-pad on all four
       panels (Libra 2 chose scale 7 and put 15,677 chrome pixels inside the
       game rect). Touch zones stay live under a rect drawn over them, so
       tapping the lower playfield pressed A or a direction. Shrinking the
       controls is explicitly NOT the fix: they are the only way to play on a
       device with no buttons. */

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
    /* The floor stays 1 rather than becoming a failure: on a tiny panel, or a
       core whose geometry is itself large, running with a slightly overlapped
       control band beats refusing to start. chrome_render clamps its own
       writes and does not rely on this loop. */

    p->scale   = s;
    p->panel_w = panel_w;
    p->panel_h = panel_h;
    p->base_w  = base_w;
    p->base_h  = base_h;
    p->max_w   = max_w;
    p->max_h   = max_h;
    /* game_w/game_h come from rect_w/rect_h, which for this layout is the
       core's BASE geometry -- the rect no longer has to hold every frame the
       core COULD send, because video_fit_rect shrinks the ones it cannot. See
       the note where rect_w is computed. video_create's buffer still comes
       from max; that part is memory safety. */
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
   ordering -- comes through untouched. ONE implementation for every writer:
   two copies would be two places to get the temp file, the trailing newline or
   the atomic rename wrong. */
static bool rewrite_ini(const char *path, const char *const *drop, int ndrop,
                        const char *block)
{
    /* Temp file in the target's directory: same filesystem, so the rename is
       atomic and a full disk fails fast. */
    char temp_path[512];
    snprintf(temp_path, sizeof temp_path, "%s.tmp", path);

    FILE *out = fopen(temp_path, "w");
    if (!out) return false;

    FILE *in = fopen(path, "r");
    int last_char_written = 0;
    if (in) {
        char line[1024];
        while (fgets(line, sizeof line, in)) {
            /* An assignment is a '=' before any '#'; comments and blanks pass
               through. */
            char *eq = strchr(line, '=');
            char *hash = strchr(line, '#');

            if (eq && (!hash || eq < hash)) {
                char k[1024];
                int  skip = 0;
                snprintf(k, sizeof k, "%.*s", (int)(eq - line), line);
                trim(k);
                for (int i = 0; i < ndrop; i++)
                    if (!strcmp(k, drop[i])) { skip = 1; break; }
                if (skip) continue;
            }

            fputs(line, out);
            size_t llen = strlen(line);
            if (llen) last_char_written = line[llen - 1];
        }
        fclose(in);
    }

    /* Without this, a source ini with no trailing newline gets the appended
       block concatenated onto its final line -- harmless today only because
       config_load truncates at the resulting '#', but it silently rewrites an
       unrelated line. */
    if (last_char_written && last_char_written != '\n') fputc('\n', out);

    fputs(block, out);

    if (fclose(out) != 0) {
        remove(temp_path);               /* temp is out of sync; drop it */
        return false;
    }

    if (rename(temp_path, path) != 0) {
        remove(temp_path);
        return false;
    }

    return true;
}

bool config_save_keys(const char *path, uint16_t key_a, uint16_t key_b)
{
    /* Dropping the old key_a/key_b lines is what makes calibration idempotent:
       the file ends with exactly one of each, first run or re-run. */
    static const char *const drop[] = { "key_a", "key_b" };
    char block[128];
    snprintf(block, sizeof block,
             "# written by first-run calibration\nkey_a = %u\nkey_b = %u\n",
             (unsigned)key_a, (unsigned)key_b);
    return rewrite_ini(path, drop, 2, block);
}

bool config_save_gray_map(const char *path, koboy_gray_map map)
{
    /* video_gray_map_name never returns NULL and names the default for an
       out-of-range map, so the file always gets a name config_load parses
       back. */
    static const char *const drop[] = { "gray_map" };
    char block[128];
    snprintf(block, sizeof block,
             "# written by the in-game GREYSCALE menu entry\ngray_map = %s\n",
             video_gray_map_name(map));
    return rewrite_ini(path, drop, 1, block);
}

/* The values the in-game FRAMES entry cycles through, ascending. Contract and
   the argument for these six are on config_next_present_divisor in config.h.
   Kept beside the loader so the ini's range and the menu's offer cannot drift;
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
    /* Checked before the write: rewrite_ini would happily produce a file
       config_load then ignores, and a choice that silently does not survive
       the relaunch is worse than one that reports it could not save. */
    if (!config_present_divisor_ok(divisor)) return false;
    snprintf(block, sizeof block,
             "# written by the in-game FRAMES menu entry\npresent_divisor = %d\n",
             divisor);
    return rewrite_ini(path, drop, 1, block);
}

/* ------------------------------------------------------------------ MOTION */

/* Indexed by koboy_wfm_policy. These strings ARE the ini's vocabulary -- the
   parser and the writer both go through them, so the token the menu writes is
   by construction the one config_load reads back. */
static const char *const WFM_NAMES[KOBOY_WFM_COUNT] = { "auto", "du4", "du" };

const char *config_wfm_policy_name(koboy_wfm_policy p)
{
    if ((int)p < 0 || (int)p >= KOBOY_WFM_COUNT) return WFM_NAMES[KOBOY_WFM_AUTO];
    return WFM_NAMES[p];
}

bool config_wfm_policy_parse(const char *s, koboy_wfm_policy *out)
{
    if (!s || !out) return false;
    for (int i = 0; i < KOBOY_WFM_COUNT; i++)
        if (!strcmp(s, WFM_NAMES[i])) { *out = (koboy_wfm_policy)i; return true; }
    return false;
}

/* The rungs, in menu order. Contract and the argument for three rather than
   all four combinations: config_next_motion in config.h. Beside the loader for
   the reason PRESENT_DIVISOR_LADDER is. */
typedef struct { bool dither; koboy_wfm_policy wfm; } motion_rung;
static const motion_rung MOTION_LADDER[] = {
    { false, KOBOY_WFM_AUTO },
    { true,  KOBOY_WFM_AUTO },
    { true,  KOBOY_WFM_DU   },
};
#define MOTION_LADDER_N ((int)(sizeof MOTION_LADDER / sizeof MOTION_LADDER[0]))

void config_next_motion(bool *dither, koboy_wfm_policy *wfm)
{
    if (!dither || !wfm) return;
    for (int i = 0; i < MOTION_LADDER_N; i++) {
        if (MOTION_LADDER[i].dither == *dither && MOTION_LADDER[i].wfm == *wfm) {
            const motion_rung *n = &MOTION_LADDER[(i + 1) % MOTION_LADDER_N];
            *dither = n->dither; *wfm = n->wfm;
            return;
        }
    }
    /* Off the ladder (a hand-edited pair): land on rung 0 rather than guessing
       what the file meant. Rung 0 is the shipped default and the one
       combination a whole game has been played at. */
    *dither = MOTION_LADDER[0].dither;
    *wfm    = MOTION_LADDER[0].wfm;
}

bool config_save_motion(const char *path, bool dither, koboy_wfm_policy wfm)
{
    static const char *const drop[] = { "force_dither", "waveform_fast" };
    char block[192];
    /* Checked before the write, as config_save_present_divisor checks its
       range: an unwritable policy would land in the file as something
       config_load silently reads back as `auto`. */
    if ((int)wfm < 0 || (int)wfm >= KOBOY_WFM_COUNT) return false;
    snprintf(block, sizeof block,
             "# written by the in-game MOTION menu entry\n"
             "force_dither = %s\nwaveform_fast = %s\n",
             dither ? "true" : "false", config_wfm_policy_name(wfm));
    return rewrite_ini(path, drop, 2, block);
}
