#ifndef KOBOY_CONFIG_H
#define KOBOY_CONFIG_H
#include "koboy.h"

/* v1 shipped these two uncommented in config/koboy.ini when gambatte and the
   Game Boy were the only choices, so an ini naming exactly them records
   packaging, not preference: config.c does NOT mark them explicit. See the
   `core=` and `scale=` parse sites. */
#define KOBOY_CORE_LEGACY_DEFAULT "gambatte_libretro.so"
#define KOBOY_SCALE_LEGACY_DEFAULT 5

typedef struct {
    int      scale;              /* 0 = pick the largest that fits */
    bool     scale_explicit;     /* user named a scale, vs config_defaults'
                                    Game-Boy 5. Without it a 96x64 Pokemon Mini
                                    rendered at 480x320 on a 1264x1680 panel.
                                    See config_resolve_profile. */
    int      present_divisor;    /* core frames per presented frame */
    int      cleanup_interval;   /* presented frames between game-rect cleanups */
    int      cleanup_max_ms;     /* wall-clock ceiling between cleanups; <=0 off */
    int      wfm_fast_policy;    /* koboy_wfm_policy for KOBOY_REFRESH_FAST */
    int      full_refresh_permille; /* dirty area (permille of game rect) above
                                       which a frame refreshes FULL instead of
                                       FAST; <= 0 disables */
    int      refresh_fixed_tiles; /* fixed per-refresh cost in 8x8 tiles, for
                                     video_split_dirty's cost model. A key, not a
                                     constant: every absolute timing here has
                                     moved by up to 2.2x between sessions. */
    /* AREA-AWARE PRESENT PACING. base is what any update costs regardless of
       size; full is what the area term ADDS at a whole-game-area rect. Both 0
       disables the throttle (pure present_divisor). See pacer_settle_us. */
    int      settle_base_ms;
    int      settle_full_ms;
    int      scale_ceiling;      /* auto-fit cap for the loaded system, 0 = none.
                                    Set from config_scale_ceiling_for_rom. */
    bool     pixel_aspect;       /* honour a core's reported pixel aspect. ON by
                                    default -- eight systems render wrongly
                                    without it, the Atari 2600 by 1.75x. A key
                                    rather than a constant so non-integer
                                    scaling can be backed out without a rebuild
                                    if it reads badly on e-ink. Same reasoning
                                    as gray_map. */
    bool     force_dither;
    /* koboy_gray_map, held as int for the reason dpad_mode is: this struct is
       memset to zero and parsed from text, so an enum-typed field would make an
       out-of-range ini value undefined rather than merely wrong. video.c's
       gray_of guards the range. */
    int      gray_map;
    bool     grab_input;
    /* Dump every raw touch event into koboy.log. OFF by default and noisy when
       on -- it exists because a Kobo koboy has never run on is diagnosed by
       what its panel SENDS, and the owner of that Kobo is the only person who
       can capture it. An ini key rather than an environment variable for
       exactly that reason: koboy.ini is a file they already edit over USB,
       and koboy.sh is not. */
    bool     trace_touch;
    int      dpad_mode;
    int      dpad_deadzone;      /* px */
    int      dpad_hysteresis;    /* px */
    uint16_t key_a, key_b;       /* 0 = not yet calibrated */
    uint16_t key_start, key_select; /* 0 = not yet calibrated; gamepad shoulder
                                        buttons by default, see config_defaults */
    char     rom_path[512];
    char     rom_dir[512];       /* where the browser looks; install-relative */
    char     core_path[512];
    bool     core_explicit;      /* user named a core (ini `core=` or --core), vs
                                    config_defaults' gambatte fallback, which is
                                    written into core_path unconditionally -- so
                                    the string alone cannot say. Only an
                                    unasked-for core may be overridden by the
                                    ROM's extension. */
    char     save_dir[512];
    /* MENU -> SCREENSHOT's PNGs. Install-relative like rom_dir; a directory of
       its own rather than save_dir because these are the files the owner takes
       OFF the device, and mixing them with .srm/.st1 invites deleting the wrong
       one. Created on demand. */
    char     shot_dir[512];
    /* koboy_layout_mode. NOT an ini key: a fact about the loaded system,
       derived from the extension by config_layout_for_rom and written by main.c
       before config_resolve_profile reads it. config_defaults leaves it at
       KOBOY_LAYOUT_DMG (0). */
    int      layout_mode;
    /* KOBOY_LAYOUT_LCD only: size the game rect from the core's MAX geometry
       rather than the frame it is drawing now. Also derived from the extension
       (config_lcd_rect_from_max_for_rom), not a preference. Defaults false,
       which is the DMG branch's own behaviour. */
    bool     lcd_rect_from_max;
    koboy_layout layout;
} koboy_config;

void config_defaults(koboy_config *c);
bool config_load(koboy_config *c, const char *path);

int config_scale_ceiling_for_rom(const char *rom_path);

/* base_w/h and max_w/h come from core_get_geometry, queried once after
   retro_load_game; passed as plain ints because config is layered BELOW core
   and has no core.h dependency.

   WHICH PAIR DRIVES THE SCALE SEARCH AND THE GAME RECT DEPENDS ON THE LAYOUT:
   base in KOBOY_LAYOUT_DMG, max in KOBOY_LAYOUT_LCD (koboy.h's koboy_profile
   comment says why). All four must be >= 1 or this returns false -- base
   included, now that it is divided by. Both pairs reach the profile unchanged. */
bool config_resolve_profile(koboy_profile *p, const koboy_config *c,
                            int panel_w, int panel_h,
                            int base_w, int base_h, int max_w, int max_h);

/* As above for NON-SQUARE PIXELS. `par` is video_pixel_aspect's 16.16 result;
   KOBOY_ASPECT_ONE makes this identical to config_resolve_profile, which is how
   that one is implemented.

   The reserved rect must know, not just the fit inside it (video_fit_par): the
   rect is max_w x max_h times an integer scale, and a wide pixel makes the
   picture wider than that, so the fit drops a whole integer step. MEASURED on
   1264x1680 before this parameter: Super Mario Bros. 768x720 -> 585x480,
   Defender 876x720 -> 640x480. Sizing the rect for the final shape gives
   878x720 and 960x720 -- correctly shaped AND bigger.

   BASE geometry, not max: a core's display aspect describes the frame it is
   rendering NOW. Against max it reads the WonderSwan's 1.5556 (announced for a
   224x144 landscape frame) against its square 224x224 max and widens an
   exactly-square rect by 56%. */
bool config_resolve_profile_par(koboy_profile *p, const koboy_config *c,
                                int panel_w, int panel_h,
                                int base_w, int base_h, int max_w, int max_h,
                                uint32_t par);

/* Do these two profiles present the game the same way -- same rect, place,
   scale, layout, buffer size? Compares only what config_resolve_profile_par
   decides, and deliberately NOT base_w/base_h: main.c's geometry poll already
   knows the base changed and is asking whether that made a difference worth a
   rebuild. The DMG rect IS sized from base so a base change can move it (SNES
   entering 512-wide hi-res); the LCD rect is not, so it never does (Game &
   Watch toggles base several times a second and must not pay a video rebuild
   plus a faceplate repaint for it). Indistinguishable without resolving both.

   max_w/max_h ARE compared even though they do not size the DMG rect:
   video_create allocates its intermediate buffer from them.

   NULL on either side is false. */
bool config_profile_presentation_same(const koboy_profile *a,
                                      const koboy_profile *b);

bool config_save_keys(const char *path, uint16_t key_a, uint16_t key_b);

/* Rewrites `path` with exactly one `gray_map = <name>` line, preserving every
   other line -- literally config_save_keys' rewrite_ini, so the two cannot
   drift. The greyscale mapping is a subjective judgement about a reflective
   panel, so it must be flippable while looking at the game and must survive the
   next launch: the ini key and the menu are ONE setting. */
bool config_save_gray_map(const char *path, koboy_gray_map map);

/* Is `v` a present_divisor koboy will run at? [1, KOBOY_PRESENT_DIVISOR_MAX].

   A predicate, not a clamp: clamping a typo'd `= 0` (atoi also reports 0 for
   junk) UP to 1 hands the user the fastest-smearing setting there is, and
   clamping `= 100000` down to 8 invents an intent nobody expressed. Rejecting
   keeps the default, the one value a full game has been played at.

   The lower bound is load-bearing: pacer_tick computes `frames % divisor`, so a
   zero reaching the pacer is a division by zero. pacer_set_divisor guards it
   too -- belt and braces; remove neither because the other exists. */
bool config_present_divisor_ok(int v);

/* Next value for the in-game FRAMES entry: first ladder entry above `cur`,
   wrapping to the lowest.

   The ladder is 1, 2, 3, 4, 6, 8 and not 1..8 because the user is judging
   PRESENTED frames per second, which goes as 1/divisor: 1->2 halves the rate,
   7->8 moves it an eighth. Short cycle matters too -- selecting this row
   returns to the game, so each step costs two taps.

   Off-ladder values are legal, run as written, and cycle UP to the next ladder
   entry rather than being snapped. At or above the top wraps to the bottom. */
int  config_next_present_divisor(int cur);

/* Rewrites `path` with exactly one `present_divisor = N` line, same rewrite_ini
   as the others. Refuses what config_present_divisor_ok rejects: a file holding
   a value config_load discards is exactly the menu/file disagreement one key
   exists to prevent. */
bool config_save_present_divisor(const char *path, int divisor);

/* ---------------------------------------------------------------- MOTION --
   The 1-bit/waveform PAIR: one menu row, two ini keys.

   ONE ROW AND NOT TWO because the thing being tested is the PAIR. `force_dither`
   and `waveform_fast` stay independent in the file, but four-level content
   under DU is the experiment that already failed on this panel (see
   koboy_wfm_policy), so two axes would offer four combinations, two of them
   known-uninteresting, and leave the coupling for the owner to infer. The
   ladder is what the MENU offers, not what the FILE permits -- the same split
   config_next_present_divisor makes. */

/* The ini token for a waveform policy ("auto", "du4", "du"), which is what
   config_load parses back. Never NULL: an out-of-range policy names the
   default, so what reaches the file is always readable back. */
const char *config_wfm_policy_name(koboy_wfm_policy p);

/* Parses one of those tokens. False (and *out untouched) for anything else. */
bool config_wfm_policy_parse(const char *s, koboy_wfm_policy *out);

/* Next rung of the MOTION ladder; both arguments in/out. Lowest to highest
   commitment:

     dither=false, wfm=auto   shipped default -- four greys, driver picks
     dither=true,  wfm=auto   1-bit content, driver still picks
     dither=true,  wfm=du     1-bit content into a two-level waveform

   THE MIDDLE RUNG IS THE CONTROL: without it an improvement could not be
   attributed to either half of the pair.

   An off-ladder pair (`force_dither = false` with `waveform_fast = du4`, legal
   in the file) runs as written and steps to the FIRST rung, the same way an
   off-ladder divisor steps up rather than being snapped. */
void config_next_motion(bool *dither, koboy_wfm_policy *wfm);

/* Rewrites `path` with exactly one `force_dither =` and one `waveform_fast =`
   line. ONE call and not two: the pair is one choice, and a failed second
   rewrite would leave the file describing a combination nobody picked. */
bool config_save_motion(const char *path, bool dither, koboy_wfm_policy wfm);

/* Promote a frame covering dirty_px of a whole_px game rect from the fast
   waveform to the flashing one? Lives here, and is tested, because the shipped
   default turns the promotion OFF and "off" is only trustworthy if the
   comparison is pinned: a >= that became a > would quietly reintroduce
   flashing with nothing failing. */
bool config_promote_full(const koboy_config *c, long dirty_px, long whole_px);

/* Which core loads this ROM? Returns a BARE core filename (no slash);
   config_resolve_paths' sibling-join makes it absolute, because dlopen() never
   looks in the cwd for a slashless name. */
const char *config_core_for_rom(const char *rom_path);

/* Which PRESENTATION -- a koboy_layout_mode. Same predicate (the extension),
   deliberately a separate function: the core can be overridden by `core=` or
   --core and the layout must NOT follow. A .mgw is a Game & Watch unit whose
   buttons are in its own artwork whichever .so interprets it. */
int config_layout_for_rom(const char *rom_path);

/* KOBOY_LAYOUT_LCD only: size the game rect from the core's MAX geometry (true)
   or the current frame (false). See config.c for why Game & Watch is the only
   true and what max would cost the two consoles sharing this layout. */
bool config_lcd_rect_from_max_for_rom(const char *rom_path);

/* Fills `l->lcd` -- what the LCD strip's controls SAY and how they are
   ARRANGED -- from the extension. Clears every field first, so a config reused
   across MENU -> CHOOSE ROM cannot keep the last system's pad; cleared is the
   retropad's own names in the four-button diamond, what chrome.c draws for a
   Game & Watch. Contract in koboy.h's koboy_lcd_pad. */
void config_lcd_pad_for_rom(koboy_layout *l, const char *rom_path);

/* Fill or clear the DMG faceplate's EXTRA discs (koboy_extra_btn in koboy.h).
   Separate from the two above for the same reason they are separate from each
   other: neither layout nor physical button complement may follow a `core=`
   override. Idempotent, and it CLEARS as well as sets -- the config outlives
   one game. */
void config_extra_buttons_for_rom(koboy_layout *l, const char *rom_path);

/* Smallest file this extension could BE, in bytes; 0 = no floor. Exists because
   of a crash: snes9x2005 divides by zero in LoROMMap on any .sfc/.smc under
   8192 bytes (`% Memory.CalculatedSize`, which rounds the file down to whole
   8 KB blocks and so is 0) -- SIGFPE inside retro_load_game, taking koboy with
   it, not a refusal. Per-SYSTEM and not global: an Atari 2600 cartridge is
   legitimately 2048 or 4096 bytes. Lives here with the other extension
   knowledge; core.c must stay ignorant of what it is loading. */
size_t config_min_rom_bytes(const char *rom_path);

bool config_join_sibling(char *out, size_t n, const char *name, const char *dir);
bool config_exe_dir(char *out, size_t n);
/* Resolves slashless core/rom/save paths against /proc/self/exe's directory --
   dlopen() never looks in the cwd. See config.c. */
void config_resolve_paths(koboy_config *c);
#endif
