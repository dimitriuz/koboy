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
make -C "$SRC" platform=unix \
    CC="${CROSS}gcc" CXX="${CROSS}g++" \
    CFLAGS="-O3 -march=armv7-a -mfpu=neon -mfloat-abi=hard -fPIC" \
    CXXFLAGS="-O3 -march=armv7-a -mfpu=neon -mfloat-abi=hard -fPIC" \
    LDFLAGS="-static-libstdc++ -static-libgcc"

cp "$SRC"/gambatte_libretro.so "$OUT"
"${CROSS}strip" "$OUT" || true
READELF="${CROSS}readelf" sh scripts/verify-core.sh "$OUT"
