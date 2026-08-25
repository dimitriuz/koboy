#!/bin/sh
# Device smoke test for koboy-probe's --takeover mode, and for the combined
# two-mode workflow docs/probe-readme.md and TESTED.md describe: run
# --coexist, then --takeover on the same device, and end up with ONE file
# carrying both sets of facts.
#
# Unlike tests/smoke_probe.sh, this one DOES stop Nickel -- --takeover is
# structurally unable to run otherwise, since Nickel's EVIOCGRAB means no
# other process can read a single input event while it's up -- and reboots
# at the end to restore it, for the same reason tests/smoke_device.sh does:
# restarting Nickel by hand outside rcS's own environment rewrites
# /mnt/onboard/.kobo/version with a placeholder identity, and only a reboot
# repairs it. Run this only when nobody is actively using the device.
set -e
: "${DEV:?set DEV=<device-ip>}"
R="ssh root@$DEV"

# Same bracket trick as smoke_device.sh, and for the same reason: a plain
# `pkill -f /usr/local/Kobo/nickel` run over ssh matches the remote shell's
# own command line (which contains the pattern) and kills itself first.
NICKEL_PAT='/usr/local/Kobo/nicke[l]'

restore() {
    $R 'reboot' || echo "restore: WARNING could not reach $DEV to reboot"
    echo "restore: rebooting $DEV; Nickel and the device identity come back with rcS"
}
trap restore EXIT

# Start from a clean slate so a stale file from a previous run cannot make
# this test pass by accident.
$R 'rm -f /mnt/onboard/koboy-probe-*.txt'

echo "== --coexist"
$R 'cd /mnt/onboard/.adds/koboy && ./koboy-probe --coexist'
coexist_out=$($R 'cat /mnt/onboard/koboy-probe-*.txt')
for k in device= platform= panel= stride= wfm_fast_ms= touch_slots= touch_transpose= \
         input_node_count= wfm_du4_capable=; do
    echo "$coexist_out" | grep -q "$k" || { echo "FAIL: coexist run missing $k"; exit 1; }
done

echo "== stopping Nickel for --takeover"
$R "pkill -f '$NICKEL_PAT' || true
    pkill -f '/usr/local/Kobo/sicke[l]' || true
    pkill -f '/usr/local/Kobo/hindenbur[g]' || true
    sleep 3"

echo "== --takeover"
$R 'cd /mnt/onboard/.adds/koboy && ./koboy-probe --takeover' || true
combined=$($R 'cat /mnt/onboard/koboy-probe-*.txt')
echo "$combined"

# THE regression this test exists to catch: --takeover must APPEND to the
# file --coexist wrote, not truncate it. Every coexist-only key must still
# be present after takeover ran, or the one pasteable TESTED.md artifact this
# tool exists to produce has just been destroyed.
for k in device= platform= panel= stride= wfm_fast_ms= touch_slots= touch_transpose= \
         input_node_count= wfm_du4_capable=; do
    echo "$combined" | grep -q "$k" || { echo "FAIL: takeover run destroyed coexist key $k"; exit 1; }
done
echo "$combined" | grep -q "mode=takeover"              || { echo "FAIL: no takeover block appended"; exit 1; }
echo "$combined" | grep -q "takeover_key_press_count="   || { echo "FAIL: missing takeover_key_press_count="; exit 1; }
echo "$combined" | grep -q "takeover_touch_contacts_seen=" || { echo "FAIL: missing takeover_touch_contacts_seen="; exit 1; }

echo "PASS smoke_probe_takeover"
