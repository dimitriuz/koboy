#!/bin/sh
# Builds gw-libretro (the Game & Watch core) for either the dev host or the
# Kobo. Companion to build-core.sh, which cross-builds gambatte and is the
# template this script follows -- see that script's long comment for why
# CC/CFLAGS/LDFLAGS must be exported into the environment rather than passed
# as `make VAR=...`: gw-libretro's build/Makefile.common uses the identical
# `CFLAGS += ...` append pattern (e.g. `CFLAGS += $(fpic) $(DEFINES)`), so a
# command-line CFLAGS would silently lose those appends the same way
# gambatte's did. Exporting instead of `make VAR=` avoids that.
#
# Unlike gambatte, gw-libretro is pure C (its own changelog, 1.4.0: "Removed
# the constcast.cpp aberration, the core is now pure C"; confirmed here --
# there are no .cpp files in third_party/gw and the final link uses $(CC),
# not $(CXX)). So there is no libstdc++ concern on this core at all, on
# either target: no CXX/CXXFLAGS to export, and -static-libstdc++ is not
# needed in LDFLAGS (only -static-libgcc, for the same TLS-related reason
# noted in verify-core.sh: even pure-C code can pull in libgcc_s helpers,
# and ld-linux-armhf.so.3 is expected and allowed).
#
# Two targets, selected by $1 (default "kobo" to match build-core.sh's
# always-cross-builds behaviour):
#
#   host  x86_64, using the system cc. This is the useful one while the
#         device is offline: koboy's SDL platform (`make host`) can dlopen
#         a host-built core and exercise the whole pipeline on the desktop.
#         Output does NOT go under dist/, which is the device package --
#         see OUT default below.
#   kobo  ARM cross-build with the same flags build-core.sh uses for
#         gambatte (-march=armv7-a -mfpu=neon -mfloat-abi=hard, glibc 2.19
#         via the Linaro 4.9-2014.09 toolchain), verified against
#         scripts/verify-core.sh.
set -e

TARGET="${1:-kobo}"
SRC="${SRC:-third_party/gw}"

[ -d "$SRC" ] || git clone --depth 1 \
    https://github.com/libretro/gw-libretro "$SRC"

make -C "$SRC" -f Makefile.libretro clean || true

case "$TARGET" in
    host)
        OUT="${OUT:-build/gw_libretro_host.so}"
        export CC="${CC:-cc}"
        export CFLAGS="-O2 -fPIC"
        export LDFLAGS=""
        make -C "$SRC" -f Makefile.libretro platform=unix
        mkdir -p "$(dirname "$OUT")"
        cp "$SRC"/gw_libretro.so "$OUT"
        echo "built host core: $OUT"
        ;;
    kobo)
        CROSS="${CROSS:-arm-linux-gnueabihf-}"
        OUT="${OUT:-build/gw_libretro_arm.so}"
        # CROSS-built, not shipped in dist/ yet: this core is exploratory
        # (task 7 measurement work), and koboy's game-rect pipeline is still
        # hardcoded to 160x144 (see docs/superpowers/specs/2026-08-26-koboy-
        # multi-system-design.md #3) so nothing can load it yet. Once the
        # resolution-agnostic pipeline and a ROM-browser core selector land,
        # this belongs in dist/ alongside gambatte_libretro.so and needs a
        # Makefile target (CORE_GW_SO / `core-gw`) mirroring $(CORE_SO) --
        # proposed rather than added here, since the Makefile is out of
        # scope for this task.
        export CC="${CROSS}gcc"
        export CFLAGS="-O3 -march=armv7-a -mfpu=neon -mfloat-abi=hard -fPIC"
        export LDFLAGS="-static-libgcc"
        make -C "$SRC" -f Makefile.libretro platform=unix
        mkdir -p "$(dirname "$OUT")"
        cp "$SRC"/gw_libretro.so "$OUT"
        "${CROSS}strip" "$OUT" || true
        READELF="${CROSS}readelf" sh scripts/verify-core.sh "$OUT"
        ;;
    *)
        echo "usage: $0 [host|kobo]" >&2
        exit 1
        ;;
esac
