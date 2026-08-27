#!/bin/sh
# Builds Beetle PCE Fast (the PC Engine / TurboGrafx-16 core) for either the
# dev host or the Kobo. Same family as build-core.sh; read that script's long
# comment for why CC/CFLAGS/LDFLAGS are EXPORTED rather than passed as
# `make VAR=...`.
#
# THE REPOSITORY NAME IS A 404 TRAP, and a new variant of one this project has
# now hit four times. `libretro/beetle-pce-fast` DOES NOT EXIST. The real
# repository is `libretro/beetle-pce-fast-libretro` -- the "-libretro" suffix
# is part of the name, not a description of it. (Previous instances:
# libretro/stella2014 404s where stella2014-libretro does not;
# libretro/gearcoleco 404s where drhelius/Gearcoleco does not; and FBNeo is
# the nastier kind where BOTH names resolve and only one is the right fork.)
# Clone the name written below, not the one you would guess.
#
# ONE CORE, ONE SYSTEM, AND DELIBERATELY THE SMALLEST OF THE THREE ROUTES IN.
# The core advertises `pce|sgx|cue|ccd|chd|toc|m3u`. koboy claims `.pce` and
# NOTHING ELSE, and both exclusions are decisions rather than omissions:
#
#   - .sgx is SUPERGRAFX, and this core cannot run it. The SuperGrafx has a
#     second VDC and a priority-mixing VPC; beetle-pce-FAST implements
#     neither, so an .sgx file here would load and render wrongly rather than
#     refuse -- the worst failure this project recognises. Running it needs
#     beetle-pce (the full one) or beetle-supergrafx, which is a second core
#     to build, ship and pay CPU for. The author's collection has SEVEN .sgx
#     files against 580 .pce. Seven titles do not buy a second core on a
#     device whose frame budget is already spent, so .sgx is not listed at
#     all: it stays invisible in the browser rather than appearing and
#     misbehaving.
#   - .chd/.cue/.ccd/.toc/.m3u are CD-ROM^2. 48 titles in the author's
#     collection, and every one of them needs a SYSTEM CARD BIOS (syscard3.pce
#     and friends) that is not ours to ship, plus CD track emulation and a
#     save path that is not the cartridge one. That is its own project and
#     nobody asked for it. Not listed.
#
# BEETLE PCE FAST rather than beetle-pce or beetle-supergrafx, chosen for the
# reason every core in this project is chosen -- the CPU. The HuC6280 is a
# 7.16 MHz 65C02 derivative, far lighter than a 68000 or a 65816, and the
# measurement agrees: 124-253 us per frame on the host over eight titles,
# the CHEAPEST of the three systems in the batch that added it. The "fast"
# fork drops the SuperGrafx VDC and some cycle-accuracy for that.
#
# C++ STANDARD, checked before committing to it rather than after -- this is
# what made libretro/stella unbuildable here (it forces -std=c++17, which
# Linaro 4.9.2 rejects outright). This tree asks for gnu++11/c++11, which
# 4.9.2 accepts.
#
# ITS LINK LINE NEEDS TWO EXTRA FLAGS, AND THE REASON IS A TRAP WORTH NAMING.
# Counting .cpp files says it should need none: the only one in the tree is
# libretro-common's UWP VFS shim, which platform=unix never compiles, so every
# object here is C -- exactly the Genesis Plus GX situation, where a plain
# -static-libgcc is enough. But this Makefile sets `LD = $(CXX)`
# UNCONDITIONALLY for every non-MSVC platform (its line 692) and appends an
# unconditional `-lrt`, so the link runs through the C++ driver and against
# the realtime library regardless of what was compiled. The first cross-build
# of this core was written on the source-counting reasoning above and
# scripts/verify-core.sh rejected it twice, once per flag:
#   "FAIL: dynamic libstdc++ dependency" then "FAIL: unexpected: librt.so.1".
# The lesson generalises: read the LINKER, not the source tree.
#
# WHICH FLAG DOES WHAT, measured rather than assumed, because the obvious
# reading is wrong. Three builds of the same objects:
#   -static-libgcc alone                 libstdc++.so.6, librt.so.1, libm, libc
#   + -static-libstdc++                  librt.so.1, libm, libc
#   + -Wl,--as-needed  (instead)         libm, libc
# So --as-needed ALONE is what produces the minimal closure -- it drops both,
# because the core references no symbol from either. -static-libstdc++ is kept
# anyway, deliberately and not as cargo: it is the flag that still holds if a
# later revision of this tree grows real C++, where --as-needed would happily
# keep a dynamic libstdc++ the device does not have. Neither is load-bearing
# on its own; verify-core.sh is the thing that actually guarantees the result,
# and it runs on every kobo build below.
#
# --as-needed is safe here for a specific reason rather than by habit: the
# tree links with -Wl,--no-undefined, so a library that really was required
# could not be silently dropped -- the link would fail instead.
#
# The shipped closure is therefore libm.so.6 + libc.so.6 and nothing else,
# which is the SMALLEST of the three cores in this batch and matches the two
# WonderSwan/Neo Geo cores rather than gambatte's.
#
# NO BIOS SHIPS AND NONE IS NEEDED for .pce. A HuCard carries its whole
# program; the system card is a CD-ROM^2 thing, and CD is not listed above.
#
# NO NON-DEFAULT SWITCHES, and two defaults are worth naming because they are
# the ones that would have hurt:
#   - `pce_fast_default_joypad_type_p1` defaults to "2 Buttons", which is
#     what the DMG faceplate can present in full (I on JOYPAD_A, II on
#     JOYPAD_B, RUN on START, Select on SELECT -- read off the core's own
#     input descriptors). koboy answers no core options at all, so the
#     default stands and PC Engine needs no extra disc. Switching to
#     "6 Buttons" would put III-VI on JOYPAD_Y/X/L/R, three of which the
#     faceplate has nowhere to draw, AND the core's own option text warns
#     that a 6-button pad "can have weird behaviors in non compatible games".
#   - the core requests RGB565, so video_submit reads two bytes a pixel and
#     not four. No WANT_32BPP-style switch is needed here; unlike fceumm this
#     tree picks the narrow format on its own.
#
# THIS SYSTEM CHANGES ITS HORIZONTAL RESOLUTION MID-GAME, which no other
# system koboy runs does to the same degree: titles switch between 256, 352
# and 512-pixel widths, sometimes between scenes. That is a front-end
# property, not a build switch, and it is handled by main.c's per-frame
# core_geometry_changed() poll -- but it is the reason this core's addition
# exercised that path harder than anything before it. See TESTED.md.
#
# SAVES GO THROUGH RETRO_MEMORY_SAVE_RAM (the sram.c path). The PC Engine's
# battery RAM is the 2 KB "Backup Unit" shared by every title that uses it
# rather than a per-cartridge chip, so unlike a Mega Drive or SNES cartridge
# ONE .srm holds several games' saves. koboy names it after the ROM like
# every other system, which means each title gets its own copy of a shared
# store -- correct per title, and it costs nothing, but it is not what real
# hardware did and a reader will wonder.
set -e

TARGET="${1:-kobo}"
SRC="${SRC:-third_party/beetle-pce-fast-libretro}"

[ -d "$SRC" ] || git clone --depth 1 \
    https://github.com/libretro/beetle-pce-fast-libretro "$SRC"

make -C "$SRC" clean || true

case "$TARGET" in
    host)
        OUT="${OUT:-build/mednafen_pce_fast_libretro_host.so}"
        export CC="${CC:-cc}"
        export CFLAGS="-O2 -fPIC"
        export LDFLAGS=""
        make -C "$SRC" platform=unix
        mkdir -p "$(dirname "$OUT")"
        cp "$SRC"/mednafen_pce_fast_libretro.so "$OUT"
        echo "built host core: $OUT"
        ;;
    kobo)
        CROSS="${CROSS:-arm-linux-gnueabihf-}"
        OUT="${OUT:-build/mednafen_pce_fast_libretro_arm.so}"
        export CC="${CROSS}gcc"
        export CXX="${CROSS}g++"
        export CFLAGS="-O3 -march=armv7-a -mfpu=neon -mfloat-abi=hard -fPIC"
        # See the header for what each of these three is doing and which one
        # actually matters (--as-needed). Do not drop one because the build
        # still passes without it: verify-core.sh is checked on every build
        # here precisely so that a wrong guess fails loudly.
        export LDFLAGS="-static-libgcc -static-libstdc++ -Wl,--as-needed"
        make -C "$SRC" platform=unix
        mkdir -p "$(dirname "$OUT")"
        cp "$SRC"/mednafen_pce_fast_libretro.so "$OUT"
        "${CROSS}strip" "$OUT" || true
        READELF="${CROSS}readelf" sh scripts/verify-core.sh "$OUT"
        ;;
    *)
        echo "usage: $0 [host|kobo]" >&2
        exit 1
        ;;
esac
