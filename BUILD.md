# Building koboy

Two builds, and they have almost nothing in common.

- **The host build** runs on any Linux box with SDL2 and a modern compiler. It
  is the whole pipeline behind an SDL window instead of an e-ink panel, and it
  is where the tests run. Five minutes, no toolchain.
- **The device build** cross-compiles for ARM against **glibc 2.19**, which no
  distribution has shipped for a decade. This is the hard part and most of
  this file.

Start with the host build. Almost nothing that matters is device-specific,
which is deliberate — `src/platform_if.h` is the seam and both sides of it are
built and tested on every run.

---

## The host build

```sh
make host        # -> build/koboy (SDL) + build/stub_core.so
make test        # 26 test binaries, 6010 checks
```

You need a C compiler, GNU make and SDL2 (`pkg-config --cflags sdl2` must
work). Nothing else.

```sh
bash tests/smoke_host.sh     # end-to-end: load a ROM, run frames, save, exit
bash tests/test_dist.sh      # packaging contract + launcher safety
bash scripts/verify-core.sh dist/<core>.so    # a built core's dependency closure
```

**`tests/test_dist.sh` splits in two and says so loudly.** Its packaging half
needs the cross toolchain and is skipped without it; its launcher half needs
only a shell and always runs. That split exists because the two used to be one
`set -e` block, so on a machine without the toolchain the script died silently
having checked nothing.

A host build can `dlopen` a host-built core, so you can exercise the real
pipeline on real content without a device. Every core script takes a `host`
target:

```sh
sh scripts/build-gw-core.sh host        # -> build/gw_libretro_host.so
./build/koboy --core build/gw_libretro_host.so --rom yourgame.mgw --frames 300
```

`--frames N` runs N frames and exits, which is how most measurement in
`TESTED.md` was taken. `--ui-script` replays a synthetic sequence of taps and
key presses into the ROM browser and the in-game menu, which is how the UI is
tested without hands.

---

## The cross toolchain, and it is fragile

### The constraint

The device runs **glibc 2.19**. Anything linking a newer versioned glibc
symbol fails to load there, whatever else is right about the binary. This was
established on the device rather than inferred:

| Fact | Value |
|---|---|
| Device glibc | 2.19 |
| A misleading symlink | `/lib/libc.so.6 -> libc-2.11.1.so` — the filename survived an upgrade; ignore it |
| A **working** device binary (`/usr/bin/fbink`) needs | `libm.so.6` and `libc.so.6`, nothing else |
| Build attribute | `Tag_Advanced_SIMD_arch: NEONv1` |

The 2021-23 Kobos are single-core Cortex-A9 with NEON; the 2024 MediaTek ones
are Cortex-A53 running a 32-bit userland. **`armv7-a` + NEON is the floor both
understand**, and it is a floor rather than a compromise to revisit: a core
built for A53 alone can emit instructions an A9 does not implement, and it
SIGILLs there.

### The one toolchain that fits, and where it comes from now

**Linaro `4.9-2014.09`**, `arm-linux-gnueabihf`. It bundles
`libc-2.19-2014.08.so` — glibc 2.19 exactly, with armv7-a/NEON/hardfloat.

**Linaro decommissioned `releases.linaro.org`.** It redirects to a contact
page. The tarball currently comes from a Wayback Machine capture:

```sh
curl -L -o gcc-linaro-4.9-2014.09.tar.xz \
  "http://web.archive.org/web/20150803153714id_/http://releases.linaro.org/14.09/components/toolchain/binaries/gcc-linaro-arm-linux-gnueabihf-4.9-2014.09_linux.tar.xz"
mkdir -p ~/.cache/koboy-toolchain
tar xf gcc-linaro-4.9-2014.09.tar.xz -C ~/.cache/koboy-toolchain/
export PATH="$HOME/.cache/koboy-toolchain/arm-linaro-4.9-2014.09/bin:$PATH"
```

**Say plainly what this is: the project's build depends on a third-party
archive of a decommissioned server.** That is a real single point of failure
and nobody should discover it the hard way.

**If that URL dies**, in rough order of effort:

1. **Find another Wayback capture.** Use the CDX API and check the status
   code before trusting a snapshot — many captures under `releases.linaro.org`
   are archived *redirects*, not the file:
   ```sh
   curl -s 'http://web.archive.org/cdx/search/cdx?url=releases.linaro.org/14.09/components/toolchain/binaries/*&output=json' | grep -i 'gnueabihf.*tar.xz'
   ```
   A `statuscode` of `200` on a non-redirect capture is what you want.
2. **Look for a mirror.** Linaro's 14.09 release was widely rehosted by
   embedded-Linux distributions and by Android build environments of that era.
   Verify any candidate against the test below before trusting it — the point
   is the glibc version, not the filename.
3. **Use a different prebuilt toolchain that targets glibc ≤ 2.19.** This was
   searched once and came up nearly empty: Debian and Ubuntu of that era have
   no clean "cross to an old release" packages, and Bootlin's archive's oldest
   `armv7-eabihf` glibc build is **2.24** (2017), already too new. If you find
   one, the symbol test below is the acceptance criterion.
4. **Build one with crosstool-NG.** See the koxtoolchain note below for what
   went wrong the first time; a fresh crosstool-NG configuration targeting
   glibc 2.19 (not 2.15) may well work where that did not.

### Why koxtoolchain was abandoned — read this before trying it

[koxtoolchain](https://github.com/koreader/koxtoolchain) is the obvious
answer: KOReader uses it for these exact devices, and it builds a crosstool-NG
toolchain against Kobo's own glibc and kernel headers.

It did not work here. Its `kobo` target's **glibc-2.15 build hung
indefinitely** inside glibc's own `./configure`, right after `running
configure fragment for ports/sysdeps/arm/elf`, printing one repeated
progress-spinner line for hours. Confirmed a real hang rather than slow
progress — `tail -4000 build.log | sort -u | wc -l` returned close to 1, one
distinct line repeated thousands of times. A 2012-era libc's autoconf idioms
do not get on with a modern host's autoconf and shell. Two independent
attempts, no workaround found.

Two things made those attempts worse but did not cause the hang, and are
worth avoiding anyway: an interactive `ct-ng oldconfig` blocking forever on a
closed stdin, and logging into a tmpfs `/tmp` that filled with a 4+ GB log.

**And note it targets glibc 2.15, which is *older* than the device's actual
2.19 floor** — so even a successful build would have been aiming below the
target. That is the reason a prebuilt toolchain was worth looking for at all.

**Do not build under `/tmp`.** It is commonly RAM-backed; a runaway log or a
large build tree there eats memory, not disk.

### Proving a candidate toolchain before trusting it with a build

Offline, thirty seconds, and it rules a toolchain in or out:

```sh
export PATH="/path/to/toolchain/bin:$PATH"
printf 'int main(void){return 0;}' > /tmp/hello.c
arm-linux-gnueabihf-gcc -O2 -march=armv7-a -mfpu=neon -mfloat-abi=hard -o /tmp/hello.arm /tmp/hello.c
readelf --dyn-syms /tmp/hello.arm | grep -o 'GLIBC_[0-9.]*' | sort -Vu
readelf -A /tmp/hello.arm | grep SIMD
```

The Linaro toolchain gives `GLIBC_2.4` and `Tag_Advanced_SIMD_arch: NEONv1`.
Anything referencing a symbol above `GLIBC_2.19` is disqualified.

That is a strong signal, not proof — the device is the final judge. With SSH
access: `scp /tmp/hello.arm root@<device>:/tmp/ && ssh root@<device>
/tmp/hello.arm`.

For reference, the highest symbol versions in the real artefacts:

| Binary | Highest GLIBC symbol |
|---|---|
| `gambatte_libretro.so` | `GLIBC_2.7` |
| `koboy-arm` | `GLIBC_2.17` — `clock_gettime`, which moved from `librt` into `libc` at 2.17, and is why koboy needs no `-lrt` |

---

## Building for the device

```sh
export PATH="$HOME/.cache/koboy-toolchain/arm-linaro-4.9-2014.09/bin:$PATH"

make kobo        # build/koboy-arm + build/koboy-probe-arm (builds FBInk first)
make dist        # -> dist/koboy-0.5.0.zip, everything under .adds/koboy/
make probe-dist  # -> dist/koboy-probe-0.5.0.zip, the probe alone
```

**`make kobo` needs that `PATH` and it is not there by default.** `CROSS`
defaults to `arm-linux-gnueabihf-`; override it if your toolchain uses another
prefix.

`make dist` is the full release: the ARM binary, the probe, fourteen cores,
the launcher, the two menu-integration files, `koboy.ini`, the documentation
and a generated `roms/README.txt`. It unpacks to 61 MB and zips to 18.6.

`make probe-dist` builds *only* `koboy-probe` — no emulator, no cores, so no
multi-minute core builds. That is the point of it: someone with a Kobo nobody
has tried can characterise their device and contribute a row to `TESTED.md`
without building the emulator at all. See `docs/probe-readme.md`.

### Upstream revisions are pinned

`scripts/pins.txt` is the only place an upstream commit is written down: one
row per project, `name url commit`. Every `scripts/build-*-core.sh` and
`scripts/build-fbink.sh` fetches through `koboy_fetch_pinned` in
`scripts/pins.sh`, which does `git fetch --depth 1 origin <sha>` into an empty
repository — one commit, no history, and pinned, which a `git clone --depth 1`
cannot be because clone takes a branch or a tag and not a commit id.

**It fails loudly.** A pin that cannot be fetched stops the build with the pin
named and an explicit instruction not to work around it by cloning `master`.
That matters because the failure it replaces was invisible: a floating clone
always succeeds and nothing tells you a different core got built.

**These pins are the builds `TESTED.md` measured.** Moving one invalidates
that file's numbers for that system, and the pin table's header says so.

To move a pin deliberately: edit the row, delete the tree under
`third_party/` and the built `.so` under `dist/`, rebuild, and re-measure.

### The cores

Fourteen cores for fifteen systems — Genesis Plus GX answers for two.

```sh
make core        # gambatte alone
make core-gw core-nes core-pm core-ws core-ngp core-a26 core-col \
     core-int core-sms core-snes core-pce core-gba core-fbneo
```

**Every core target is a real file target, not `.PHONY`.** A cross-build is
minutes of work and `make dist` must not repeat it on every invocation, so a
core is rebuilt only when its `.so` is missing. **To force a rebuild, delete
the `.so`.**

Each script's header carries the decisions behind that core, and several of
them are load-bearing. A sample of what is in there:

- **Four upstream names are traps.** `libretro/gw`, `libretro/stella2014`,
  `libretro/gearcoleco` and `libretro/beetle-pce-fast` all 404 — the real
  repositories are `libretro/gw-libretro`,
  `libretro/stella2014-libretro`, `drhelius/Gearcoleco` and
  `libretro/beetle-pce-fast-LIBRETRO`. FBNeo is the fifth and worst, because
  **both** `libretro/FBNeo` and `finalburnneo/FBNeo` exist and are real FBNeo
  repositories; only the libretro fork has `src/burner/libretro` at all.
- **Four cores were chosen by measurement, not reputation**, with the losing
  candidates cross-built and benchmarked: RACE over beetle-ngp (~3× faster and
  pure C), Genesis Plus GX over Gearsystem (~2.5× over 121 titles), snes9x2005
  over snes9x2010 and snes9x (1.36×/1.58× faster), gpSP over mGBA and vba-next
  (1.21× and 2.01×). Each script's header has the table.
- **`libretro/stella` is rejected for one reason**: it forces `-std=c++17`,
  which Linaro GCC 4.9.2 rejects outright.
- **fceumm is built with `WANT_32BPP=0`**, which gets RGB565 instead of
  XRGB8888 and halves what the pixel pipeline reads per frame.
- **FBNeo compiles 7-Zip support OUT** for the device: `lib7z` does not build
  against glibc 2.19's headers.

### `make dist` cannot ship content, and this is enforced

`tests/test_dist.sh` asserts on the **archive's own file listing** that
nothing with a ROM or BIOS extension is in it. This stopped being hypothetical
when ColecoVision and Intellivision were added: those two BIOS files sit on
the author's disk while the build scripts run, and a packaging step that
picked one up would look exactly like a working build.

The same test asserts every core is present *by name*. That list is not
decoration — it was added after it had already drifted twice, including once
where gpSP was a prerequisite of the `dist` rule but had no `cp`, producing a
package that built, zipped and passed everything while missing a whole
system's core. **When you add a core to the Makefile, add it there in the same
commit.**

### C++ in the cores, and none in koboy

koboy itself is C with **no dependency beyond libc, libm and libdl**. Several
cores are C++, and they are built with `-static-libstdc++ -static-libgcc`,
because the device has no `libstdc++.so.6` matching what a modern cross
toolchain links against. `scripts/verify-core.sh` is the acceptance test and
every build script runs it as its last step:

- the ELF machine is ARM;
- no dynamic `NEEDED` entry for `libstdc++`;
- the full `NEEDED` list is a subset of `{libc, libm, libdl, libpthread,
  libgcc_s, ld-linux-armhf}`, matched by anchored whole-name comparison.

**`ld-linux-armhf.so.3` is on that allowlist deliberately.** It is the dynamic
loader, not a library — every dynamically linked armhf binary needs it,
including the device's own working `fbink`. It appears because
`-static-libstdc++` pulls in libstdc++'s exception-handling globals, which use
TLS. Traced with a link map, and confirmed by `dlopen(RTLD_NOW)` of the real
core **on the physical device**: `RTLD_NOW` resolves every dynamic symbol
immediately, so a genuinely unresolvable dependency would have failed there
and loudly.

Nine of the fourteen cores are pure C and need `libm` + `libc` or less. The
WonderSwan and Neo Geo Pocket cores need `libc` **alone**. FBNeo is the only
one that pulls in `libpthread`.

### Two build gotchas worth knowing before you hit them

**Never pass `CFLAGS` to a core's `make` on the command line.** GNU Make
command-line variables cannot be appended to by the makefile's own `+=`, even
with no `override` involved — verified with a three-line test makefile before
the diagnosis was trusted. libretro makefiles all do
`CXXFLAGS += $(INCFLAGS)`, so a command-line `CXXFLAGS` silently makes that a
no-op and the build fails with `gambatte.h: No such file or directory`, which
looks like a missing submodule and is not. Every script here **exports** them
as environment variables instead, which have lower precedence than a
makefile's own assignments.

**FBInk needs one patch under this toolchain.** Its `Makefile` adds
`-fno-semantic-interposition` unconditionally for non-Clang compilers. That is
GCC ≥ 5 only, and GCC treats an unknown `-f` option as an **error**.
`scripts/build-fbink.sh` gates that line on the compiler actually accepting
the option, then re-greps to confirm the edit landed. It is applied by the
script rather than committed as a patch file because `third_party/fbink/` is a
gitignored clone with nothing to carry a patch against. The edit is
idempotent.

FBInk is built `MINIMAL` plus exactly three toggles — `DRAW` and `BITMAP` for
the on-panel fatal-error screen, `INPUT` for `fbink_input_scan`. Everything
else (OpenType, image decoding, Unifont) is dead weight, because koboy blits
its own gray8 straight into the mapped framebuffer. Note that a `MINIMAL`
build still *defines* every public symbol with a `return -ENOSYS` body, so an
`nm` check cannot catch a dropped toggle; the script asserts on the actual
`-DFBINK_WITH_*` defines in the recorded compiler invocation instead
(`build/fbink-build.log`).

---

## Testing culture

This is not optional and it is the one rule to read before writing a test.

**Three tests in v1 passed whether or not the code they guarded existed.** One
took four review rounds to fix. So: **after writing any safety or regression
test, break the thing it guards and confirm the test fails.** Record the
mutant and its output in the commit message.

Two failure modes worth knowing, because they are classes rather than
accidents:

- **A test that can only fail via undefined behaviour is not a test.** A
  sentinel guard band cannot detect an unclamped `memset` whose length
  underflows to near `SIZE_MAX`, because where glibc actually writes for such a
  length is an implementation detail that on x86-64 lands *inside* the buffer,
  not in the guard. The fix was to stop observing UB and assert the clamped
  values directly.
- **A grep for a word matches the comment explaining the word.** Two
  assertions in `tests/test_dist.sh` were satisfied by a 60-line comment that
  named all three variables the gate was supposed to test, and by an unrelated
  line elsewhere in the file. Every grep there now runs over a
  comment-stripped *slice* of the script and matches the shell construct
  rather than a word in it.

And the corollary from the v1 endgame: a first-run deadlock survived twenty
per-task reviews because the scripted-run branch skips calibration —
**every automated test took the one path that could not reach the bug**. When
a code path exists only for scripted runs, ask what it is hiding.

---

## Deploying to a device

`docs/device-workflow.md` is the full version, including the traps. The one
that has already cost a reboot:

**Never restart the reader software from a process that did not inherit its
environment.** It rewrites `/mnt/onboard/.kobo/version` with a placeholder
serial, and that file is how every e-ink tool on the device identifies it —
KOReader included. Only a reboot repairs it. `scripts/koboy.sh` refuses to
touch anything unless `PLATFORM`, `PRODUCT` and `NICKEL_HOME` are all set, and
the gate exists because this happened once, by hand.

There is a narrower, genuinely safe way to exercise a build over SSH:
`./koboy --frames N` run directly, bypassing `koboy.sh`. It never stops the
reader software and never touches the input grab, so it exercises the core,
the save path and the whole video pipeline — and exercises none of the
takeover, the touch d-pad or the in-game menu. Most of `TESTED.md`'s device
measurements were taken that way.

And a measurement caveat that is not a formality: **benchmarking this device
back to back gives numbers that climb.** A first pass with no gaps read up to
2.4× high and kept rising through the batch; an isolated re-run of the same
binary on the same ROM came back at the first row's figure. Ten seconds of
idle between runs, every time.

---

## Reference

| File | What it holds |
|---|---|
| `docs/cross-compiling.md` | The toolchain in full, and every dead end |
| `docs/device-workflow.md` | Deploying, launching, diagnosing, and the traps |
| `docs/probe-readme.md` | `koboy-probe`, for characterising an untried device |
| `INTERNALS.md` | Architecture, and the decisions the hardware forced |
| `TESTED.md` | Every measurement, and everything not established |
| `LICENSES.md` | koboy is GPL-3; every core's own terms |
| `scripts/pins.txt` | The upstream commit of every shipped dependency |
