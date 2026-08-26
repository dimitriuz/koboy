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
             .adds/koboy/nm-koboy .adds/koboy/kfmon-koboy.ini \
             .adds/koboy/README.md .adds/koboy/TESTED.md \
             .adds/koboy/roms/README.txt; do
        unzip -Z1 "$Z" | grep -qx "$f" || { echo "FAIL: missing $f"; exit 1; }
    done

    # NO BIOS AND NO ROM, ever. Every core koboy ships either needs no BIOS or
    # links its own free one (PokeMini's freebios/, RACE's ngpBios.c) -- but a
    # dumped "[BIOS] Nintendo Pokemon Mini (World).min", or the boot.rom /
    # boot1.rom that sit beside a real WonderSwan or Neo Geo Pocket
    # collection, or an .nes that wandered in from a test directory, is not
    # ours to distribute, and a packaging step that copied one would look
    # exactly like a working build. `.rom` covers the two boot files by
    # extension. Checked on the zip's own listing, which is the artefact that
    # actually ships.
    if unzip -Z1 "$Z" | grep -qiE '\.(min|nes|gb|gbc|mgw|ws|wsc|ngp|ngc|srm|ngf|flash|rom)$'; then
        echo "FAIL: the package contains content or a BIOS:"
        unzip -Z1 "$Z" | grep -iE '\.(min|nes|gb|gbc|mgw|ws|wsc|ngp|ngc|srm|ngf|flash|rom)$'
        exit 1
    fi

    # The generated roms/README.txt must name every extension the browser
    # actually lists. It is the only instruction a user gets, and a system
    # whose files are accepted but never mentioned is a system nobody knows
    # to copy anything for. Extracted and read, not assumed from the Makefile.
    rd=$(mktemp -d)
    unzip -qo "$Z" .adds/koboy/roms/README.txt -d "$rd"
    for ext in .gb .gbc .mgw .nes .min .ws .wsc .ngp .ngc; do
        grep -qF -- "$ext" "$rd/.adds/koboy/roms/README.txt" \
            || { echo "FAIL: roms/README.txt does not mention $ext"; rm -rf "$rd"; exit 1; }
    done
    rm -rf "$rd"
    echo "ok: packaging"
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
