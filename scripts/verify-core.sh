#!/bin/sh
# Verifies a cross-built core will actually dlopen on a Kobo.
set -e
SO="${1:-dist/gambatte_libretro.so}"
READELF="${READELF:-readelf}"
[ -f "$SO" ] || { echo "FAIL: $SO missing"; exit 1; }

arch=$($READELF -h "$SO" | sed -n 's/.*Machine: *//p')
echo "$arch" | grep -qi arm || { echo "FAIL: not ARM: $arch"; exit 1; }

# C++ runtime must be linked in statically: Kobo's libstdc++ is older than any
# modern toolchain's, so a dynamic dependency on it will fail at dlopen time.
if $READELF -d "$SO" | grep -q 'NEEDED.*libstdc++'; then
    echo "FAIL: dynamic libstdc++ dependency -- rebuild with -static-libstdc++"
    exit 1
fi

allowed='libc\.so|libm\.so|libdl\.so|libpthread\.so|libgcc_s\.so'
bad=$($READELF -d "$SO" | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p' | grep -Ev "$allowed" || true)
[ -z "$bad" ] || { echo "FAIL: unexpected dependencies:"; echo "$bad"; exit 1; }

echo "PASS core dependency closure is device-safe"
$READELF -d "$SO" | sed -n 's/.*NEEDED.*\[\(.*\)\]/  needs \1/p'
