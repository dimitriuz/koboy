#!/bin/sh
# Builds libretro/PokeMini (the Pokemon Mini core) for either the dev host or
# the Kobo. Same family as build-core.sh (gambatte), build-gw-core.sh (Game &
# Watch) and build-fceumm-core.sh (NES) -- read build-core.sh's long comment
# for why CC/CFLAGS/LDFLAGS are EXPORTED rather than passed as `make VAR=...`;
# PokeMini's Makefile.libretro appends to CFLAGS the same way, so the same
# rule applies.
#
# libretro/PokeMini is the only Pokemon Mini core there is, so there was no
# choice to justify -- but it happens to be the easy case: pure C (no .cpp in
# the tree, final link is $(CC)), so the closure is libm + libc and
# scripts/verify-core.sh passes without -static-libstdc++.
#
# NO BIOS SHIPS AND NONE IS NEEDED. The core links its own free BIOS
# (third_party/pokemini/freebios/, GPL'd with the core) and falls back to it
# when the system directory has no dumped one. MEASURED, not read off a
# README: pointed at an EMPTY system directory, `Pokemon Tetris (Europe)` runs
# to its language-select screen and animates. koboy therefore needs no system-
# directory concept for this core, and the `[BIOS] Nintendo Pokemon Mini
# (World).min` file sitting in a normal collection is not a dependency.
set -e

TARGET="${1:-kobo}"
SRC="${SRC:-third_party/pokemini}"

[ -d "$SRC" ] || git clone --depth 1 \
    https://github.com/libretro/PokeMini "$SRC"

make -C "$SRC" -f Makefile.libretro clean || true

case "$TARGET" in
    host)
        OUT="${OUT:-build/pokemini_libretro_host.so}"
        export CC="${CC:-cc}"
        export CFLAGS="-O2 -fPIC"
        export LDFLAGS=""
        make -C "$SRC" -f Makefile.libretro platform=unix
        mkdir -p "$(dirname "$OUT")"
        cp "$SRC"/pokemini_libretro.so "$OUT"
        echo "built host core: $OUT"
        ;;
    kobo)
        CROSS="${CROSS:-arm-linux-gnueabihf-}"
        OUT="${OUT:-build/pokemini_libretro_arm.so}"
        export CC="${CROSS}gcc"
        export CFLAGS="-O3 -march=armv7-a -mfpu=neon -mfloat-abi=hard -fPIC"
        export LDFLAGS="-static-libgcc"
        make -C "$SRC" -f Makefile.libretro platform=unix
        mkdir -p "$(dirname "$OUT")"
        cp "$SRC"/pokemini_libretro.so "$OUT"
        "${CROSS}strip" "$OUT" || true
        READELF="${CROSS}readelf" sh scripts/verify-core.sh "$OUT"
        ;;
    *)
        echo "usage: $0 [host|kobo]" >&2
        exit 1
        ;;
esac
