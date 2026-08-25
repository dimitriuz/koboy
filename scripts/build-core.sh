#!/bin/sh
# Cross-builds gambatte-libretro for Kobo. Requires koxtoolchain on PATH.
# The device is a single-core Cortex-A9 with NEON, and the 2024 MediaTek Kobos
# are Cortex-A53 running a 32-bit userland, so armv7-a+neon is the common
# denominator that runs on both. A core tuned for A53 will SIGILL on an A9.
set -e
CROSS="${CROSS:-arm-kobo-linux-gnueabihf-}"
SRC="${SRC:-third_party/gambatte-libretro}"
OUT="${OUT:-dist/gambatte_libretro.so}"

[ -d "$SRC" ] || git clone --depth 1 \
    https://github.com/libretro/gambatte-libretro "$SRC"

mkdir -p "$(dirname "$OUT")"
make -C "$SRC" clean || true
# CC/CXX/CFLAGS/CXXFLAGS/LDFLAGS are exported into the environment rather than
# passed as `make VAR=...` command-line arguments. GNU Make command-line
# variables cannot be appended to by the makefile's own `+=` (verified: even
# without an `override` directive, a later `CXXFLAGS += ...` in the makefile
# is silently dropped for a variable set on the command line). gambatte-libretro's
# Makefile.common adds its own required `-Ilibgambatte/include` etc. via
# `CXXFLAGS += $(INCFLAGS)`; passing CFLAGS/CXXFLAGS as command-line args -- as
# opposed to environment variables -- makes that append a no-op and the build
# fails with "gambatte.h: No such file or directory". As environment
# variables the flag values below reach the compiler unchanged, and the
# Makefile's own include paths still get appended on top.
export CC="${CROSS}gcc"
export CXX="${CROSS}g++"
export CFLAGS="-O3 -march=armv7-a -mfpu=neon -mfloat-abi=hard -fPIC"
export CXXFLAGS="-O3 -march=armv7-a -mfpu=neon -mfloat-abi=hard -fPIC"
export LDFLAGS="-static-libstdc++ -static-libgcc"
make -C "$SRC" platform=unix

cp "$SRC"/gambatte_libretro.so "$OUT"
"${CROSS}strip" "$OUT" || true
READELF="${CROSS}readelf" sh scripts/verify-core.sh "$OUT"
