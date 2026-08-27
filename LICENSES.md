# Licences

koboy is **GPL-3** (see [`LICENSE`](LICENSE) for the full text). A release
archive is not GPL-3 all the way down, though, and this file is the detail.

## What is actually in a release

Two kinds of thing, and the distinction matters because it is the reason this
file has a table in it rather than one line:

- **`koboy` itself** — one ARM binary. It is this project's own C code plus
  **FBInk linked statically into it**. FBInk is GPL-3, koboy is GPL-3, and the
  shipped binary is one work under GPL-3.
- **Fifteen emulator cores** — `gambatte_libretro.so` and the fourteen beside
  it. Each is a separate shared object built from a separate upstream project
  and shipped as its own file. koboy loads one at runtime with `dlopen`,
  chosen from the ROM's extension, and talks to it across the libretro C ABI.
  Nothing from a core is linked into `koboy`, and `koboy` contains no core's
  code. That is the standard libretro arrangement.

Those are the facts of how the pieces fit together. This file does not draw a
legal conclusion from them; a reader who needs one now has what they need to
form it.

**Three of the fifteen cores carry a non-commercial clause.** They are marked
in the table and set out again below it. Those are their own upstream terms
and koboy's licence does not change them.

## koboy's own code

| | |
|---|---|
| Licence | GNU General Public License, version 3 or later |
| Full text | [`LICENSE`](LICENSE) |
| Covers | everything under `src/`, `scripts/`, `tests/`, `config/` and the documentation |

## Statically linked into the koboy binary

| Component | Upstream | Pinned commit | Licence |
|---|---|---|---|
| FBInk | <https://github.com/NiLuJe/FBInk> | `886f25f13368` | GPL-3.0-or-later |

FBInk is the only third-party code inside the `koboy` binary. It is what
abstracts the e-ink refresh ioctls, Kobo's device identification table and the
`/dev/input/event*` classification. Its own submodules (`font8x8`,
`i2c-tools`, `libevdev`, `libunibreak`, `stb`) carry their own licences and
are pinned along with it; see `third_party/fbink/` after a build.

## The cores, loaded at runtime with `dlopen`

Every one is fetched and built from source by `scripts/build-*-core.sh` at the
commit `scripts/pins.txt` records. No core's source is reproduced in this
repository; the pinned commit is how you get the exact source a shipped binary
was built from.

| System(s) | Core | Upstream | Pinned commit | Licence |
|---|---|---|---|---|
| Game Boy, Game Boy Color | gambatte | <https://github.com/libretro/gambatte-libretro> | `d9d6cd06382d` | GPL-2.0 |
| Game & Watch | gw-libretro | <https://github.com/libretro/gw-libretro> | `91d599b951e7` | zlib/libpng |
| NES | fceumm | <https://github.com/libretro/libretro-fceumm> | `236ccdfc911e` | GPL-2.0 |
| Pokémon Mini | PokeMini | <https://github.com/libretro/PokeMini> | `132111b76343` | GPL-3.0-or-later |
| WonderSwan, WS Color | beetle-wswan | <https://github.com/libretro/beetle-wswan-libretro> | `4b01295838ea` | GPL-2.0 |
| Neo Geo Pocket, NGP Color | RACE | <https://github.com/libretro/RACE> | `c7810dd7f172` | GPL-2.0 |
| Atari 2600 | stella2014 | <https://github.com/libretro/stella2014-libretro> | `4a7da82595d2` | GPL-2.0 |
| ColecoVision | Gearcoleco | <https://github.com/drhelius/Gearcoleco> | `6b0e75dd9a52` | GPL-3.0 |
| Intellivision | FreeIntv | <https://github.com/libretro/FreeIntv> | `ef3e0fe322be` | GPL-2.0-or-later |
| Master System, Game Gear, Mega Drive | Genesis Plus GX | <https://github.com/libretro/Genesis-Plus-GX> | `b7e79b3641eb` | **own terms — NON-COMMERCIAL** |
| SNES | snes9x2005 | <https://github.com/libretro/snes9x2005> | `deb49d80d183` | **mixed; the Snes9x core is NON-COMMERCIAL** |
| PC Engine / TurboGrafx-16 | beetle-pce-fast | <https://github.com/libretro/beetle-pce-fast-libretro> | `bebe2b13a840` | GPL-2.0 |
| Game Boy Advance | gpSP | <https://github.com/libretro/gpsp> | `8d268a6bb2cd` | GPL-2.0 |
| Arcade | FinalBurn Neo | <https://github.com/libretro/FBNeo> | `ae41c16e10a1` | **own terms — NON-COMMERCIAL** |

Fifteen systems, fourteen rows: Genesis Plus GX answers for Master System,
Game Gear and Mega Drive out of one shared object.

Every entry above was read out of the licence file in the built tree, not
taken from a summary. The file each came from:

| Core | File read |
|---|---|
| gambatte | `COPYING` |
| gw-libretro | `LICENSE` |
| fceumm | `Copying` |
| PokeMini | `LICENSE` |
| beetle-wswan | `COPYING` |
| RACE | `license.txt` |
| stella2014 | `stella/license.txt` |
| Gearcoleco | `LICENSE` |
| FreeIntv | `LICENSE` |
| Genesis Plus GX | `LICENSE.txt` |
| snes9x2005 | `copyright` |
| beetle-pce-fast | `COPYING` |
| gpSP | `COPYING` |
| FinalBurn Neo | `LICENSE.md`, which points at `src/license.txt` |

## The three non-commercial cores, in their own words

Quoted rather than paraphrased, because the wording is the whole content.

### Genesis Plus GX — Master System, Game Gear, Mega Drive

From `LICENSE.txt`:

> Redistributions may not be sold, nor may they be used in a commercial
> product or activity.

Its own terms throughout — a BSD-shaped licence with that clause added, plus a
requirement that modified redistributions include complete source. Parts are
copyright the MAME team.

### snes9x2005 — SNES

Its `copyright` file is three layers, and only the innermost restricts use.
The libretro glue is MIT; it was ported from `ndssfc`, which is
GPL-2-or-later; and it is based on **Snes9x**, whose terms say:

> Permission to use, copy, modify and/or distribute Snes9x in both binary and
> source form, for non-commercial purposes, is hereby granted without fee […]
>
> Snes9x is freeware for PERSONAL USE only. Commercial users should seek
> permission of the copyright holders first.

This one is worth naming explicitly because it is easy to miss: the file opens
with a permissive MIT grant, and the restriction is 160 lines further down.

### FinalBurn Neo — arcade

From `src/license.txt`:

> You may freely use, modify, and distribute both the FB Neo source code and
> binary, however the following restrictions apply […]
>
> - You may not sell, lease, rent or otherwise seek to gain monetary profit
>   from FB Neo;
> - You must make public any changes you make to the source code;
> - You must include, verbatim, the full text of this license;
> - You may not distribute FB Neo with ROM images unless you have the legal
>   right to distribute them;
> - You may not ask for donations to support your work on any project that
>   uses the FB Neo source code.

FBNeo also carries a great deal of MAME code and states that it is therefore
subject to the MAME licence as well.

Note the fourth bullet in passing: koboy ships no ROM images of any kind, for
any system. See "Content" below.

## Content: ROMs and BIOS files

Neither is here and neither ever will be.

koboy ships **no game content**. `tests/test_dist.sh` asserts on the release
archive's own file listing that nothing with a content extension is in it.

koboy ships **no BIOS**. Two systems need one — ColecoVision wants
`colecovision.rom`, Intellivision wants `exec.bin` and `grom.bin` — and both
are the console manufacturers' copyrighted code. The owner supplies them; the
same test asserts no `.rom` or `.bin` reaches the package, and `.gitignore`
covers them so one cannot reach the repository either. Every other system
needs no BIOS or links a free replacement (PokeMini's `freebios/`, RACE's
`ngpBios.c`, gpSP's open-source BIOS).

## Rebuilding a shipped binary from source

The pinned commits above are what makes this a real offer rather than a
gesture. Given a koboy tag:

```sh
export PATH="$HOME/.cache/koboy-toolchain/arm-linaro-4.9-2014.09/bin:$PATH"
make dist
```

fetches every upstream at the commit in `scripts/pins.txt`, builds it with the
flags in its own `scripts/build-*-core.sh`, and produces the same archive. A
pin that cannot be fetched stops the build rather than quietly substituting
`master` — see `scripts/pins.sh`. `BUILD.md` has the rest.
