# koboy

A Game Boy emulator for Kobo e-readers. It runs a libretro core (gambatte),
renders the DMG's four greys straight onto the e-ink panel through FBInk, and
reads the page-turn buttons and the touchscreen directly from evdev.

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
- **Your own ROMs.** None are included, and none ever will be. Point `rom=` in
  `koboy.ini` at a `.gb` file you are entitled to have.

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

- The two page-turn buttons are A and B, mapped out of the box to the codes the
  Libra 2 emits (193 and 194). If yours differ, clear `key_a`/`key_b` in
  `koboy.ini` and the next launch asks you to press each one and records what it
  sees. That prompt can always be dismissed by tapping the screen, so a Kobo
  with no page-turn buttons at all keeps the on-screen controls and plays.
- The touchscreen is the d-pad: the lower-left region of the faceplate. The
  default `cross` mode splits the drawn cross into four fixed zones, which is
  what the drawing implies; `dpad_mode = relative` instead steers from wherever
  you first touched, thumb-pad style, and needs a drag rather than a tap.
- The power button quits, and so does `SIGTERM`. Battery saves are written
  atomically every ten seconds and again on exit.
- Exiting puts the panel back the way Nickel left it and starts Nickel again.
  You should end up back on the home screen without rebooting.

Everything else is in `koboy.ini`, which documents each key inline: render
scale, which waveform to use for fast refreshes, d-pad geometry, and the two
ghosting mitigations that ship *disabled* --- the driver's own waveform choice
made them redundant, and measuring showed they were the only thing making the
panel flash. The file explains that in place, so nobody turns them back on
thinking they were forgotten.

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
