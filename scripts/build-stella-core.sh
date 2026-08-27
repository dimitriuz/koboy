#!/bin/sh
# Builds stella2014 (the Atari 2600 core) for either the dev host or the Kobo.
# Seventh in the series after build-core.sh (gambatte), build-gw-core.sh,
# build-fceumm-core.sh, build-pokemini-core.sh, build-wswan-core.sh and
# build-race-core.sh; read build-core.sh's long comment first for why
# CC/CXX/CFLAGS/CXXFLAGS/LDFLAGS are EXPORTED rather than passed as
# `make VAR=...`. This core's Makefile.common appends with
# `CXXFLAGS += $(INCFLAGS)` exactly like gambatte's, so a command-line
# CXXFLAGS would silently drop its own -I paths.
#
# THE REPOSITORY NAME IS NOT `libretro/stella2014`, which 404s -- it is
# `libretro/stella2014-libretro`. The same trap `libretro/gw` was.
#
# stella2014 AND NOT `libretro/stella` (which does exist, and is the current
# Stella 7.x), and the reason is not a preference: modern Stella's own
# libretro Makefile hard-codes `CXXFLAGS += -std=c++17`, and the device's
# toolchain REFUSES that flag outright --
#
#   arm-linux-gnueabihf-g++: error: unrecognized command line option '-std=c++17'
#
# -- because Linaro 4.9.2 predates the switch by three years. That is not a
# porting job, it is a different compiler, and the toolchain is pinned to 4.9
# by the device's glibc 2.19 (docs/cross-compiling.md). So this is the only
# Atari 2600 core this project can build at all, and it happens to also be the
# one written for weak hardware: Stella 3.9.3, no debugger, no C++17.
#
# NO BIOS SHIPS AND NONE IS NEEDED, measured the same way every claim in this
# series is: the core never asks for GET_SYSTEM_DIRECTORY at all (probe:
# "sysdir asked no"), and all 82 .a26 files in the author's collection load
# and run to gameplay against an EMPTY system directory. A 2600 cartridge is
# the whole machine above the TIA; there is no boot ROM to want.
#
# NO NON-DEFAULT SWITCHES. The core already asks for RGB565 (probe: "fmt
# RGB565", pitch 320 at 160 wide), which is the format video_submit wants for
# the reason build-fceumm-core.sh spells out. Every core option it exposes --
# the blend/ghosting filters especially -- is left at its default, and the
# blend filters MUST stay there: `blend_frames_null_*` is the default, and
# anything else makes every pixel change every frame, which is the exact
# gambatte_mix_frames problem src/core.c already documents (it destroys
# dirty-rect tracking and layers fake ghosting on real e-ink ghosting).
#
# SAVES DO NOT GO THROUGH RETRO_MEMORY_SAVE_RAM here, and that is measured
# too: retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) is 0 for all 82 titles.
# A 2600 cartridge has no battery; the AtariVox/SaveKey that a handful of
# later titles support is a controller-port peripheral this core does not
# emulate. So a .srm is never written for this system, and koboy's save-state
# path is the only save it has.
set -e
# The upstream revision is PINNED, and scripts/pins.txt is where it is
# written down -- one table for every core, so a release can be rebuilt.
# See that file's header for why a floating `git clone --depth 1` of
# master was not good enough.
. "$(dirname "$0")/pins.sh"

TARGET="${1:-kobo}"
SRC="${SRC:-third_party/stella2014}"

koboy_fetch_pinned stella2014 "$SRC"

make -C "$SRC" clean || true

case "$TARGET" in
    host)
        OUT="${OUT:-build/stella2014_libretro_host.so}"
        export CC="${CC:-cc}"
        export CXX="${CXX:-c++}"
        export CFLAGS="-O2 -fPIC"
        export CXXFLAGS="-O2 -fPIC"
        export LDFLAGS=""
        make -C "$SRC" platform=unix
        mkdir -p "$(dirname "$OUT")"
        cp "$SRC"/stella2014_libretro.so "$OUT"
        echo "built host core: $OUT"
        ;;
    kobo)
        CROSS="${CROSS:-arm-linux-gnueabihf-}"
        OUT="${OUT:-build/stella2014_libretro_arm.so}"
        export CC="${CROSS}gcc"
        export CXX="${CROSS}g++"
        export CFLAGS="-O3 -march=armv7-a -mfpu=neon -mfloat-abi=hard -fPIC"
        export CXXFLAGS="-O3 -march=armv7-a -mfpu=neon -mfloat-abi=hard -fPIC"
        # -static-libstdc++ because this one IS C++ (.cxx throughout, final
        # link with $(CXX)) -- the gambatte case, not the pure-C one. See
        # scripts/verify-core.sh for why the ld-linux-armhf.so.3 that comes
        # with it is expected and permitted.
        export LDFLAGS="-static-libstdc++ -static-libgcc"
        make -C "$SRC" platform=unix
        mkdir -p "$(dirname "$OUT")"
        cp "$SRC"/stella2014_libretro.so "$OUT"
        "${CROSS}strip" "$OUT" || true
        READELF="${CROSS}readelf" sh scripts/verify-core.sh "$OUT"
        ;;
    *)
        echo "usage: $0 [host|kobo]" >&2
        exit 1
        ;;
esac
