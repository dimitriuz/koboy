# Cross-compiling koboy and the gambatte core

koboy runs on Kobo e-readers, which have no compiler on-device and an older
glibc than any distro cross-compiler targets. Getting a binary that actually
runs there means two separate builds:

1. **A cross toolchain** targeting the device's actual glibc, used to
   compile `koboy` itself and the `probe` binary (Tasks 17/18) via
   `make kobo`.
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

## Ground truth from the device

Before settling on a toolchain, we got SSH access to a real Kobo
(`ssh root@<device>`, busybox `ash`, no `ldd`) and checked what it actually
needs, rather than guessing from koxtoolchain's target config:

| Fact | Value |
|---|---|
| Device glibc | `2.19` (`GNU C Library (crosstool-NG 1.24.0.103_75d7525) ... version 2.19`) |
| Misleading symlink | `/lib/libc.so.6 -> libc-2.11.1.so` — filename kept across an upgrade, ignore it |
| A **working** Kobo ARM binary (`/usr/bin/fbink`) needs | only `libm.so.6` and `libc.so.6` |
| Build attribute | `Tag_Advanced_SIMD_arch: NEONv1` |

This puts the real ceiling at **glibc 2.19**, not whatever a given
toolchain happens to target -- anything linking against a newer versioned
glibc symbol will fail to `dlopen`/exec on the device regardless of `-march`
or `-mfpu`.

## What we tried for the toolchain, and what actually worked

**koxtoolchain** (`https://github.com/koreader/koxtoolchain`, used by
KOReader for the same devices) is designed to solve exactly this: it builds
a crosstool-ng toolchain against Kobo's own glibc/kernel headers rather than
whatever a distro ships. In practice, on this host, its `kobo` target's
`glibc-2.15` build hung indefinitely inside glibc's own `./configure` --
specifically right after `running configure fragment for
ports/sysdeps/arm/elf`, printing one repeated progress-spinner line for
hours (confirmed: `tail -4000 build.log | sort -u | wc -l` returned close to
1, i.e. one distinct line repeated thousands of times -- a real hang, not
slow progress). A 2012-era libc's autoconf idioms do not get on with a
2026 host's autoconf/shell. Two build attempts confirmed this
independently; we did not find a workaround and moved on rather than
force it. (Two operational mistakes made the first two attempts worse than
the underlying hang -- an interactive `ct-ng oldconfig` blocking forever on
a closed stdin, and logging to a tmpfs `/tmp` that filled with a 4+ GB log
-- but neither of those caused the actual `./configure` hang; they only
made it harder to see.)

Since koxtoolchain's own build target (`glibc 2.15`) was in any case *older*
than the device's actual floor of `2.19`, we looked for a **prebuilt**
toolchain targeting `armv7-a` hardfloat with a glibc at or below `2.19`
instead of bootstrapping one. Ubuntu/Debian cross-compiler packages of that
era don't fit cleanly (no distinct "cross to an old release" packages), and
Bootlin's toolchain archive's oldest `armv7-eabihf` glibc build is `2.24`
(from 2017) -- already too new. **Linaro's own archived toolchain
releases** turned out to be exactly what's needed: their `4.9-2014.09`
`arm-linux-gnueabihf` release bundles `libc-2.19-2014.08.so` -- glibc 2.19,
precisely at the device's ceiling, with `armv7-a`/NEON/hardfloat support.

Linaro decommissioned `releases.linaro.org` (it now redirects to a contact
page), but the actual tarball is still recoverable from the Wayback Machine,
which holds a real (HTTP 200, non-redirect) capture of the binary:

```sh
curl -L -o gcc-linaro-4.9-2014.09.tar.xz \
  "http://web.archive.org/web/20150803153714id_/http://releases.linaro.org/14.09/components/toolchain/binaries/gcc-linaro-arm-linux-gnueabihf-4.9-2014.09_linux.tar.xz"
tar xf gcc-linaro-4.9-2014.09.tar.xz -C /home/you/.cache/koboy-toolchain/
```

(Use the Wayback CDX API, `web.archive.org/cdx/search/cdx?url=...&output=json`,
to confirm a given snapshot's `statuscode` is `200` before trusting it --
many captures under `releases.linaro.org` are just archived redirects, not
the file itself.)

**Do not build under `/tmp`.** It is commonly a RAM-backed tmpfs; a runaway
log or a large build tree there consumes system memory, not disk. Use a
real-disk path (e.g. `~/.cache/koboy-toolchain/`).

### Proving the toolchain works, before trusting it with gambatte

A symbol-version check, entirely offline, rules a candidate toolchain in or
out immediately:

```sh
export PATH="/path/to/arm-linaro-4.9-2014.09/bin:$PATH"
arm-linux-gnueabihf-gcc -O2 -march=armv7-a -mfpu=neon -mfloat-abi=hard -o hello.arm hello.c
readelf --dyn-syms hello.arm | grep -o 'GLIBC_[0-9.]*' | sort -Vu
```

We measured `GLIBC_2.4` as the highest version referenced -- comfortably
under the device's `2.19` ceiling. `readelf -A hello.arm` also confirmed
`Tag_Advanced_SIMD_arch: NEONv1`, matching the device's own attribute
exactly.

That alone is a strong signal but not proof; the device itself is the
final judge. With the Kobo reachable over SSH:

```sh
scp hello.arm root@<device>:/tmp/ && ssh root@<device> /tmp/hello.arm
```

We ran exactly this and it printed the test string with exit code 0 on the
real device -- definitive confirmation the toolchain's output actually
runs there, not just that it looks plausible on paper.

The same check against the actual shipped `dist/gambatte_libretro.so`
(rather than the trivial `hello.c`) shows a slightly larger, still
comfortably-safe set:

```sh
$ readelf --dyn-syms dist/gambatte_libretro.so | grep -o 'GLIBC_[0-9.]*' | sort -Vu
GLIBC_2.4
GLIBC_2.7
```

Highest is `GLIBC_2.7`, still well under the device's `2.19` ceiling.

## Building the gambatte core

```sh
export PATH="/path/to/arm-linaro-4.9-2014.09/bin:$PATH"
export CROSS=arm-linux-gnueabihf-
make core          # runs scripts/build-core.sh, then verify-core.sh
```

(`CROSS` now defaults to `arm-linux-gnueabihf-`, the Linaro toolchain's
actual binary prefix, in both the `Makefile` and `scripts/build-fbink.sh`.
It used to default to koxtoolchain's `arm-kobo-linux-gnueabihf-` tuple, which
no compiler on this host provides, since that toolchain build never
completed. `scripts/build-core.sh` now defaults to the same prefix, and `make core`
forwards the Makefile's `CROSS` to it explicitly, so overriding `CROSS` in one
place is enough. Override it if your toolchain lives under a different
prefix.)

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

**A real GNU Make gotcha `scripts/build-core.sh` works around:** passing
`CFLAGS`/`CXXFLAGS` to `make` as command-line arguments (`make CFLAGS=...`)
freezes those variables for the rest of the run -- the makefile's own
`CXXFLAGS += $(INCFLAGS)` (in gambatte-libretro's `Makefile.common`, which
supplies `-Ilibgambatte/include` etc.) becomes a silent no-op, even with no
`override` directive involved. We verified this with a three-line test
makefile before trusting the diagnosis. The build then fails with
`gambatte.h: No such file or directory`, which looks like a missing
submodule but isn't one. The fix -- already applied in
`scripts/build-core.sh` -- is to `export` `CC`/`CXX`/`CFLAGS`/`CXXFLAGS`/
`LDFLAGS` as environment variables instead of passing them as `make`
arguments: environment variables have *lower* precedence than a makefile's
own assignments, so the required flag values still reach the compiler
unchanged, while the makefile's own `+=` can still extend them.

## Verifying the result

`scripts/verify-core.sh` is the acceptance test, not a nice-to-have. It
checks:

- the ELF machine is ARM;
- there is no dynamic `NEEDED` entry for `libstdc++`;
- the full `NEEDED` list is a subset of `{libc, libm, libdl, libpthread,
  libgcc_s, ld-linux-armhf}` -- i.e., only what's guaranteed present on
  the device.

`scripts/build-core.sh` runs it automatically (using the cross `readelf`,
via `READELF="${CROSS}readelf"`) as its last step, so `make core` either
finishes with `PASS core dependency closure is device-safe` or fails loudly
before anything is copied where a later task could pick it up.

**Why `ld-linux-armhf.so.3` is on the allowlist.** The core built with the
Linaro 4.9-2014.09 toolchain carries a `NEEDED` entry for
`ld-linux-armhf.so.3` -- the dynamic loader/interpreter itself, not a
library the way `libc`/`libm` are. This traces to `libstdc++.a(eh_globals.o)`
being pulled in by `-static-libstdc++` (C++'s per-thread exception-handling
globals, `__cxa_eh_globals`, implemented via a `__tls_get_addr`-based TLS
variable on targets with native TLS, which ARM has) -- confirmed by
generating a link map (`-Wl,-Map=...`) and by testing that a trivial empty
`.cpp` `.so` built with identical flags does *not* pick up this NEEDED
entry, so it is specific to gambatte's/libretro-common's C++ code pulling
in exception-support object modules, not an artifact of the flags
themselves.

It is permitted rather than treated as a hazard because it is categorically
different from what the allowlist exists to catch (a glibc version mismatch,
or a dynamic `libstdc++`): every dynamically-linked armhf binary needs the
loader, including the device's own working `/usr/bin/fbink` -- a device
that couldn't provide it couldn't run anything dynamically linked at all.
And this was not taken on faith: an actual `dlopen(RTLD_NOW)` of the real
built core **on the physical Kobo** succeeded completely --
`RTLD_NOW` forces immediate resolution of every dynamic symbol including
`__tls_get_addr`, so a genuinely unresolvable dependency would have failed
loudly right there, not silently deferred:

```
$ ssh root@<device> './load_core.arm ./gambatte_libretro.so'
DLOPEN_OK
retro_api_version = 1
retro_get_system_info present: yes
DLCLOSE_OK
EXIT=0
```

With `ld-linux-armhf.so.3` added to the allowlist (with this reasoning as
an inline comment in the script), `scripts/verify-core.sh` passes cleanly
against the real cross-built core:

```
$ READELF=arm-linux-gnueabihf-readelf sh scripts/verify-core.sh dist/gambatte_libretro.so
PASS core dependency closure is device-safe
  needs libm.so.6
  needs libc.so.6
  needs ld-linux-armhf.so.3
```

## A note for later tasks

Task 14's `RETRO_ENVIRONMENT_GET_VARIABLE` handler in `src/core.c` returns
`false` for any `gambatte_*` core option it doesn't recognise, whereas
RetroArch's own convention is `true` with `value = NULL` (meaning "no
value set, but the key is acknowledged"). This was flagged as an open
question rather than changed here. If, once a real cross-built core is
exercised on real content (Tasks 17-19), gambatte behaves oddly around core
options it queries but we don't explicitly stub, that callback is the first
place to look.

## Building FBInk

`koboy` links FBInk statically. FBInk is what abstracts the e-ink refresh
ioctls (mxcfb on this device, sunxi and MTK elsewhere), Kobo's device
identification table, and the `/dev/input/event*` classification that would
otherwise have to be reimplemented and kept current per device.

```sh
export PATH="/path/to/arm-linaro-4.9-2014.09/bin:$PATH"
make fbink        # or just `make kobo`, which depends on it
```

`scripts/build-fbink.sh` clones `NiLuJe/FBInk` (with submodules -- the
vendored `i2c-tools` is a hard dependency of a `KOBO=true` build) into
`third_party/fbink` and builds `Release/libfbink.a` with:

```
CROSS_TC=arm-linux-gnueabihf
KOBO=true MINIMAL=true DRAW=true BITMAP=true INPUT=true
CFLAGS=-O2 -fomit-frame-pointer -pipe -march=armv7-a -mfpu=neon -mfloat-abi=hard
```

**Why static.** The device does carry working FBInk binaries, but not
necessarily a `libfbink.so` whose ABI matches the header we compiled
against, and a missing or older shared library is a startup failure on a
device with no terminal to report it on. Static costs ~190 KB of archive,
paid once.

**Why `MINIMAL` plus three toggles.** `DRAW` gives `fbink_cls`, `BITMAP`
gives `fbink_print` -- between them, the on-panel fatal-error screen, which
is the only text koboy ever asks FBInk to draw. `INPUT` gives
`fbink_input_scan`. Everything else FBInk can do (OpenType, image decoding,
QImageScale, Unifont) is dead weight here, because koboy blits its own
gray8 straight into the mmap'ed framebuffer.

Note that a `MINIMAL` build still *defines* every public symbol when a
feature is off -- the bodies just become `return -ENOSYS`. So an `nm` check
cannot catch a dropped feature toggle, and `build-fbink.sh` asserts on the
actual `-DFBINK_WITH_*` defines in the recorded compiler invocation instead
(`build/fbink-build.log`).

### The one patch the old toolchain needs

FBInk's `Makefile` adds `-fno-semantic-interposition` unconditionally for
any non-Clang compiler. That option is GCC >= 5 only, and GCC treats an
unknown `-f` option as an *error*, not a warning. With Linaro GCC 4.9.2 --
the only toolchain we found that targets glibc 2.19 -- the first casualty is
the vendored `i2c-tools` build:

```
arm-linux-gnueabihf-gcc: error: unrecognized command line option
    '-fno-semantic-interposition'
make[1]: *** [lib/Module.mk:75: lib/smbus.ao] Error 1
```

`scripts/build-fbink.sh` gates that one line on the compiler actually
accepting the option, then re-greps to confirm the edit landed. It is applied
by the script rather than committed as a patch file because
`third_party/fbink/` is a gitignored clone, so there is nothing to carry a
patch against. The edit is idempotent.

### Result

```
$ readelf -h build/koboy-arm | grep Machine
  Machine:                           ARM
$ readelf -d build/koboy-arm | grep NEEDED
 0x00000001 (NEEDED)  Shared library: [libdl.so.2]
 0x00000001 (NEEDED)  Shared library: [libm.so.6]
 0x00000001 (NEEDED)  Shared library: [libc.so.6]
$ readelf --dyn-syms build/koboy-arm | grep -o 'GLIBC_[0-9.]*' | sort -Vu
GLIBC_2.4
GLIBC_2.7
GLIBC_2.17
$ readelf -A build/koboy-arm | grep SIMD
  Tag_Advanced_SIMD_arch: NEONv1
```

`GLIBC_2.17` is new relative to the core's `2.4`/`2.7` and is worth naming:
it is `clock_gettime`, which moved from `librt` into `libc` at glibc 2.17.
Still comfortably under the device's 2.19 floor, and it is why `koboy-arm`
needs no `-lrt`.
