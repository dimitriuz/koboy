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

if [ -z "$1" ]; then
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
             .adds/koboy/nm-koboy .adds/koboy/kfmon-koboy.ini \
             .adds/koboy/README.md .adds/koboy/TESTED.md; do
        unzip -Z1 "$Z" | grep -qx "$f" || { echo "FAIL: missing $f"; exit 1; }
    done
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
echo "PASS test_dist"
