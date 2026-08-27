#!/bin/sh
# Builds RACE (the Neo Geo Pocket / Pocket Color core) for either the dev host
# or the Kobo. Same family as build-core.sh (gambatte), build-gw-core.sh,
# build-fceumm-core.sh, build-pokemini-core.sh and build-wswan-core.sh -- read
# build-core.sh's long comment for why CC/CFLAGS/LDFLAGS are EXPORTED rather
# than passed as `make VAR=...`; RACE's Makefile uses the same
# `CFLAGS += $(fpic) $(INCFLAGS)` append pattern, so the same rule applies.
#
# RACE, not beetle-ngp, and both were BUILT AND MEASURED rather than chosen off
# a reputation. The reasons are in the order scripts/verify-core.sh, the device
# and a real collection care about:
#   - RACE is PURE C (no .cpp in the tree, final link is $(CC)), so the closure
#     is libm + libc and verify-core.sh passes with no -static-libstdc++.
#     beetle-ngp compiles five C++ translation units (sound.cpp, T6W28_Apu.cpp,
#     Blip_Buffer.cpp, Stereo_Buffer.cpp, mempatcher.cpp) and links with $(CXX),
#     so it would need the same -static-libstdc++ treatment gambatte gets --
#     passable, but a larger closure for no gain here, since koboy plays no
#     audio at all and audio is most of what those five files are.
#   - RACE is roughly 3x faster. Measured on the host over 900 frames per
#     title across all ten .ngp files in the author's collection and four .ngc:
#     0.16-0.25 ms/frame against beetle-ngp's 0.44-0.85. The device is a
#     single-core Cortex-A9 and this project's frame budget is already spent on
#     video_submit, so a 3x cheaper emulation stage is not academic.
#   - Compatibility held on everything available to try: all ten .ngp titles
#     and the four .ngc probes booted to gameplay on BOTH cores, with matching
#     geometry (160x152), matching pixel format and mean frame luma within
#     0.01. beetle-ngp remains the accuracy reference if a title is ever
#     reported broken; swapping is one line in this script and one in
#     src/config.c's table.
#
# NO BIOS SHIPS AND NONE IS NEEDED, measured the same way build-pokemini-
# core.sh's claim was: pointed at an EMPTY system directory, every title runs.
# RACE never asks for GET_SYSTEM_DIRECTORY (probe: "asked for
# system_directory: no") -- it links its own HLE BIOS (ngpBios.c). The
# boot.rom that sits beside a normal Neo Geo Pocket collection is not a
# dependency, and romlist.c's allowlist keeps it out of the browser.
#
# SAVES DO NOT GO THROUGH RETRO_MEMORY_SAVE_RAM on this system, on either
# core, and that is measured too: retro_get_memory_size(RETRO_MEMORY_SAVE_RAM)
# is 0 for all ten titles. A Neo Geo Pocket cartridge saves into its FLASH,
# and both cores write that themselves as a `<rom>.ngf` (RACE) or
# `<rom>.flash` (beetle-ngp) file in the directory the frontend answers
# GET_SAVE_DIRECTORY with. koboy already answers it with cfg.save_dir, so
# saves work -- but through a different mechanism from sram.c, which is why
# tests/test_config.c pins that answer rather than leaving it incidental.
set -e
# The upstream revision is PINNED, and scripts/pins.txt is where it is
# written down -- one table for every core, so a release can be rebuilt.
# See that file's header for why a floating `git clone --depth 1` of
# master was not good enough.
. "$(dirname "$0")/pins.sh"

TARGET="${1:-kobo}"
SRC="${SRC:-third_party/race}"

koboy_fetch_pinned race "$SRC"

make -C "$SRC" clean || true

case "$TARGET" in
    host)
        OUT="${OUT:-build/race_libretro_host.so}"
        export CC="${CC:-cc}"
        export CFLAGS="-O2 -fPIC"
        export LDFLAGS=""
        make -C "$SRC" platform=unix
        mkdir -p "$(dirname "$OUT")"
        cp "$SRC"/race_libretro.so "$OUT"
        echo "built host core: $OUT"
        ;;
    kobo)
        CROSS="${CROSS:-arm-linux-gnueabihf-}"
        OUT="${OUT:-build/race_libretro_arm.so}"
        export CC="${CROSS}gcc"
        export CFLAGS="-O3 -march=armv7-a -mfpu=neon -mfloat-abi=hard -fPIC"
        export LDFLAGS="-static-libgcc"
        make -C "$SRC" platform=unix
        mkdir -p "$(dirname "$OUT")"
        cp "$SRC"/race_libretro.so "$OUT"
        "${CROSS}strip" "$OUT" || true
        READELF="${CROSS}readelf" sh scripts/verify-core.sh "$OUT"
        ;;
    *)
        echo "usage: $0 [host|kobo]" >&2
        exit 1
        ;;
esac
