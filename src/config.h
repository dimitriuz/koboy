#ifndef KOBOY_CONFIG_H
#define KOBOY_CONFIG_H
#include "koboy.h"

/* What v1 shipped uncommented in config/koboy.ini, back when gambatte was the
   only core. An ini that names exactly this is indistinguishable from one that
   was never edited, so config.c does NOT treat it as an explicit pick. See the
   long comment at the `core=` parse site. */
#define KOBOY_CORE_LEGACY_DEFAULT "gambatte_libretro.so"

/* The same trap, one field over. v1 shipped `scale = 5` uncommented when the
   Game Boy was the only system, so an ini naming 5 records packaging rather
   than preference and does NOT mark the scale explicit. See the `scale=` parse
   site and config_resolve_profile. */
#define KOBOY_SCALE_LEGACY_DEFAULT 5

typedef struct {
    int      scale;              /* 0 = pick the largest that fits */
    bool     scale_explicit;     /* the user named a scale, as opposed to
                                    config_defaults' Game-Boy 5. Without this
                                    a 96x64 Pokemon Mini rendered at 480x320 --
                                    a postage stamp on a 1264x1680 panel --
                                    because a number measured for the Game Boy
                                    was being applied to every system. See
                                    config_resolve_profile. */
    int      present_divisor;    /* core frames per presented frame */
    int      cleanup_interval;   /* presented frames between game-rect cleanups */
    int      cleanup_max_ms;     /* wall-clock ceiling between cleanups; <=0 off */
    int      wfm_fast_policy;    /* koboy_wfm_policy for KOBOY_REFRESH_FAST */
    int      full_refresh_permille; /* dirty area (permille of game rect) above
                                       which a frame is refreshed with FULL
                                       instead of FAST; <= 0 disables */
    int      refresh_fixed_tiles; /* fixed per-refresh cost, in 8x8 tiles, used
                                     by video_split_dirty's cost model. A
                                     config key rather than a constant because
                                     every absolute timing this project has
                                     measured moved by up to a factor of 2.2
                                     between sessions -- see config/koboy.ini. */
    int      scale_ceiling;      /* auto-fit cap for the loaded system, 0 = none.
                                    Set from config_scale_ceiling_for_rom at the
                                    same point the core is chosen. */
    bool     pixel_aspect;       /* honour a core's reported pixel aspect. ON by
                                    default -- eight systems render wrongly
                                    without it, the Atari 2600 by 1.75x. It is a
                                    key rather than a constant because it
                                    changed the presentation of every system but
                                    the Game Boy and Game & Watch, and NONE of
                                    that has been seen on a real panel: if
                                    non-integer scaling turns out to read badly
                                    on e-ink, this is the way back without a
                                    rebuild. Same reasoning as gray_map. */
    bool     force_dither;
    /* koboy_gray_map, held as an int for the same reason dpad_mode is: this
       struct is memset to zero and parsed from text, and an enum-typed field
       would make an out-of-range ini value undefined rather than merely
       wrong. video.c's gray_of guards the range. */
    int      gray_map;
    bool     grab_input;
    int      dpad_mode;
    int      dpad_deadzone;      /* px */
    int      dpad_hysteresis;    /* px */
    uint16_t key_a, key_b;       /* 0 = not yet calibrated */
    uint16_t key_start, key_select; /* 0 = not yet calibrated; gamepad shoulder
                                        buttons by default, see config_defaults */
    char     rom_path[512];
    char     rom_dir[512];       /* where the browser looks; install-relative */
    char     core_path[512];
    bool     core_explicit;      /* the user named a core (ini `core=` or
                                    --core), as opposed to config_defaults'
                                    gambatte fallback. Needed because the
                                    default is written into core_path
                                    unconditionally, so the string alone
                                    cannot say whether anyone asked for it --
                                    and only an unasked-for core may be
                                    overridden by the ROM's extension. */
    char     save_dir[512];
    /* koboy_layout_mode. NOT an ini key and not a user preference: it is a
       fact about the loaded system, derived from the ROM's own extension by
       config_layout_for_rom the same way the core is by config_core_for_rom,
       and written here by main.c before config_resolve_profile reads it.
       config_defaults leaves it at KOBOY_LAYOUT_DMG (0), which is what every
       caller that never sets it -- every existing test, and the placeholder
       profile main.c resolves before a ROM has been chosen -- gets. */
    int      layout_mode;
    koboy_layout layout;
} koboy_config;

void config_defaults(koboy_config *c);
bool config_load(koboy_config *c, const char *path);

/* base_w/base_h/max_w/max_h come from core_get_geometry (src/core.h), queried
   once after retro_load_game -- config.c has no core.h dependency of its own
   (layering: config is lower-level than core), so the caller resolves the
   query and passes the four numbers through as plain ints rather than this
   header taking on a struct koboy_core it does not otherwise need.

   WHICH PAIR DRIVES THE SCALE SEARCH AND THE GAME RECT DEPENDS ON THE
   LAYOUT: base_w/base_h in KOBOY_LAYOUT_DMG, max_w/max_h in
   KOBOY_LAYOUT_LCD. See the koboy_profile comment in koboy.h for why, and
   config.c's own note where rect_w is computed for what it bought and what
   makes it safe. All four must be >= 1 or this returns false, the same way an
   impossibly small panel already does -- base included, now that it is
   divided by. Both pairs are carried into the profile unchanged. */
int config_scale_ceiling_for_rom(const char *rom_path);

bool config_resolve_profile(koboy_profile *p, const koboy_config *c,
                            int panel_w, int panel_h,
                            int base_w, int base_h, int max_w, int max_h);

/* As above, for a core whose PIXELS ARE NOT SQUARE. `par` is the pixel aspect
   video_pixel_aspect derives from the core's display aspect and its BASE
   geometry, 16.16; KOBOY_ASPECT_ONE makes this function identical to
   config_resolve_profile, which is literally how that one is implemented.

   Why the reserved rect has to know at all, when the fit that places each
   frame inside it (video_fit_par) already does: the rect is sized as
   max_w x max_h times an integer scale, and a wider-than-tall pixel makes the
   picture wider than max_w * scale. The fit then has nowhere to put it and
   drops a whole integer step. MEASURED, on a 1264x1680 panel, before this
   parameter existed: Super Mario Bros. went from 768x720 filling its rect to
   585x480 inside it, Defender from 876x720 to 640x480. Correctly shaped and a
   third smaller is not a fix. Sizing the rect for the shape the content will
   actually be shown at gives 878x720 and 960x720 instead -- both correctly
   shaped AND bigger than what square scaling produced.

   BASE geometry, not max, and the difference is not academic. The display
   aspect a core reports describes the picture it is rendering NOW, so the
   pixel shape has to be derived against that same frame; deriving it against
   max instead reads the WonderSwan's 1.5556 (announced for a 224x144
   landscape frame) against its square 224x224 max buffer and widens a rect
   whose pixels are exactly square by 56%. */
bool config_resolve_profile_par(koboy_profile *p, const koboy_config *c,
                                int panel_w, int panel_h,
                                int base_w, int base_h, int max_w, int max_h,
                                uint32_t par);
/* Do these two resolved profiles present the game the same way -- same rect,
   in the same place, at the same scale, in the same layout, out of a buffer
   the same size? Compares only what config_resolve_profile_par decides, and
   deliberately NOT base_w/base_h: "the core is drawing a different frame now"
   is the question the caller already knows the answer to; this one asks
   whether that made any difference worth a rebuild for.

   It exists because main.c's geometry poll cannot decide that from the inputs
   any more. The DMG rect is sized from base, so a base change CAN move it
   (SNES entering a 512-wide hi-res mode); the LCD rect is not, so a base
   change never does (Game & Watch, which toggles base several times a second
   and must not pay a video rebuild and a full faceplate repaint for it). The
   two cases are indistinguishable without resolving both and looking.

   max_w/max_h are in the comparison even though they do not size the DMG rect:
   video_create allocates its intermediate buffer from them, so a max that
   moved needs the rebuild whatever the rect did.

   NULL on either side is false -- two things one of which does not exist are
   not the same presentation. */
bool config_profile_presentation_same(const koboy_profile *a,
                                      const koboy_profile *b);

bool config_save_keys(const char *path, uint16_t key_a, uint16_t key_b);

/* Rewrites `path` with exactly one `gray_map = <name>` line, preserving every
   other line, comment and blank included -- the same read-filter-rename dance
   config_save_keys does for the calibrated keys, and literally the same code
   underneath, so the two cannot drift.

   It exists because the greyscale mapping is a SUBJECTIVE judgement about how
   a reflective panel looks, which means the owner has to be able to flip
   between mappings while looking at the game, and a choice made that way has
   to survive the next launch. The ini key and the in-game menu are one
   setting, not two: the menu writes the same key config_load reads. */
bool config_save_gray_map(const char *path, koboy_gray_map map);

/* Should a frame whose dirty rect covers dirty_px of a whole_px game rect be
   promoted from the fast waveform to the flashing one? Lives here, and is
   tested, because the shipped default turns the promotion off and "off" is only
   trustworthy if the comparison itself is pinned: a >= that became a > would
   quietly reintroduce flashing with nothing failing. */
bool config_promote_full(const koboy_config *c, long dirty_px, long whole_px);

/* Resolve a slashless core/rom/save path against the directory containing the
   running executable. See the long comment in config.c: dlopen() never looks in
   the cwd for a name with no slash, so a bare core name could not be found on
   the device at all. */
/* Which core should load this ROM? Returns a bare core filename (no slash) --
   config_resolve_paths' sibling-join turns it into an absolute path later, for
   the reason dlopen documents and this project learned the hard way. */
const char *config_core_for_rom(const char *rom_path);

/* Which PRESENTATION should this ROM get -- a koboy_layout_mode. Same
   predicate as config_core_for_rom (the extension, and nothing else), and
   deliberately a second function rather than a second return value from that
   one: the core can be overridden by an explicit `core=` or --core, and the
   layout must NOT follow that override. A .mgw is a Game & Watch unit whose
   buttons are drawn into its own artwork whichever shared object ends up
   interpreting it, so naming a core by hand cannot make a DMG faceplate the
   right answer for it. */
int config_layout_for_rom(const char *rom_path);

/* Fill or clear the DMG faceplate's EXTRA discs for this ROM -- see
   koboy_extra_btn in koboy.h. A third function rather than more return values
   from the two above for the same reason those two are separate: the core can
   be overridden by `core=` or --core, and neither the layout nor the physical
   button complement may follow that override. A Pokemon Mini cartridge has a
   C button, and a WonderSwan an A and a B that land on L1/R1 once the screen
   is rotated, whatever shared object ends up interpreting them. Idempotent,
   and it CLEARS as well as sets, because the config outlives one game. */
void config_extra_buttons_for_rom(koboy_layout *l, const char *rom_path);

/* The smallest file this ROM's extension could possibly BE, in bytes, or 0
   for "no floor". A fourth function keyed on the extension, and it exists
   because of a crash rather than a preference.
   snes9x2005 divides by zero in LoROMMap on any .sfc/.smc under 8192 bytes
   -- `% Memory.CalculatedSize`, where CalculatedSize rounds the file down to
   whole 8 KB blocks and so is 0. That is SIGFPE inside retro_load_game, not
   a refusal: it takes koboy down with it, which is a worse failure than any
   this project has shipped. The floor is per-SYSTEM and cannot be global --
   an Atari 2600 cartridge is legitimately 2048 or 4096 bytes -- so it lives
   here beside the other extension knowledge rather than in core.c, which
   must stay ignorant of what it is loading. */
size_t config_min_rom_bytes(const char *rom_path);

bool config_join_sibling(char *out, size_t n, const char *name, const char *dir);
bool config_exe_dir(char *out, size_t n);
void config_resolve_paths(koboy_config *c);
#endif
