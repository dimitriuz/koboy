#!/bin/sh
# Builds Genesis Plus GX (the Master System / Game Gear core) for either the
# dev host or the Kobo. Same family as build-core.sh; read that script's long
# comment for why CC/CFLAGS/LDFLAGS are EXPORTED rather than passed as
# `make VAR=...` -- this Makefile appends with `CFLAGS += $(fpic) $(INCFLAGS)`
# like every other core here.
#
# ONE CORE, TWO SYSTEMS, and that is deliberate: a Master System and a Game
# Gear are the same VDP and the same Z80 with a different viewport, so every
# candidate covers both. Genesis Plus GX also covers the Mega Drive, which
# koboy does not list -- config_core_for_rom's table only routes .sms and .gg
# here, for the same reason .pc2 and .ngpc are absent from it: an extension
# the browser accepts but nobody has ever loaded is an untested claim.
#
# GPGX rather than Gearsystem or SMS Plus GX, and all three were BUILT before
# choosing:
#   - PURE C for this target. Its only .cpp is a UWP VFS shim that the unix
#     platform never compiles, so the closure is libm + libc and
#     scripts/verify-core.sh passes with no -static-libstdc++. Gearsystem is
#     90 C++ translation units and would need the gambatte treatment.
#   - ROUGHLY 2.5x FASTER than Gearsystem, measured on the host over 900
#     frames per title: 0.094-0.247 ms/frame across the author's 68 .sms and
#     53 Game Gear files, against Gearsystem's 0.227-0.415 on the same
#     titles. On a single-core Cortex-A9 whose frame budget is already spent
#     on video_submit, that is not academic.
#   - SMS Plus GX (`libretro/smsplus-gx`, which does exist) is DISQUALIFIED,
#     not merely slower: it SEGFAULTS inside retro_load_game on the first
#     title tried, calling a null log function pointer it kept from a
#     GET_LOG_INTERFACE that the frontend refused. koboy's src/core.c refuses
#     that query too, so it would crash on the device in exactly the same
#     way. Verified under gdb.
#   Compatibility held everywhere it could be tried: all 68 .sms and all 53
#   Game Gear files (38 .gg + 15 .GG -- the collection really is mixed case)
#   load and run.
#
# NO BIOS SHIPS AND NONE IS NEEDED. The core DOES ask for
# GET_SYSTEM_DIRECTORY, unlike most in this project, but only to look for the
# optional Master System boot ROM behind its own `genesis_plus_gx_bios`
# option -- which defaults to disabled, and koboy answers no core options at
# all, so the default stands. Measured against an EMPTY system directory:
# every one of the 121 titles above runs.
#
# NO NON-DEFAULT SWITCHES, and two of the defaults are worth naming because
# they are exactly the settings that would have hurt:
#   - `genesis_plus_gx_lcd_filter` (a Game Gear LCD-ghosting simulation)
#     defaults to disabled. It is the gambatte_mix_frames problem in another
#     coat: it would make every pixel change every frame, destroying
#     dirty-rect tracking and layering fake ghosting on real e-ink ghosting.
#   - `genesis_plus_gx_gg_extra` defaults to disabled, which is what keeps a
#     Game Gear frame at its true 160x144 instead of exposing the SMS-sized
#     frame behind it.
#
# SAVES DO GO THROUGH RETRO_MEMORY_SAVE_RAM here -- the sram.c path, the
# first system since the Game Boy to use it -- and 16 of the author's 121
# titles have a battery (Phantasy Star, Ys, Golden Axe Warrior, the Shining
# Force Gaidens, ...). BUT THE SIZE THIS CORE REPORTS IS NOT CONSTANT: at
# load time retro_get_memory_size(SAVE_RAM) answers 0x10000, the buffer's
# real size, and once emulation is running it instead answers "index of the
# highest byte that is not 0xFF, plus one" (libretro/libretro.c) -- which for
# a real save is anything from 285 to 32160 bytes, and 0 for an untouched
# one. koboy pins the length at ROM-load time for exactly this reason; see
# core_sram in src/core.c and the test in tests/test_core.c.
set -e
# The upstream revision is PINNED, and scripts/pins.txt is where it is
# written down -- one table for every core, so a release can be rebuilt.
# See that file's header for why a floating `git clone --depth 1` of
# master was not good enough.
. "$(dirname "$0")/pins.sh"

TARGET="${1:-kobo}"
SRC="${SRC:-third_party/gpgx}"

koboy_fetch_pinned gpgx "$SRC"

make -C "$SRC" -f Makefile.libretro clean || true

case "$TARGET" in
    host)
        OUT="${OUT:-build/genesis_plus_gx_libretro_host.so}"
        export CC="${CC:-cc}"
        export CFLAGS="-O2 -fPIC"
        export LDFLAGS=""
        make -C "$SRC" -f Makefile.libretro platform=unix
        mkdir -p "$(dirname "$OUT")"
        cp "$SRC"/genesis_plus_gx_libretro.so "$OUT"
        echo "built host core: $OUT"
        ;;
    kobo)
        CROSS="${CROSS:-arm-linux-gnueabihf-}"
        OUT="${OUT:-build/genesis_plus_gx_libretro_arm.so}"
        export CC="${CROSS}gcc"
        export CFLAGS="-O3 -march=armv7-a -mfpu=neon -mfloat-abi=hard -fPIC"
        # -static-libgcc only, no -static-libstdc++: there is no C++ here.
        export LDFLAGS="-static-libgcc"
        make -C "$SRC" -f Makefile.libretro platform=unix
        mkdir -p "$(dirname "$OUT")"
        cp "$SRC"/genesis_plus_gx_libretro.so "$OUT"
        "${CROSS}strip" "$OUT" || true
        READELF="${CROSS}readelf" sh scripts/verify-core.sh "$OUT"
        ;;
    *)
        echo "usage: $0 [host|kobo]" >&2
        exit 1
        ;;
esac
