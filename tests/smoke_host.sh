#!/bin/sh
# Runs the host binary headless for 120 frames against the stub core and
# asserts it exits cleanly having presented at least one frame.
set -e
: "${ROM:=build/fake.gb}"
[ -f "$ROM" ] || printf '\0' > "$ROM"
out=$(SDL_VIDEODRIVER=dummy ./build/koboy --core build/stub_core.so \
        --rom "$ROM" --frames 120 --quiet 2>&1)
echo "$out"
echo "$out" | grep -q "presented=" || { echo "FAIL: no presentation counter"; exit 1; }
presented=$(echo "$out" | sed -n 's/.*presented=\([0-9]*\).*/\1/p')
[ "$presented" -ge 1 ] || { echo "FAIL: presented $presented frames"; exit 1; }
echo "PASS smoke_host presented=$presented"

# The browser is invisible to every other test in this suite, because they all
# pass --rom and take the MODE_PLAY shortcut. That is precisely the shape of
# the blind spot that hid v1's first-run deadlock through twenty reviews, so
# this run exists to take the other path.
romdir="$(mktemp -d)"
: > "$romdir/AAA TEST.gb"
script="$(mktemp)"
printf 'idle 2\ntap 40 200\n' > "$script"

out=$(SDL_VIDEODRIVER=dummy ./build/koboy --core build/stub_core.so \
        --rom-dir "$romdir" --ui-script "$script" \
        --panel 1264x1680 --frames 30 2>&1)
echo "$out"
echo "$out" | grep -q "chose $romdir/AAA TEST.gb" \
    || { echo "FAIL: browser did not select the only rom"; exit 1; }
echo "$out" | grep -q '^presented=' \
    || { echo "FAIL: run did not reach the emulator loop"; exit 1; }
rm -rf "$romdir" "$script"
echo "ok: rom browser"
