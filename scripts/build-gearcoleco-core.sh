#!/bin/sh
# Builds Gearcoleco (the ColecoVision core) for either the dev host or the
# Kobo. Same family as build-core.sh (gambatte) and the five scripts after
# it; read build-core.sh's long comment first for why CC/CXX/CFLAGS/CXXFLAGS/
# LDFLAGS are EXPORTED rather than passed as `make VAR=...`.
#
# THE REPOSITORY IS `drhelius/Gearcoleco`, NOT `libretro/gearcoleco`, which
# 404s -- the same trap `libretro/gw` was, and the second time in this project
# that a plausible libretro/ name did not exist. Its libretro port lives in
# platforms/libretro/, which is why $SRC points there rather than at the
# repository root.
#
# THIS CORE NEEDS A BIOS AND CANNOT RUN WITHOUT ONE, which makes it the first
# in koboy. Established by RENDERING A FRAME, not by reading: against an empty
# system directory the core loads the cartridge, reports 256x192, runs 300
# frames without complaint -- and every one of those frames is a static bitmap
# that says NO BIOS. src/GearcolecoCore.cpp:1184 chooses kNoBiosImage
# (src/no_bios.h) over the real framebuffer whenever the BIOS is absent, so
# "it loaded" and "it works" are not the same question here and only a
# rendered frame tells them apart. With the BIOS present the same title runs
# to its option screen.
#
# That is real hardware, not a core quirk: a ColecoVision cartridge boots into
# the console's own 8 KB ROM, which draws the title card and the "PRESS BUTTON
# ON KEYPAD" prompt every game relies on. It is Coleco's copyrighted code and
# it is NOT OURS TO SHIP, so it is not in dist/ and tests/test_dist.sh asserts
# that no .rom ever gets into the package. The owner supplies it:
#
#   put colecovision.rom (8192 bytes) beside the koboy binary, in
#   .adds/koboy/ -- that is the directory koboy answers GET_SYSTEM_DIRECTORY
#   with (src/core.c, resolved from koboy.ini's save_dir). `coleco.rom` also
#   works; the core tries colecovision.rom first and falls back to it
#   (platforms/libretro/libretro.cpp, load_bootroms).
#
# The author's own copy is at batocera/BIOS/Machines/COL - ColecoVision/
# coleco.rom -- 8192 bytes, sha1 45bedc4cbdeac66c7df59e9e599195c778d86a92,
# the standard dump. Note that the emudeck bios/colecovision directory is a
# symlink to an EMPTY folder, so looking there and concluding "the owner has
# no ColecoVision BIOS" would have been wrong.
#
# `libretro/blueMSX-libretro` was the alternative and is worse for this
# purpose, for a reason that has nothing to do with emulation quality: it is a
# multi-machine emulator that reads its machine definitions from disk, so it
# wants blueMSX's whole `Machines/` and `Databases/` asset tree under the
# system directory (libretro.c:1196-1197), not one 8 KB file. A bigger
# dependency for the owner to satisfy, for a system Gearcoleco already runs.
#
# NO NON-DEFAULT SWITCHES. The core already asks for RGB565. Its options are
# left alone -- including gearcoleco_no_sprite_limit, which sounds like an
# improvement and is a behaviour change (ColecoVision sprite flicker is what
# several games use to signal danger).
#
# SAVES DO NOT GO THROUGH RETRO_MEMORY_SAVE_RAM here: measured,
# retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) is 0 for all 28 .col files in
# the author's collection. A stock ColecoVision cartridge has no battery.
set -e
# The upstream revision is PINNED, and scripts/pins.txt is where it is
# written down -- one table for every core, so a release can be rebuilt.
# See that file's header for why a floating `git clone --depth 1` of
# master was not good enough.
. "$(dirname "$0")/pins.sh"

TARGET="${1:-kobo}"
SRC="${SRC:-third_party/gearcoleco}"
# The libretro port is a subdirectory of the emulator's own repository, unlike
# every other core in this series.
LR="$SRC/platforms/libretro"

koboy_fetch_pinned gearcoleco "$SRC"

make -C "$LR" clean || true

case "$TARGET" in
    host)
        OUT="${OUT:-build/gearcoleco_libretro_host.so}"
        export CC="${CC:-cc}"
        export CXX="${CXX:-c++}"
        export CFLAGS="-O2 -fPIC"
        export CXXFLAGS="-O2 -fPIC"
        export LDFLAGS=""
        make -C "$LR" platform=unix
        mkdir -p "$(dirname "$OUT")"
        cp "$LR"/gearcoleco_libretro.so "$OUT"
        echo "built host core: $OUT"
        ;;
    kobo)
        CROSS="${CROSS:-arm-linux-gnueabihf-}"
        OUT="${OUT:-build/gearcoleco_libretro_arm.so}"
        export CC="${CROSS}gcc"
        export CXX="${CROSS}g++"
        export CFLAGS="-O3 -march=armv7-a -mfpu=neon -mfloat-abi=hard -fPIC"
        export CXXFLAGS="-O3 -march=armv7-a -mfpu=neon -mfloat-abi=hard -fPIC"
        # C++ throughout, so -static-libstdc++ like gambatte and stella2014.
        export LDFLAGS="-static-libstdc++ -static-libgcc"
        make -C "$LR" platform=unix
        mkdir -p "$(dirname "$OUT")"
        cp "$LR"/gearcoleco_libretro.so "$OUT"
        "${CROSS}strip" "$OUT" || true
        READELF="${CROSS}readelf" sh scripts/verify-core.sh "$OUT"
        ;;
    *)
        echo "usage: $0 [host|kobo]" >&2
        exit 1
        ;;
esac
