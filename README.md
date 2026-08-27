# koboy

**Retro games on a Kobo e-reader.** Copy one folder to your Kobo over USB,
put some ROMs beside it, and launch it from the reader's own menu. It plays
fifteen systems, from the Game Boy to the Super Nintendo to 1980s arcade
boards, on the e-ink screen you already own.

No jailbreak. Nothing is written outside `.adds/koboy/` — no firmware
changes, no `KoboRoot.tgz`, nothing the update process will ever notice.
**Uninstalling is deleting one folder.**

One device is verified: a **Kobo Libra 2**. Everything else is untested
rather than known-broken — see [What is verified](#what-is-verified).

---

## Systems

| System | File extension | Emulator core | BIOS needed? |
|---|---|---|---|
| Game Boy / Game Boy Color | `.gb` `.gbc` | gambatte | no |
| Game Boy Advance | `.gba` | gpSP | no — built into the core |
| Game & Watch | `.mgw` | gw-libretro | no |
| NES | `.nes` | fceumm | no |
| SNES | `.sfc` `.smc` | snes9x2005 | no |
| Master System / Game Gear | `.sms` `.gg` | Genesis Plus GX | no |
| Mega Drive / Genesis | `.md` | Genesis Plus GX | no |
| PC Engine / TurboGrafx-16 | `.pce` | beetle-pce-fast | no |
| Atari 2600 | `.a26` | stella2014 | no |
| WonderSwan / WS Color | `.ws` `.wsc` | beetle-wswan | no |
| Neo Geo Pocket / Color | `.ngp` `.ngc` | RACE | no |
| Pokémon Mini | `.min` | PokeMini | no |
| **ColecoVision** | `.col` | Gearcoleco | **yes — you supply it** |
| **Intellivision** | `.int` | FreeIntv | **yes — you supply it** |
| Arcade | `.zip` | FinalBurn Neo | no, for the 1980s boards |

**Thirteen of the fifteen need no BIOS at all.** Two do, and neither file is
distributed here because neither is ours to distribute. Put them in
`.adds/koboy/` itself — the folder the `koboy` program is in, not `roms/`:

| System | File | Size | Without it |
|---|---|---|---|
| ColecoVision | `colecovision.rom` (or `coleco.rom`) | 8192 bytes | every game shows a `NO BIOS` screen |
| Intellivision | `exec.bin` | 8192 bytes | nothing runs |
| Intellivision | `grom.bin` | 2048 bytes | nothing runs |

If you have a MiSTer Intellivision setup, `boot0.rom` **is** `exec.bin` and
`boot1.rom` **is** `grom.bin`, byte for byte (checked by comparison, not
assumed — `boot1.rom` and `boot2.rom` are both 2048 bytes and the wrong one
gives you a machine that runs and draws garbage). Copy and rename; leave the
originals alone.

**ROMs are your own problem.** None are included, none ever will be, and
there are no links here.

### Files that will not appear in the browser, on purpose

koboy picks the emulator from the file extension and has no other signal, so
a few common extensions are deliberately not claimed. If a game is missing,
it is almost certainly one of these:

- **`.bin` and `.gen` for Mega Drive.** `.bin` belongs to a dozen systems
  before it belongs to the Mega Drive — and to both Intellivision BIOS files
  above. Rename a Mega Drive `.bin` to `.md` and it works.
- **`.sgx`** (SuperGrafx) and **`.chd` / `.cue`** (CD-based games). The PC
  Engine core cannot run either, and would draw a SuperGrafx game wrongly
  rather than refuse it.
- **A `.sfc` or `.smc` under 8 KB** is refused with a message. That is on
  purpose: the SNES core crashes on such a file instead of rejecting it, and
  the two things that produce one are a half-finished download and the
  `._name.smc` stubs macOS leaves on memory cards. Neither is a game.

---

## Install

1. Plug the Kobo into a computer with USB. It appears as a drive.
2. **Unzip `koboy-<version>.zip` at the top level of that drive.** It creates
   one folder, `.adds/koboy/`, and writes nothing anywhere else.
3. Copy your games into `.adds/koboy/roms/`. Subfolders work — the browser
   walks them one level at a time, which is what makes a thousand-file
   collection usable.
4. Set up one launcher, by copying a file that is already in the folder:
   - **NickelMenu** (most people): copy `.adds/koboy/nm-koboy` to
     `.adds/nm/koboy`.
   - **KFMon**: copy `.adds/koboy/kfmon-koboy.ini` to
     `.adds/kfmon/config/koboy.ini`, and put any PNG at `/koboy.png` for it
     to watch.
5. Eject the drive and let the library scan finish. **The menu entry appears
   after the Kobo next restarts its reading software**, because NickelMenu
   reads its configuration at startup.

You need NickelMenu or KFMon. koboy will not start without one, and this is
enforced rather than recommended — see [Why it will not start over
SSH](#why-it-will-not-start-over-ssh).

### The download is 18.6 MB, and 13.6 of that is arcade

`fbneo_libretro.so` is the FinalBurn Neo arcade core: 41 MB of the 61 MB the
folder takes up on the card, and 13.6 MB of the 18.6 MB you download.
**If you do not have an arcade romset, delete
`.adds/koboy/fbneo_libretro.so`.** Nothing else depends on it, no other
system changes, and `.zip` files simply stop appearing in the browser.

If you do keep it, `.adds/koboy/README-fbneo.txt` has the arcade specifics —
the romset version it has to match, why some zips in a complete set are not
games, and where to put `hiscore.dat`.

---

## Playing

- **The two page-turn buttons are A and B.** They work out of the box on a
  Libra 2. If yours do not, clear `key_a` and `key_b` in
  `.adds/koboy/koboy.ini` and the next launch asks you to press each button
  and records what it sees. You can always tap the screen to skip that, so a
  Kobo with no page-turn buttons at all still plays with the on-screen
  controls.
- **The touchscreen is the d-pad**, in the lower-left of the drawn faceplate,
  with the other buttons around it.
- **`MENU`** is a drawn box on the panel. It opens save states (three slots
  per game), reset, the three screen settings below, a different game, and
  quit.
- **The power button quits**, and puts you back on the home screen without a
  reboot.
- **Battery saves are automatic** — the cartridge's own save memory is
  written every ten seconds and again on exit, the same as the real hardware.

---

## Screen settings: the section that actually matters

E-ink is not a slow LCD, it is a different thing. Three settings in the
in-game `MENU` decide how the picture looks, and **the default is not the
best setting for most games**. Try them in this order.

### 1. `MOTION` — turn this on first

Cycles: `4 GREYS / AUTO` → `1-BIT / AUTO` → `1-BIT / DU`

**If moving things leave smears behind them, this is the fix.** It renders the
game in pure black and white — dithered, like a newspaper photograph — instead
of four shades of grey.

That sounds like a downgrade and it is the single biggest improvement
available on this device. The reason is that the screen's *fast* refresh can
only drive a pixel fully black or fully white. Ask it for a middle grey in a
hurry and the pixel lands somewhere between: the old image does not clear, the
new one arrives on top of it, and a jumping sprite leaves a faint ghost above
itself and a bright band below. Give it black and white only and every
transition completes.

Verified on the panel by the device's owner, playing Super Mario Bros.:
*"motion is much better in both 1bit modes, no white flashing now, even
scrolling looks not bad."* Scrolling was the worst case and had been the
oldest unsolved problem in this project.

Two notes. The two `1-BIT` settings look identical in practice — measurement
later explained why (`AUTO` was already choosing the same refresh mode), so
pick either. And **the Game Boy is the one system with something to lose**:
its four shades already *are* the four the screen can show, so it is the one
case where four greys may look better. Try both.

### 2. `FRAMES` — how often the screen redraws

Cycles: every frame, every 2nd, every 3rd … up to every 8th.

**Fewer, complete redraws beat more incomplete ones.** A full-screen update on
this panel takes about 150 ms to finish, and nothing in the device stops you
asking for another one before the last has settled — the driver accepts work
it cannot do and says nothing. Ask too often during a scroll and you get a
washed-out mess.

At the shipped setting, koboy asks for a frame every third one and then holds
it back if the previous update is probably still settling, sized by how much
of the picture changed. That means a menu or a mostly-static game stays
responsive while a scroll slows down on its own — which is the thing a fixed
setting cannot do.

**Raise this if scrolling looks washed out. Lower it if the game feels
laggy.** The owner had reached "every 8th" by hand before the automatic
throttle existed; measured afterwards, "every 3rd" plus the throttle delivered
24% *more* frames than their manual setting while never overdriving the panel.
So if you are unsure, 3 is the answer.

### 3. `GREYSCALE` — how colour becomes four greys

Cycles: `BALANCED` (default), `LUMA`, `BRIGHT`, `EQUAL`, `VALUE`

Only matters on colour systems; a Game Boy or a mono WonderSwan looks the same
under all five.

The obvious way to turn colour into grey — standard TV luma — weights blue at
about 11%, so **a bright blue sky comes out as the darkest of the four
levels**. Sonic Pocket Adventure's first zone renders with a black sky that
Sonic disappears into. `BALANCED` reweights the colours and lifts the shadows,
which cuts the pixels crushed to black from 6.7% to 2.5% across a sample of
real gameplay frames.

Change it if a game looks too dark or too washed out. `VALUE` is the one to
avoid on most games — it can blow bright scenes out to white and make a HUD
vanish.

### If the picture looks wrong, try this

| What you see | Try |
|---|---|
| Moving sprites leave grey smears | `MOTION` → `1-BIT` |
| Scrolling washes the screen out | `MOTION` → `1-BIT`, then raise `FRAMES` |
| The game feels laggy or slow | lower `FRAMES` |
| The whole picture is too dark | `GREYSCALE` → `BRIGHT` or `EQUAL` |
| A blue sky is nearly black | `GREYSCALE` → anything but `LUMA` |
| Bright areas blow out; the HUD vanishes | `GREYSCALE` → away from `VALUE` |
| A Game Boy game looks worse in 1-bit | `MOTION` → `4 GREYS`; it is the one system where that is expected |
| The picture is a small box on a big screen | some systems cap their size on purpose, for speed. `scale` in `koboy.ini` overrides it |

Everything is remembered — the menu writes your choice back to `koboy.ini`.

### What it is like to actually play

Honestly: **excellent for games with small moving parts, and a compromise for
full-screen scrollers.** Tetris, a strategy game, a turn-based RPG, a
single-screen arcade board — these are what an e-ink screen is for. A
side-scrolling platformer works and is playable, but it will never look like
an LCD, and `MOTION` is the difference between "playable" and "unpleasant".

The frame rate is not the emulator's fault. Every one of the fifteen systems
emulates a frame in 2 to 4.4 milliseconds on this hardware — a whole Mega
Drive for 4 ms. Getting the picture onto the panel is what costs 14 to 23 ms.
See [INTERNALS.md](INTERNALS.md) if that is interesting to you.

---

## Why it will not start over SSH

koboy has to stop the Kobo's own reading software to run at all — it holds an
exclusive claim on every button and the touchscreen, so while it is up nothing
else can read a single press. Starting it again afterwards is the hard part.

The Kobo sets up a handful of environment variables before it first launches
that software, and a copy started *without* them **rewrites the file that
identifies the device**, replacing the real serial number with a placeholder.
That file is what every e-ink tool on the device reads to know which Kobo it
is on, so afterwards everything — KOReader included — quietly loses its
per-device tuning until you reboot. That was measured on a Libra 2, not
theorised. It happened once, by hand.

A program launched from NickelMenu or KFMon inherits that environment, which
is what makes the restart safe. So koboy's launcher checks for it, and if it
is missing it refuses to touch anything: it draws a message on the screen,
leaves everything running, and exits. An SSH launch cannot damage the device;
it simply declines to start.

---

## What is verified

**One device: a Kobo Libra 2.** Games played on it, exits cleanly to the home
screen without a reboot.

[TESTED.md](TESTED.md) is the real record and is more careful than this
summary. In short:

**Played by a person, on the device:** Game Boy, NES and Game & Watch. Those
three sessions are also what verifies the parts of koboy that only a finger
can reach — stopping and restarting the reader software, the touch d-pad, the
drawn `MENU` and everything in it, and the game browser.

**Run on the device but not played:** all fifteen systems. Each one loads,
works out its screen geometry, paces itself at its own frame rate and renders
at a measured speed. Those runs were driven automatically over SSH, so they
answer "does it work" and say nothing about "is it nice to play".

**Not established, and worth knowing before you rely on it:**

- The two BIOS files have never been read off a real memory card. Both were
  verified on a desktop.
- **Save states have never been written and re-read on the device.** Cartridge
  battery saves have — Game Boy, NES and Game Boy Advance games have all
  written a save file on real hardware and read it back byte-identical, and
  the case where the file is damaged leaves it alone rather than making it
  worse. Save states are a different mechanism and only desktop tests cover
  them.
- Twelve of the fifteen systems have never had a thumb on their controls, so
  nothing says whether their on-screen buttons are laid out usably.
- One Game Boy Advance file out of 1693 in a real collection crashes the
  emulator. It is homebrew, and no check can catch it before it runs.

**Every other Kobo is unverified — not known broken, just unmeasured.** koboy
works out what it can from the hardware at runtime (screen size, colour depth,
touch orientation, which refresh modes exist), so it has a fair chance
elsewhere. A fair chance is not the same as tested.

**If you run it on another Kobo, please add a row.** You do not need to build
anything: `koboy-probe`, included in the download, characterises a device
safely over SSH without stopping anything or changing anything. TESTED.md's
"How to add a row" section is the walkthrough, and a report saying "the d-pad
was unusable, the touch axes came out sideways" is worth far more than
silence.

---

## More

- [BUILD.md](BUILD.md) — building from source, and the toolchain, which is the
  hard part.
- [INTERNALS.md](INTERNALS.md) — how it works, and the decisions the hardware
  forced that look like mistakes from the outside.
- [TESTED.md](TESTED.md) — every measurement, and everything that is not
  established.
- [LICENSES.md](LICENSES.md) — koboy is **GPL-3**; the fifteen emulator cores
  each carry their own licence and three of them restrict commercial use.
