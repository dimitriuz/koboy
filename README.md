# koboy

A retro emulator for Kobo e-readers, built Game-Boy-first. It runs a libretro
core picked from the ROM's own extension --- gambatte for `.gb`/`.gbc`,
gw-libretro for Game & Watch `.mgw`, fceumm for NES `.nes`, PokeMini for
Pokemon Mini `.min`, beetle-wswan for WonderSwan and WonderSwan Color
`.ws`/`.wsc`, RACE for Neo Geo Pocket and Pocket Color `.ngp`/`.ngc`,
stella2014 for Atari 2600 `.a26`, Gearcoleco for ColecoVision `.col`,
FreeIntv for Intellivision `.int`, Genesis Plus GX for Master System `.sms`
and Game Gear `.gg`, FinalBurn Neo for arcade `.zip` --- renders four greys
straight onto the e-ink panel through FBInk, and reads the page-turn buttons
and the touchscreen directly from evdev.

Nine of the eleven systems need no BIOS file. **ColecoVision and Intellivision
do**, and neither file ships with koboy because neither is ours to
distribute --- see "BIOS files" below.

**Arcade is a separate download.** The FinalBurn Neo core is 41 MB, ten times
the rest of koboy put together, and most people running a Game Boy on an
e-reader have no arcade romset --- so it ships as `koboy-fbneo-<version>.zip`,
unzipped on top of the main archive. See "Arcade" below.

The two greyscale handhelds suit the panel best: a WonderSwan and a Neo Geo
Pocket both drive eight shades of grey natively, so a mono title loses almost
nothing on the way to four. Their Color halves lose what any colour system
loses --- see [TESTED.md](TESTED.md).

It is built around what the hardware can actually do rather than around what a
desktop emulator expects: each refresh lets the panel controller pick its own
waveform from the pixels actually changing, only changed rectangles are pushed,
and frames are presented at a third of the emulated rate because e-ink cannot go
faster. Forcing the fast four-level DU4 waveform instead is quicker per refresh
but cannot erase, which ghosted badly in play, so `waveform_fast = du4` is an
option in `koboy.ini` and not the default. On a Kobo Libra 2 that is a playable
Game Boy.

## What you need

- A Kobo. **Verified on the Libra 2 only** --- see [TESTED.md](TESTED.md), and
  please add a row if you run it on anything else.
- NickelMenu or KFMon, to launch it. koboy will not run without one; see
  "Launching" below for why that is a hard requirement.
- **Your own ROMs.** None are included, and none ever will be. Either point
  `rom=` in `koboy.ini` at a specific `.gb`/`.gbc`/`.mgw`/`.nes`/`.min`/
  `.ws`/`.wsc`/`.ngp`/`.ngc`/`.a26`/`.col`/`.int`/`.sms`/`.gg`/`.zip` file,
  or drop several into `.adds/koboy/roms/` (the `rom_dir` default) and pick
  one from koboy's own on-panel menu at launch -- see "Playing" below.
  Subfolders work: the browser walks them one level at a time, which is what
  makes a 1386-file NES collection usable. Files that are not ROMs are not
  listed, so the `.pal` palettes, `boot.rom`, `boot1.rom` and `boot0`-`boot3`
  a real collection carries stay out of the way --- including the two files
  that ARE a BIOS.

## Arcade

Install `koboy-fbneo-<version>.zip` over the main archive --- it adds one file,
`fbneo_libretro.so`, to `.adds/koboy/`. After that a `.zip` in `roms/` appears
in the browser and runs.

**The romset has to match the core.** An arcade "ROM" is a zip of one board's
individual EPROM dumps, checked by name and CRC against the emulator's own
database, and FinalBurn Neo changes that database between releases. This build
is **FinalBurn Neo v1.0.0.03, the revision of 2025-07-24**. A set built for a
different FBNeo release will not load, and a MAME set of any vintage will not
load at all --- both failures look exactly like a broken emulator.

**Some zips in a complete set are not games.** `neogeo.zip`, `midssio.zip`,
`namcoc69.zip` and friends are device and BIOS dumps that games load beside
themselves. They list in the browser and cannot be started. Leave them
alone --- `tapper.zip` does not run without `midssio.zip`.

**No BIOS file is needed for the 1980s boards**: an arcade PCB carries its
whole program. Later hardware (Neo Geo, CPS) wants a BIOS zip in `roms/`
beside the games, not in the koboy directory.

**This is the panel's best fit so far, with one exception.** The golden-age
cabinets turned their monitor on its side, so Galaga, Dig Dug, Donkey Kong,
Ms. Pac-Man and Frogger are portrait games on a portrait e-reader --- and
they are single-screen, so the smearing that spoils a scrolling platformer
cannot happen. The vertical *shooters* (Xevious, 1942) scroll continuously
and do smear, exactly as a Game Boy platformer does.

**Coin, then Start.** A board will not begin until it has been paid: `SELECT`
is Coin and `START` is Start. The faceplate's `B` and `A` are the board's
buttons 1 and 2, and the two extra discs are buttons 3 and 4.

**Saving.** An arcade board has no battery, so there is no `.srm` --- the
in-game MENU's save states are the way to keep a game mid-play.

**High scores** are the other half, and they need one file you supply. Put
`hiscore.dat` (FinalBurn Neo's own, matching this build) in
`.adds/koboy/fbneo/` --- create the directory. koboy turns the feature on for
you; without the file it is simply inert. With it, each board writes
`.adds/koboy/fbneo/<board>.hi` when you leave the game and reads it back next
time. Verified on Ms. Pac-Man: after a 220-point game the attract screen reads
`HIGH SCORE 220` on the next launch, where a fresh install shows a blank.

## BIOS files

Two of the eleven systems cannot run without the console's own boot ROM, which
is copyrighted and is not distributed here. Put the files in `.adds/koboy/`
itself --- the directory the `koboy` binary lives in, not `roms/`:

| System | File | Size | Without it |
|---|---|---|---|
| ColecoVision | `colecovision.rom` (or `coleco.rom`) | 8192 | every game shows a `NO BIOS` screen |
| Intellivision | `exec.bin` | 8192 | nothing runs |
| Intellivision | `grom.bin` | 2048 | nothing runs |

If you have a MiSTer Intellivision setup, `boot0.rom` **is** `exec.bin` and
`boot1.rom` **is** `grom.bin`, byte for byte. `boot2.rom` (Intellivoice) and
`boot3.rom` (ECS) are not needed. Rename copies rather than the originals.

Every other system here needs nothing: the Game Boy, Game & Watch, NES,
WonderSwan, Neo Geo Pocket, Atari 2600, Master System, Game Gear and arcade
cores either need no boot ROM or link a free replacement. (Arcade's own
device zips are romset members, not BIOS files in this sense --- they live in
`roms/` beside the games. See "Arcade".)

## Install

Unzip `koboy-<version>.zip` at the root of the drive the Kobo shows up as over
USB. That creates one directory, `.adds/koboy/`, and writes nothing anywhere
else: no `KoboRoot.tgz`, nothing under `/usr`, nothing the firmware updater or
the recovery partition will ever look at. Uninstalling is deleting that
directory.

Then, one of:

- **NickelMenu**: copy `.adds/koboy/nm-koboy` to `.adds/nm/koboy`.
- **KFMon**: copy `.adds/koboy/kfmon-koboy.ini` to
  `.adds/kfmon/config/koboy.ini` and put a PNG at `/mnt/onboard/koboy.png` for
  it to watch.

Put a ROM somewhere on the drive and set `rom=` in `.adds/koboy/koboy.ini`.
Eject, and let the library rescan finish. The menu entry appears after Nickel
next restarts, because NickelMenu reads its configuration at Nickel's startup.

## Launching

From the Kobo's own menu. Not from a shell over ssh --- and koboy enforces that
rather than merely recommending it.

koboy has to stop Nickel to run at all: Nickel holds an exclusive grab on every
input device, so while it is up nothing else can read a button press. Starting
it again afterwards is the hard part. Kobo's `rcS` exports a set of variables
(`PLATFORM`, `PRODUCT`, the NTX hardware identity) before Nickel is first
launched, and a Nickel started *without* them rewrites
`/mnt/onboard/.kobo/version` with a placeholder serial --- which is the file
FBInk reads to identify the device, so afterwards every FBInk-based tool on the
device, KOReader included, silently loses its per-device quirks until the next
reboot. That was measured on a Libra 2, not theorised.

A process spawned by NickelMenu or KFMon inherits that environment from Nickel
itself, which is what makes the restart safe. So `koboy.sh` checks for it, and
if it is missing it refuses to touch Nickel at all: it draws a message on the
panel, leaves Nickel running, and exits non-zero. An ssh launch therefore
cannot damage the device identity; it just declines to start.

## Playing

- If `rom=` in `koboy.ini` is unset, koboy opens a MAIN MENU instead of going
  straight to a game: **RECENT** (the last few ROMs actually played, newest
  first -- empty on a first run), **ALL GAMES** (the contents of `rom_dir`,
  default `roms` i.e. `.adds/koboy/roms/`, one folder at a time),
  or **QUIT**. ALL GAMES pages with the page-turn buttons, jumps by starting
  letter with a tap on the right-edge strip or with both page-turn buttons
  together, and is picked with a tap; set `rom=` to a specific path to skip
  this menu entirely and go straight to that ROM.
- The two page-turn buttons are A and B, mapped out of the box to the codes the
  Libra 2 emits (193 and 194). If yours differ, clear `key_a`/`key_b` in
  `koboy.ini` and the next launch asks you to press each one and records what it
  sees. That prompt can always be dismissed by tapping the screen, so a Kobo
  with no page-turn buttons at all keeps the on-screen controls and plays.
- The touchscreen is the d-pad: the lower-left region of the faceplate. The
  default `cross` mode splits the drawn cross into four fixed zones, which is
  what the drawing implies; `dpad_mode = relative` instead steers from wherever
  you first touched, thumb-pad style, and needs a drag rather than a tap.
- A drawn **MENU** box opens the in-game menu: save to or load from one of
  three per-ROM save-state slots, reset the core, switch to a different ROM
  (back to the same MAIN MENU above, so RECENT is reachable mid-session too),
  or quit. The save-state format is gambatte's
  own serialised core state, written and read through `safefile.c`'s
  all-or-nothing helpers so a crash or a full disk mid-write cannot leave a
  slot half-written; loading a slot never touches the core until the whole
  blob has been read.
- The power button quits, and so does `SIGTERM`. Battery saves (the
  cartridge's own SRAM, not save states) are written atomically every ten
  seconds and again on exit.
- Exiting puts the panel back the way Nickel left it and starts Nickel again.
  You should end up back on the home screen without rebooting.

Everything else is in `koboy.ini`, which documents each key inline: render
scale, which waveform to use for fast refreshes, d-pad geometry, `rom_dir` for
ALL GAMES above, and the two ghosting mitigations that ship *disabled* ---
the driver's own waveform choice made them redundant, and measuring showed
they were the only thing making the panel flash. The file explains that in
place, so nobody turns them back on thinking they were forgotten.

One key, `refresh_fixed_tiles`, ships at a starting guess (40) rather than a
measurement: it trades off merging nearby dirty rectangles against refreshing
them separately, and the on-device tuning run that would pin down its real
value has not happened yet. See [TESTED.md](TESTED.md).

If something goes wrong, `.adds/koboy/koboy.log` has the whole story --- the
launcher logs every step of stopping and restarting Nickel, and koboy's own
output goes to the same file.

## Building

`make test` runs the unit tests on the host. `make host` builds a desktop SDL
build, which is the practical way to work on anything that is not
device-specific. `make kobo` cross-compiles for the device and `make dist`
produces the zip; both need an ARM toolchain targeting glibc 2.19 or older, for
reasons documented at length in `docs/cross-compiling.md`.

## Licence

koboy is free software: you can redistribute it and/or modify it under the terms
of the GNU General Public License as published by the Free Software Foundation,
either version 3 of the License, or (at your option) any later version.

It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
PURPOSE. See the GNU General Public License for more details:
<https://www.gnu.org/licenses/>.

gambatte is licensed under the GPL version 2; FBInk under the GPL version 3 or
later. Neither project's code is reproduced here --- both are fetched and built
by the scripts in `scripts/`.
