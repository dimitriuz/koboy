#!/bin/sh
# Builds snes9x2005 (the SNES core) for either the dev host or the Kobo.
# Same family as build-core.sh; read that script's long comment for why
# CC/CFLAGS/LDFLAGS are EXPORTED rather than passed as `make VAR=...`.
#
# THE V1 SPEC RULED SNES OUT OF SCOPE ON CPU GROUNDS. This script exists
# because that judgement was re-tested rather than inherited, and the
# measurement said the opposite -- see docs/superpowers/specs and TESTED.md
# for the numbers. Read the numbers before deciding this core is replaceable.
#
# SNES9X2005 rather than snes9x2010 or snes9x, and ALL THREE WERE BUILT AND
# MEASURED before choosing (host, x86_64, 600 frames after a 60-frame warmup,
# mean us per retro_run, scripts/corebench.c):
#
#              title                     2005    2010   snes9x
#              Super Mario World         229.5   274.9   400.2
#              Zelda - Link to the Past  208.6   280.3   356.3
#              Super Metroid             156.7   307.0   364.8
#              Chrono Trigger            292.9   346.4   391.8
#              F-Zero                    354.2   942.4   551.3
#              Donkey Kong Country       191.3   308.0   396.7
#              Star Fox       (SuperFX)  748.6   619.2   942.3
#              Kirby Super Star  (SA-1)  520.5   570.6   804.9
#              Yoshi's Island (SuperFX2) 217.1   332.4   413.0
#              ---------------------------------------------
#              sum                      2919.4  3981.2  4621.6
#
#   - FASTEST overall by 1.36x over snes9x2010 and 1.58x over snes9x. On a
#     single-core Cortex-A9 where the frame budget left for emulation is
#     10-12 ms, that margin is the difference between shipping the system and
#     not. snes9x2010 wins on exactly one title (Star Fox) and loses badly on
#     another (F-Zero, 2.7x slower), so the aggregate is not a coin flip.
#   - PURE C. There is no .cpp in the tree at all, so the closure is libc +
#     libm and scripts/verify-core.sh passes with no -static-libstdc++.
#     snes9x is 37 C++ translation units and would need the gambatte
#     treatment; snes9x2010 is also pure C but is the slower of the two.
#   - NO C++ STANDARD PROBLEM, checked before committing to it rather than
#     after: this is what made libretro/stella unbuildable here (it forces
#     -std=c++17, which Linaro 4.9.2 rejects outright). snes9x2005 asks for
#     no C++ standard because it compiles no C++.
#
# THE COMPATIBILITY WARNING THAT TURNED OUT TO BE FALSE, and this is the part
# worth not taking on faith. The folklore -- and the brief this core was
# added under -- says snes9x2005 drops the special-chip titles: SuperFX (Star
# Fox, Yoshi's Island) and SA-1 (Kirby Super Star). IT DOES NOT. This
# revision carries both. RENDERED AND LOOKED AT, not inferred from a
# successful load, because "the core accepted the ROM" is exactly the check
# that would have missed it:
#   - Star Fox reaches the Corneria stage and draws the polygonal Arwing and
#     terrain (frame 1000 of a scripted attract run). Those polygons ARE the
#     GSU's output; a stubbed SuperFX draws a black screen.
#   - Yoshi's Island reaches gameplay with Yoshi and Baby Mario on screen,
#     pixel-comparable to the same frame under snes9x.
#   - Kirby Super Star reaches its "Is this your first time playing?" prompt,
#     which is past the SA-1 boot the folklore says it cannot survive.
# A whole-collection sweep is in TESTED.md. What this means practically is
# that the compatibility cost of choosing the FASTEST core here is much
# smaller than the brief expected -- but it is a property of THIS core at
# THIS revision, so re-render those three titles before swapping it.
#
# LTO AND STRICT ALIASING. Left at the tree's defaults deliberately. The
# emulated machine's memory is one byte array reinterpreted through every
# width the SNES buses use, so the tree violates C type-based aliasing by
# design; the sibling snes9x2010 Makefile carries a long comment about GCC 9
# miscompiling the sprite path under LTO for exactly that reason. Linaro
# 4.9.2 is well before that, and this fork's own build already passes
# -fno-strict-aliasing where it matters. Do not "tidy" -O3 into -Ofast here:
# fast-math changes the APU's arithmetic.
#
# SAVES GO THROUGH RETRO_MEMORY_SAVE_RAM (the sram.c path), and UNLIKE
# GENESIS PLUS GX THE SIZE THIS CORE REPORTS IS CONSTANT: measured with
# scripts/corebench.c, which prints the save length at load and again after
# the warmup, snes9x2005 answers the same number both times on every
# battery-backed title tried. So core_sram's pin-at-load (src/core.c, commit
# 1fb3802) is not LOAD-BEARING for this system the way it is for GPGX -- but
# it is also not harmful, and it is the same code path, so nothing special is
# done here. Recorded so the next person does not go looking for a bug.
set -e

TARGET="${1:-kobo}"
SRC="${SRC:-third_party/snes9x2005}"

[ -d "$SRC" ] || git clone --depth 1 \
    https://github.com/libretro/snes9x2005 "$SRC"

make -C "$SRC" clean || true

case "$TARGET" in
    host)
        OUT="${OUT:-build/snes9x2005_libretro_host.so}"
        export CC="${CC:-cc}"
        export CFLAGS="-O2 -fPIC"
        export LDFLAGS=""
        make -C "$SRC" platform=unix
        mkdir -p "$(dirname "$OUT")"
        cp "$SRC"/snes9x2005_libretro.so "$OUT"
        echo "built host core: $OUT"
        ;;
    kobo)
        CROSS="${CROSS:-arm-linux-gnueabihf-}"
        OUT="${OUT:-build/snes9x2005_libretro_arm.so}"
        export CC="${CROSS}gcc"
        export CFLAGS="-O3 -march=armv7-a -mfpu=neon -mfloat-abi=hard -fPIC"
        # -static-libgcc only, no -static-libstdc++: there is no C++ here.
        export LDFLAGS="-static-libgcc"
        make -C "$SRC" platform=unix
        mkdir -p "$(dirname "$OUT")"
        cp "$SRC"/snes9x2005_libretro.so "$OUT"
        "${CROSS}strip" "$OUT" || true
        READELF="${CROSS}readelf" sh scripts/verify-core.sh "$OUT"
        ;;
    *)
        echo "usage: $0 [host|kobo]" >&2
        exit 1
        ;;
esac
