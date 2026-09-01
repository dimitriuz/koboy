# koboy

**Retro games on a Kobo e-reader.** Copy one folder over USB, drop some ROMs
beside it, launch it from the reader's own menu. Fifteen systems, the Game Boy
and the Super Nintendo and 1980s arcade boards among them, on the e-ink screen
you already own.

**Support the developer:**
<a href="https://ko-fi.com/W3Q224VFOR" target="_blank"><img src="https://ko-fi.com/img/githubbutton_sm.svg" alt="ko-fi"></a>
<a href="https://www.buymeacoffee.com/dmitriileshchenko" target="_blank"><img src="https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png" alt="Buy Me a Coffee" style="height: 30px !important;width: 140px !important;" ></a>

---

<table>
<tr>
<td width="50%"><img src="docs/screenshots/Legend_of_Zelda_The_-_Link_s_Awakening_USA_Europe_Rev_2-002.png" alt="Link's Awakening on a Game Boy faceplate"></td>
<td width="50%"><img src="docs/screenshots/Streets_of_Rage_2_USA-002.png" alt="Streets of Rage 2 with a six-button Mega Drive pad"></td>
</tr>
<tr>
<td align="center"><em>Game Boy: the faceplate koboy started with</em></td>
<td align="center"><em>Mega Drive: six buttons, laid out like the real pad</em></td>
</tr>
</table>

**[More screenshots, grouped by system →](docs/SCREENSHOTS.md)**

---

Most of the code was written by Claude, directed and reviewed by a
professional software developer.

---

## Systems

| System | File extension | Emulator core | BIOS needed? |
|---|---|---|---|
| Game Boy / Game Boy Color | `.gb` `.gbc` | gambatte | no |
| Game Boy Advance | `.gba` | gpSP | no, it's built into the core |
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
| ColecoVision | `.col` | Gearcoleco | **yes, you supply it** |
| Intellivision | `.int` | FreeIntv | **yes, you supply it** |
| Arcade | `.zip` | FinalBurn Neo | no, for the 1980s boards |

Thirteen of the fifteen need no BIOS. Two do, and those files aren't ours to
hand out. They go in `.adds/koboy/` itself, beside the `koboy` program, not in
`roms/`:

| System | File | Size | Without it |
|---|---|---|---|
| ColecoVision | `colecovision.rom` (or `coleco.rom`) | 8192 bytes | every game shows a `NO BIOS` screen |
| Intellivision | `exec.bin` | 8192 bytes | nothing runs |
| Intellivision | `grom.bin` | 2048 bytes | nothing runs |

---

## Install
0. **You need NickelMenu or KFMon.** koboy won't start without one.
1. Plug the Kobo into a computer over USB. It shows up as a drive.
2. Unzip `koboy-<version>.zip` at the top level of that drive. You get one
   folder, `.adds/koboy/`, and a note at the root you can delete afterwards.

   The zip will look empty: everything is inside `.adds`, whose leading dot
   hides it from Finder, Explorer and Linux file managers alike. That's the
   right shape, `.adds` is where every Kobo add-on lives, and your extractor
   creates it for you.
3. Put your games in `.adds/koboy/roms/`. Subfolders work: the browser walks
   them one level at a time, which is what keeps a thousand-file collection
   usable.
4. Set up one launcher, by copying a file that's already in the folder:
   - NickelMenu (most people): copy `.adds/koboy/nm-koboy` to
     `.adds/nm/koboy`.
   - KFMon: copy `.adds/koboy/kfmon-koboy.ini` to
     `.adds/kfmon/config/koboy.ini`, and put any PNG at `/koboy.png` for it to
     watch.
5. Eject the drive and let the library scan finish. **The menu entry only
   appears after the Kobo next restarts its reading software**, because
   NickelMenu reads its config at startup.

### The download is 18.6 MB, and 13.6 of that is arcade

`fbneo_libretro.so` is the FinalBurn Neo arcade core: 41 MB of the 61 MB the
folder takes on the card, 13.6 MB of the 18.6 MB you download. **If you
haven't got an arcade romset, delete `.adds/koboy/fbneo_libretro.so`.**
Nothing else depends on it: `.zip` files just stop appearing in the browser
and no other system changes.

If you keep it, [`packaging/README-fbneo.txt`](packaging/README-fbneo.txt) has
the arcade details: which romset version it has to match, why some zips in a
complete set aren't games, why a board ignores your coin for its first ten
seconds, and where `hiscore.dat` goes. It ships as
`.adds/koboy/README-fbneo.txt` too, so it's on the device.

---

## Playing

- The two page-turn buttons are A and B. They work out of the box on a Libra
  2. If yours don't, clear `key_a` and `key_b` in `.adds/koboy/koboy.ini` and
  the next launch asks you to press each one and records what it sees. A tap
  on the screen skips that, so a Kobo with no page-turn buttons at all still
  plays on the on-screen controls.
- The touchscreen is the d-pad, lower-left on the drawn faceplate, with the
  other buttons around it.
- `MENU` is a drawn box on the panel: save states (three slots a game), reset,
  the three screen settings below, a screenshot, a different game, quit.
- The power button quits, back to the home screen, no reboot.
- Battery saves happen on their own. The cartridge's own save memory is
  written every ten seconds and again on exit, same as the real hardware.

---

## Screen settings

E-ink doesn't redraw like an LCD, and for most games the shipped default isn't
the best setting. Three rows of the in-game `MENU` decide how the picture
looks. Try them in this order.

### 1. `MOTION`: start here

Cycles: `4 GREYS / AUTO` → `1-BIT / AUTO` → `1-BIT / DU`

If moving things leave smears behind them, this is the fix. The game renders
in pure black and white, dithered like a newspaper photograph, instead of four
shades of grey.

### 2. `FRAMES`: how often the screen redraws

Cycles: every frame, every 2nd, every 3rd … up to every 8th.

Fewer complete redraws beat more incomplete ones. A full-screen update takes
about 150 ms to settle, and nothing in the device stops you asking for the
next one before that: the driver accepts work it can't do and says nothing.
Ask too often mid-scroll and you get a washed-out mess.

At the shipped setting koboy asks every third frame, then holds even that back
while the previous update is probably still settling, scaled by how much of the
picture changed. A menu or a mostly-static game stays responsive, a scroll
slows itself down, and no fixed number can do both.

Raise it if scrolling washes out, lower it if the game feels laggy. The owner
had worked their way to "every 8th" by hand before the automatic throttle
existed; measured afterwards, "every 3rd" plus the throttle delivered 24%
*more* frames than that and never overdrove the panel. If you're unsure, 3.

### 3. `GREYSCALE`: how colour becomes four greys

Cycles: `BALANCED` (default), `LUMA`, `BRIGHT`, `EQUAL`, `VALUE`. Change it if
a game looks too dark or too washed out. It only matters on colour systems: a
Game Boy or a mono WonderSwan looks the same under all five.

The obvious conversion, standard TV luma, weights blue at about 11%, so a
bright blue sky comes out as the darkest of the four levels. `BALANCED`
reweights the colours and lifts the shadows, which cuts the pixels crushed to
black from 6.7% to 2.5% across a sample of real gameplay frames.

### If the picture looks wrong

| What you see | Try |
|---|---|
| Moving sprites leave grey smears | `MOTION` → `1-BIT` |
| Scrolling washes the screen out | `MOTION` → `1-BIT`, then raise `FRAMES` |
| The game feels laggy or slow | lower `FRAMES` |
| The whole picture is too dark | `GREYSCALE` → `BRIGHT` or `EQUAL` |
| A blue sky is nearly black | `GREYSCALE` → anything but `LUMA` |
| Bright areas blow out; the HUD vanishes | `GREYSCALE` → away from `VALUE` |
| A Game Boy game looks worse in 1-bit | `MOTION` → `4 GREYS`; it's the one system where that's expected |
| The picture is a small box on a big screen | some systems cap their size on purpose, for speed. `scale` in `koboy.ini` overrides it |

The menu writes your choice back to `koboy.ini`, so it sticks.

### What it's like to play

Excellent for games with small moving parts, a compromise for full-screen
scrollers. Tetris, a strategy game, a turn-based RPG, a single-screen arcade
board: that is what an e-ink screen is for. A side-scrolling platformer works
and is playable, but it will never look like an LCD, and `MOTION` is the
difference between playable and unpleasant.

The frame rate isn't the emulator's fault. All fifteen systems emulate a frame
in 2 to 4.4 milliseconds on this hardware, a whole Mega Drive in 4 ms. Getting
the picture onto the panel costs 14 to 23 ms.
[INTERNALS.md](INTERNALS.md) has the rest.

---

## Screenshots

`MENU` → `SCREENSHOT` writes a PNG of the whole panel into
`.adds/koboy/screenshots/`, named after the game and numbered:
`Super_Mario_Land-001.png`, `-002.png`, and so on. Copy them off over USB like
any other file. `shot_dir` in `koboy.ini` moves the directory, and the number
is read off it each time, so shots you keep are never overwritten by a later
session.

It photographs the game, not the menu. The menu is drawn over the picture, so
choosing that row doesn't take the shot then and there; it arms one, the menu
closes, and the capture comes off the next frame the panel is shown. That's
why the row reads `SCREENSHOT 004 (AFTER THIS MENU)`: 004 is the file you're
about to get. The small `SCREENSHOT 004 SAVED` plaque that appears below the
game a moment later is drawn after the file is written, so it's never in the
picture.

Two practical notes. An e-ink game doesn't pause, so what you capture is the
frame after your last touch: for an action game, arm it somewhere you can
stand still. And the shot is exactly what the panel shows, four greys,
faceplate and all. It isn't a clean emulator frame.

Every image in [docs/SCREENSHOTS.md](docs/SCREENSHOTS.md) was made this way.

---

## Will it run on my Kobo?

Two models are confirmed by a person holding one. Everything else is grouped by
how much of koboy is already known to work on hardware like it. koboy works out
your screen size, colour depth, touch orientation and refresh modes at runtime,
so there is no list of "supported" models in the code — this is a statement
about evidence, not about capability.

### Confirmed working

| Model | Codename | What was confirmed |
|---|---|---|
| **Libra 2** | Io, Mark 9 | Everything. Games played by hand, all fifteen systems rendered, cartridge saves and save states round-tripped, exits cleanly to the home screen. [TESTED.md](TESTED.md) is the real record. |
| **Aura H2O** | Dahlia, Mark 5 | Menus and the ROM browser, by the owner who reported [#1](https://github.com/dimitriuz/koboy/issues/1). Its touchscreen speaks a different dialect from the Libra 2's, and 0.5.5 is the release that got it right. |

### Should work

These share their touch dialect with one of the two above, and have nothing else
koboy has not already met. That is a good reason to expect them to work and it
is not the same as somebody having tried.

- **Like the Aura H2O** — Aura, Aura One, Aura One LE, Glo HD, Touch 2.0,
  Aura SE, Aura SE r2, Nia
- **Like the Libra 2** — Clara 2E, Elipsa 2E, Libra Colour

### Worth trying, but nobody has

Everything here is implemented and none of it has been confirmed on any device.

- **Clara HD, Forma, Libra H2O, Aura H2O², Aura H2O² r2, Clara B&W,
  Clara Colour** — a third dialect ("Snow"). koboy handles it and no device
  using it has ever been tried.
- **Touch A/B/C, Mini, Glo, Aura HD** — single-touch panels, handled by a code
  path no device has exercised. These are also the oldest and slowest hardware
  Kobo made, so even if the controls work the emulation may be too slow to
  enjoy. The Game Boy is the one to try first.
- **Elipsa, Sage** — a different display engine from every other Kobo. The
  touch side should be fine; whether anything appears on screen is genuinely
  unknown.
- **Clara Colour, Libra Colour** — colour panels. koboy draws four greys
  through a colour filter, which should work and may look muddy.

If your model is not listed at all it is probably newer than this file. Try it.

### If it does not work

Open `.adds/koboy/koboy.ini`, set `trace_touch = true`, tap around for a few
seconds, and attach `.adds/koboy/koboy.log` to a bug report. That log is what
turned #1 from two failed guesses into a fix, and it is the single most useful
thing you can send. `docs/kobo-touch-protocols.md` explains what it contains.

If you run it on another Kobo, please add a row. There's nothing to build:
`koboy-probe` ships in the download and characterises a device over SSH
without stopping anything or changing anything. TESTED.md's "How to add a row"
section is the walkthrough, and a report saying "the d-pad was unusable, the
touch axes came out sideways" is worth far more than silence.

---

## More

- [BUILD.md](BUILD.md): building from source, and the toolchain, which is the
  hard part.
- [INTERNALS.md](INTERNALS.md): how it works, and the decisions the hardware
  forced that look like mistakes from the outside.
- [TESTED.md](TESTED.md): every measurement, and everything that isn't
  established.
- [LICENSES.md](LICENSES.md): koboy is GPL-3; the fifteen emulator cores each
  carry their own licence and three of them restrict commercial use.
