#!/bin/sh
# Device smoke test for koboy-probe's coexisting mode.
#
# Unlike tests/smoke_device.sh, this one is safe to run against a device that
# is actively being used: --coexist never stops Nickel, never changes
# framebuffer depth, and has no restore path -- so there is nothing here to
# reboot for afterwards, and no killall of Nickel/hindenburg/sickel at the
# top. The screen will flicker through refresh-timing test patterns for well
# under a minute; the reader UI is untouched underneath and Nickel keeps
# running the whole time.
set -e
: "${DEV:?set DEV=<device-ip>}"
R="ssh root@$DEV"

$R 'cd /mnt/onboard/.adds/koboy && ./koboy-probe --coexist'
out=$($R 'cat /mnt/onboard/koboy-probe-*.txt')
echo "$out"

for k in device= platform= panel= stride= wfm_fast_ms= touch_slots= touch_transpose=; do
    echo "$out" | grep -q "$k" || { echo "FAIL: missing $k"; exit 1; }
done

# THE acceptance criterion for coexisting mode: it must not have disturbed
# Nickel at all. If this fails, koboy-probe --coexist did something it
# structurally must never do.
$R 'pgrep -f /usr/local/Kobo/nickel >/dev/null' || { echo "FAIL: probe killed Nickel"; exit 1; }

echo "PASS smoke_probe"
