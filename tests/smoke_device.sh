#!/bin/sh
# Device smoke test. Requires DEV=<ip> and a deployed .adds/koboy.
#
# Nickel MUST be stopped first: it holds EVIOCGRAB on event0/1/2, so no other
# process can read input at all while it runs.
#
# This test reboots at the end, and that is a deliberate reversal: restarting
# Nickel in place works, but a Nickel started without rcS's full environment
# rewrites /mnt/onboard/.kobo/version and destroys the device identity FBInk
# depends on. See restore() for the measured before/after. A reboot is clean and
# repairs it; the in-place path is not safe to ship until someone verifies what
# Nickel writes when restarted by hand.
set -e
: "${DEV:?set DEV=<device-ip>}"
R="ssh root@$DEV"

# Bracket trick, and it is load-bearing rather than style: `pkill -f nickel`
# run over ssh matches the *remote shell's own command line*, because that
# command line contains the pattern. It kills itself before it kills Nickel.
# Observed: the ssh connection died mid-command with exit 255. The pattern
# below cannot match the command containing it.
NICKEL_PAT='/usr/local/Kobo/nicke[l]'

# hindenburg must go too, and this is not tidiness. MEASURED: killing only
# nickel and leaving hindenburg running got the device rebooted out from under
# us twice, with "PMU2: Watchdog timeout triggered" in dmesg -- hindenburg is
# Kobo's supervisor and takes a missing Nickel as a reason to restart the
# hardware. Killing nickel + sickel + hindenburg together, the device stayed up
# through 180 seconds of Nickel being absent (verified by an unchanged
# /proc/sys/kernel/random/boot_id and a monotonically rising uptime).

restore() {
    # 1. Framebuffer depth FIRST, and it must be "-d 32", NOT "-r".
    #    MEASURED: fbdepth's -r is --rota and *requires* an argument, so
    #    "fbdepth -r" exits 255 with "option requires an argument -- 'r'" and
    #    changes nothing. There is no restore flag; you name the depth you want.
    $R 'fbdepth -d 32 >/dev/null 2>&1 || true' || true

    # 2. Reboot, and this is a REVERSAL of the obvious approach. Restarting
    #    Nickel in place does work in the narrow sense -- with
    #    LD_LIBRARY_PATH=/usr/local/Kobo, NICKEL_HOME, LANG and a DBUS session
    #    address it comes up and stays up indefinitely, verified. Do not do it
    #    anyway. MEASURED consequence: rcS also exports PLATFORM, PRODUCT (from
    #    /bin/kobo_config.sh) and the NTX hardware identity, and a Nickel
    #    started without them REWRITES /mnt/onboard/.kobo/version with a
    #    placeholder serial and an empty device-code field:
    #
    #      before  N4181B1025136,4.1.15,4.38.23684,...,00000000-...-000000000388
    #      after   11:22:33:44:55:66,4.1.15,4.38.23684,...,
    #
    #    That file is how FBInk identifies the device. With it damaged, the
    #    device's own /usr/bin/fbink reported deviceName='Unknown!' deviceId=15
    #    devicePlatform='Mark ?' hasEclipseWfm=0 -- so every FBInk-based tool on
    #    the device, KOReader included, loses its per-device quirks, and koboy
    #    correctly stops offering DU4. It persists until a clean boot.
    #
    #    A reboot runs rcS with the full environment and repairs the file
    #    (verified: the serial and device code came back and hasEclipseWfm
    #    returned to 1). It costs the user ~30 seconds and cannot corrupt
    #    anything. Until someone reproduces rcS's environment exactly and
    #    verifies that Nickel writes correct persistent state, this is the only
    #    restore worth shipping.
    $R 'reboot' || echo "restore: WARNING could not reach $DEV to reboot"
    echo "restore: rebooting $DEV; Nickel and the device identity come back with rcS"
}

# Unconditional: a failed assertion below must still hand the device back.
trap restore EXIT

$R "pkill -f '$NICKEL_PAT' || true
    pkill -f '/usr/local/Kobo/sicke[l]' || true
    pkill -f '/usr/local/Kobo/hindenbur[g]' || true
    sleep 3"
# Two runs. The default one is what ships; the forced one keeps the original
# acceptance criterion ("wfm_fast=DU4") honest now that the shipped default is
# AUTO. AUTO is the default because forcing DU4 -- a non-flashing waveform that
# cannot erase -- left a Game Boy sprite's previous position on the panel, and
# the EPDC driver is the only party that can see which updates are erasing.
out=$($R 'cd /mnt/onboard/.adds/koboy && ./koboy --frames 300 --selftest 2>&1' || true)
echo "$out"
forced=$($R 'cd /mnt/onboard/.adds/koboy && ./koboy --frames 60 --selftest --waveform du4 2>&1' || true)

echo "$out" | grep -q "panel=1264x1680"  || { echo "FAIL: panel not detected"; exit 1; }
echo "$out" | grep -q "presented="        || { echo "FAIL: no frames presented"; exit 1; }
# The panel must really be DU4-capable: FBInk silently downgrades WFM_DU4 to
# GC4 without the hasEclipseWfm quirk, so this is the check that the capability
# is genuinely there rather than merely requested.
echo "$out" | grep -q "wfm_du4_capable=1" || { echo "FAIL: DU4 not available on this panel"; exit 1; }
echo "$out" | grep -q "wfm_fast=AUTO"     || { echo "FAIL: default fast waveform is not AUTO"; exit 1; }
echo "$forced" | grep -q "wfm_fast=DU4"   || { echo "FAIL: --waveform du4 did not select DU4"; exit 1; }
echo "PASS smoke_device"
