#!/bin/sh
# Device smoke test. Requires DEV=<ip> and a deployed .adds/koboy.
#
# Nickel MUST be stopped first: it holds EVIOCGRAB on event0/1/2, so no other
# process can read input at all while it runs.
#
# This test does NOT reboot. It stops Nickel, runs, and restarts Nickel --
# which is what KOReader does, and what scripts/koboy.sh relies on every time
# the user exits a game. `reboot` remains the escape hatch if a restart ever
# leaves Nickel visibly broken (see restore() below); it is not the default,
# because rebooting the user's e-reader after every test run is needlessly
# disruptive.
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
    #    MEASURED on the device: fbdepth's -r is --rota and *requires* an
    #    argument, so "fbdepth -r" exits 255 with
    #    "option requires an argument -- 'r'" and changes nothing. There is no
    #    restore flag; you name the depth you want. When this silently failed,
    #    the panel stayed at 8bpp and an explicitly restarted Nickel was gone
    #    again within ten seconds -- Nickel reads the depth at startup and does
    #    not survive one it does not expect.
    $R 'fbdepth -d 32 >/dev/null 2>&1 || true' || true
    # 2. Nickel's ENVIRONMENT, which is the part that is easy to miss and was
    #    measured the hard way. Started with a bare `nickel -platform kobo
    #    -skipFontLoad`, Nickel exits within five seconds every time: it is a Qt
    #    application whose libraries live in /usr/local/Kobo, and /etc/init.d/rcS
    #    exports LD_LIBRARY_PATH, NICKEL_HOME, LANG and a DBUS session address
    #    (rcS lines 324-337) long before it launches Nickel. With those four
    #    supplied, Nickel stayed up indefinitely. hindenburg goes first, since
    #    Nickel expects it. setsid so both survive this ssh session closing.
    $R 'export NICKEL_HOME=/mnt/onboard/.kobo
        export LD_LIBRARY_PATH=/usr/local/Kobo
        export LANG=en_US.UTF-8
        [ -p /tmp/nickel-hardware-status ] || mkfifo /tmp/nickel-hardware-status
        if [ -z "$DBUS_SESSION_BUS_ADDRESS" ]; then
            DBUS_SESSION_BUS_ADDRESS=$(/bin/dbus-daemon --session --print-address --fork 2>/dev/null)
            export DBUS_SESSION_BUS_ADDRESS
        fi
        pgrep -f "/usr/local/Kobo/hindenbur[g]" >/dev/null 2>&1 ||
            (setsid /usr/local/Kobo/hindenburg >/dev/null 2>&1 &)
        sleep 1
        (setsid env LIBC_FATAL_STDERR_=1 /usr/local/Kobo/nickel \
            -platform kobo -skipFontLoad >/dev/null 2>&1 &)
        sleep 8
        if pgrep -f "/usr/local/Kobo/nicke[l]" >/dev/null 2>&1; then
            echo "restore: nickel is running"
        else
            # The escape hatch, kept because it is the one thing that always
            # works. A Kobo reboot is clean; it just costs the user 30 seconds.
            echo "restore: nickel did not come back -- rebooting to recover"
            reboot
        fi' || echo "restore: WARNING could not reach $DEV to restore"
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
