#!/bin/sh
# Builds beetle-wswan (the WonderSwan / WonderSwan Color core) for either the
# dev host or the Kobo. Fifth in the series after build-core.sh (gambatte),
# build-gw-core.sh (Game & Watch), build-fceumm-core.sh (NES) and
# build-pokemini-core.sh (Pokemon Mini); read build-core.sh's long comment
# first for why CC/CFLAGS/LDFLAGS are EXPORTED rather than passed as
# `make VAR=...`. This core's Makefile appends to CFLAGS the same way
# (`CFLAGS += $(fpic) $(INCFLAGS) $(FLAGS)` in its unix block), so a
# command-line CFLAGS would silently drop its own -I paths and the build
# would fail on a missing mednafen header.
#
# libretro/beetle-wswan is the only maintained WonderSwan core, so there was
# no choice to justify -- but it is the easy case anyway: despite being a
# mednafen port it has been fully C-ified upstream (MEASURED: `find . -name
# '*.cpp'` returns nothing, and the final link is $(CC)), so the closure is
# libm + libc and scripts/verify-core.sh passes without -static-libstdc++.
#
# NO BIOS SHIPS AND NONE IS NEEDED. MEASURED against an EMPTY system
# directory, the way build-pokemini-core.sh's claim was: the core never asks
# for GET_SYSTEM_DIRECTORY at all (probe: "asked for system_directory: no"),
# and every title tried -- mono and Color -- runs to gameplay. The boot.rom /
# boot1.rom files that sit in a normal WonderSwan collection are not a
# dependency, and romlist.c's allowlist keeps them out of the browser.
#
# NO NON-DEFAULT SWITCHES. The unix block already selects WANT_16BPP and
# FRONTEND_SUPPORTS_RGB565, which is the format koboy wants for the reason
# build-fceumm-core.sh spells out (RGB565 halves what video_submit reads per
# frame, and video_submit is this project's measured bottleneck). Verified by
# probe: `pixel_format requested: RGB565`, pitch 448 at 224x144.
set -e

TARGET="${1:-kobo}"
SRC="${SRC:-third_party/wswan}"

[ -d "$SRC" ] || git clone --depth 1 \
    https://github.com/libretro/beetle-wswan-libretro "$SRC"

make -C "$SRC" clean || true

case "$TARGET" in
    host)
        OUT="${OUT:-build/mednafen_wswan_libretro_host.so}"
        export CC="${CC:-cc}"
        export CFLAGS="-O2 -fPIC"
        export LDFLAGS=""
        make -C "$SRC" platform=unix
        mkdir -p "$(dirname "$OUT")"
        cp "$SRC"/mednafen_wswan_libretro.so "$OUT"
        echo "built host core: $OUT"
        ;;
    kobo)
        CROSS="${CROSS:-arm-linux-gnueabihf-}"
        OUT="${OUT:-build/mednafen_wswan_libretro_arm.so}"
        export CC="${CROSS}gcc"
        export CFLAGS="-O3 -march=armv7-a -mfpu=neon -mfloat-abi=hard -fPIC"
        # -static-libgcc only, no -static-libstdc++: there is no C++ here.
        export LDFLAGS="-static-libgcc"
        make -C "$SRC" platform=unix
        mkdir -p "$(dirname "$OUT")"
        cp "$SRC"/mednafen_wswan_libretro.so "$OUT"
        "${CROSS}strip" "$OUT" || true
        READELF="${CROSS}readelf" sh scripts/verify-core.sh "$OUT"
        ;;
    *)
        echo "usage: $0 [host|kobo]" >&2
        exit 1
        ;;
esac
