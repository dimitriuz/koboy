#!/bin/sh
# Cross-builds a static libfbink.a for the Kobo.
#
# FBInk is what abstracts the e-ink refresh ioctls (mxcfb/sunxi/mtk), the Kobo
# device identification table and the input-device classification we would
# otherwise have to reimplement and keep current per device. We link it
# *statically*: the device carries FBInk binaries, but not necessarily a
# libfbink.so of a version matching this header, and a missing/older shared
# library is a startup failure with no terminal to report it on.
#
# The build is deliberately a MINIMAL one plus three feature toggles:
#   DRAW   -> fbink_cls / fbink_fill_rect_gray, for the fatal-error screen
#   BITMAP -> fbink_print, ditto (the only text koboy ever asks FBInk to draw)
#   INPUT  -> fbink_input_scan, to classify /dev/input/event* by capability
# Everything else (OpenType, image decoding, QImageScale, unifont) is dead
# weight here: koboy blits its own gray8 straight into the mmap'ed fb.
set -e
# The upstream revision is PINNED; scripts/pins.txt is the table.
. "$(dirname "$0")/pins.sh"
CROSS="${CROSS:-arm-linux-gnueabihf-}"
SRC="${SRC:-third_party/fbink}"
ARM_FLAGS="${ARM_FLAGS:--march=armv7-a -mfpu=neon -mfloat-abi=hard}"

# Pinned like the cores, and with more at stake than any of them: FBInk is
# linked STATICALLY into koboy-arm, so its revision is part of the shipped
# emulator rather than of a file beside it. The third argument asks
# koboy_fetch_pinned to initialise submodules, which it does from the SHAs the
# pinned commit itself records -- so this one line pins font8x8, i2c-tools,
# libevdev, libunibreak and stb as well.
koboy_fetch_pinned fbink "$SRC" submodules

# FBInk's Makefile adds -fno-semantic-interposition unconditionally for
# non-Clang compilers. That option is GCC >= 5 only, and the toolchain that
# actually targets the device's glibc 2.19 is Linaro GCC 4.9.2, which errors
# out on it (it is an error, not a warning, for unknown -f options), taking
# the vendored i2c-tools build down with it. Gate it on the compiler actually
# accepting it. Applied here rather than committed as a patch file because the
# checkout is a gitignored clone.
if ! grep -q 'fno-semantic-interposition -E -x c' "$SRC/Makefile"; then
    sed -i 's/^\t\tEXTRA_CFLAGS+=-fno-semantic-interposition$/\t\tifeq "$(shell $(CC) -fno-semantic-interposition -E -x c \/dev\/null >\/dev\/null 2>\&1 \&\& echo 1 || echo 0)" "1"\n\t\t\tEXTRA_CFLAGS+=-fno-semantic-interposition\n\t\tendif/' \
        "$SRC/Makefile"
    grep -q 'fno-semantic-interposition -E -x c' "$SRC/Makefile" || {
        echo "FAIL: could not gate -fno-semantic-interposition in $SRC/Makefile" >&2
        exit 1
    }
fi

# CROSS_TC is FBInk's own cross-compile knob; it wants the tuple without the
# trailing dash, and derives CC/AR/RANLIB (gcc-ar/gcc-ranlib, for LTO) from it.
TC=$(printf '%s' "$CROSS" | sed 's/-$//')

LOG="${LOG:-build/fbink-build.log}"
mkdir -p "$(dirname "$LOG")"
# Not piped through tee: POSIX sh has no pipefail, and a pipeline would hide a
# failing make behind tee's exit status.
if ! make -C "$SRC" CROSS_TC="$TC" staticlib \
        KOBO=true MINIMAL=true DRAW=true BITMAP=true INPUT=true \
        CFLAGS="-O2 -fomit-frame-pointer -pipe $ARM_FLAGS" >"$LOG" 2>&1; then
    cat "$LOG" >&2
    echo "FAIL: FBInk build failed (full log in $LOG)" >&2
    exit 1
fi

LIB="$SRC/Release/libfbink.a"
[ -f "$LIB" ] || { echo "FAIL: $LIB was not produced" >&2; exit 1; }

# A MINIMAL build still *defines* every public symbol when a feature is off --
# the bodies just become `return -ENOSYS`. So checking nm output would not
# catch a dropped feature toggle. Check the actual compiler invocation instead.
for def in FBINK_FOR_KOBO FBINK_WITH_DRAW FBINK_WITH_BITMAP FBINK_WITH_INPUT; do
    grep -q -- "-D$def" "$LOG" || {
        echo "FAIL: fbink.c was not compiled with -D$def (see $LOG)" >&2; exit 1; }
done
# And the symbols we actually call must exist at all.
for sym in fbink_open fbink_init fbink_get_state fbink_get_fb_pointer \
           fbink_refresh fbink_cls fbink_print fbink_input_scan; do
    "${CROSS}nm" -g "$LIB" 2>/dev/null | grep -q " T $sym\$" || {
        echo "FAIL: $LIB does not define $sym" >&2; exit 1; }
done
echo "PASS libfbink.a built for $TC ($(wc -c <"$LIB") bytes)"
