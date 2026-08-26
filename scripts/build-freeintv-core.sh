#!/bin/sh
# Builds FreeIntv (the Intellivision core) for either the dev host or the
# Kobo. Same family as build-core.sh; read that script's long comment for why
# CC/CFLAGS/LDFLAGS are EXPORTED rather than passed as `make VAR=...` --
# FreeIntv's Makefile.common uses the same `CFLAGS += $(INCFLAGS)` append.
#
# `libretro/FreeIntv`, and there was no choice to make: it is the only
# libretro Intellivision core. It is also the easy build -- pure C (no .cpp in
# the tree, final link with $(CC)), so no -static-libstdc++.
#
# THIS CORE NEEDS TWO BIOS FILES AND CANNOT RUN WITHOUT THEM. Without them the
# CP1610 has no reset vector and the core logs "HALT!" every frame; with them
# every title in the author's 26-file collection boots to gameplay. What it
# wants, in the directory koboy answers GET_SYSTEM_DIRECTORY with (src/core.c
# -- that is .adds/koboy/, resolved from koboy.ini's save_dir):
#
#   exec.bin   8192 bytes   the Executive ROM
#   grom.bin   2048 bytes   the Graphics ROM (the character set)
#
# Neither is ours to ship, so neither is in dist/ and tests/test_dist.sh
# asserts no .bin or .rom reaches the package.
#
# WHICH MiSTer BOOT ROM IS WHICH, settled by CONTENT rather than by guessing
# from size -- boot1.rom and boot2.rom are both 2048 bytes, so size alone
# cannot tell them apart and picking the wrong one produces a machine that
# runs and draws garbage. The author's Intellivision directory carries
# boot0-boot3.rom in MiSTer's naming; `cmp` against a batocera BIOS tree that
# names its files properly gives byte-for-byte identity:
#
#   boot0.rom  8192 B  == exec.bin   sha1 5a65b922b562cb1f57dab51b73151283f0e20c7a
#   boot1.rom  2048 B  == grom.bin   sha1 f9608bb4ad1cfe3640d02844c7ad8e0bcd974917
#   boot2.rom  2048 B  -- NOT a dependency (the Intellivoice speech ROM)
#   boot3.rom 24576 B  -- NOT a dependency (the ECS expansion ROM)
#
# Confirmed by running: with boot0/boot1 copied to exec.bin/grom.bin, Atlantis
# draws its title screen with legible text -- and that text comes out of GROM,
# so a wrong grom.bin could not produce it. romlist.c's allowlist keeps all
# four boot roms out of the browser either way.
#
# XRGB8888, NOT RGB565, and it is not negotiable: src/libretro.c:1433 sets the
# format unconditionally with no core option behind it. That is the only core
# koboy ships that costs video_submit four bytes per source pixel instead of
# two; video.c has handled both formats since v1 (video_xrgb8888_to_gray), so
# it is a cost, not a blocker. 352x224 fixed, base == max, so the game rect
# never re-fits mid-session.
#
# SAVES DO NOT GO THROUGH RETRO_MEMORY_SAVE_RAM: measured 0 for all 26 titles.
# An Intellivision cartridge has no battery.
set -e

TARGET="${1:-kobo}"
SRC="${SRC:-third_party/freeintv}"

[ -d "$SRC" ] || git clone --depth 1 \
    https://github.com/libretro/FreeIntv "$SRC"

make -C "$SRC" clean || true

case "$TARGET" in
    host)
        OUT="${OUT:-build/freeintv_libretro_host.so}"
        export CC="${CC:-cc}"
        export CFLAGS="-O2 -fPIC"
        export LDFLAGS=""
        make -C "$SRC" platform=unix
        mkdir -p "$(dirname "$OUT")"
        cp "$SRC"/freeintv_libretro.so "$OUT"
        echo "built host core: $OUT"
        ;;
    kobo)
        CROSS="${CROSS:-arm-linux-gnueabihf-}"
        OUT="${OUT:-build/freeintv_libretro_arm.so}"
        export CC="${CROSS}gcc"
        export CFLAGS="-O3 -march=armv7-a -mfpu=neon -mfloat-abi=hard -fPIC"
        # -static-libgcc only, no -static-libstdc++: there is no C++ here.
        export LDFLAGS="-static-libgcc"
        make -C "$SRC" platform=unix
        mkdir -p "$(dirname "$OUT")"
        cp "$SRC"/freeintv_libretro.so "$OUT"
        "${CROSS}strip" "$OUT" || true
        READELF="${CROSS}readelf" sh scripts/verify-core.sh "$OUT"
        ;;
    *)
        echo "usage: $0 [host|kobo]" >&2
        exit 1
        ;;
esac
