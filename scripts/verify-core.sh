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

# ld-linux-armhf.so.3 (the dynamic loader/interpreter itself) is permitted:
# every dynamically-linked armhf binary needs it, including the device's own
# working /usr/bin/fbink -- a device that couldn't provide it couldn't run
# anything dynamically linked at all, so it is not a hazard the way a
# mismatched glibc or a dynamic libstdc++ would be. It shows up here because
# -static-libstdc++ pulls in libstdc++.a(eh_globals.o) (C++'s per-thread
# __cxa_eh_globals, TLS-based via __tls_get_addr on targets with native TLS,
# which ARM has); confirmed by a link map and by the fact that a trivial
# empty .cpp built with identical flags does *not* need it, so it's specific
# to gambatte's C++ code, not an artifact of the flags themselves. Validated
# by an actual on-device dlopen(RTLD_NOW) of the real cross-built core
# (DLOPEN_OK, retro_api_version=1, DLCLOSE_OK) -- stronger evidence than any
# static check, which is why it's allowed here rather than treated as a fail.
# #10: this used to be `grep -Ev` against an unanchored alternation, which
# matches by SUBSTRING -- a library literally named e.g. reallibc.so.6 would
# satisfy `libc\.so` as a substring of itself and silently pass, because
# grep -E never required the match to consume the whole field. `case` matches
# the entire candidate string against each glob, so nothing can ride in on the
# head or tail of a permitted name. The permitted set itself is unchanged.
bad=""
for lib in $($READELF -d "$SO" | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p'); do
    case "$lib" in
        libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|libgcc_s.so.*|ld-linux-armhf.so.*) ;;
        *) bad="$bad$lib
" ;;
    esac
done
[ -z "$bad" ] || { echo "FAIL: unexpected dependencies:"; printf '%s' "$bad"; exit 1; }

echo "PASS core dependency closure is device-safe"
$READELF -d "$SO" | sed -n 's/.*NEEDED.*\[\(.*\)\]/  needs \1/p'
