#!/bin/sh
# Builds FinalBurn Neo (the arcade core) for either the dev host or the Kobo.
# Same family as build-core.sh (gambatte) and the nine scripts after it; read
# build-core.sh's long comment first for why CC/CXX/CFLAGS/CXXFLAGS/LDFLAGS are
# EXPORTED rather than passed as `make VAR=...`.
#
# THE REPOSITORY IS `libretro/FBNeo`, NOT `finalburnneo/FBNeo`, and this is a
# NEW variant of the trap `libretro/gw` and `libretro/gearcoleco` each sprang:
# both names exist and both are real FBNeo repositories. The difference is that
# only the libretro fork carries the libretro port at all --
# `src/burner/libretro/` is simply absent from the upstream tree, at every
# revision, so a clone of the "obvious" upstream name builds nothing.
#
# THE REVISION IS PINNED, and that is the whole reason this script exists in
# this shape. FBNeo is a ROM-SET-VERSIONED emulator: a driver's expected ROM
# names, sizes and CRCs change between revisions, and a set that does not match
# the build fails to load with the same "couldn't find rom" the user would see
# from a genuinely broken core. The owner's collection is matched to
# `FinalBurn Neo (ClrMame Pro XML, Arcade only).dat` version 1.0.0.03, dated
# 2025-07-24. The version number alone does NOT pin a revision -- FBNeo has
# carried VER_ALPHA 3 continuously since 2021-05-01 ("the WIP cycle begins
# again...", upstream 8ccb9e976) and still does -- so the pin is the LAST
# COMMIT OF THE DAY THE DAT WAS PUBLISHED. Verified against the dat before
# building: all 227 of the owner's zips were CRC-checked member by member
# against it, and every pre-1990 board matched exactly, including the device
# zips a set needs beside a game (tapper wants midssio.zip, which is present).
# The hash itself now lives in scripts/pins.txt with the other fourteen --
# one mechanism instead of two -- but the REASON stays here, because it is
# not the reason the others are pinned. Theirs is reproducibility; this
# one's is that the content on the user's card has to match the build.
#
# NO SUBSET, DELIBERATELY, and it was measured rather than assumed. FBNeo's
# libretro Makefile supports partial builds (Makefile.pre68k, Makefile.cps12,
# Makefile.neogeo, selected with SUBSET=), and `pre68k` is exactly the era this
# core was added for -- except that its own header says "TODO: finish adding
# all drivers" and it is telling the truth: it carries the Galaxian tree
# (galaxian, frogger, scobra) but NOT `pre90s`, which is where d_galaga.cpp
# (Galaga, Dig Dug, Xevious), d_pacman.cpp (Ms. Pac-Man, Pengo), d_dkong.cpp
# and d_mappy.cpp live. Every headline title of the era is in the half that is
# missing, so the subset would have to be written from scratch here and kept
# in step with upstream by hand. The two things a subset buys are build time
# and file size; the first is not a problem (a full build is 100 seconds wall
# on a 16-thread host, 20 CPU-minutes) and the second turned out not to be a
# problem either -- see the size note below. So: full build, every board FBNeo
# has, and the boards outside this task's pre-1990 scope are a bonus that
# costs nothing rather than a goal.
#
# THE CORE IS 41 MB AND IT DOES GO IN THE MAIN PACKAGE -- which is a REVERSAL,
# because for most of this project's life it did not. It shipped in its own
# archive behind a `make fbneo-dist` target that no longer exists, on the
# grounds that 41 MB against the other cores' 18 was a tenfold blowup on a
# 4 MB download.
#
# What changed is that somebody measured the number that actually decides a
# download. This core DEFLATES 67%, to 13.6 MB, because one arcade driver
# table is much like the next -- so the whole package is 18.6 MB rather than
# the 45+ the uncompressed figure implies. At that size a second archive, a
# second README and a `.zip` row in the browser that failed to load until the
# user found the other download all cost more than they saved.
#
# The size is now the OWNER'S choice instead of the packager's: deleting
# .adds/koboy/fbneo_libretro.so reclaims 41 MB on the card and costs arcade
# support and nothing else. The generated roms/README.txt and README-fbneo.txt
# both say so by filename. tests/test_dist.sh asserts the core IS in the
# package, that it has real bytes in it, and that the package stays under a
# 32 MB cap -- which exists so the NEXT large core is a decision somebody
# makes rather than a download that quietly doubles.
#
# gnu++98, WHICH IS WHY THIS CORE IS BUILDABLE HERE AT ALL. FBNeo's Makefile
# pins -std=gnu++98 itself (Makefile.common), so unlike `libretro/stella` --
# which hard-codes -std=c++17 and is therefore not buildable with the Linaro
# 4.9.2 the device's glibc 2.19 forces on us -- nothing has to be talked out
# of a standard the compiler does not have.
#
# ONE SWITCH OFF THE DEFAULTS, AND THE COMPILER CHOSE IT.
# INCLUDE_7Z_SUPPORT=0, because with it on the ARM cross-build DOES NOT
# COMPILE: dep/libs/lib7z/CpuArch.c reads HWCAP_NEON and HWCAP2_CRC32/SHA1/
# SHA2/AES out of the kernel headers, and glibc 2.19's do not define them
# (`error: HWCAP_NEON undeclared`). That is the same class of wall
# `libretro/stella`'s -std=c++17 was: a modern assumption meeting a 2014
# toolchain. Here it costs nothing to step around, because koboy hands this
# core a .zip and only a .zip -- config_core_for_rom's table and
# romlist_is_rom between them make .7z unreachable -- so the support being
# compiled out removes a path no koboy user could take. The HOST build keeps
# it on (its headers have the macros) so this script is not silently building
# two different cores for a reason unrelated to the platform... except that it
# IS a difference, so it is written down here rather than discovered: the host
# core can open a .7z and the device core cannot, and no koboy code path on
# either reaches that difference.
#
# NO OTHER SWITCHES. In particular USE_CYCLONE (the ARM 68000 dynarec) stays
# off: the generic `platform=unix` path leaves it at 0, the pre-1990 boards
# this was added for are Z80/6502/6809 machines that never reach a 68000 core,
# and turning on hand-written ARM assembly on a toolchain this old to speed up
# boards outside the task's scope is a bad trade. INCLUDE_CHD_SUPPORT stays on
# for the mirror-image reason 7z is off: it compiles, so leaving it alone is
# the smaller change.
#
# INCLUDE_7Z_SUPPORT is passed as a `make VAR=` COMMAND-LINE argument, which
# build-core.sh's comment warns against for CC/CFLAGS/LDFLAGS -- deliberately,
# and the distinction is the one that comment is actually about. That warning
# is about variables the makefile APPENDS to with `+=`, which a command-line
# value silently swallows. This is a plain boolean the makefile only ever
# reads, and it is assigned with `=` at the top of FBNeo's Makefile, so an
# exported environment variable would NOT override it -- a command-line
# argument is the only thing that does.
#
# NO BIOS FOR THE ERA THIS SHIPS FOR, and that is a fact about the boards, not
# an omission: a 1981 arcade PCB has its whole program in the cartridge-slot
# ROMs. FBNeo does want a BIOS zip for later hardware (neogeo.zip, and the
# device zips some boards need beside the game), and those live BESIDE THE
# GAME in the roms directory, not in the system directory -- they are romset
# members as far as FBNeo is concerned. Nothing is shipped either way.
#
# SAVES DO NOT GO THROUGH RETRO_MEMORY_SAVE_RAM. Measured across the owner's
# pre-1990 boards: retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) is 0 on every
# one of them. An arcade PCB has no battery-backed cartridge; what it has is a
# high-score table in ordinary RAM, which FBNeo persists through its own
# hiscore.dat mechanism and not through the libretro save-RAM interface.
# retro_serialize_size() is non-zero, so koboy's SAVE STATES are the working
# way to keep a game -- that is the mechanism the MENU already offers.
set -e
# The upstream revision is PINNED -- see scripts/pins.txt, and the long
# comment above for why THIS core's pin is about romsets rather than about
# reproducible builds.
. "$(dirname "$0")/pins.sh"

TARGET="${1:-kobo}"
SRC="${SRC:-third_party/fbneo}"
LR="$SRC/src/burner/libretro"

# koboy_fetch_pinned fetches the pinned commit DIRECTLY (`git fetch --depth 1
# origin <sha>`), which is strictly better than what this script used to do
# here: a --filter=blob:none clone of the whole history followed by a checkout,
# needed only because `git clone --depth 1` cannot take a commit id. FBNeo is
# the largest tree koboy builds and it was paying for that history every time.
koboy_fetch_pinned fbneo "$SRC"

make -C "$LR" clean || true

case "$TARGET" in
    host)
        OUT="${OUT:-build/fbneo_libretro_host.so}"
        export CC="${CC:-cc}"
        export CXX="${CXX:-c++}"
        export CFLAGS="-fPIC"
        export CXXFLAGS="-fPIC"
        export LDFLAGS=""
        make -C "$LR" -j"$(nproc 2>/dev/null || echo 4)" platform=unix
        mkdir -p "$(dirname "$OUT")"
        cp "$LR"/fbneo_libretro.so "$OUT"
        echo "built host core: $OUT"
        ;;
    kobo)
        CROSS="${CROSS:-arm-linux-gnueabihf-}"
        OUT="${OUT:-build/fbneo_libretro_arm.so}"
        export CC="${CROSS}gcc"
        export CXX="${CROSS}g++"
        export CFLAGS="-march=armv7-a -mfpu=neon -mfloat-abi=hard -fPIC"
        export CXXFLAGS="-march=armv7-a -mfpu=neon -mfloat-abi=hard -fPIC"
        # C++ throughout, so -static-libstdc++ like gambatte, stella2014 and
        # Gearcoleco. -lpthread comes from FBNeo's own Makefile and is inside
        # scripts/verify-core.sh's allowlist.
        export LDFLAGS="-static-libstdc++ -static-libgcc"
        make -C "$LR" -j"$(nproc 2>/dev/null || echo 4)" platform=unix INCLUDE_7Z_SUPPORT=0
        mkdir -p "$(dirname "$OUT")"
        cp "$LR"/fbneo_libretro.so "$OUT"
        "${CROSS}strip" "$OUT" || true
        READELF="${CROSS}readelf" sh scripts/verify-core.sh "$OUT"
        ;;
    *)
        echo "usage: $0 [host|kobo]" >&2
        exit 1
        ;;
esac
