Put .gb, .gbc, .gba, .mgw, .nes, .min, .ws, .wsc, .ngp, .ngc, .a26,
.col, .int, .sms, .gg, .md, .sfc, .smc, .pce and .zip files here.
Subdirectories work; the browser walks them one level at a time.
koboy lists them at startup and picks the core from the extension:
.gb/.gbc use gambatte, .mgw uses gw (Game & Watch),
.nes uses fceumm (NES), .min uses PokeMini (Pokemon Mini),
.ws/.wsc use wswan (WonderSwan, WonderSwan Color),
.ngp/.ngc use race (Neo Geo Pocket, Neo Geo Pocket Color),
.a26 uses stella2014 (Atari 2600),
.col uses gearcoleco (ColecoVision),
.int uses freeintv (Intellivision),
.sms/.gg/.md use genesis_plus_gx (Master System, Game Gear, Mega Drive),
.sfc/.smc use snes9x2005 (SNES),
.pce uses mednafen_pce_fast (PC Engine / TurboGrafx-16),
.gba uses gpSP (Game Boy Advance),
.zip uses fbneo (arcade) -- SEE BELOW, and see README-fbneo.txt.

EXTENSIONS THAT ARE DELIBERATELY NOT LISTED. If a file you expect
is missing from the browser, it is almost certainly one of these,
and each is a decision rather than a gap:
  .bin  NOT read as Mega Drive. .bin is the extension of a dozen
        other systems (TI-99, Odyssey 2, Atari 5200, Vectrex ...)
        and of the Intellivision BIOS files below. koboy picks the
        core from the extension alone, so claiming .bin would send
        somebody else's file to a Mega Drive emulator. Rename a
        Mega Drive .bin to .md and it works.
  .gen  Also Mega Drive, also not listed: one system, one extension.
        Rename to .md.
  .sgx  SuperGrafx. The PC Engine core here cannot emulate that
        hardware and would draw it wrongly rather than refuse.
  .chd/.cue  PC Engine CD and Mega CD. These need a system-card or
        BIOS image and CD emulation that koboy does not have.

A NOTE ON SNES FILES. A .sfc or .smc smaller than 8192 bytes is
refused with a message about being too short. That is on purpose:
the SNES core crashes on such a file instead of rejecting it, and
the two things that produce one are a half-finished download and
the ._name.smc stubs macOS leaves on FAT32 cards. Neither is a game.

TWO SYSTEMS NEED A BIOS THAT IS NOT OURS TO SHIP. Put these files in
the koboy directory itself (the one above this one, beside koboy):
  ColecoVision   colecovision.rom   8192 bytes
  Intellivision  exec.bin           8192 bytes
  Intellivision  grom.bin           2048 bytes
Without them a .col shows a NO BIOS screen and a .int does not run.
Every other system here needs no BIOS file at all -- the Game Boy
Advance included: its core carries an open-source BIOS inside it.

ARCADE IS INCLUDED, AND IT IS THE BIG FILE. fbneo_libretro.so is
41 MB of the 61 MB this package unpacks to -- every other core put
together is 18 MB, and most of those are under 3. If you
have no arcade romset, DELETE .adds/koboy/fbneo_libretro.so and
you get that space back; nothing else needs it and no other
system stops working. Keep it and .zip files in this directory
run. README-fbneo.txt, beside the koboy binary, has the rest --
which romset version, why some zips are not games, and where
hiscore.dat goes.
