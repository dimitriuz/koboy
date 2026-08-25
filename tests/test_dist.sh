#!/bin/sh
# The packaging contract: unzip-to-install, no rootfs modification, and a launch
# script whose Nickel restore runs unconditionally.
set -e
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

# The restore path must be a trap, not a trailing line: a crash must still bring
# Nickel back, or the user sees what looks like a bricked device.
grep -q "trap .* EXIT" scripts/koboy.sh || { echo "FAIL: no EXIT trap"; exit 1; }
grep -q "fbdepth" scripts/koboy.sh      || { echo "FAIL: no 8bpp switch"; exit 1; }

# fbdepth has no restore flag: -r is --rota and takes an argument of its own, so
# a bare "fbdepth -r" exits 255 having changed nothing. The restore must name a
# depth with -d. (Measured on the device; the original brief had this wrong.)
if grep -qE '^[^#]*fbdepth +-r *($|[^0-9-])' scripts/koboy.sh; then
    echo "FAIL: 'fbdepth -r' without an argument is not a depth restore"; exit 1
fi
grep -q 'fbdepth -d' scripts/koboy.sh || { echo "FAIL: no 'fbdepth -d' restore"; exit 1; }

# Refusing to restart Nickel without Nickel's environment is the one safety
# property that cannot regress: a hand-started Nickel rewrites the device
# identity file, and only a reboot repairs it.
for v in PLATFORM PRODUCT NICKEL_HOME; do
    grep -q "$v" scripts/koboy.sh || { echo "FAIL: env gate does not check $v"; exit 1; }
done
grep -q -- '--message' scripts/koboy.sh || { echo "FAIL: refusal does not report on the panel"; exit 1; }

sh -n scripts/koboy.sh
echo "PASS test_dist"
