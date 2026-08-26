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
# TAP FIRST, deliberately: no leading `idle`. ui_list_init sets prev_touch =
# true so a fresh list demands a release before it accepts a tap, and a script
# beginning with `tap` therefore had its press swallowed and its release eaten
# as the priming edge -- selecting nothing and exiting 0, i.e. a CI run that
# went green having tested nothing. Confirmed on hardware with
# `printf 'tap 300 300\n'`. run_list now feeds one released state before the
# script's first entry; this line is what proves it, so do not "helpfully" put
# an `idle` back in front of it.
#
# y=90, not the original 200: this run takes no --panel, so the SDL backend's
# default 1072x1448 applies, and 90 is that geometry's row-0 centre under the
# CURRENT UI_MAX_ROWS=24 (KOBOY_CHROME_MARGIN=8 on every side, row_h=55,
# body top at y=63, centre at 63+55/2). It was ~200 back when UI_MAX_ROWS was
# 10 and rows were more than twice as tall; a row-density change is exactly
# the kind of thing that silently strands a hardcoded pixel coordinate
# outside every row, so if this ever goes stale again the failure here is
# "browser did not select the only rom", not something subtler.
printf 'tap 40 90\n' > "$script"

# Explicit rc capture, not the bare `out=$(...)` the run above uses: under
# set -e a nonzero exit from inside a command substitution assignment aborts
# the script AT THAT LINE, before the "$out" echo or any FAIL message below
# it ever runs. A prior mutant that made the browser ignore its script hung
# until the timeout below killed it (rc=124) and the script exited silently
# with no diagnostic at all -- `|| rc=$?` keeps the assignment out of the
# tail position of the AND-OR list so -e does not fire on it, and `timeout`
# turns "hangs forever" into "fails after 30s" so an unattended run reports a
# failure instead of never finishing.
rc=0
out=$(SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy --core build/stub_core.so \
        --rom-dir "$romdir" --ui-script "$script" \
        --panel 1264x1680 --frames 30 2>&1) || rc=$?
echo "$out"
if [ "$rc" -ne 0 ]; then
    echo "FAIL: browser run exited $rc (124 means it hit the 30s timeout)"
    rm -rf "$romdir" "$script"
    exit 1
fi
echo "$out" | grep -q "chose $romdir/AAA TEST.gb" \
    || { echo "FAIL: browser did not select the only rom"; rm -rf "$romdir" "$script"; exit 1; }
echo "$out" | grep -q '^presented=' \
    || { echo "FAIL: run did not reach the emulator loop"; rm -rf "$romdir" "$script"; exit 1; }
rm -rf "$romdir" "$script"
echo "ok: browser selects from a tap-first script"

# A bounded unattended run that chose NO rom must fail loudly. Exiting 0 there
# is how a scripted browser run reports success for having tested nothing --
# the same failure shape as the tap-first bug above, one layer out.
romdir="$(mktemp -d)"; : > "$romdir/AAA TEST.gb"
script="$(mktemp)"
printf 'idle 4\n' > "$script"
rc=0
out=$(SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy --core build/stub_core.so \
        --rom-dir "$romdir" --ui-script "$script" \
        --panel 1264x1680 --frames 30 2>&1) || rc=$?
echo "$out"
if [ "$rc" -eq 0 ]; then
    echo "FAIL: a script that selected nothing exited 0"
    rm -rf "$romdir" "$script"; exit 1
fi
if [ "$rc" -eq 124 ]; then
    echo "FAIL: a script that selected nothing hung until the 30s timeout"
    rm -rf "$romdir" "$script"; exit 1
fi
echo "$out" | grep -q '^presented=' && {
    echo "FAIL: a run that chose no rom still entered the emulator loop"
    rm -rf "$romdir" "$script"; exit 1; }
rm -rf "$romdir" "$script"
echo "ok: an unselecting script fails the run (rc=$rc)"

# The "+N MORE ROMS NOT SHOWN" row a truncated scan appends must not be
# selectable as if it were a real ROM -- run_list's disabled_index exists
# for exactly this, and nothing above exercises it: every other browser run
# in this file has too few ROMs to ever truncate. KOBOY_ROMLIST_CAP_TEST
# dials the cap down to something a test can reach in milliseconds (see
# src/romlist.c) instead of requiring 20000 real files.
romdir="$(mktemp -d)"
: > "$romdir/AAA.gb"; : > "$romdir/BBB.gb"; : > "$romdir/CCC.gb"
: > "$romdir/DDD.gb"; : > "$romdir/EEE.gb"
# With the cap at 2, only AAA.gb and BBB.gb survive, and the browser's third
# row (row index 2) is the synthetic overflow row -- see the y math below,
# which mirrors ui_list_init's own row_h derivation for a 1264x1680 panel
# with KOBOY_CHROME_MARGIN=8 on every side (h=1664, slots=26, row_h=64).
script="$(mktemp)"
printf 'tap 200 232\n' > "$script"   # row 2: y = 8 + 64 + 2*64 + 64/2
rc=0
out=$(KOBOY_ROMLIST_CAP_TEST=2 SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy \
        --core build/stub_core.so --rom-dir "$romdir" --ui-script "$script" \
        --panel 1264x1680 --frames 30 2>&1) || rc=$?
echo "$out"
if [ "$rc" -ne 4 ]; then
    echo "FAIL: tapping the overflow row exited $rc, wanted 4 (nothing real selected)"
    rm -rf "$romdir" "$script"; exit 1
fi
echo "$out" | grep -q '^presented=' && {
    echo "FAIL: tapping the overflow row still reached the emulator loop"
    rm -rf "$romdir" "$script"; exit 1; }
rm -rf "$romdir" "$script"
echo "ok: the overflow row cannot be picked as a rom (rc=$rc)"

# Positive control for the geometry above: the SAME cap and rom_dir, tapping
# row 0 instead, must select the real ROM there. Without this, the negative
# result above would be equally consistent with "the tap coordinates simply
# missed every row" as with "the overflow row correctly refused the tap".
romdir="$(mktemp -d)"
: > "$romdir/AAA.gb"; : > "$romdir/BBB.gb"; : > "$romdir/CCC.gb"
: > "$romdir/DDD.gb"; : > "$romdir/EEE.gb"
script="$(mktemp)"
printf 'tap 200 104\n' > "$script"   # row 0: y = 8 + 64 + 0*64 + 64/2
rc=0
out=$(KOBOY_ROMLIST_CAP_TEST=2 SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy \
        --core build/stub_core.so --rom-dir "$romdir" --ui-script "$script" \
        --panel 1264x1680 --frames 30 2>&1) || rc=$?
echo "$out"
if [ "$rc" -ne 0 ]; then
    echo "FAIL: tapping row 0 exited $rc (124 means it hit the 30s timeout)"
    rm -rf "$romdir" "$script"; exit 1
fi
echo "$out" | grep -q "chose $romdir/AAA.gb" \
    || { echo "FAIL: row 0 did not select AAA.gb"; rm -rf "$romdir" "$script"; exit 1; }
rm -rf "$romdir" "$script"
echo "ok: row 0 still selects normally at the same geometry"

# The menu is reachable only through a live touch (input_take_menu_request is
# fed by pf->poll_input, and MODE_PLAY's poll loop has no --ui-script hook the
# way MODE_BROWSE's run_list does), so unlike the browser above this run does
# NOT open or drive the menu -- it only proves the menu-capable build still
# runs a normal --rom session end to end. Driving MODE_MENU from a script
# would need the emulator loop to accept --ui-script too, which is deferred.
#
# Same explicit rc capture and timeout as the browser run above, for the same
# reason: under `set -e`, a bare `out=$(...)` aborts the script AT THAT LINE
# on a nonzero exit, before any FAIL message below it ever runs, and a hang
# here would otherwise block the suite forever instead of failing it.
romdir="$(mktemp -d)"; : > "$romdir/AAA TEST.gb"
rc=0
out=$(SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy --core build/stub_core.so \
        --rom "$romdir/AAA TEST.gb" --panel 1264x1680 --frames 60 2>&1) || rc=$?
echo "$out"
if [ "$rc" -ne 0 ]; then
    echo "FAIL: menu-capable build exited $rc (124 means it hit the 30s timeout)"
    rm -rf "$romdir"
    exit 1
fi
echo "$out" | grep -q '^presented=' \
    || { echo "FAIL: menu-capable build did not run"; rm -rf "$romdir"; exit 1; }
rm -rf "$romdir"
echo "ok: menu build runs"
