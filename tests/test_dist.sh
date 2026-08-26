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
    Z=$(ls dist/koboy-*.zip | head -1)
    [ -n "$Z" ] || { echo "FAIL: no zip produced"; exit 1; }

    # Nothing may install outside .adds/koboy: no root, no brick risk.
    if unzip -Z1 "$Z" | grep -v '^\.adds/koboy/'; then
        echo "FAIL: zip writes outside .adds/koboy/"; exit 1
    fi
    if unzip -Z1 "$Z" | grep -q 'KoboRoot'; then
        echo "FAIL: ships a KoboRoot.tgz"; exit 1
    fi

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
             .adds/koboy/nm-koboy .adds/koboy/kfmon-koboy.ini \
             .adds/koboy/README.md .adds/koboy/TESTED.md \
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
    BAD='\.(min|nes|gb|gbc|mgw|ws|wsc|ngp|ngc|a26|col|int|sms|gg|zip|srm|ngf|flash|rom|bin)$'
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
    for ext in .gb .gbc .mgw .nes .min .ws .wsc .ngp .ngc .a26 .col .int .sms .gg .zip; do
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
    # .zip is the first extension the browser lists whose CORE IS NOT IN THIS
    # PACKAGE, so listing the extension without saying where the core comes
    # from would be worse than not listing it: the user would see the row
    # appear and the load fail with nothing to act on. Asserted by the archive
    # name, not by a vague word like "separate".
    grep -qF -- "koboy-fbneo-" "$rd/.adds/koboy/roms/README.txt" \
        || { echo "FAIL: roms/README.txt lists .zip but never names the arcade archive"; rm -rf "$rd"; exit 1; }
    rm -rf "$rd"

    # THE ARCADE CORE IS NOT IN THE MAIN PACKAGE, and this is the assertion
    # that keeps it that way. 41 MB against the whole rest of koboy's 4 MB is
    # the entire reason `make fbneo-dist` exists; a stray `cp` in the dist rule
    # would inflate the download tenfold for every user who has no arcade
    # romset, and nothing else here would notice.
    if unzip -Z1 "$Z" | grep -q 'fbneo'; then
        echo "FAIL: the main package carries the arcade core -- it ships separately"
        unzip -Z1 "$Z" | grep 'fbneo'
        exit 1
    fi
    echo "ok: packaging"

    # ------------------------------------------------- the arcade package
    # Built and checked here rather than left to a human, for the reason the
    # main package is: a deliverable nothing verifies is a deliverable that
    # rots. The core itself is not rebuilt on every run (dist/fbneo_libretro.so
    # is a non-phony target, like every other core), so this costs one zip
    # after the first build.
    make fbneo-dist
    ZF=$(ls dist/koboy-fbneo-*.zip | head -1)
    [ -n "$ZF" ] || { echo "FAIL: no arcade zip produced"; exit 1; }
    if unzip -Z1 "$ZF" | grep -v '^\.adds/koboy/'; then
        echo "FAIL: arcade zip writes outside .adds/koboy/"; exit 1
    fi
    unzip -Z1 "$ZF" | grep -qx '.adds/koboy/fbneo_libretro.so' \
        || { echo "FAIL: arcade zip has no core in it"; exit 1; }
    # It must NOT duplicate the main package. The two are installed on top of
    # each other, so a second copy of koboy or koboy.ini here would overwrite
    # whatever the user already has -- including a koboy.ini they edited.
    for f in koboy koboy.sh koboy.ini koboy-probe; do
        if unzip -Z1 "$ZF" | grep -qx ".adds/koboy/$f"; then
            echo "FAIL: arcade zip duplicates $f from the main package"; exit 1
        fi
    done
    # No romset, by the same rule and the same regex as the main package.
    if unzip -Z1 "$ZF" | grep -qiE "$BAD"; then
        echo "FAIL: the arcade package contains content:"
        unzip -Z1 "$ZF" | grep -iE "$BAD"
        exit 1
    fi
    # The one instruction that decides whether a failed load is diagnosable:
    # an FBNeo romset is version-matched, and a set built for another release
    # fails exactly like a broken core.
    rdf=$(mktemp -d)
    unzip -qo "$ZF" .adds/koboy/README-fbneo.txt -d "$rdf"
    grep -qF -- "1.0.0.03" "$rdf/.adds/koboy/README-fbneo.txt" \
        || { echo "FAIL: arcade README does not state the FBNeo version the set must match"; rm -rf "$rdf"; exit 1; }
    # And the one save mechanism an arcade board HAS. There is no .srm here --
    # retro_get_memory_size(SAVE_RAM) is 0 on all 227 boards measured -- so
    # high scores go through FBNeo's own hiscore.dat, which koboy enables
    # (src/core.c) and the owner supplies. Named by FILE and by DIRECTORY,
    # because "put it in the system folder" is not actionable: the core looks
    # in a `fbneo` SUBdirectory that does not exist until someone makes it.
    grep -qF -- "hiscore.dat" "$rdf/.adds/koboy/README-fbneo.txt" \
        || { echo "FAIL: arcade README does not name hiscore.dat"; rm -rf "$rdf"; exit 1; }
    grep -qF -- ".adds/koboy/fbneo/" "$rdf/.adds/koboy/README-fbneo.txt" \
        || { echo "FAIL: arcade README does not say where hiscore.dat goes"; rm -rf "$rdf"; exit 1; }
    rm -rf "$rdf"
    echo "ok: arcade packaging"
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
