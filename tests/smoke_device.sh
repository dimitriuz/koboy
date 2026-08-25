#!/bin/sh
# Device smoke test. Requires DEV=<ip> and a deployed .adds/koboy.
#
# Nickel MUST be stopped first: it holds EVIOCGRAB on event0/1/2, so no other
# process can read input at all while it runs.
#
# This test reboots the device at the end. That is deliberate: it is the
# cheapest way to put a Kobo whose Nickel was killed back into a known-good
# state, and a Kobo reboot is clean.
set -e
: "${DEV:?set DEV=<device-ip>}"
R="ssh root@$DEV"

$R 'pkill -f "/usr/local/Kobo/nickel" || true; sleep 2'
out=$($R 'cd /mnt/onboard/.adds/koboy && ./koboy --frames 300 --selftest 2>&1' || true)
echo "$out"
$R 'reboot' || true

echo "$out" | grep -q "panel=1264x1680" || { echo "FAIL: panel not detected"; exit 1; }
echo "$out" | grep -q "wfm_fast=DU4"     || { echo "FAIL: fast waveform is not DU4"; exit 1; }
echo "$out" | grep -q "presented="       || { echo "FAIL: no frames presented"; exit 1; }
echo "PASS smoke_device"
