# Cross-compiling koboy and the gambatte core

koboy runs on Kobo e-readers, which have no compiler on-device and an older
glibc than any distro cross-compiler targets. Getting a binary that actually
runs there means two separate builds:

1. **koxtoolchain**: a `gcc`/`glibc` cross toolchain built to match Kobo's
   (old) C library, used to compile `koboy` itself and the `probe` binary
   (Tasks 17/18) via `make kobo`.
2. **gambatte-libretro**: the emulation core, cross-compiled with that same
   toolchain via `make core` / `scripts/build-core.sh`, and checked with
   `scripts/verify-core.sh` before it ever touches a device.

## Try the prebuilt core first (and expect it to fail)

libretro's buildbot publishes nightly prebuilt cores, including
`linux/armhf/latest/gambatte_libretro.so.zip`. It costs nothing to try it
before building a whole toolchain:

```sh
curl -LO https://buildbot.libretro.com/nightly/linux/armhf/latest/gambatte_libretro.so.zip
unzip gambatte_libretro.so.zip
sh scripts/verify-core.sh gambatte_libretro.so
```

We did exactly this. `readelf -h` shows a real ARM binary, but it fails
`verify-core.sh` for the reason the script exists:

```
FAIL: dynamic libstdc++ dependency -- rebuild with -static-libstdc++
```

`readelf -d` lists `NEEDED libstdc++.so.6`, plus `ld-linux-armhf.so.3`,
against a glibc/libstdc++ pair built for a generic modern Debian buildbot,
not Kobo's userland. It also carries no `Tag_NEON` build attribute — it was
built for `VFPv3-D16`, not `armv7-a+neon`. Either defect alone would be
enough to fail on-device; it has both. This is expected, not a bug in the
core: it saves nothing but five minutes, and confirms why a real
cross-compile is necessary rather than a shortcut around it.

## Why koxtoolchain, not a distro cross-compiler

Arch's `arm-linux-gnueabihf-gcc` (or any generic distro cross toolchain)
links against whatever glibc that distro ships, which is newer than the one
on a Kobo. A binary built that way references symbol versions
(`GLIBC_2.3x`) the device's `libc.so.6` doesn't have, and refuses to even
`dlopen`/exec. [koxtoolchain](https://github.com/koreader/koxtoolchain) (used
by KOReader for the same devices) builds a crosstool-ng toolchain against the
actual Kobo glibc/kernel headers, so the result matches the device.

### Building it

```sh
git clone https://github.com/koreader/koxtoolchain
cd koxtoolchain
./gen-tc.sh kobo
```

This bootstraps crosstool-ng itself (cloned from
`benoit-pierre/crosstool-ng`) and then runs a full `binutils` + `gcc` +
`glibc` build for the `arm-kobo-linux-gnueabihf` target tuple -- the same
prefix `scripts/build-core.sh` and the Makefile's `CROSS` default expect.
It is a from-scratch toolchain bootstrap, so budget real wall-clock time
(historically on the order of an hour or more on a modern multi-core
machine) and expect it to run unattended in the background.

Build dependencies (Arch): `base-devel curl git gperf help2man unzip wget`,
plus `ncurses`, `bison`, `flex`, `texinfo`/`gawk`. If any are missing,
install them before starting -- crosstool-ng fails deep into the build
otherwise, wasting the earlier steps.

When it finishes, the toolchain lands under `~/x-tools/arm-kobo-linux-gnueabihf/`
and its `bin/` directory should be added to `PATH` (or reference the
binaries with an absolute prefix via `CROSS=`).

## Building the gambatte core

```sh
export PATH="$HOME/x-tools/arm-kobo-linux-gnueabihf/bin:$PATH"
make core          # runs scripts/build-core.sh, then verify-core.sh
```

`scripts/build-core.sh` clones `libretro/gambatte-libretro` into
`third_party/gambatte-libretro` and builds it with:

```
CFLAGS/CXXFLAGS = -O3 -march=armv7-a -mfpu=neon -mfloat-abi=hard -fPIC
LDFLAGS         = -static-libstdc++ -static-libgcc
```

**`armv7-a -mfpu=neon`**: the 2021-23 Kobos are single-core Cortex-A9 with
NEON; the 2024 MediaTek Kobos are Cortex-A53 but run a 32-bit userland.
`armv7-a+neon` is the instruction set both understand. A core built with a
newer/narrower `-march` (e.g. tuned for A53 alone) can encode instructions
an A9 does not implement and SIGILL there -- this is the floor common
denominator, not a compromise to revisit later.

**`-static-libstdc++ -static-libgcc`**: gambatte is C++. If the C++ runtime
is linked dynamically, the resulting `.so` depends on the cross toolchain's
`libstdc++.so.6`, which is newer than the one already on the Kobo (or
entirely absent from its shared library search path). `dlopen()` on-device
then fails at load time -- exactly what happened to the prebuilt buildbot
core above. Statically linking the C++ runtime removes that dependency
entirely, at the cost of a larger `.so` (paid once, at build time).

## Verifying the result

`scripts/verify-core.sh` is the acceptance test, not a nice-to-have. It
checks:

- the ELF machine is ARM;
- there is no dynamic `NEEDED` entry for `libstdc++`;
- the full `NEEDED` list is a subset of `{libc, libm, libdl, libpthread,
  libgcc_s}` -- i.e., only what's guaranteed present on the device.

`scripts/build-core.sh` runs it automatically (using the cross `readelf`,
via `READELF="${CROSS}readelf"`) as its last step, so `make core` either
finishes with `PASS core dependency closure is device-safe` or fails loudly
before anything is copied where a later task could pick it up.

## A note for later tasks

Task 14's `RETRO_ENVIRONMENT_GET_VARIABLE` handler in `src/core.c` returns
`false` for any `gambatte_*` core option it doesn't recognise, whereas
RetroArch's own convention is `true` with `value = NULL` (meaning "no
value set, but the key is acknowledged"). This was flagged as an open
question rather than changed here. If, once a real cross-built core is
exercised on real content (Tasks 17-19), gambatte behaves oddly around core
options it queries but we don't explicitly stub, that callback is the first
place to look.
