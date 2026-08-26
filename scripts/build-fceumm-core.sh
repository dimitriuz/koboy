#!/bin/sh
# Builds libretro-fceumm (the NES core) for either the dev host or the Kobo.
# Third in the series after build-core.sh (gambatte) and build-gw-core.sh
# (Game & Watch); read build-core.sh's long comment first for why
# CC/CFLAGS/LDFLAGS are EXPORTED rather than passed as `make VAR=...`.
# fceumm's Makefile.common uses the same `CFLAGS += $(INCFLAGS)` append
# pattern gambatte's and gw's do, so a command-line CFLAGS would silently
# drop the core's own -Isrc/... and the build would fail on a missing header.
#
# fceumm, not QuickNES or Nestopia, and the reasons are in that order:
#   - it is PURE C (no .cpp anywhere in the tree, and the final link is
#     $(CC)), so the dependency ceiling scripts/verify-core.sh enforces is
#     met without -static-libstdc++ and without the ld-linux-armhf.so.3 that
#     gambatte's static libstdc++ drags in;
#   - it is the performance-oriented NES core. The device is a single-core
#     Cortex-A9; Nestopia is the cycle-accurate one and costs for it;
#   - it covers a normal collection: `valid_extensions: fds|nes|unf|unif`
#     and several hundred mappers, against QuickNES's ~25.
#
# THREE non-default switches, all passed as `make VAR=` -- which is correct
# HERE and not a contradiction of the CFLAGS rule above: these are plain
# selector variables the makefile assigns with `:=` and never appends to, so
# the command line is the only way to override the `platform=unix` block's
# own `WANT_32BPP := 1`.
#
#   WANT_32BPP=0  MEASURED: with it at the unix default of 1 the core asks
#                 for XRGB8888. koboy handles that (video.c has the path),
#                 but RGB565 halves the bytes video_submit reads per frame
#                 -- 120KB instead of 240KB at 256x240 -- and video_submit
#                 is this project's measured bottleneck (CLAUDE.md). It is
#                 also the same LUT path gambatte has been profiled on.
#                 Verified by probe: `pixel_format requested: RGB565`,
#                 pitch 512, and a correct Zelda title screen.
#   HAVE_HDPACK=0 HD packs are replacement PNG/WebP artwork loaded from a
#                 directory koboy will never create. Enabled it drags rpng,
#                 rwebp, rvp8 and rvorbis into the .so for a feature that
#                 cannot fire here.
#   HAVE_NTSC=0   The NTSC composite filter is a per-pixel cost on a device
#                 with none to spare, and koboy quantises to four greys --
#                 there is nothing for a colour-artifact filter to say.
set -e

TARGET="${1:-kobo}"
SRC="${SRC:-third_party/fceumm}"
SWITCHES="WANT_32BPP=0 HAVE_HDPACK=0 HAVE_NTSC=0"

[ -d "$SRC" ] || git clone --depth 1 \
    https://github.com/libretro/libretro-fceumm "$SRC"

make -C "$SRC" -f Makefile.libretro clean || true

case "$TARGET" in
    host)
        OUT="${OUT:-build/fceumm_libretro_host.so}"
        export CC="${CC:-cc}"
        export CFLAGS="-O2 -fPIC"
        export LDFLAGS=""
        make -C "$SRC" -f Makefile.libretro platform=unix $SWITCHES
        mkdir -p "$(dirname "$OUT")"
        cp "$SRC"/fceumm_libretro.so "$OUT"
        echo "built host core: $OUT"
        ;;
    kobo)
        CROSS="${CROSS:-arm-linux-gnueabihf-}"
        OUT="${OUT:-build/fceumm_libretro_arm.so}"
        export CC="${CROSS}gcc"
        export CFLAGS="-O3 -march=armv7-a -mfpu=neon -mfloat-abi=hard -fPIC"
        # -static-libgcc only, no -static-libstdc++: there is no C++ here.
        export LDFLAGS="-static-libgcc"
        make -C "$SRC" -f Makefile.libretro platform=unix $SWITCHES
        mkdir -p "$(dirname "$OUT")"
        cp "$SRC"/fceumm_libretro.so "$OUT"
        "${CROSS}strip" "$OUT" || true
        READELF="${CROSS}readelf" sh scripts/verify-core.sh "$OUT"
        ;;
    *)
        echo "usage: $0 [host|kobo]" >&2
        exit 1
        ;;
esac
