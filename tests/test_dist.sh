#!/bin/sh
# The packaging contract: unzip-to-install, no rootfs modification, and a launch
# script whose Nickel restore runs unconditionally.
#
# Usage: tests/test_dist.sh [LAUNCHER]
#
# With no argument it builds the zip and checks everything. Given a launcher
# path it checks only that script, which is what the MUTATION EXPERIMENTS use:
# every assertion about the launcher below must FAIL when the property it
# guards is removed, and the only way to show that is to run these exact
# assertions -- not a paraphrase of them -- against a deliberately broken copy.
# Three tests on this branch have already turned out to pass with the code they
# guarded deleted; two of them were in this file.
set -e

SH="${1:-scripts/koboy.sh}"
[ -f "$SH" ] || { echo "FAIL: no launcher at $SH"; exit 1; }

# The packaging half needs the ARM cross toolchain (see docs/cross-compiling.md);
# the launcher half needs nothing but a shell. They used to be one `set -e`
# block, so on a host without the toolchain this script died inside `make dist`
# at line 20 and exited 2 with no diagnostic, having run ZERO of the launcher
# assertions -- while CLAUDE.md lists it among the host tests. A test that
# silently checks nothing on the machine it is documented to run on is the same
# failure this branch keeps finding, so the split is explicit and the skip is
# LOUD.
CROSS="${CROSS:-arm-linux-gnueabihf-}"
SKIPPED=""
if [ -z "$1" ] && ! command -v "${CROSS}gcc" >/dev/null 2>&1; then
    SKIPPED="yes"
    echo "SKIP: no ${CROSS}gcc on PATH -- packaging assertions (make dist, zip"
    echo "SKIP: contents) NOT run. Launcher assertions below still run."
    echo "SKIP: put the Linaro toolchain on PATH to check packaging too;"
    echo "SKIP: see docs/cross-compiling.md."
fi

# Independent of the toolchain: verify-core.sh is a shell script and this check
# stubs readelf outright, so it runs on any host. It used to sit AFTER the
# packaging block and was therefore skipped along with it.
if [ -z "$1" ]; then
    # #10: the allowlist matched by substring, so a library named reallibc.so.6
    # would have passed. Not attacker-facing -- it is a build-time script -- but
    # anchors are free, and an allowlist that accepts a superstring is not an
    # allowlist. The stub answers both invocations verify-core.sh actually makes
    # (readelf -h for the architecture check, readelf -d for the NEEDED list) so
    # the run reaches the dependency check itself instead of failing earlier on
    # a fake ELF header.
    fake="$(mktemp -d)"
    cat > "$fake/readelf" <<'EOS'
#!/bin/sh
case "$*" in
    *-h*) echo "  Machine:                           ARM" ;;
    *-d*) echo " 0x00000001 (NEEDED)             Shared library: [reallibc.so.6]" ;;
esac
EOS
    chmod +x "$fake/readelf"
    if PATH="$fake:$PATH" sh scripts/verify-core.sh /bin/true >/dev/null 2>&1; then
        echo "FAIL: verify-core.sh accepted reallibc.so.6"
        rm -rf "$fake"; exit 1
    fi
    rm -rf "$fake"
    echo "ok: verify-core.sh allowlist is anchored"
fi

if [ -z "$1" ] && [ -z "$SKIPPED" ]; then
    make dist
    # THE ZIP `make dist` JUST BUILT, by exact name -- not `ls | head -1`,
    # which is what this was and which is a test checking the wrong artifact.
    # dist/ is git-ignored and accumulates: on a working tree that had built
    # 0.1.0, 0.5.0 and 0.5.1, the glob's first entry was the 0.1.0 archive
    # from a previous day, so every packaging assertion below -- the core
    # list, the BIOS ban, the size cap -- was passing against a stale package
    # while the one about to ship went unexamined. It only stayed invisible
    # because that old zip was ALSO a complete build; the day it is not, this
    # reports a package nobody has.
    #
    # ./VERSION, the one place the release number lives -- the same file the
    # Makefile and release.yml's tag gate read. It used to be an awk one-liner
    # against `VERSION :=` in the Makefile, copied here and into the workflow,
    # which is three parsers for a five-character string.
    V=$(tr -d '[:space:]' < VERSION 2>/dev/null)
    [ -n "$V" ] || { echo "FAIL: cannot read ./VERSION"; exit 1; }
    Z="dist/koboy-$V.zip"
    [ -f "$Z" ] || { echo "FAIL: make dist produced no $Z"; exit 1; }

    # Nothing may install outside .adds/koboy: no root, no brick risk. The
    # ONE exception is KOBOY-INSTALL.md, an inert note at the drive root --
    # named here rather than allowed by a wildcard, so a SECOND stray file
    # still fails this. It is why the two greps below are separate: the first
    # is the safety property, the second is the reason the exception exists.
    if unzip -Z1 "$Z" | grep -v '^\.adds/koboy/' | grep -v '^KOBOY-INSTALL\.md$'; then
        echo "FAIL: zip writes outside .adds/koboy/"; exit 1
    fi
    # ...and the archive must not LOOK empty. Every payload path begins with
    # `.adds`, which Linux file managers, Finder and Explorer all hide, so
    # without a visible top-level entry the package opens as a blank window
    # and reads as a broken download. This asserts the entry a human sees --
    # a property no other check here covers, since every one of them is
    # satisfied by an archive that is entirely hidden.
    # `grep -qv` is NOT the way to write this: on the grep here it returns 1
    # for input that plainly contains a non-matching line, so the assertion
    # failed on a correct archive. Substituting the empty test keeps the
    # meaning and does not depend on how -q and -v combine.
    if [ -z "$(unzip -Z1 "$Z" | cut -d/ -f1 | grep -v '^\.')" ]; then
        echo "FAIL: every top-level entry is hidden; the zip looks empty"; exit 1
    fi
    if ! unzip -p "$Z" KOBOY-INSTALL.md | grep -q 'root'; then
        echo "FAIL: KOBOY-INSTALL.md missing or does not say where to extract"; exit 1
    fi
    if unzip -Z1 "$Z" | grep -q 'KoboRoot'; then
        echo "FAIL: ships a KoboRoot.tgz"; exit 1
    fi

    # EVERY SHIPPED CORE IS NAMED HERE, and the last three were added after
    # this list was found to have DRIFTED: snes9x2005 and beetle-pce-fast had
    # been shipping for a batch without an entry, and gpSP was added to the
    # dist rule's prerequisites but not to its `cp` block -- a package that
    # built, zipped and passed everything while missing a whole system's core.
    # The failure is silent by construction: a listed .gba loads nothing and
    # says "cannot open core", which reads as a broken emulator. When a core
    # is added to the Makefile, it goes here in the same commit.
    for f in .adds/koboy/koboy .adds/koboy/koboy-probe .adds/koboy/koboy.sh \
             .adds/koboy/koboy.ini .adds/koboy/gambatte_libretro.so \
             .adds/koboy/gw_libretro.so \
             .adds/koboy/fceumm_libretro.so \
             .adds/koboy/pokemini_libretro.so \
             .adds/koboy/mednafen_wswan_libretro.so \
             .adds/koboy/race_libretro.so \
             .adds/koboy/stella2014_libretro.so \
             .adds/koboy/gearcoleco_libretro.so \
             .adds/koboy/freeintv_libretro.so \
             .adds/koboy/genesis_plus_gx_libretro.so \
             .adds/koboy/snes9x2005_libretro.so \
             .adds/koboy/mednafen_pce_fast_libretro.so \
             .adds/koboy/gpsp_libretro.so \
             .adds/koboy/fbneo_libretro.so \
             .adds/koboy/nm-koboy .adds/koboy/kfmon-koboy.ini \
             .adds/koboy/README.md .adds/koboy/TESTED.md \
             .adds/koboy/README-fbneo.txt \
             .adds/koboy/roms/README.txt; do
        unzip -Z1 "$Z" | grep -qx "$f" || { echo "FAIL: missing $f"; exit 1; }
    done

    # NO BIOS AND NO ROM, ever, and this assertion stopped being hypothetical
    # with this batch. Most cores koboy ships need no BIOS or link their own
    # free one (PokeMini's freebios/, RACE's ngpBios.c) -- but TWO NOW
    # GENUINELY REQUIRE A COPYRIGHTED ONE that lives on the author's own disk
    # while these scripts run: Gearcoleco will not draw a single game frame
    # without colecovision.rom, and FreeIntv halts without exec.bin and
    # grom.bin. They are not ours to distribute; the owner installs them by
    # hand (see roms/README.txt, which the package DOES carry) and the build
    # must never quietly pick one up. `.rom` and `.bin` cover all three by
    # extension, alongside the content extensions -- a dumped "[BIOS]
    # Nintendo Pokemon Mini (World).min", the boot.rom / boot1.rom that sit
    # beside a real WonderSwan, Neo Geo Pocket or Intellivision collection, or
    # an .nes that wandered in from a test directory. A packaging step that
    # copied any of them would look exactly like a working build. Checked on
    # the zip's own listing, which is the artefact that actually ships.
    #
    # `.zip` joins the list with this batch and is the one entry that needs
    # its own sentence, because the artefact being checked IS a zip: this
    # matches a zip INSIDE the package, which is what an arcade romset is
    # (galaga.zip). An arcade set is content exactly like a .nes is, and it is
    # the first content type whose extension collides with the container it
    # would be smuggled in.
    #
    # .sfc/.smc/.pce/.gba joined later, and one extension CANNOT join: `.md`.
    # The package deliberately carries README.md and TESTED.md, so a `md` alt
    # here would fail every build. That is the same collision src/config.c
    # refused `.bin` over, from the packaging side, and it is why the Mega
    # Drive has no entry in this guard.
    BAD='\.(min|nes|gb|gbc|gba|mgw|ws|wsc|ngp|ngc|a26|col|int|sms|gg|sfc|smc|pce|zip|srm|ngf|flash|rom|bin)$'
    if unzip -Z1 "$Z" | grep -qiE "$BAD"; then
        echo "FAIL: the package contains content or a BIOS:"
        unzip -Z1 "$Z" | grep -iE "$BAD"
        exit 1
    fi

    # The generated roms/README.txt must name every extension the browser
    # actually lists. It is the only instruction a user gets, and a system
    # whose files are accepted but never mentioned is a system nobody knows
    # to copy anything for. Extracted and read, not assumed from the Makefile.
    rd=$(mktemp -d)
    unzip -qo "$Z" .adds/koboy/roms/README.txt -d "$rd"
    for ext in .gb .gbc .gba .mgw .nes .min .ws .wsc .ngp .ngc .a26 .col .int .sms .gg .zip; do
        grep -qF -- "$ext" "$rd/.adds/koboy/roms/README.txt" \
            || { echo "FAIL: roms/README.txt does not mention $ext"; rm -rf "$rd"; exit 1; }
    done
    # The BIOS instruction is the only thing standing between a user and a
    # ColecoVision that shows NO BIOS or an Intellivision that shows nothing,
    # so it is asserted by NAME. A README that lists the extensions but not
    # the files is a README that makes two of the eleven systems look broken.
    for f in colecovision.rom exec.bin grom.bin; do
        grep -qF -- "$f" "$rd/.adds/koboy/roms/README.txt" \
            || { echo "FAIL: roms/README.txt does not name $f"; rm -rf "$rd"; exit 1; }
    done
    # THE ARCADE CORE IS 39 MB OF A ~40 MB INSTALL, and this asserts that the
    # file a user reads first tells them so BY FILENAME. It used to assert the
    # opposite thing -- that roms/README.txt named the second archive the core
    # arrived in -- because arcade shipped separately; one archive replaced
    # two, and the question the text has to answer changed with it, from
    # "where do I get this" to "what is all this space and can I have it
    # back". A vague "you can delete the arcade core" would not do: the user
    # is looking at a directory of .so files and needs the name.
    grep -qF -- "fbneo_libretro.so" "$rd/.adds/koboy/roms/README.txt" \
        || { echo "FAIL: roms/README.txt lists .zip but never names fbneo_libretro.so, the file that can be deleted"; rm -rf "$rd"; exit 1; }
    rm -rf "$rd"

    # THE ARCADE CORE IS IN THE PACKAGE, and this assertion is the INVERSE of
    # the one it replaces. Arcade shipped in its own archive for most of this
    # project's life and this block asserted the main package did NOT carry
    # fbneo; koboy now ships one archive, so the same line of defence has to
    # point the other way. Inverted rather than deleted: an assertion nobody
    # replaced is how the `cp` for gpSP went missing from the dist rule while
    # every test passed.
    #
    # The named-cores loop above already requires the .so by path. This adds
    # the thing that loop cannot see -- that the recipe copies a file with
    # real bytes in it. A zero-length or truncated `cp` satisfies a listing
    # and produces "cannot open core" on the device. 30 MB is a floor, not a
    # size check: the core is 39.2 MB stripped and zip gets it to about 12,
    # so any plausible build clears it and an empty file does not.
    # `unzip -Z` (not -Z1, which is names only, and not -Z1 -v, which prints
    # the zipfile's central-directory header and no per-file rows at all):
    # field 4 of a -Z row is the UNCOMPRESSED size.
    fbz=$(unzip -Z "$Z" | awk '$NF ~ /fbneo_libretro\.so$/ { print $4 }')
    [ -n "$fbz" ] || { echo "FAIL: no fbneo_libretro.so in the package"; exit 1; }
    [ "$fbz" -ge 31457280 ] || {
        echo "FAIL: fbneo_libretro.so is $fbz bytes uncompressed -- a truncated core"
        exit 1; }
    # THE PACKAGE HAS A SIZE CAP, and its job survived the switch to one
    # archive even though its number did not. The cap does not exist to keep
    # the download small -- the owner has decided 18.6 MB is fine -- it exists
    # so that a core landing in the tens of megabytes is a DECISION somebody
    # makes rather than a download that quietly doubles. That was the original
    # reasoning when arcade shipped separately and it is unchanged; only the
    # baseline moved.
    #
    # 32 MB, measured rather than guessed. The package unpacks to 61 MB and
    # zips to 18.6, and FBNeo is 41.1 MB of that 61 deflating to 13.6 -- a 67%
    # squeeze, because one driver table is much like the next. That is the
    # fact that made one archive reasonable and nobody had measured it while
    # the split was in place. So this cap is ~1.7x headroom.
    # Tighter than the 3x this cap used to carry, deliberately: with FBNeo
    # already inside, the next thing big enough to matter is big enough that
    # 1.7x is the right amount of rope.
    #
    # Tripping this is NOT automatically a bug. It is a prompt to decide
    # whether the new core belongs in the archive at all, and to say so here
    # when you raise the number.
    zbytes=$(wc -c < "$Z")
    zcap=33554432
    [ "$zbytes" -le "$zcap" ] || {
        echo "FAIL: package is $zbytes bytes, over the $zcap cap"
        echo "      Something large was added to dist. Decide whether it"
        echo "      belongs in the archive, then either drop it or raise the"
        echo "      cap on purpose, with the reason written down."
        unzip -Z "$Z" | sort -k4 -n -r | head -5
        exit 1; }
    echo "ok: package $zbytes bytes, under the $zcap cap"

    echo "ok: packaging"

    # ------------------------------------------- the arcade instructions
    # These three assertions outlived the archive they were written for. They
    # used to run against `make fbneo-dist`'s own README inside a second zip;
    # arcade now ships in the one package and the same text ships as
    # .adds/koboy/README-fbneo.txt, so the assertions moved rather than went.
    # Each guards a failure that looks exactly like a broken emulator:
    #
    #   the FBNeo version   an arcade romset is version-matched, and a set
    #                       built for another release fails with the same
    #                       "couldn't find rom" a genuinely broken core gives
    #   hiscore.dat         the only save mechanism an arcade board HAS.
    #                       retro_get_memory_size(SAVE_RAM) is 0 on all 227
    #                       boards measured, so there is no .srm; high scores
    #                       go through FBNeo's own file, which koboy enables
    #                       (src/core.c) and the owner supplies
    #   the DIRECTORY       "put it in the system folder" is not actionable:
    #                       the core looks in an `fbneo` SUBdirectory that
    #                       does not exist until somebody makes it
    rdf=$(mktemp -d)
    unzip -qo "$Z" .adds/koboy/README-fbneo.txt -d "$rdf"
    grep -qF -- "1.0.0.03" "$rdf/.adds/koboy/README-fbneo.txt" \
        || { echo "FAIL: arcade README does not state the FBNeo version the set must match"; rm -rf "$rdf"; exit 1; }
    grep -qF -- "hiscore.dat" "$rdf/.adds/koboy/README-fbneo.txt" \
        || { echo "FAIL: arcade README does not name hiscore.dat"; rm -rf "$rdf"; exit 1; }
    grep -qF -- ".adds/koboy/fbneo/" "$rdf/.adds/koboy/README-fbneo.txt" \
        || { echo "FAIL: arcade README does not say where hiscore.dat goes"; rm -rf "$rdf"; exit 1; }
    # And the size lever, by filename, in the file a user reads when they are
    # looking at the koboy directory rather than at roms/.
    grep -qF -- "fbneo_libretro.so" "$rdf/.adds/koboy/README-fbneo.txt" \
        || { echo "FAIL: arcade README never names the file that can be deleted"; rm -rf "$rdf"; exit 1; }
    rm -rf "$rdf"
    echo "ok: arcade instructions"
fi

# ------------------------------------------------------- launcher assertions
#
# Every grep below runs over a COMMENT-STRIPPED SLICE of the script, never over
# the whole file, and matches the shell construct rather than a word that
# appears in it. Both rules come from the two vacuous assertions this section
# replaces:
#
#   for v in PLATFORM PRODUCT NICKEL_HOME; do grep -q "$v" scripts/koboy.sh; done
#     koboy.sh carries a 60-line comment explaining the environment gate, and
#     that comment names all three variables. Gutting the real gate down to one
#     variable -- or deleting it outright -- still matched all three greps.
#
#   grep -q 'fbdepth -d' scripts/koboy.sh
#     satisfied by the FORWARD switch to 8bpp (`fbdepth -d 8`) further down the
#     file, so deleting the depth restore from restore() still passed.
strip_comments() { grep -v '^[[:space:]]*#'; }

# The restore path must be a trap, not a trailing line: a crash must still bring
# Nickel back, or the user sees what looks like a bricked device.
strip_comments <"$SH" | grep -qE '^ *trap .* EXIT' || {
    echo "FAIL: no EXIT trap"; exit 1; }

# The forward switch to 8bpp. Named by its depth, so it cannot stand in for the
# restore and the restore cannot stand in for it.
strip_comments <"$SH" | grep -qE 'fbdepth +-d +8( |$)' || {
    echo "FAIL: no 'fbdepth -d 8' forward switch"; exit 1; }

# fbdepth has no restore flag: -r is --rota and takes an argument of its own, so
# a bare "fbdepth -r" exits 255 having changed nothing. The restore must name a
# depth with -d. (Measured on the device; the original brief had this wrong.)
if strip_comments <"$SH" | grep -qE 'fbdepth +-r *($|[^0-9-])'; then
    echo "FAIL: 'fbdepth -r' without an argument is not a depth restore"; exit 1
fi

# The restore itself: inside restore(), and putting back the depth that was
# REMEMBERED before anything changed it. That is what distinguishes it from the
# 8bpp switch above -- a different depth, in a different place.
RESTORE=$(sed -n '/^restore() {/,/^}$/p' "$SH" | strip_comments || true)
[ -n "$RESTORE" ] || { echo "FAIL: no restore() function"; exit 1; }
printf '%s\n' "$RESTORE" | grep -qE 'fbdepth +-d +"?\$ORIG_BPP"?' || {
    echo "FAIL: restore() does not put the framebuffer back to \$ORIG_BPP"; exit 1; }
printf '%s\n' "$RESTORE" | grep -q 'nickel' || {
    echo "FAIL: restore() does not restart Nickel"; exit 1; }

# Refusing to restart Nickel without Nickel's environment is the one safety
# property that cannot regress: a hand-started Nickel rewrites the device
# identity file, and only a reboot repairs it. Asserted against the gate block
# alone, so the essay above it cannot answer for it.
GATE=$(sed -n '/^missing=""/,/^fi$/p' "$SH" | strip_comments || true)
[ -n "$GATE" ] || { echo "FAIL: no environment gate"; exit 1; }
for v in PLATFORM PRODUCT NICKEL_HOME; do
    printf '%s\n' "$GATE" | grep -q "$v" || {
        echo "FAIL: env gate does not test $v"; exit 1; }
done
printf '%s\n' "$GATE" | grep -qE '\[ +-n +"\$missing" +\]' || {
    echo "FAIL: env gate does not act on what it found"; exit 1; }
printf '%s\n' "$GATE" | grep -qE '^ *exit 3' || {
    echo "FAIL: env gate does not refuse the launch"; exit 1; }
printf '%s\n' "$GATE" | grep -q -- '--message' || {
    echo "FAIL: refusal does not report on the panel"; exit 1; }

sh -n "$SH"
if [ -n "$SKIPPED" ]; then
    echo "PASS test_dist (launcher only -- packaging SKIPPED, no ${CROSS}gcc)"
else
    echo "PASS test_dist"
fi
