#!/bin/sh
# koboy launcher.
#
# Nickel must be stopped for koboy to run at all: it holds EVIOCGRAB on every
# event node, so while it is up nothing else can read a single key or touch.
# Stopping it also frees the framebuffer, which lets us drop the panel to 8bpp
# -- a quarter of the memory bandwidth per refresh, and free of charge because
# the pipeline already produces gray8.
#
# Everything hard about this script is the way back. Nickel is not a program you
# can simply re-exec; see restore() and the environment gate below.
#
# Copyright (C) 2026 the koboy authors. GPLv3 or later.

PATH="/sbin:/bin:/usr/sbin:/usr/bin:/usr/lib:"

DIR=$(cd "$(dirname "$0")" 2>/dev/null && pwd) || DIR=$(dirname "$0")
LOG="$DIR/koboy.log"
KOBOY="$DIR/koboy"
INI="$DIR/koboy.ini"

# One rotation, so a device left running for months does not fill the user's
# library partition with a log nobody reads.
if [ -f "$LOG" ] && [ "$(wc -c <"$LOG" 2>/dev/null || echo 0)" -gt 262144 ]; then
    mv -f "$LOG" "$LOG.1"
fi

log() { echo "$(date '+%F %T') $*" >>"$LOG"; }

log "=== koboy.sh start (pid $$, dir $DIR)"

# ------------------------------------------------------------ environment gate
#
# THE most important check in this file. koboy restarts Nickel when the game
# exits, and restarting Nickel is only safe from a process that inherited
# Nickel's own environment -- which is exactly what a launch from NickelMenu or
# KFMon gives us, because both spawn us from inside Nickel.
#
# rcS exports PLATFORM and PRODUCT (from /bin/kobo_config.sh) and the NTX
# hardware identity before Nickel is started, and MEASURED on this project's
# Libra 2: a Nickel started WITHOUT them rewrote /mnt/onboard/.kobo/version,
#
#   before  N4181B1025136,4.1.15,4.38.23684,...,00000000-...-000000000388
#   after   11:22:33:44:55:66,4.1.15,4.38.23684,...,
#
# replacing the serial with a placeholder and emptying the device-code field.
# That file is how FBInk identifies the device, so afterwards the device's own
# fbink reported deviceName='Unknown!', devicePlatform='Mark ?' and
# hasEclipseWfm=0 -- every FBInk-based tool on the device, KOReader included,
# silently lost its per-device quirks. Only a reboot repairs it.
#
# A shell over ssh has none of that environment (measured: HOME, LOGNAME, PATH,
# PWD, SHELL, SHLVL, SSH_*, USER and nothing else). So an ssh launch is refused
# here, before Nickel is touched, rather than being allowed to damage the device
# identity on its way out. It is reconstructible in principle -- PLATFORM and
# PRODUCT can be read back out of udevd's environ -- but nobody has verified
# what else a hand-assembled Nickel writes to persistent state, and the cost of
# being wrong is a device that needs a reboot to identify itself again.
missing=""
for v in PLATFORM PRODUCT NICKEL_HOME; do
    eval "val=\${$v}"
    [ -n "$val" ] || missing="$missing $v"
done

if [ -n "$missing" ]; then
    log "REFUSED: not launched from Nickel; missing env:$missing"
    log "         Nickel is untouched and still running. Launch koboy from the"
    log "         Kobo's own menu (NickelMenu/KFMon), not from a shell."
    # Same on-panel error path as every other fatal: on a device with no
    # terminal, an error that reaches only this log is invisible.
    # Newlines are explicit, and the lines are short. FBInk wraps at the column
    # edge, not at word boundaries -- MEASURED: 38 columns at this font size on
    # a 1264px-wide panel, so a 42-character line came out as "...the Kobo m /
    # enu,". Keeping every line under ~30 characters leaves room for panels
    # narrower than this one.
    "$KOBOY" --config "$INI" --message "$(printf \
        'Start koboy from the Kobo\nmenu, not from a shell.\nNickel was left running.')" \
        >>"$LOG" 2>&1
    log "=== koboy.sh refused, rc=3"
    exit 3
fi

log "env ok: PLATFORM=$PLATFORM PRODUCT=$PRODUCT NICKEL_HOME=$NICKEL_HOME"
log "        WIFI_MODULE=${WIFI_MODULE:-unset} INTERFACE=${INTERFACE:-unset}"

# ----------------------------------------------------------------- one at once
# NickelMenu spawns a second copy without complaint if the entry is tapped
# twice, and on e-ink a second tap is *likely*: the menu closes silently and
# nothing visible happens for a second. Two koboys would fight over the
# framebuffer and the input grabs, and then each would start Nickel on the way
# out, leaving two of those. mkdir is the atomic claim; a plain -e test is not.
LOCK=/tmp/koboy.lock
if ! mkdir "$LOCK" 2>/dev/null; then
    other=$(cat "$LOCK/pid" 2>/dev/null)
    if [ -n "$other" ] && [ -d "/proc/$other" ]; then
        log "REFUSED: koboy is already running as pid $other"
        exit 4
    fi
    # Nobody holds it: a SIGKILL of the script itself skips the traps and leaves
    # the directory behind, and /tmp is only cleared at boot.
    log "clearing a stale lock left by pid ${other:-unknown}"
    rm -rf "$LOCK"
    mkdir "$LOCK" 2>/dev/null || { log "REFUSED: cannot take $LOCK"; exit 4; }
fi
echo $$ >"$LOCK/pid"

# ------------------------------------------------------------------- restore
#
# Remembered before anything changes it, so the panel goes back to exactly the
# depth and rotation Nickel was using rather than to an assumed 32bpp/portrait.
ORIG_BPP=$(fbdepth -g 2>/dev/null)
ORIG_ROTA=$(fbdepth -o 2>/dev/null)
case "$ORIG_BPP"  in ''|*[!0-9]*) ORIG_BPP=32 ;; esac
case "$ORIG_ROTA" in ''|*[!0-9]*) ORIG_ROTA=-1 ;; esac
log "framebuffer was ${ORIG_BPP}bpp rota=$ORIG_ROTA"

wifi_down() {
    # Nickel does not cope with the radio being up while it starts (its own
    # WiFi state machine expects to bring the module up itself), so it goes
    # down before Nickel comes back. KOBOY_KEEP_WIFI=1 skips this: developers
    # test this script over ssh, and ssh on this hardware runs over the very
    # interface being torn down.
    [ -n "$WIFI_MODULE" ] || return 0
    grep -q "^$WIFI_MODULE" /proc/modules 2>/dev/null || return 0
    if [ "$KOBOY_KEEP_WIFI" = "1" ]; then
        # Do not use this for anything but a development session. MEASURED: with
        # the radio left up, the restarted Nickel logged
        #   kmod_module_insert_module: Failed to insert module '8723ds.ko': File exists
        # -- it tries to load the driver itself and finds it already there -- and
        # the device rebooted on its own about three minutes later.
        log "restore: KOBOY_KEEP_WIFI=1, leaving the radio up (development only)"
        return 0
    fi
    log "restore: taking WiFi down ($WIFI_MODULE on ${INTERFACE:-wlan0})"
    if [ -x /sbin/dhcpcd ]; then
        env -u LD_LIBRARY_PATH dhcpcd -d -k "${INTERFACE:-wlan0}" >/dev/null 2>&1
    fi
    killall -q -TERM udhcpc default.script dhcpcd dhcpcd-dbus
    wpa_cli terminate >/dev/null 2>&1
    ifconfig "${INTERFACE:-wlan0}" down 2>/dev/null
    usleep 250000 2>/dev/null || sleep 1
    # Kobo's busybox rmmod is modprobe -r in disguise; the firmware uses rmmod,
    # so do the same rather than inventing a second convention.
    rmmod "$WIFI_MODULE" 2>/dev/null
    if grep -q '^sdio_wifi_pwr' /proc/modules 2>/dev/null; then
        usleep 250000 2>/dev/null || sleep 1
        rmmod sdio_wifi_pwr 2>/dev/null
    fi
}

restored=""
restore() {
    # Idempotent: the signal traps below call this and then exit, which fires
    # the EXIT trap as well. Restarting Nickel twice would leave two of it.
    [ -n "$restored" ] && return 0
    restored=1

    # 1. THE PANEL FIRST, before Nickel can draw into it. If anything left the
    #    framebuffer at 8bpp, Nickel renders into a depth it does not expect.
    #    Note "-d", not "-r": fbdepth's -r is --rota and requires an argument
    #    of its own -- "fbdepth -r" exits 255 having changed nothing.
    if fbdepth -d "$ORIG_BPP" -r "$ORIG_ROTA" >>"$LOG" 2>&1; then
        log "restore: framebuffer back to ${ORIG_BPP}bpp rota=$ORIG_ROTA (now $(fbdepth -g 2>/dev/null)bpp)"
    else
        rc=$?
        log "restore: WARNING fbdepth -d $ORIG_BPP -r $ORIG_ROTA failed rc=$rc"
        fbdepth -d 32 >>"$LOG" 2>&1 || log "restore: WARNING fallback to 32bpp failed too"
    fi

    # 2. The one late rcS export a launch from Nickel does not already carry:
    #    Nickel's own Qt libraries live here and it will not start without them.
    export LD_LIBRARY_PATH="/usr/local/Kobo"
    export QT_GSTREAMER_PLAYBIN_AUDIOSINK="alsasink"
    export QT_GSTREAMER_PLAYBIN_AUDIOSINK_DEVICE_PARAMETER="bluealsa:DEV=00:00:00:00:00:00"

    # 3. Back to / before Nickel is started. Nickel is started from / at boot,
    #    and a stale working directory (or OLDPWD) on the user partition is
    #    what makes USB mass storage misbehave later: the partition cannot be
    #    unmounted while a process holds a directory on it open.
    cd / 2>/dev/null || true
    unset OLDPWD

    # 4. Radio down before Nickel starts.
    wifi_down

    # 5. Recreate the hardware-status FIFO. udev writes device events into it
    #    and Nickel is the reader; it is removed while koboy runs precisely
    #    because a FIFO with no reader blocks its writers, and it must exist
    #    again before Nickel starts looking for events.
    rm -f /tmp/nickel-hardware-status
    mkfifo /tmp/nickel-hardware-status 2>>"$LOG" || log "restore: WARNING mkfifo failed"

    sync

    # 6. An external SD card must be unmounted BY US, counter-intuitive as that
    #    is: Nickel shows an "unrecognised filesystem" complaint about a card it
    #    finds already mounted. The udevadm trigger below then enqueues one add
    #    event, which Nickel consumes and remounts the card itself. The internal
    #    /mnt/onboard is NOT touched -- this script lives on it.
    if [ -e /dev/mmcblk1p1 ] && grep -q ' /mnt/sd ' /proc/mounts 2>/dev/null; then
        log "restore: unmounting /mnt/sd for Nickel to re-detect"
        umount /mnt/sd 2>>"$LOG"
    fi

    # 7. Nickel. hindenburg is Kobo's supervisor and goes back first: MEASURED,
    #    leaving it dead while Nickel is missing got the device rebooted out
    #    from under us by the watchdog ("PMU2: Watchdog timeout triggered").
    log "restore: starting hindenburg and nickel"
    /usr/local/Kobo/hindenburg >>"$LOG" 2>&1 &
    LIBC_FATAL_STDERR_=1 /usr/local/Kobo/nickel -platform kobo -skipFontLoad >>"$LOG" 2>&1 &

    # 8. Replay the device events Nickel missed while it was gone.
    udevadm trigger >>"$LOG" 2>&1 &

    # 9. The boot animation script, because that is what a KFMon-style watcher
    #    waits on to know the launch finished. Nickel kills it on startup, so
    #    there is nothing here to reap.
    ( /etc/init.d/on-animator.sh >/dev/null 2>&1 ) &

    log "restore: done"
}

finish() {
    restore
    rm -rf "$LOCK"
}

# Unconditional. A crash, an assertion, a kill -TERM: Nickel comes back either
# way, because the alternative is a user staring at a dead panel and reaching
# for the reset paperclip.
trap 'finish' EXIT
trap 'log "signal INT";  finish; exit 130' INT
trap 'log "signal TERM"; finish; exit 143' TERM

# --------------------------------------------------------------- stop Nickel
#
# By name, via killall, and deliberately not `pkill -f /usr/local/Kobo/nickel`:
# a pattern passed to pkill -f matches the command line of the shell running it
# too, so that form kills its own launcher before it kills Nickel.
log "stopping Nickel"
killall -q -TERM nickel hindenburg sickel fickel strickel fontickel \
                 adobehost foxitpdf iink
# Wait for it to actually be gone; starting on top of a dying Nickel means
# fighting it for the framebuffer and the input grabs.
i=0
while pkill -0 nickel 2>/dev/null; do
    [ "$i" -ge 40 ] && { log "WARNING nickel still alive after 10s"; break; }
    usleep 250000 2>/dev/null || sleep 1
    i=$((i + 1))
done
log "Nickel stopped after $((i * 250))ms"

# See restore() step 5: with no reader, a writer on this FIFO blocks, and udev
# is not a process to leave blocked.
rm -f /tmp/nickel-hardware-status

# --------------------------------------------------------------------- 8bpp
# A bandwidth optimisation, not a precondition: at 8bpp a refresh moves a
# quarter of the bytes, and the pipeline already outputs gray8. If it fails,
# the native depth is perfectly playable, so this is a warning and not an exit.
if fbdepth -d 8 >>"$LOG" 2>&1; then
    log "framebuffer at $(fbdepth -g 2>/dev/null)bpp"
else
    log "WARNING could not switch to 8bpp; continuing at ${ORIG_BPP}bpp"
fi

# ---------------------------------------------------------------------- play
log "exec: $KOBOY --config $INI"
"$KOBOY" --config "$INI" >>"$LOG" 2>&1
rc=$?
log "koboy exited rc=$rc"

# restore() runs from the EXIT trap; nothing to call here.
exit "$rc"
