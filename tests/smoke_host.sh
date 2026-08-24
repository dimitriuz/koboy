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
