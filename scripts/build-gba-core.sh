#!/bin/sh
# Builds gpSP (the Game Boy Advance core) for either the dev host or the Kobo.
# Same family as build-core.sh; read that script's long comment for why
# CC/CFLAGS/LDFLAGS are EXPORTED rather than passed as `make VAR=...`.
#
# THE V1 SPEC RULED GBA OUT OF SCOPE ON CPU GROUNDS, exactly as it ruled out
# SNES. SNES was re-measured and shipped; this script exists because that made
# the GBA judgement suspect too. See TESTED.md and the task report for the
# numbers -- and read them before deciding this core is replaceable.
#
# GPSP RATHER THAN MGBA OR VBA-NEXT, AND ALL THREE WERE BUILT AND MEASURED
# before choosing. Host, x86_64, 600 frames after a 2400-frame warmup, mean us
# per retro_run, scripts/corebench.c --mash (so these are GAMEPLAY frames, not
# a title screen -- see corebench's state_cb for why that distinction matters
# far more on this system than on any previous one):
#
#              title                        gpSP   mGBA   vba-next
#              Advance Wars 2               377.4  310.2   791.1
#              Fire Emblem                  319.3  339.1   580.2
#              Final Fantasy Tactics Adv.   493.9  632.7   934.2
#              Golden Sun                   250.0  336.4   480.3
#              Metroid Fusion               220.1  310.6   463.0
#              Castlevania: Aria of Sorrow  328.4  453.5   659.4
#              Super Mario Advance 2        335.9  410.9   951.5
#              Astro Boy: Omega Factor      458.5  570.9   748.7
#              -----------------------------------------------------
#              sum                         2783.5 3364.3  5608.4
#
#   - FASTEST overall by 1.21x over mGBA and 2.01x over vba-next -- AND THAT
#     TABLE UNDERSTATES IT, which is the important part. gpSP was built here
#     with HAVE_DYNAREC=0, an interpreter, because its dynarec targets ARM (and
#     x86_32, and MIPS) and this host is x86_64. The ARM build below turns the
#     dynarec ON, and neither of the other two has one at all. So the host
#     column is gpSP's worst case and the other two cores' only case.
#   - THE BRIEF'S PREMISE THAT VBA-NEXT IS "FASTER THAN MGBA" IS FALSE at
#     these revisions, and not marginally: vba-next is the SLOWEST of the
#     three on all eight titles, by 1.67x against mGBA in aggregate. That is
#     received wisdom from a decade ago, when mGBA was new; mGBA 0.11 has had
#     ten years of work since. Recorded because it is the kind of thing that
#     gets re-derived from folklore rather than measured.
#   - NO C++ STANDARD PROBLEM, checked before committing to it rather than
#     after -- this is what made libretro/stella unbuildable here. gpSP is C
#     except for video.cc and cpu.cc, which it compiles with -std=c++11
#     -fno-rtti -fno-exceptions and LINKS WITH GCC on purpose (its own
#     Makefile says so): the closure is libm + libc, and verify-core.sh passes
#     with no -static-libstdc++ at all.
#   - NO BIOS FILE IS NEEDED. gpSP carries an open-source GBA BIOS
#     (bios/open_gba_bios.bin, assembled into the core through bios_data.S) and
#     falls back to it whenever no official image is supplied. koboy supplies
#     none, so every title runs on that one. This is the reason a GBA needs no
#     entry in roms/README.txt's BIOS section, unlike ColecoVision and
#     Intellivision.
#
# WHAT THE DYNAREC NEEDS FROM THE KERNEL, and why this is not a formality.
# gpSP's ARM dynarec writes ARM instructions into a buffer and then executes
# them, so it needs memory that is both writable and executable. With
# MMAP_JIT_CACHE (set below via HAVE_DYNAREC, see the Makefile's unix branch)
# it asks mmap for PROT_READ|PROT_WRITE|PROT_EXEC, and a hardened kernel can
# refuse. Refusal is not a build-time failure and not a crash: the core would
# quietly fall back and be measured as fast when it is not. So the ARM build
# is verified ON THE DEVICE with corebench, never inferred from the host.
#
# CPU_ARCH AND HAVE_DYNAREC ARE PASSED ON THE MAKE COMMAND LINE, and that is
# load-bearing rather than belt-and-braces. gpSP's Makefile decides the
# dynarec's target architecture from `uname -a` OF THE BUILD HOST: under
# `platform=unix` on an x86_64 machine it sets CPU_ARCH := x86_32 and
# HAVE_DYNAREC := 1, which for a CROSS build would assemble x86 stubs into an
# ARM shared object. A command-line assignment is the only kind make lets
# override an in-makefile `:=`, which is why these two are not exported like
# everything else here.
set -e
# The upstream revision is PINNED, and scripts/pins.txt is where it is
# written down -- one table for every core, so a release can be rebuilt.
# See that file's header for why a floating `git clone --depth 1` of
# master was not good enough.
. "$(dirname "$0")/pins.sh"

TARGET="${1:-kobo}"
SRC="${SRC:-third_party/gpsp}"

koboy_fetch_pinned gpsp "$SRC"

make -C "$SRC" clean || true

case "$TARGET" in
    host)
        OUT="${OUT:-build/gpsp_libretro_host.so}"
        export CC="${CC:-cc}"
        export CXX="${CXX:-c++}"
        export CFLAGS="-O2"
        export LDFLAGS=""
        # HAVE_DYNAREC=0 on the host on purpose: see the table above. An
        # x86_64 host cannot run gpSP's x86_32 dynarec anyway, and leaving the
        # Makefile to guess produces a core that mixes 32-bit stubs into a
        # 64-bit object.
        make -C "$SRC" platform=unix HAVE_DYNAREC=0 CPU_ARCH=none
        mkdir -p "$(dirname "$OUT")"
        cp "$SRC"/gpsp_libretro.so "$OUT"
        echo "built host core: $OUT"
        ;;
    kobo)
        CROSS="${CROSS:-arm-linux-gnueabihf-}"
        OUT="${OUT:-build/gpsp_libretro_arm.so}"
        export CC="${CROSS}gcc"
        export CXX="${CROSS}g++"
        export AR="${CROSS}ar"
        # -mtune, not -mcpu: the device is a Freescale i.MX6 SoloLite, a
        # single Cortex-A9 (CPU part 0xc09), but -march stays at the baseline
        # armv7-a the rest of this project builds against.
        # -marm and NOT thumb, and this one is not cosmetic: the dynarec
        # emits ARM instructions and returns into them, so the stubs in
        # arm/arm_stub.S and the code around them have to be ARM too.
        export CFLAGS="-marm -march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=hard"
        # -static-libgcc only, no -static-libstdc++: gpSP links with gcc and
        # uses no libstdc++ symbols, which verify-core.sh below re-checks
        # rather than takes on trust.
        export LDFLAGS="-static-libgcc"
        make -C "$SRC" platform=unix CPU_ARCH=arm HAVE_DYNAREC=1
        mkdir -p "$(dirname "$OUT")"
        cp "$SRC"/gpsp_libretro.so "$OUT"
        "${CROSS}strip" "$OUT" || true
        READELF="${CROSS}readelf" sh scripts/verify-core.sh "$OUT"
        ;;
    *)
        echo "usage: $0 [host|kobo]" >&2
        exit 1
        ;;
esac
