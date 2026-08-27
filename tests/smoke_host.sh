#!/bin/sh
# Runs the host binary headless for 120 frames against the stub core and
# asserts it exits cleanly having presented at least one frame.
set -e
: "${ROM:=build/fake.gb}"
[ -f "$ROM" ] || printf '\0' > "$ROM"
# Build first, and this is load-bearing rather than convenience. Every check
# below runs ./build/koboy, which `make test` does NOT rebuild -- it builds the
# test binaries only. A source edit followed by a bare `sh tests/smoke_host.sh`
# therefore exercises the PREVIOUS binary, silently: observed while adding the
# legacy-ini check below, which "failed" against a build predating the change
# it was written for and passed the moment the binary caught up. A smoke test
# that can report on code that is not in the binary is worse than no smoke
# test, because it is believed. Set SMOKE_NO_BUILD=1 to skip (for a caller
# that has already built, or is testing a binary on purpose).
[ -n "${SMOKE_NO_BUILD:-}" ] || make host >/dev/null
# Explicit rc capture rather than a bare `out=$(...)`, for the reason spelled
# out at length further down: under `set -e` a nonzero exit inside a command
# substitution assignment aborts the script AT THIS LINE, before the echo or
# any FAIL message below. This was the FIRST run in the file and therefore the
# first to notice any breakage, so a mutant anywhere in startup killed the
# whole suite with no diagnostic whatsoever -- confirmed while mutation-testing
# core selection, where `--core` losing its explicit flag made this run fail to
# open a core and the suite exit 1 having printed nothing at all.
rc=0
out=$(SDL_VIDEODRIVER=dummy ./build/koboy --core build/stub_core.so \
        --rom "$ROM" --frames 120 --quiet 2>&1) || rc=$?
echo "$out"
[ "$rc" -eq 0 ] || { echo "FAIL: baseline run exited $rc"; exit 1; }
echo "$out" | grep -q "presented=" || { echo "FAIL: no presentation counter"; exit 1; }
presented=$(echo "$out" | sed -n 's/.*presented=\([0-9]*\).*/\1/p')
[ "$presented" -ge 1 ] || { echo "FAIL: presented $presented frames"; exit 1; }
echo "PASS smoke_host presented=$presented"

# ---------------------------------------------------------------- pacing
# koboy paced EVERY system at the Game Boy's 59.7275 Hz until this check
# existed (docs/FOLLOWUPS.md #38, #57 -- two FinalBurn Neo boards report 30
# fps and therefore ran at double speed). The unit tests in test_pacing.c
# prove the pacer honours whatever rate it is handed and that the conversion
# from the core's fps is right; NOTHING in them can prove main.c actually
# hands the pacer the core's number rather than the constant, because main.c
# has no unit test. That wiring is exactly the "code path every automated test
# takes the other way round" shape this project has been bitten by, so it is
# checked here, end to end, with a stopwatch.
#
# KOBOY_STUB_FPS makes the stub core report 20 fps (tests/stub_core.c reads
# it). 30 frames at 20 fps is 1.5 s of deliberate sleeping; at the Game Boy's
# 59.7275 it would be 0.50 s. The threshold sits between the two with room on
# both sides, so this discriminates the wiring without being a flaky timing
# test: the gap it has to resolve is a factor of three.
pace_run() {
    t0=$(date +%s%N)
    KOBOY_STUB_FPS="$1" SDL_VIDEODRIVER=dummy ./build/koboy \
        --core build/stub_core.so --rom "$ROM" --frames 30 --quiet >/dev/null 2>&1 \
        || { echo "FAIL: pacing run at $1 fps exited nonzero"; exit 1; }
    t1=$(date +%s%N)
    echo $(( (t1 - t0) / 1000000 ))
}
slow_ms=$(pace_run 20)
fast_ms=$(pace_run 59.7275)
[ "$slow_ms" -ge 1200 ] || {
    echo "FAIL: 30 frames at a core-reported 20 fps took ${slow_ms}ms, expected >=1200ms"
    echo "      (main.c is pacing at KOBOY_FRAME_US instead of the core's rate)"
    exit 1; }
[ "$fast_ms" -le 900 ] || {
    echo "FAIL: 30 frames at 59.7275 fps took ${fast_ms}ms, expected <=900ms"
    exit 1; }
echo "PASS smoke_host pacing 20fps=${slow_ms}ms 59.7275fps=${fast_ms}ms"

# ...and the MID-RUN change, which is the other half of the same claim.
# SET_SYSTEM_AV_INFO carries timing as well as geometry, and main.c re-paces
# from the branch it already polls for geometry. KOBOY_STUB_FPS_LATE makes the
# stub announce a new rate from inside retro_run() at frame 10.
# 40 frames = 10 at 60 fps (0.17 s) + 30 at 15 fps (2.0 s) = ~2.2 s. If the
# announcement is ignored the whole run is 40 frames at 60 fps = 0.67 s.
t0=$(date +%s%N)
KOBOY_STUB_FPS=60 KOBOY_STUB_FPS_LATE=15 SDL_VIDEODRIVER=dummy ./build/koboy \
    --core build/stub_core.so --rom "$ROM" --frames 40 --quiet >/dev/null 2>&1 \
    || { echo "FAIL: mid-run pacing change run exited nonzero"; exit 1; }
t1=$(date +%s%N)
late_ms=$(( (t1 - t0) / 1000000 ))
[ "$late_ms" -ge 1600 ] || {
    echo "FAIL: a mid-run drop to 15 fps took ${late_ms}ms, expected >=1600ms"
    echo "      (SET_SYSTEM_AV_INFO's timing half is being ignored)"
    exit 1; }
echo "PASS smoke_host mid-run repace=${late_ms}ms"

# ------------------------------------------------------- non-square pixels
# The stub's geometry is the Game Boy's 160x144, base AND max. Told to report
# a 4:3 DISPLAY aspect for it, the pixels become 1.2:1, the reserved rect goes
# from 160 source columns wide to 192, and at the Game Boy's scale 5 the game
# rect goes from 800x720 to 960x720.
#
# THIS RUN IS THE ONLY THING THAT COVERS main.c's WIRING. The unit tests prove
# video_pixel_aspect, video_fit_par and config_resolve_profile_par; none of
# them can prove main.c asks the core for an aspect and passes it on, because
# main.c has no unit test. It also covers the one staleness case that is
# invisible to the geometry test on either side of it: base and max here are
# EXACTLY the placeholder profile's numbers, so the only thing that can make
# the startup profile stale is the aspect itself.
rc=0
out=$(KOBOY_STUB_ASPECT=1.33333 SDL_VIDEODRIVER=dummy ./build/koboy \
        --core build/stub_core.so --rom "$ROM" --panel 1264x1680 --frames 3 2>&1) || rc=$?
[ "$rc" -eq 0 ] || { echo "FAIL: non-square run exited $rc"; echo "$out"; exit 1; }
echo "$out" | grep -q "game 960x720" || {
    echo "FAIL: a 4:3 display aspect on a 160x144 core did not widen the game rect"
    echo "$out" | grep -E "game [0-9]+x[0-9]+"
    exit 1; }
# ...and the same core WITHOUT the aspect is the 800x720 the Game Boy has
# always had, so the check above is measuring the aspect and not the weather.
out=$(SDL_VIDEODRIVER=dummy ./build/koboy --core build/stub_core.so \
        --rom "$ROM" --panel 1264x1680 --frames 3 2>&1) || { echo "FAIL: square run"; exit 1; }
echo "$out" | grep -q "game 800x720" || {
    echo "FAIL: a square-pixel core no longer gets the Game Boy's 800x720 rect"
    echo "$out" | grep -E "game [0-9]+x[0-9]+"
    exit 1; }
echo "PASS smoke_host pixel aspect 800x720 square / 960x720 at 4:3"

# ...and `pixel_aspect = false` puts it back, end to end. The correction
# changed the presentation of EIGHT systems and none of it has been seen on an
# e-ink panel, so there has to be a way back that is not a rebuild -- the same
# argument that made gray_map a key. Asserted through the real binary because
# the value is read in main.c: config.c could parse the key perfectly and
# main.c still ignore it, which is the wire this file exists to test.
d_pa=$(mktemp -d)
cat > "$d_pa/off.ini" <<EOF
pixel_aspect = false
EOF
rc=0
out=$(KOBOY_STUB_ASPECT=1.33333 SDL_VIDEODRIVER=dummy ./build/koboy \
        --config "$d_pa/off.ini" --core build/stub_core.so --rom "$ROM" \
        --panel 1264x1680 --frames 3 2>&1) || rc=$?
[ "$rc" -eq 0 ] || { echo "FAIL: pixel_aspect=false run exited $rc"; echo "$out"; rm -rf "$d_pa"; exit 1; }
# Asserted as the ABSENCE of the widened rect, not the presence of the square
# one. "game 800x720" is also what the PLACEHOLDER profile line says before the
# core's geometry is known, and that line is printed on every run whatever the
# aspect -- so grepping for it passes with the key honoured, ignored, or never
# parsed at all. Confirmed: both mutants (main.c ignoring the key, config.c
# never parsing it) sailed through that version of this check.
#
# 960x720 appears only on the re-resolved line, which is emitted only when the
# aspect actually widens the rect. Its absence is therefore the thing that can
# only be true when the key was read AND honoured, and the positive half is
# already asserted twenty lines above.
echo "$out" | grep -q "game 960x720" && {
    echo "FAIL: pixel_aspect=false still widened the rect"
    echo "$out" | grep -E "game [0-9]+x[0-9]+"
    rm -rf "$d_pa"; exit 1; }
echo "$out" | grep -q "presented=" || {
    echo "FAIL: pixel_aspect=false run never reached the emulator loop"
    rm -rf "$d_pa"; exit 1; }
rm -rf "$d_pa"
echo "ok: pixel_aspect = false restores the square-pixel presentation"

# The startup flow (MAIN MENU -> ALL GAMES -> the browser) is invisible to
# every other test in this suite, because they all pass --rom and take the
# MODE_PLAY fast path straight past it. That is precisely the shape of the
# blind spot that hid v1's first-run deadlock through twenty reviews, so this
# run exists to take the other path -- now TWO screens deep instead of one,
# since task 5 put a MAIN MENU in front of the browser.
romdir="$(mktemp -d)"
: > "$romdir/AAA TEST.gb"
script="$(mktemp)"
# TAP FIRST, deliberately: no leading `idle`. ui_list_init sets prev_touch =
# true so a fresh list demands a release before it accepts a tap, and a script
# beginning with `tap` therefore had its press swallowed and its release eaten
# as the priming edge -- selecting nothing and exiting 0, i.e. a CI run that
# went green having tested nothing. Confirmed on hardware with
# `printf 'tap 300 300\n'`. run_list now feeds one released state before the
# script's first entry, on EVERY screen it drives (see its script_i comment),
# which is what lets two taps in a row -- one per screen below -- both land.
#
# Both coordinates are this run's --panel 1264x1680 geometry: row_h=64
# (KOBOY_CHROME_MARGIN=8 on every side, h=1664, 26 slots of 64px under the
# CURRENT UI_MAX_ROWS=24), so row r's centre is 8 + 64 + r*64 + 64/2.
#   tap 200 168  -- MAIN MENU row 1 ("ALL GAMES"; row 0 is "RECENT")
#   tap 200 104  -- the browser's row 0, the only rom in $romdir
# A row-density or MAIN MENU item-order change is exactly the kind of thing
# that silently strands a hardcoded pixel coordinate outside every row, so if
# this ever goes stale again the failure here is "did not select the only
# rom", not something subtler.
#
# $romdir is deliberately FLAT. Folder rows sort above files (src/romlist.c),
# so a subdirectory here would push the ROM off row 0 and this tap would
# descend into a folder instead -- and, since descending is a perfectly
# ordinary thing for the browser to do, the run would go on to exit 4 rather
# than say anything about a stale coordinate. The grep below is what actually
# discriminates: it names the exact path, so selecting anything else fails.
# Folder navigation gets its own runs further down.
printf 'tap 200 168\ntap 200 104\n' > "$script"

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

# ------------------------------------------------- folder navigation
# The browser lists ONE directory and descends into subdirectory rows, which
# is invisible to every run above (they all use a flat $romdir). Two runs,
# because "descending works" and "coming back up works" fail independently --
# and the second is a genuine discriminator rather than a repeat: it taps the
# SAME row index in the subdirectory as the first, and only lands on a
# different file if ".." really went up a level.
#
# Row geometry is the same 1264x1680 derivation as every other browser tap in
# this file (row_h=64, row r's centre is 8 + 64 + r*64 + 32), aimed at rows
# whose contents are pinned by the sort order src/romlist.c guarantees:
#   root:   row 0 "Game and Watch/"   row 1 "AAA TEST.gb"   (dirs first)
#   sub:    row 0 ".."                row 1 "BALL.mgw"      (".." first)
romdir="$(mktemp -d)"
: > "$romdir/AAA TEST.gb"
mkdir "$romdir/Game and Watch"
: > "$romdir/Game and Watch/BALL.mgw"
script="$(mktemp)"
#   tap 200 168 -- MAIN MENU row 1, ALL GAMES
#   tap 200 104 -- browser root row 0, the "Game and Watch/" folder
#   tap 200 168 -- subdirectory row 1, BALL.mgw (row 0 is "..")
printf 'tap 200 168\ntap 200 104\ntap 200 168\n' > "$script"
rc=0
out=$(SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy --core build/stub_core.so \
        --rom-dir "$romdir" --ui-script "$script" \
        --panel 1264x1680 --frames 30 2>&1) || rc=$?
echo "$out"
if [ "$rc" -ne 0 ]; then
    echo "FAIL: folder-descent run exited $rc (124 means it hit the 30s timeout)"
    rm -rf "$romdir" "$script"; exit 1
fi
# THE COMPATIBILITY ASSERTION, end to end: a ROM inside a subdirectory must
# resolve to the same path the flattened list produced -- rom_dir + "/" +
# "Game and Watch/BALL.mgw" -- because that string is what the .srm, the save
# states and recent.dat are all keyed on. A user with a save on the device
# loses it silently if this ever changes: the game loads, with no progress.
echo "$out" | grep -q "chose $romdir/Game and Watch/BALL.mgw" \
    || { echo "FAIL: descending into a folder did not select the rom inside it"; \
         rm -rf "$romdir" "$script"; exit 1; }
rm -rf "$romdir" "$script"
echo "ok: the browser descends into a folder and loads from it"

romdir="$(mktemp -d)"
: > "$romdir/AAA TEST.gb"
mkdir "$romdir/Game and Watch"
: > "$romdir/Game and Watch/BALL.mgw"
script="$(mktemp)"
#   tap 200 168 -- MAIN MENU row 1, ALL GAMES
#   tap 200 104 -- root row 0, into "Game and Watch/"
#   tap 200 104 -- subdirectory row 0, "..", back to the root
#   tap 200 168 -- root row 1, "AAA TEST.gb"
# The last tap is the discriminator: at this coordinate the SUBDIRECTORY
# holds BALL.mgw, so a ".." that did nothing (or that quietly re-listed the
# same folder) selects a different file and the grep fails.
printf 'tap 200 168\ntap 200 104\ntap 200 104\ntap 200 168\n' > "$script"
rc=0
out=$(SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy --core build/stub_core.so \
        --rom-dir "$romdir" --ui-script "$script" \
        --panel 1264x1680 --frames 30 2>&1) || rc=$?
echo "$out"
if [ "$rc" -ne 0 ]; then
    echo "FAIL: dot-dot run exited $rc (124 means it hit the 30s timeout)"
    rm -rf "$romdir" "$script"; exit 1
fi
echo "$out" | grep -q "chose $romdir/AAA TEST.gb" \
    || { echo "FAIL: '..' did not come back up to the root"; \
         rm -rf "$romdir" "$script"; exit 1; }
rm -rf "$romdir" "$script"
echo "ok: '..' goes back up one level"

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
# row (row index 2) is the synthetic overflow row. $romdir is flat on purpose,
# for the same reason as the tap-first run far above: a folder row would sort
# above both ROMs and shift every index here by one. See the y math below,
# which mirrors ui_list_init's own row_h derivation for a 1264x1680 panel
# with KOBOY_CHROME_MARGIN=8 on every side (h=1664, slots=26, row_h=64).
# First tap navigates MAIN MENU -> ALL GAMES (row 1), same as the tap-first
# script far above; the second is the one under test here.
script="$(mktemp)"
printf 'tap 200 168\ntap 200 232\n' > "$script"   # row 2: y = 8 + 64 + 2*64 + 64/2
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
# Same MAIN MENU navigation tap first, then the browser's row 0.
printf 'tap 200 168\ntap 200 104\n' > "$script"   # row 0: y = 8 + 64 + 0*64 + 64/2
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

# A plain --rom session with no script at all, which is what almost every user
# run is: it proves the menu-capable build still starts, plays and exits with
# nothing driving it. The runs BELOW drive the menu itself, through the
# --ui-script `menu` verb.
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

# ------------------------------------------- present_divisor, end to end
#
# TWO separate claims, and they fail independently, so they get two runs each.
#
# 1. The ini's value reaches the thing that decides what the panel sees. This
#    is the one that needed KOBOY_STUB_ANIMATE: koboy suppresses an unchanged
#    frame outright, so the stub's normally-static frame presents exactly once
#    whatever the divisor is, and `presented=` -- the only end-to-end handle
#    there is -- would read 1 for every value. With the walking pixel on, every
#    frame differs and the count becomes 120/divisor exactly. That is an
#    assertion that can tell 3 from 6, which is the whole point: a check whose
#    expected value is also what a broken build produces is not a check.
#
# 2. The in-game MENU's FRAMES row cycles the value and writes it back. That
#    needs the `menu` verb, and this is the first automated coverage the
#    MODE_MENU handlers in src/main.c have ever had (docs/FOLLOWUPS.md #47).
romdir="$(mktemp -d)"; : > "$romdir/AAA TEST.gb"
ini="$(mktemp)"; script="$(mktemp)"

# --- 1. the divisor reaches the panel path
for pair in "3 40" "6 20"; do
    want_d=${pair% *}; want_p=${pair#* }
    printf 'present_divisor = %s\n' "$want_d" > "$ini"
    rc=0
    out=$(KOBOY_STUB_ANIMATE=1 SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy \
            --core build/stub_core.so --rom "$romdir/AAA TEST.gb" \
            --config "$ini" --panel 1264x1680 --frames 120 2>&1) || rc=$?
    echo "$out" | grep -E 'present_divisor|^presented='
    if [ "$rc" -ne 0 ]; then
        echo "FAIL: divisor $want_d run exited $rc"
        rm -rf "$romdir" "$ini" "$script"; exit 1
    fi
    # 120 core frames / divisor, and the two divisors give two DIFFERENT
    # numbers -- which is what separates "the divisor was honoured" from "the
    # run presented some frames".
    echo "$out" | grep -q "^presented=$want_p\$" || {
        echo "FAIL: present_divisor $want_d presented $(echo "$out" | grep '^presented=')," \
             "wanted presented=$want_p"
        rm -rf "$romdir" "$ini" "$script"; exit 1; }
    # And the pacer itself agrees, read back off the LIVE pacer by main.c.
    echo "$out" | grep -q "koboy: present_divisor $want_d\$" || {
        echo "FAIL: log did not report present_divisor $want_d off the live pacer"
        rm -rf "$romdir" "$ini" "$script"; exit 1; }
done
echo "ok: present_divisor from the ini reaches the panel (120/d presented frames)"

# A hand-edited nonsense value must not divide by zero in pacer_tick, and must
# not be clamped to 1 either -- it keeps the default. Asserted through the
# PRESENTED COUNT and not only the log line, so a build that logged 3 while
# pacing at 1 still fails.
printf 'present_divisor = 0\n' > "$ini"
rc=0
out=$(KOBOY_STUB_ANIMATE=1 SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy \
        --core build/stub_core.so --rom "$romdir/AAA TEST.gb" \
        --config "$ini" --panel 1264x1680 --frames 120 2>&1) || rc=$?
echo "$out" | grep -E 'present_divisor|^presented='
if [ "$rc" -ne 0 ]; then
    echo "FAIL: present_divisor = 0 exited $rc (136 would be the SIGFPE this guards)"
    rm -rf "$romdir" "$ini" "$script"; exit 1
fi
echo "$out" | grep -q '^presented=40$' || {
    echo "FAIL: present_divisor = 0 did not fall back to the default 3"
    rm -rf "$romdir" "$ini" "$script"; exit 1; }
echo "ok: present_divisor = 0 keeps the default rather than dividing by zero"

# --- 2. the MENU row cycles it, and the choice lands in the ini
#
# Row geometry is the same 1264x1680 derivation as every browser tap above
# (row_h=64, row r's centre is 8 + 64 + r*64 + 32), against the MENU's own
# order in src/main.c: 0 SAVE STATE, 1 LOAD STATE, 2 RESET GAME,
# 3 GREYSCALE, 4 FRAMES, 5 CHOOSE ROM, 6 RESUME, 7 QUIT.
#   menu         -- open the in-game MENU from inside the emulator loop
#   tap 200 360  -- row 4, FRAMES
#
# TWO starting values, and that is the discriminating part: a handler wired to
# a constant, or one that ignored the ini and started from the default, would
# pass a single 3 -> 4 check. 3 -> 4 and 4 -> 6 together pin both that the
# starting value was read and that the LADDER (1,2,3,4,6,8) is what steps it.
printf 'menu\ntap 200 360\n' > "$script"
for pair in "3 4" "4 6"; do
    from=${pair% *}; to=${pair#* }
    printf '# keep me\npresent_divisor = %s\n' "$from" > "$ini"
    rc=0
    out=$(SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy \
            --core build/stub_core.so --rom "$romdir/AAA TEST.gb" \
            --config "$ini" --ui-script "$script" \
            --panel 1264x1680 --frames 60 2>&1) || rc=$?
    echo "$out" | grep -E 'present_divisor'
    if [ "$rc" -ne 0 ]; then
        echo "FAIL: scripted FRAMES run from $from exited $rc"
        rm -rf "$romdir" "$ini" "$script"; exit 1
    fi
    echo "$out" | grep -q "koboy: present_divisor = $to\$" || {
        echo "FAIL: FRAMES did not cycle $from -> $to"
        rm -rf "$romdir" "$ini" "$script"; exit 1; }
    # PERSISTED, not merely applied: the menu and the ini key are one setting.
    grep -q "^present_divisor = $to\$" "$ini" || {
        echo "FAIL: the ini does not say present_divisor = $to after the menu"
        cat "$ini"; rm -rf "$romdir" "$ini" "$script"; exit 1; }
    # ...and the rest of the file survived the rewrite.
    grep -q '^# keep me$' "$ini" || {
        echo "FAIL: saving the divisor destroyed the rest of the ini"
        cat "$ini"; rm -rf "$romdir" "$ini" "$script"; exit 1; }
done
echo "ok: MENU -> FRAMES cycles the divisor and writes it back to the ini"

# LIVE ON THE RUNNING PACER, not merely written to a file for next launch --
# which is the half of the promise the ini check above cannot see. Starting
# from 1, the row cycles to 2, and the 120 core frames that follow present 60
# times instead of 120. Without the pacer_set_divisor call in main.c's
# MENU_FRAMES branch this run presents 120 and everything else about it still
# passes: the log line and the ini would both say 2.
printf 'menu\ntap 200 360\n' > "$script"
printf 'present_divisor = 1\n' > "$ini"
rc=0
out=$(KOBOY_STUB_ANIMATE=1 SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy \
        --core build/stub_core.so --rom "$romdir/AAA TEST.gb" \
        --config "$ini" --ui-script "$script" \
        --panel 1264x1680 --frames 120 2>&1) || rc=$?
echo "$out" | grep -E 'present_divisor|^presented='
if [ "$rc" -ne 0 ]; then
    echo "FAIL: live-pacer run exited $rc"
    rm -rf "$romdir" "$ini" "$script"; exit 1
fi
echo "$out" | grep -q '^presented=60$' || {
    echo "FAIL: the FRAMES row did not change the RUNNING pacer (wanted presented=60)"
    rm -rf "$romdir" "$ini" "$script"; exit 1; }
echo "ok: the FRAMES row takes effect on the running pacer, not only in the ini"

# The SAME hook over the GREYSCALE row, which is what makes this coverage
# rather than a one-off: docs/FOLLOWUPS.md #47 was filed against MENU_GRAY's
# handler, and until the `menu` verb existed no test could reach it.
#   tap 200 296  -- row 3, GREYSCALE
printf 'menu\ntap 200 296\n' > "$script"
printf 'gray_map = balanced\n' > "$ini"
rc=0
out=$(SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy \
        --core build/stub_core.so --rom "$romdir/AAA TEST.gb" \
        --config "$ini" --ui-script "$script" \
        --panel 1264x1680 --frames 60 2>&1) || rc=$?
echo "$out" | grep -E 'gray_map'
if [ "$rc" -ne 0 ]; then
    echo "FAIL: scripted GREYSCALE run exited $rc"
    rm -rf "$romdir" "$ini" "$script"; exit 1
fi
# balanced is entry 2 of luma/bright/balanced/equal/value, so "next" is equal
# -- a value that is neither the default nor entry 0, so neither "the handler
# did nothing" nor "the handler reset to the first entry" can pass this.
echo "$out" | grep -q 'koboy: gray_map = equal$' || {
    echo "FAIL: GREYSCALE did not cycle balanced -> equal"
    rm -rf "$romdir" "$ini" "$script"; exit 1; }
grep -q '^gray_map = equal$' "$ini" || {
    echo "FAIL: the ini does not say gray_map = equal after the menu"
    cat "$ini"; rm -rf "$romdir" "$ini" "$script"; exit 1; }
echo "ok: MENU -> GREYSCALE cycles the mapping and writes it back (closes #47)"

# A NEGATIVE control for both of the above, and it is the check that stops the
# two runs before it from being self-fulfilling. The same script, aimed at a
# row that is neither settings row (row 2, RESET GAME): if the taps were
# landing anywhere at all -- or if the handler fired regardless of which row
# was chosen -- this run would move a setting too. Neither key may change.
printf 'menu\ntap 200 232\n' > "$script"
printf 'present_divisor = 3\ngray_map = balanced\n' > "$ini"
rc=0
out=$(SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy \
        --core build/stub_core.so --rom "$romdir/AAA TEST.gb" \
        --config "$ini" --ui-script "$script" \
        --panel 1264x1680 --frames 60 2>&1) || rc=$?
if [ "$rc" -ne 0 ]; then
    echo "FAIL: scripted RESET GAME run exited $rc"
    rm -rf "$romdir" "$ini" "$script"; exit 1
fi
echo "$out" | grep -q 'koboy: present_divisor = ' && {
    echo "FAIL: tapping RESET GAME changed present_divisor"
    rm -rf "$romdir" "$ini" "$script"; exit 1; }
echo "$out" | grep -q 'koboy: gray_map = ' && {
    echo "FAIL: tapping RESET GAME changed gray_map"
    rm -rf "$romdir" "$ini" "$script"; exit 1; }
grep -q '^present_divisor = 3$' "$ini" || {
    echo "FAIL: the ini's present_divisor moved without being asked"
    rm -rf "$romdir" "$ini" "$script"; exit 1; }
rm -rf "$romdir" "$ini" "$script"
echo "ok: tapping a different MENU row moves neither setting"

# ------------------------------------------- SAVE STATE / LOAD STATE, scripted
#
# Two taps past a row a script can now reach, which is why run_slot_picker was
# wired to the same cursor: an unscripted run_list with no live input does not
# EXIT, it polls until something kills the run, so a script that tapped SAVE
# STATE would have hung for the timeout instead of failing. This run is what
# stops that from being a claim.
#
#   menu         -- open the MENU
#   tap 200 104  -- row 0, SAVE STATE   (row 1, y=168, is LOAD STATE)
#   tap 200 104  -- the slot picker's row 0, slot 1
#
# It also makes the FIRST automated save state this project has ever written.
# docs/FOLLOWUPS.md #76: the device half is still not done.
romdir="$(mktemp -d)"; : > "$romdir/AAA TEST.gb"
savedir="$(mktemp -d)"; script="$(mktemp)"
printf 'menu\ntap 200 104\ntap 200 104\n' > "$script"
rc=0
out=$(SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy --core build/stub_core.so \
        --rom "$romdir/AAA TEST.gb" --save-dir "$savedir" --ui-script "$script" \
        --panel 1264x1680 --frames 60 2>&1) || rc=$?
echo "$out" | grep -E 'state'
if [ "$rc" -ne 0 ]; then
    echo "FAIL: scripted SAVE STATE exited $rc (124 means the slot picker hung)"
    rm -rf "$romdir" "$savedir" "$script"; exit 1
fi
echo "$out" | grep -q '^koboy: saved state 1$' || {
    echo "FAIL: the script did not save to slot 1"
    rm -rf "$romdir" "$savedir" "$script"; exit 1; }
# The FILE, not just the message -- and slot 1's name specifically, so a
# picker that handed back the wrong index would fail here rather than pass.
[ -s "$savedir/AAA TEST.st1" ] || {
    echo "FAIL: no state file in $savedir"; ls -la "$savedir"
    rm -rf "$romdir" "$savedir" "$script"; exit 1; }

# ...and reading it back is a separate run, because "wrote something" and "the
# core accepted it" fail independently. Row 1 this time: LOAD STATE.
printf 'menu\ntap 200 168\ntap 200 104\n' > "$script"
rc=0
out=$(SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy --core build/stub_core.so \
        --rom "$romdir/AAA TEST.gb" --save-dir "$savedir" --ui-script "$script" \
        --panel 1264x1680 --frames 60 2>&1) || rc=$?
echo "$out" | grep -E 'state'
if [ "$rc" -ne 0 ]; then
    echo "FAIL: scripted LOAD STATE exited $rc"
    rm -rf "$romdir" "$savedir" "$script"; exit 1
fi
echo "$out" | grep -q '^koboy: loaded state 1$' || {
    echo "FAIL: the script did not load slot 1"
    rm -rf "$romdir" "$savedir" "$script"; exit 1; }
rm -rf "$romdir" "$savedir" "$script"
echo "ok: MENU -> SAVE STATE writes slot 1 and LOAD STATE reads it back"

# ------------------------------------------------- core chosen by extension
#
# The DECISION lives in src/main.c (tests/test_config.c covers the predicate
# and the core_explicit flag on their own), and it is only observable end to
# end, because it depends on three things no unit test has: a real
# /proc/self/exe, config_join_sibling resolving the chosen name against that
# directory, and dlopen actually being handed the result.
#
# So the binary is run from a COPY in its own directory, with two stand-in
# cores beside it named exactly what the resolver will ask for. Copying
# rather than running ./build/koboy is the point: the sibling join is against
# the executable's directory, so an isolated directory is what proves the
# join happened instead of some cwd-relative accident.
d="$(mktemp -d)"
cp build/koboy       "$d/koboy"
cp build/stub_core.so "$d/gambatte_libretro.so"
cp build/stub_core.so "$d/gw_libretro.so"
cp build/stub_core.so "$d/fceumm_libretro.so"
cp build/stub_core.so "$d/pokemini_libretro.so"
cp build/stub_core.so "$d/mednafen_wswan_libretro.so"
cp build/stub_core.so "$d/race_libretro.so"
cp build/stub_core.so "$d/stella2014_libretro.so"
cp build/stub_core.so "$d/gearcoleco_libretro.so"
cp build/stub_core.so "$d/freeintv_libretro.so"
cp build/stub_core.so "$d/genesis_plus_gx_libretro.so"
printf '\0' > "$d/GAME.mgw"
printf '\0' > "$d/GAME.gb"
printf '\0' > "$d/GAME.nes"
printf '\0' > "$d/GAME.min"
printf '\0' > "$d/GAME.ws"
# UPPERCASE on purpose, and only here: the device partition is FAT32 and a
# real collection carries mixed case (the author's Game Gear directory has
# both .gg and .GG). config_core_for_rom lowercases the candidate, not the
# pattern, and this is the only place that claim is exercised end to end.
printf '\0' > "$d/GAME.WSC"
printf '\0' > "$d/GAME.ngp"
printf '\0' > "$d/GAME.NGC"
printf '\0' > "$d/GAME.a26"
printf '\0' > "$d/GAME.col"
printf '\0' > "$d/GAME.int"
printf '\0' > "$d/GAME.sms"
# UPPERCASE again, and this one is the case the whole rule was written for:
# the author's Game Gear directory holds 38 files ending .gg and 15 ending
# .GG, side by side in one folder.
printf '\0' > "$d/GAME.GG"
# The two systems on the LCD strip. 32768 bytes rather than one, because
# config_min_rom_bytes floors a .sfc/.smc at 8192 -- snes9x2005 raises SIGFPE
# inside retro_load_game below one mapping block.
head -c 32768 /dev/zero > "$d/GAME.sfc"
head -c 32768 /dev/zero > "$d/GAME.md"
head -c 32768 /dev/zero > "$d/GAME.gba"

# .mgw with no --core: the Game & Watch core, resolved beside the binary.
rc=0
out=$(SDL_VIDEODRIVER=dummy timeout 30 "$d/koboy" --rom "$d/GAME.mgw" \
        --panel 1264x1680 --frames 10 2>&1) || rc=$?
echo "$out"
[ "$rc" -eq 0 ] || { echo "FAIL: .mgw run exited $rc"; rm -rf "$d"; exit 1; }
echo "$out" | grep -qx "koboy: core $d/gw_libretro.so" \
    || { echo "FAIL: .mgw did not select the gw core"; rm -rf "$d"; exit 1; }
echo "$out" | grep -q '^presented=' \
    || { echo "FAIL: .mgw run never reached the emulator loop"; rm -rf "$d"; exit 1; }
echo "ok: .mgw selects gw_libretro.so"

# .gb with no --core: unchanged behaviour. This is the control -- without it,
# a config_core_for_rom that answered "gw" for EVERYTHING would still pass
# the assertion above.
rc=0
out=$(SDL_VIDEODRIVER=dummy timeout 30 "$d/koboy" --rom "$d/GAME.gb" \
        --panel 1264x1680 --frames 10 2>&1) || rc=$?
echo "$out"
[ "$rc" -eq 0 ] || { echo "FAIL: .gb run exited $rc"; rm -rf "$d"; exit 1; }
echo "$out" | grep -qx "koboy: core $d/gambatte_libretro.so" \
    || { echo "FAIL: .gb no longer selects gambatte"; rm -rf "$d"; exit 1; }
echo "$out" | grep -q "with a C button" \
    && { echo "FAIL: a .gb grew a third face button"; rm -rf "$d"; exit 1; }
echo "ok: .gb still selects gambatte_libretro.so"

# .nes and .min, the two systems added after the .mgw pair above. Same shape,
# same reason it has to be end to end: config_core_for_rom's table is unit
# tested, but only a real run proves main.c joined the name against
# /proc/self/exe's directory and dlopen was handed the result.
rc=0
out=$(SDL_VIDEODRIVER=dummy timeout 30 "$d/koboy" --rom "$d/GAME.nes" \
        --panel 1264x1680 --frames 10 2>&1) || rc=$?
echo "$out"
[ "$rc" -eq 0 ] || { echo "FAIL: .nes run exited $rc"; rm -rf "$d"; exit 1; }
echo "$out" | grep -qx "koboy: core $d/fceumm_libretro.so" \
    || { echo "FAIL: .nes did not select the NES core"; rm -rf "$d"; exit 1; }
echo "$out" | grep -q "LCD layout" \
    && { echo "FAIL: a .nes was given the LCD layout"; rm -rf "$d"; exit 1; }
echo "$out" | grep -q "extra buttons" \
    && { echo "FAIL: a .nes grew an extra face button"; rm -rf "$d"; exit 1; }
echo "ok: .nes selects fceumm_libretro.so and keeps the DMG faceplate"

rc=0
out=$(SDL_VIDEODRIVER=dummy timeout 30 "$d/koboy" --rom "$d/GAME.min" \
        --panel 1264x1680 --frames 10 2>&1) || rc=$?
echo "$out"
[ "$rc" -eq 0 ] || { echo "FAIL: .min run exited $rc"; rm -rf "$d"; exit 1; }
echo "$out" | grep -qx "koboy: core $d/pokemini_libretro.so" \
    || { echo "FAIL: .min did not select the Pokemon Mini core"; rm -rf "$d"; exit 1; }
echo "$out" | grep -q "LCD layout" \
    && { echo "FAIL: a .min was given the LCD layout"; rm -rf "$d"; exit 1; }
# THE THIRD FACE BUTTON, end to end. config.c decides it and chrome.c draws
# it, both unit tested -- but main.c has to ASK, and a missed call there is
# invisible to every unit test and shows up on the device as a Pokemon Mini
# with no C button. The .gb and .nes runs either side are the control.
echo "$out" | grep -qx "koboy: faceplate DMG, extra buttons: C" \
    || { echo "FAIL: a .min did not get the C button"; rm -rf "$d"; exit 1; }
echo "ok: .min selects pokemini_libretro.so and keeps the DMG faceplate"

# .ws/.wsc and .ngp/.ngc, the two systems after those. Same shape and the
# same reason, plus one this pair adds: TWO of the four names are UPPERCASE,
# which is the only end-to-end exercise of the case-insensitive match that
# a FAT32 partition makes mandatory.
for pair in "GAME.ws mednafen_wswan_libretro.so" \
            "GAME.WSC mednafen_wswan_libretro.so" \
            "GAME.ngp race_libretro.so" \
            "GAME.NGC race_libretro.so"; do
    rom=${pair%% *}; want=${pair##* }
    rc=0
    out=$(SDL_VIDEODRIVER=dummy timeout 30 "$d/koboy" --rom "$d/$rom" \
            --panel 1264x1680 --frames 10 2>&1) || rc=$?
    echo "$out"
    [ "$rc" -eq 0 ] || { echo "FAIL: $rom run exited $rc"; rm -rf "$d"; exit 1; }
    echo "$out" | grep -qx "koboy: core $d/$want" \
        || { echo "FAIL: $rom did not select $want"; rm -rf "$d"; exit 1; }
    echo "$out" | grep -q "LCD layout" \
        && { echo "FAIL: $rom was given the LCD layout"; rm -rf "$d"; exit 1; }
    echo "ok: $rom selects $want and keeps the DMG faceplate"
done

# THE WONDERSWAN'S TWO EXTRA DISCS, end to end, with the .ngp run as the
# control. beetle-wswan's rotated key map puts the console's own A and B on
# JOYPAD_L / JOYPAD_R, so without these two a portrait title cannot be
# started -- measured: `Kaze no Klonoa - Moonlight Museum` in portrait
# responds to START and JOYPAD_L and to nothing else. config.c decides them
# and chrome.c draws them, both unit tested, but main.c has to ASK.
rc=0
out=$(SDL_VIDEODRIVER=dummy timeout 30 "$d/koboy" --rom "$d/GAME.ws" \
        --panel 1264x1680 --frames 10 2>&1) || rc=$?
[ "$rc" -eq 0 ] || { echo "FAIL: .ws faceplate run exited $rc"; rm -rf "$d"; exit 1; }
echo "$out" | grep -qx "koboy: faceplate DMG, extra buttons: L1 R1" \
    || { echo "FAIL: a .ws did not get the L1/R1 pair"; rm -rf "$d"; exit 1; }
rc=0
out=$(SDL_VIDEODRIVER=dummy timeout 30 "$d/koboy" --rom "$d/GAME.ngp" \
        --panel 1264x1680 --frames 10 2>&1) || rc=$?
[ "$rc" -eq 0 ] || { echo "FAIL: .ngp faceplate run exited $rc"; rm -rf "$d"; exit 1; }
echo "$out" | grep -q "extra buttons" \
    && { echo "FAIL: a .ngp grew extra face buttons"; rm -rf "$d"; exit 1; }
echo "ok: .ws gets L1/R1 and .ngp gets neither"

# .a26, .col, .int, .sms and .gg -- four more systems, five more extensions,
# same shape and the same reason. .GG is uppercase because that is what half
# of a real Game Gear folder looks like.
for pair in "GAME.a26 stella2014_libretro.so" \
            "GAME.col gearcoleco_libretro.so" \
            "GAME.int freeintv_libretro.so" \
            "GAME.sms genesis_plus_gx_libretro.so" \
            "GAME.GG genesis_plus_gx_libretro.so"; do
    rom=${pair%% *}; want=${pair##* }
    rc=0
    out=$(SDL_VIDEODRIVER=dummy timeout 30 "$d/koboy" --rom "$d/$rom" \
            --panel 1264x1680 --frames 10 2>&1) || rc=$?
    echo "$out"
    [ "$rc" -eq 0 ] || { echo "FAIL: $rom run exited $rc"; rm -rf "$d"; exit 1; }
    echo "$out" | grep -qx "koboy: core $d/$want" \
        || { echo "FAIL: $rom did not select $want"; rm -rf "$d"; exit 1; }
    echo "$out" | grep -q "LCD layout" \
        && { echo "FAIL: $rom was given the LCD layout"; rm -rf "$d"; exit 1; }
    echo "ok: $rom selects $want and keeps the DMG faceplate"
done

# THE INTELLIVISION'S AND COLECOVISION'S DISCS, end to end, with .a26/.sms as
# the controls. Both of these systems have a TWELVE-KEY KEYPAD that the
# faceplate cannot draw, and in both cases titles refuse to start without it:
# an Intellivision reaches the keypad by HOLDING the KEY disc (FreeIntv's
# JOYPAD_L modifier, which draws a mini keypad the d-pad steers), a
# ColecoVision by pressing K1/K2 (Gearcoleco puts keypad 1 and 2 on
# JOYPAD_Y/JOYPAD_X, and the console BIOS asks for one before any cartridge
# starts). The LABELS are asserted, not just the count, because two discs
# reporting the wrong bits look exactly like two working discs.
rc=0
out=$(SDL_VIDEODRIVER=dummy timeout 30 "$d/koboy" --rom "$d/GAME.int" \
        --panel 1264x1680 --frames 10 2>&1) || rc=$?
[ "$rc" -eq 0 ] || { echo "FAIL: .int faceplate run exited $rc"; rm -rf "$d"; exit 1; }
echo "$out" | grep -qx "koboy: faceplate DMG, extra buttons: KEY TOP" \
    || { echo "FAIL: a .int did not get the KEY/TOP pair"; rm -rf "$d"; exit 1; }
rc=0
out=$(SDL_VIDEODRIVER=dummy timeout 30 "$d/koboy" --rom "$d/GAME.col" \
        --panel 1264x1680 --frames 10 2>&1) || rc=$?
[ "$rc" -eq 0 ] || { echo "FAIL: .col faceplate run exited $rc"; rm -rf "$d"; exit 1; }
echo "$out" | grep -qx "koboy: faceplate DMG, extra buttons: K1 K2" \
    || { echo "FAIL: a .col did not get the K1/K2 pair"; rm -rf "$d"; exit 1; }
for rom in GAME.a26 GAME.sms GAME.GG; do
    rc=0
    out=$(SDL_VIDEODRIVER=dummy timeout 30 "$d/koboy" --rom "$d/$rom" \
            --panel 1264x1680 --frames 10 2>&1) || rc=$?
    [ "$rc" -eq 0 ] || { echo "FAIL: $rom faceplate run exited $rc"; rm -rf "$d"; exit 1; }
    echo "$out" | grep -q "extra buttons" \
        && { echo "FAIL: $rom grew extra face buttons"; rm -rf "$d"; exit 1; }
done
echo "ok: .int gets KEY/TOP, .col gets K1/K2, .a26 and the Sega pair get neither"

# ------------------------------------------- battery saves, for a NES cart
#
# NES cartridges have battery-backed SRAM and koboy already has a save path
# that was verified on hardware against a Zelda Game Boy cartridge -- but
# "the wiring carries over" is exactly the assumption that, if wrong, looks
# identical to a working save until someone loses a playthrough. So this
# drives the whole chain for a .nes: core_sram -> the periodic/final
# sram_save -> a file on disk with the CORE's bytes in it.
#
# The stub core writes A0..A7 into its save RAM on every retro_run
# (tests/stub_core.c), which is what makes this an assertion rather than an
# existence check: an empty or zero-filled .srm would be indistinguishable
# from a file the shell created, and od(1) below names the exact bytes.
sd="$(mktemp -d)"
[ -e "$sd/GAME.srm" ] && { echo "FAIL: save dir was not clean"; rm -rf "$d" "$sd"; exit 1; }
rc=0
out=$(SDL_VIDEODRIVER=dummy timeout 30 "$d/koboy" --rom "$d/GAME.nes" \
        --save-dir "$sd" --panel 1264x1680 --frames 10 2>&1) || rc=$?
echo "$out"
[ "$rc" -eq 0 ] || { echo "FAIL: .nes save run exited $rc"; rm -rf "$d" "$sd"; exit 1; }
[ -f "$sd/GAME.srm" ] \
    || { echo "FAIL: a .nes run wrote no .srm at all"; rm -rf "$d" "$sd"; exit 1; }
bytes=$(od -An -tx1 "$sd/GAME.srm" | tr -s ' ' | sed 's/^ //;s/ $//')
[ "$bytes" = "a0 a1 a2 a3 a4 a5 a6 a7" ] \
    || { echo "FAIL: .srm holds '$bytes', not the core's save RAM"; \
         rm -rf "$d" "$sd"; exit 1; }
# And it comes back: the same path, read whole, is what a second session does.
rc=0
out=$(SDL_VIDEODRIVER=dummy timeout 30 "$d/koboy" --rom "$d/GAME.nes" \
        --save-dir "$sd" --panel 1264x1680 --frames 10 2>&1) || rc=$?
echo "$out"
[ "$rc" -eq 0 ] || { echo "FAIL: .nes reload run exited $rc"; rm -rf "$d" "$sd"; exit 1; }
echo "$out" | grep -q "loaded $sd/GAME.srm" \
    || { echo "FAIL: the second session did not load the .srm back"; \
         rm -rf "$d" "$sd"; exit 1; }
rm -rf "$sd"
echo "ok: a .nes battery save is written and read back"

# --core beats the extension. A .mgw ROM with an explicit core must open THAT
# core -- and the explicit path is deliberately a THIRD file, distinguishable
# from both siblings above, so "it picked the explicit one" cannot be
# confused with "it picked gw and gw happens to be a stub too".
cp build/stub_core.so "$d/explicit_core.so"
rc=0
out=$(SDL_VIDEODRIVER=dummy timeout 30 "$d/koboy" --core "$d/explicit_core.so" \
        --rom "$d/GAME.mgw" --panel 1264x1680 --frames 10 2>&1) || rc=$?
echo "$out"
[ "$rc" -eq 0 ] || { echo "FAIL: explicit --core run exited $rc"; rm -rf "$d"; exit 1; }
echo "$out" | grep -qx "koboy: core $d/explicit_core.so" \
    || { echo "FAIL: --core lost to the .mgw extension"; rm -rf "$d"; exit 1; }
echo "ok: --core beats the extension"

# And the same for an ini `core=`, which is a separate assignment in a
# separate file (src/config.c, not src/main.c) and can regress on its own.
cat > "$d/explicit.ini" <<EOF
core = $d/explicit_core.so
EOF
rc=0
out=$(SDL_VIDEODRIVER=dummy timeout 30 "$d/koboy" --config "$d/explicit.ini" \
        --rom "$d/GAME.mgw" --panel 1264x1680 --frames 10 2>&1) || rc=$?
echo "$out"
[ "$rc" -eq 0 ] || { echo "FAIL: ini core= run exited $rc"; rm -rf "$d"; exit 1; }
echo "$out" | grep -qx "koboy: core $d/explicit_core.so" \
    || { echo "FAIL: ini core= lost to the .mgw extension"; rm -rf "$d"; exit 1; }
echo "ok: ini core= beats the extension"

# THE UPGRADE CASE. Every koboy.ini written before choice-by-extension existed
# carries a literal `core = gambatte_libretro.so`, because v1 shipped that line
# uncommented. It records packaging, not preference, so it must NOT pin the
# core -- otherwise a .mgw is listed and then handed to gambatte, which is the
# failure the user would actually hit after redeploying and carrying their old
# ini forward (docs/device-workflow.md tells them to). Asserted end to end
# here, and not only in tests/test_config.c, because the flag is read in
# src/main.c: config.c could get the flag right and main.c still ignore it.
cat > "$d/legacy.ini" <<EOF
core = gambatte_libretro.so
EOF
rc=0
out=$(SDL_VIDEODRIVER=dummy timeout 30 "$d/koboy" --config "$d/legacy.ini" \
        --rom "$d/GAME.mgw" --panel 1264x1680 --frames 10 2>&1) || rc=$?
echo "$out"
[ "$rc" -eq 0 ] || { echo "FAIL: legacy ini run exited $rc"; rm -rf "$d"; exit 1; }
echo "$out" | grep -qx "koboy: core $d/gw_libretro.so" \
    || { echo "FAIL: a stale 'core = gambatte_libretro.so' pinned the core"; rm -rf "$d"; exit 1; }
echo "ok: a legacy ini core= does not pin the core"

# WHICH PRESENTATION, end to end. The layout is chosen in src/main.c from the
# ROM's extension (config_layout_for_rom), and nothing else in this file or in
# the unit suite proves main.c actually SETS it -- config.c could get the
# predicate right and main.c never read it. Reusing the same $d directory and
# the same two stand-in ROMs as the core-selection runs above.
#
# The stub core reports 160x144, so the numbers here are not a Game & Watch
# geometry; what is being asserted is which BRANCH ran, which the log names.
rc=0
out=$(SDL_VIDEODRIVER=dummy timeout 30 "$d/koboy" --rom "$d/GAME.mgw" \
        --panel 1264x1680 --frames 10 2>&1) || rc=$?
echo "$out"
[ "$rc" -eq 0 ] || { echo "FAIL: .mgw layout run exited $rc"; rm -rf "$d"; exit 1; }
echo "$out" | grep -q "LCD layout" \
    || { echo "FAIL: a .mgw did not get the LCD layout"; rm -rf "$d"; exit 1; }
# Full panel width, which is the visible half of the fix ("too small" was the
# report). 1264 is the --panel width above.
echo "$out" | grep -q "game 1264x" \
    || { echo "FAIL: the LCD layout did not fill the panel width"; rm -rf "$d"; exit 1; }
echo "$out" | grep -q '^presented=' \
    || { echo "FAIL: the .mgw layout run never reached the emulator loop"; rm -rf "$d"; exit 1; }
echo "ok: .mgw gets the full-width LCD layout"

# The control. Without it, a main.c that set the LCD layout unconditionally
# would pass the run above and quietly replace the Game Boy faceplate too.
rc=0
out=$(SDL_VIDEODRIVER=dummy timeout 30 "$d/koboy" --rom "$d/GAME.gb" \
        --panel 1264x1680 --frames 10 2>&1) || rc=$?
echo "$out"
[ "$rc" -eq 0 ] || { echo "FAIL: .gb layout run exited $rc"; rm -rf "$d"; exit 1; }
echo "$out" | grep -q "LCD layout" \
    && { echo "FAIL: a .gb was given the LCD layout"; rm -rf "$d"; exit 1; }
# And the Game Boy rect is exactly where it has always been.
echo "$out" | grep -q "scale 5, game 800x720 at (232,84)" \
    || { echo "FAIL: the Game Boy rect moved"; rm -rf "$d"; exit 1; }
echo "ok: .gb keeps the DMG faceplate at scale 5"

# ...AND THE THREE CONSOLES ON THAT STRIP. A SNES pad is A B X Y L R and a
# six-button Mega Drive is A B C X Y Z; the DMG faceplate has two spare
# pockets, the LCD strip has a d-pad, a per-system face block, L1, R1, SELECT
# and START. The Game Boy Advance is here for a different reason -- its pad
# FITS the faceplate, and it moved because the faceplate's two spare pockets
# are FACE pockets while a GBA's L and R are a left one and a right one; see
# config_layout_for_rom. Asserted end to end for the same reason the .mgw run
# above is: config.c can get the predicate right and src/main.c never read it.
# `--core` is given because the stub stands in for all three cores here.
for ext in sfc md gba; do
    rc=0
    out=$(SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy --core build/stub_core.so \
            --rom "$d/GAME.$ext" --panel 1264x1680 --frames 10 2>&1) || rc=$?
    # `|| true` because this line is INFORMATIONAL and the file runs under
    # `set -e`: a build that dropped the layout also drops this log line, and
    # an unguarded grep would abort the script before the check below could
    # say why. (Found by mutant, not by reasoning.)
    echo "$out" | grep -E "core geometry" || true
    [ "$rc" -eq 0 ] || { echo "FAIL: .$ext layout run exited $rc"; rm -rf "$d"; exit 1; }
    echo "$out" | grep -q "LCD layout" \
        || { echo "FAIL: a .$ext did not get the LCD layout"; rm -rf "$d"; exit 1; }
    echo "$out" | grep -q '^presented=' \
        || { echo "FAIL: the .$ext layout run never reached the emulator loop"; rm -rf "$d"; exit 1; }
done
echo "ok: .sfc, .md and .gba get the LCD control strip"

# GEOMETRY CHURN: a re-fit happens when, and only when, the RECT moves.
#
# main.c used to decide this from the inputs -- max moved, re-fit; base moved,
# do not -- and that was right while the reserved rect was max_w x max_h times
# an integer. It is not right any more: in KOBOY_LAYOUT_DMG the rect is sized
# from BASE (see config.c), so a base change there really does move the rect,
# the chrome drawn around it and the buffers underneath it. main.c now
# resolves the candidate profile and compares the ANSWER
# (config_profile_presentation_same), which is the only way to tell the two
# layouts apart.
#
# THREE runs, and all three are needed. The LCD one alone would pass against a
# frontend that never re-fits at all; the DMG one alone would pass against one
# that re-fits on every announcement, which is the several-times-a-second
# video_destroy/video_create + full faceplate repaint + forced full-rect
# refresh this whole branch exists to avoid on e-ink.

# (1) LCD, base churn: SILENT for a GAME & WATCH. This is the real case -- a
#     .mgw title alternates between the whole unit and the LCD alone several
#     times a second, and ITS LCD rect comes from max, so nothing about the
#     presentation moves. Which geometry an LCD rect comes from is now a
#     per-system question (config_lcd_rect_from_max_for_rom); run (1b) below
#     is the other side of it.
rc=0
out=$(KOBOY_STUB_OSCILLATE=1 SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy \
        --core build/stub_core.so --rom "$d/GAME.mgw" --frames 120 2>&1) || rc=$?
[ "$rc" -eq 0 ] || { echo "FAIL: LCD oscillating-base run exited $rc"; exit 1; }
echo "$out" | grep -q "LCD layout" \
    || { echo "FAIL: the .mgw oscillating run did not get the LCD layout"; exit 1; }
n=$(echo "$out" | grep -c "geometry settled" || true)
[ "$n" -eq 0 ] || { echo "FAIL: LCD base churn re-fit $n time(s); its rect comes from max"; exit 1; }
echo "$out" | grep -q "presented=" || { echo "FAIL: LCD oscillating run never presented"; exit 1; }
echo "ok: base-only churn does not re-fit in the LCD layout"

# (1b) LCD, base churn, on a CONSOLE: RE-FITS, because .sfc/.smc/.md size the
#      LCD rect from base. Same layout, same stub, same oscillation as (1) --
#      only the extension differs, which is what makes this measure the
#      per-system flag rather than the layout. Without it, `lcd_rect_from_max`
#      could be hardwired true and (1) would still pass.
#
#      A .sfc is used rather than a .md because the SNES also carries the
#      scale ceiling, so this run doubles as the end-to-end proof that the
#      ceiling is live in the LCD layout: the stub's 160x144 max at the
#      Game Boy's geometry gives a capped rect either side of the churn.
rc=0
out=$(KOBOY_STUB_OSCILLATE=1 SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy \
        --core build/stub_core.so --rom "$d/GAME.sfc" --frames 120 2>&1) || rc=$?
[ "$rc" -eq 0 ] || { echo "FAIL: LCD console oscillating-base run exited $rc"; exit 1; }
echo "$out" | grep -q "LCD layout" \
    || { echo "FAIL: the .sfc oscillating run did not get the LCD layout"; exit 1; }
n=$(echo "$out" | grep -c "geometry settled" || true)
[ "$n" -gt 0 ] || {
    echo "FAIL: a .sfc under the LCD layout never re-fit; its rect must follow base"
    exit 1; }
echo "$out" | grep -q "presented=" || { echo "FAIL: LCD console oscillating run never presented"; exit 1; }
echo "ok: base churn DOES re-fit for a console in the LCD layout ($n re-fits)"

# (2) DMG, base churn: RE-FITS, and the log has to show the rect actually
#     following base. The stub alternates base between its 160x144 max and
#     half of it, so at the Game Boy's scale 5 the rect must be seen at BOTH
#     800x720 and 400x360. Asserting the two rects rather than just a nonzero
#     count is what makes this measure the base-sized rect instead of merely
#     measuring that something was logged.
rc=0
out=$(KOBOY_STUB_OSCILLATE=1 SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy \
        --core build/stub_core.so --rom "$ROM" --frames 120 2>&1) || rc=$?
[ "$rc" -eq 0 ] || { echo "FAIL: DMG oscillating-base run exited $rc"; exit 1; }
n=$(echo "$out" | grep -c "geometry settled" || true)
[ "$n" -gt 0 ] || { echo "FAIL: DMG base churn never re-fit; the rect is sized from base"; exit 1; }
echo "$out" | grep -q "game 800x720" \
    || { echo "FAIL: the full-base DMG rect is not 800x720"; exit 1; }
echo "$out" | grep -q "game 400x360" \
    || { echo "FAIL: the half-base DMG rect is not 400x360 -- the rect is not following base"; exit 1; }
echo "$out" | grep -q "presented=" || { echo "FAIL: DMG oscillating run never presented"; exit 1; }
echo "ok: base churn re-fits in the DMG layout, and the rect follows base ($n re-fits)"

# (3) A max change re-fits.
rc=0
out=$(KOBOY_STUB_MAXGROW=1 SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy \
        --core build/stub_core.so --rom "$ROM" --frames 120 2>&1) || rc=$?
[ "$rc" -eq 0 ] || { echo "FAIL: max-grow run exited $rc"; exit 1; }
echo "$out" | grep -q "geometry settled" \
    || { echo "FAIL: a max change did NOT re-fit"; exit 1; }
echo "ok: a max change does re-fit"

# (4) MAX GROWS AND BASE DOES NOT -- the case none of the three above can
#     reach, and the one that says why max_w/max_h are in the comparison at
#     all. The DMG rect comes from base, so the presentation is IDENTICAL
#     either side of this announcement; the re-fit is still required, because
#     video_create sized its intermediate buffer from the old max and the
#     bounds guard will now accept frames that do not fit it. Dropping max
#     from config_profile_presentation_same passes every other check in this
#     file and fails here.
rc=0
out=$(KOBOY_STUB_MAXONLY=1 SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy \
        --core build/stub_core.so --rom "$ROM" --frames 120 2>&1) || rc=$?
[ "$rc" -eq 0 ] || { echo "FAIL: max-only run exited $rc"; exit 1; }
echo "$out" | grep -q "geometry settled" \
    || { echo "FAIL: max grew with base unchanged and koboy did NOT re-fit"; exit 1; }
# ...and the rect really did stay put, so this is the max-only case and not a
# rect change wearing its clothes.
echo "$out" | grep "geometry settled" | grep -q "game 800x720" \
    || { echo "FAIL: the max-only re-fit moved the rect; that is a different case"; exit 1; }
echo "ok: a max-only change re-fits without moving the rect"


# THE PER-SYSTEM SCALE CEILING, end to end.
#
# Sizing the rect from the frame a core really draws quadrupled SNES's picture
# and, MEASURED on the device, cost its heaviest titles real speed: Star Fox
# 93%->67%, Kirby Super Star 96%->78%. At scale 3 the picture is still 2.25x
# the old one and everything measured is back above 95%, so .sfc/.smc carry a
# ceiling of 3 while every other system auto-fits.
#
# Asserted with the SAME geometry on both runs and only the extension
# differing, which is the only way to show the ceiling is doing the work
# rather than the geometry: 256x224 auto-fits to scale 4 (1024x896), and the
# ceiling takes .sfc to scale 3 (768x672). A test that let the geometry differ
# too would pass with the ceiling deleted.
#
# THE .sfc RUN NOW GOES THROUGH THE LCD LAYOUT, which is why the layout is
# asserted alongside the rect. That layout fits FRACTIONALLY to the full panel
# width and has no margin loop to fall back on, so the cap in
# config_resolve_profile_par's LCD branch is the only thing between Star Fox
# and the 67% the ceiling was added to prevent -- and 768x672 here is that cap
# doing the work, not an integer scale search. Uncapped, this run resolves to
# 1264x1106.
d_sc=$(mktemp -d)
cp build/stub_core.so "$d_sc/"
head -c 32768 /dev/zero > "$d_sc/GAME.sfc"
head -c 32768 /dev/zero > "$d_sc/GAME.gb"
out=$(KOBOY_STUB_GEOM=256x224 SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy \
        --core "$d_sc/stub_core.so" --rom "$d_sc/GAME.sfc" \
        --panel 1264x1680 --frames 2 2>&1) || { echo "FAIL: .sfc ceiling run"; rm -rf "$d_sc"; exit 1; }
echo "$out" | grep -q "game 768x672" || {
    echo "FAIL: a .sfc did not take the scale-3 ceiling"
    echo "$out" | grep -E "game [0-9]+x[0-9]+"; rm -rf "$d_sc"; exit 1; }
echo "$out" | grep -q "LCD layout" || {
    echo "FAIL: the .sfc ceiling run did not go through the LCD layout,"
    echo "      so the cap it proves is not the one the device will use"
    rm -rf "$d_sc"; exit 1; }
out=$(KOBOY_STUB_GEOM=256x224 SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy \
        --core "$d_sc/stub_core.so" --rom "$d_sc/GAME.gb" \
        --panel 1264x1680 --frames 2 2>&1) || { echo "FAIL: .gb ceiling run"; rm -rf "$d_sc"; exit 1; }
echo "$out" | grep -q "game 1024x896" || {
    echo "FAIL: a system with no ceiling stopped auto-fitting"
    echo "$out" | grep -E "game [0-9]+x[0-9]+"; rm -rf "$d_sc"; exit 1; }
echo "ok: .sfc takes the scale-3 ceiling in the LCD layout, an uncapped system still auto-fits"

# AND THE IN-GAME MENU STILL OPENS UNDER THAT LAYOUT. MENU is the only way
# back to the ROM browser once a game is running, so a layout change that
# stranded it strands the device in the game -- and MODE_MENU's handlers had
# no automated coverage at all until the --ui-script `menu` verb existed.
# Driven on a .sfc specifically: it is the extension that changed layout AND
# carries the ceiling, so this run exercises both together.
#
#   menu         -- open the in-game MENU from inside the emulator loop
#   tap 200 360  -- row 4, FRAMES (row_h=64, centre = 8 + 64 + r*64 + 32)
#
# FRAMES rather than RESUME because it leaves EVIDENCE: the divisor cycles
# 3 -> 4 and is written back, so this fails if the menu opened but its rows
# were not where the tap landed. A run that merely exited 0 would pass with
# the menu never drawn.
mini="$(mktemp)"; mscript="$(mktemp)"
printf '# keep me\npresent_divisor = 3\n' > "$mini"
printf 'menu\ntap 200 360\n' > "$mscript"
rc=0
out=$(KOBOY_STUB_GEOM=256x224 SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy \
        --core "$d_sc/stub_core.so" --rom "$d_sc/GAME.sfc" --config "$mini" \
        --ui-script "$mscript" --panel 1264x1680 --frames 60 2>&1) || rc=$?
[ "$rc" -eq 0 ] || {
    echo "FAIL: scripted MENU run under the LCD layout exited $rc"
    echo "$out" | tail -5; rm -rf "$d_sc" "$mini" "$mscript"; exit 1; }
echo "$out" | grep -q "LCD layout" || {
    echo "FAIL: the scripted MENU run was not in the LCD layout"
    rm -rf "$d_sc" "$mini" "$mscript"; exit 1; }
echo "$out" | grep -q "koboy: present_divisor = 4\$" || {
    echo "FAIL: MENU -> FRAMES did not cycle under the LCD layout"
    echo "$out" | grep -i "divisor"; rm -rf "$d_sc" "$mini" "$mscript"; exit 1; }
grep -q "^present_divisor = 4\$" "$mini" || {
    echo "FAIL: the LCD-layout menu did not write the divisor back"
    cat "$mini"; rm -rf "$d_sc" "$mini" "$mscript"; exit 1; }
rm -f "$mini" "$mscript"
echo "ok: the in-game MENU opens and acts under the LCD layout"

# THE THREE SEGA CEILINGS, end to end, and they are here because these three
# rects were the largest koboy produced and the owner reported all three as
# slow in play. Device numbers behind each value are in src/config.c's
# `ceiling` comment and TESTED.md.
#
# Each is a PAIR at identical stub geometry with only the INI differing: the
# shipped default (which takes the ceiling) against an explicit `scale =` big
# enough to reach the uncapped fit (which by design overrides it). That is the
# only control available for the Game Gear, and it is the reason the whole
# loop is written this way rather than against a .gb: a .gb at 160x144 IS the
# Game Boy by max geometry, so it takes the measured 5 and lands on exactly
# the answer the ceiling produces. A pair that compared those two would agree
# with the ceiling deleted.
#
# The Game Gear row is the one worth reading: the SAME 160x144 frame gives
# 960x864 uncapped and 800x720 capped, and 800x720 is the Game Boy's own
# picture. That is where this system was believed to be for months.
#
# The .gba row joins them and is the tightest cap in the table: 240x160 is the
# smallest frame koboy scales, so it auto-fits furthest -- 1264x842 uncapped
# against 960x640 held. Its uncapped column is FIT-limited rather than
# scale-limited (the strip's fractional fit runs out of panel width before it
# runs out of scale), which is why the free scale is 6 and not 5: an ini
# naming exactly 5 does not mark intent (KOBOY_SCALE_LEGACY_DEFAULT) and the
# pair would compare the ceiling with itself.
for row in "sms 256x192 4 768x576 1024x768" \
           "gg  160x144 6 800x720 960x864" \
           "md  320x224 6 960x672 1264x884" \
           "gba 240x160 6 960x640 1264x842"; do
    set -- $row
    ext=$1; geom=$2; free_scale=$3; want_cap=$4; want_free=$5
    d_sg=$(mktemp -d)
    cp build/stub_core.so "$d_sg/"
    head -c 32768 /dev/zero > "$d_sg/GAME.$ext"
    printf 'scale = %s\n' "$free_scale" > "$d_sg/free.ini"
    out=$(KOBOY_STUB_GEOM=$geom SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy \
            --core "$d_sg/stub_core.so" --rom "$d_sg/GAME.$ext" \
            --panel 1264x1680 --frames 2 2>&1) || {
        echo "FAIL: .$ext ceiling run"; rm -rf "$d_sg"; exit 1; }
    echo "$out" | grep -q "game $want_cap" || {
        echo "FAIL: a .$ext did not take its ceiling (wanted game $want_cap)"
        echo "$out" | grep -E "game [0-9]+x[0-9]+"; rm -rf "$d_sg"; exit 1; }
    out=$(KOBOY_STUB_GEOM=$geom SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy \
            --core "$d_sg/stub_core.so" --rom "$d_sg/GAME.$ext" \
            --config "$d_sg/free.ini" --panel 1264x1680 --frames 2 2>&1) || {
        echo "FAIL: .$ext uncapped control run"; rm -rf "$d_sg"; exit 1; }
    echo "$out" | grep -q "game $want_free" || {
        echo "FAIL: an explicit scale did not reach .$ext's uncapped $want_free,"
        echo "      so the pair above is not measuring the ceiling"
        echo "$out" | grep -E "game [0-9]+x[0-9]+"; rm -rf "$d_sg"; exit 1; }
    rm -rf "$d_sg"
done
echo "ok: .sms, .gg, .md and .gba each take their own measured ceiling"

rm -rf "$d_sc"

rm -rf "$d"

# ------------------------------------------------- the greyscale mapping
#
# END TO END, because every other check on gray_map stops at config.c. This
# one asserts the ini key reaches the LIVE koboy_video: main.c logs the map
# read back OFF video_get_gray_map(vid), not off cfg, so a main.c that parsed
# the key and then handed video_create something else fails here.
#
# Three cases, and the third is the one that matters. Asserting only that
# "gray_map = value" logs "value" would pass against a binary that echoed the
# ini string without plumbing it anywhere; asserting only the DEFAULT would
# pass against a binary that ignored the key entirely. Both directions, plus
# a bad name, are needed for the pair to distinguish anything.
gd="$(mktemp -d)"
: > "$gd/GAME.gb"

printf 'rom_dir = %s\ngray_map = value\n' "$gd" > "$gd/koboy.ini"
rc=0
out=$(SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy --config "$gd/koboy.ini" \
        --core build/stub_core.so --rom "$ROM" --frames 30 2>&1) || rc=$?
[ "$rc" -eq 0 ] || { echo "FAIL: gray_map run exited $rc"; echo "$out"; exit 1; }
echo "$out" | grep -q "gray_map value" \
    || { echo "FAIL: gray_map = value did not reach video_create"; echo "$out"; exit 1; }

printf 'rom_dir = %s\n' "$gd" > "$gd/koboy.ini"
rc=0
out=$(SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy --config "$gd/koboy.ini" \
        --core build/stub_core.so --rom "$ROM" --frames 30 2>&1) || rc=$?
[ "$rc" -eq 0 ] || { echo "FAIL: default gray_map run exited $rc"; exit 1; }
echo "$out" | grep -q "gray_map balanced" \
    || { echo "FAIL: the shipped default is not balanced"; echo "$out"; exit 1; }

# A name nobody recognises must keep the value already set, NOT fall back to
# entry 0 (luma) -- which is the exact mapping this setting exists to move
# away from, so a typo must never silently reinstate it.
printf 'rom_dir = %s\ngray_map = equal\ngray_map = nonsense\n' "$gd" > "$gd/koboy.ini"
rc=0
out=$(SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy --config "$gd/koboy.ini" \
        --core build/stub_core.so --rom "$ROM" --frames 30 2>&1) || rc=$?
[ "$rc" -eq 0 ] || { echo "FAIL: bad gray_map run exited $rc"; exit 1; }
echo "$out" | grep -q "gray_map equal" \
    || { echo "FAIL: an unknown gray_map name did not keep the previous value"; echo "$out"; exit 1; }
rm -rf "$gd"
echo "ok: gray_map reaches the video pipeline, defaults to balanced, survives a typo"

# ------------------------------------------------------------- rotation
#
# END TO END, for exactly the reason the gray_map block above is: every other
# check on rotation stops at core.c or video.c, and the thing that can still be
# missing is the WIRE BETWEEN THEM in main.c. A koboy that handled
# SET_ROTATION perfectly in core.c and never called video_set_rotation would
# pass tests/test_core.c and tests/test_video_pipeline.c and present Galaga
# sideways on the device.
#
# The stub announces a quarter turn from inside retro_load_game -- where
# FinalBurn Neo announces it -- with a deliberately NON-SQUARE 288x224
# geometry, so "transposed" and "not transposed" are different numbers rather
# than the same ones twice.
rc=0
out=$(KOBOY_STUB_ROTATE=3 SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy \
        --core build/stub_core.so --rom "$ROM" --panel 1264x1680 --frames 30 2>&1) || rc=$?
[ "$rc" -eq 0 ] || { echo "FAIL: rotated run exited $rc"; echo "$out"; exit 1; }
# The geometry line reports the PRESENTED shape: 288x224 turned is 224x288.
echo "$out" | grep -q "core geometry 224x288" \
    || { echo "FAIL: a quarter-turned core did not present transposed geometry"; echo "$out"; exit 1; }
echo "$out" | grep -q "quarter turn" \
    || { echo "FAIL: the rotation was never reported"; echo "$out"; exit 1; }
# AND THE FRAMES ACTUALLY LANDED, which is the assertion that catches the one
# mutant the two greps above cannot: a koboy that handles SET_ROTATION in
# core.c (so the log lines are right) and never calls video_set_rotation.
# The stub reports max == base at a NON-SQUARE 288x224, so the reserved rect
# is 224 wide; an un-rotated 288-wide frame fails video_pipeline_run's bound
# check and is DROPPED SILENTLY -- the run still exits 0, still prints both
# lines above, and shows a black game rect. "0 rects emitted" is the only
# outward sign, so it is what is asserted.
echo "$out" | grep -qE 'stopped, [1-9][0-9]* presented frames' \
    || { echo "FAIL: a rotated core presented no frames -- the turn never reached video.c"; echo "$out"; exit 1; }
echo "$out" | grep -qE '[1-9][0-9]* rects emitted' \
    || { echo "FAIL: a rotated core emitted no rects"; echo "$out"; exit 1; }

# The un-rotated control, so the assertions above cannot pass by matching a
# string koboy prints unconditionally.
rc=0
out=$(SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy \
        --core build/stub_core.so --rom "$ROM" --panel 1264x1680 --frames 30 2>&1) || rc=$?
[ "$rc" -eq 0 ] || { echo "FAIL: unrotated control run exited $rc"; exit 1; }
echo "$out" | grep -q "quarter turn" \
    && { echo "FAIL: a core that asked for no rotation was reported as rotated"; echo "$out"; exit 1; }
echo "ok: SET_ROTATION reaches the video pipeline and transposes the game rect"

# ----------------------------------------------- too short to be a cartridge
# THE ONLY GUARD IN THIS PROJECT THAT EXISTS TO STOP A CRASH RATHER THAN A
# WRONG PICTURE, and therefore the one whose wiring is worth an end-to-end
# check the same way pacing and pixel aspect are above.
#
# snes9x2005 raises SIGFPE inside retro_load_game for a .sfc/.smc under 8192
# bytes: LoROMMap does `% Memory.CalculatedSize`, and CalculatedSize rounds the
# file down to whole 8 KB blocks, so it is zero. Measured on the real core --
# every size from 0 to 1024 exits 136, 8192 does not, and the backtrace is
# retro_load_game -> LoadROM -> InitROM -> LoROMMap. That kills koboy outright:
# no error, no way back to the browser. The file that found it is real and is
# the kind every FAT32 collection grows -- a 212-byte macOS AppleDouble stub
# named `._something.smc` sitting beside the game it describes.
#
# test_config.c proves config_min_rom_bytes returns 8192 for .sfc/.smc and 0
# for everything else. NOTHING in it can prove main.c actually consults it
# before handing the file to a core, because main.c has no unit test -- the
# same gap the pacing block above was written for.
#
# THE STUB CORE IS USED ON PURPOSE, not the real SNES one. The guard is keyed
# on the ROM's EXTENSION and fires before any core is opened, so the stub is
# enough to exercise the wiring -- and it makes the mutant crisp rather than
# fatal: the stub happily accepts a 212-byte file and runs, so with the guard
# removed this run SUCCEEDS instead of dying, and the check below turns red on
# an exit code of 0 rather than on a signal. A test that can only fail by
# crashing the thing it tests is hard to trust.
short_sfc=build/short_cartridge.sfc
head -c 212 /dev/zero > "$short_sfc"
rc=0
out=$(SDL_VIDEODRIVER=dummy ./build/koboy --core build/stub_core.so \
        --rom "$short_sfc" --frames 5 2>&1) || rc=$?
[ "$rc" -ne 0 ] || {
    echo "FAIL: a 212-byte .sfc was accepted (exit 0)"
    echo "      main.c is not consulting config_min_rom_bytes before loading;"
    echo "      the real SNES core would have died of SIGFPE here."
    exit 1; }
echo "$out" | grep -qi "too short to be a cartridge" || {
    echo "FAIL: a 212-byte .sfc was refused, but not for being too short"
    echo "$out"
    exit 1; }

# ...and the floor is a FLOOR, not a ban on the extension. 8192 bytes is the
# first size the core survives, so it must load. Without this the check above
# is equally satisfied by a koboy that refuses every .sfc ever offered to it.
ok_sfc=build/ok_cartridge.sfc
head -c 8192 /dev/zero > "$ok_sfc"
rc=0
SDL_VIDEODRIVER=dummy ./build/koboy --core build/stub_core.so \
    --rom "$ok_sfc" --frames 5 --quiet >/dev/null 2>&1 || rc=$?
[ "$rc" -eq 0 ] || {
    echo "FAIL: an 8192-byte .sfc was refused (exit $rc); the floor is off by one"
    exit 1; }

# ...and it is SNES-ONLY. An Atari 2600 cartridge really is 2048 or 4096 bytes
# and a Game Boy ROM can be small too, so a floor applied to every system would
# refuse real content to guard a crash those systems do not have. Same 212
# bytes, a .gb name, and it must load.
short_gb=build/short_cartridge.gb
head -c 212 /dev/zero > "$short_gb"
rc=0
SDL_VIDEODRIVER=dummy ./build/koboy --core build/stub_core.so \
    --rom "$short_gb" --frames 5 --quiet >/dev/null 2>&1 || rc=$?
[ "$rc" -eq 0 ] || {
    echo "FAIL: a 212-byte .gb was refused (exit $rc); the floor is not SNES-only"
    exit 1; }
rm -f "$short_sfc" "$ok_sfc" "$short_gb"
echo "PASS smoke_host cartridge floor: 212B .sfc refused, 8192B .sfc ok, 212B .gb ok"

# ------------------------------- a ROM that fails to load must not kill koboy
# REPORTED FROM THE DEVICE, TWICE: selecting a game from RECENT exited koboy
# back to Nickel. Any failure to load the startup ROM used to call fatal() and
# return 1, which on a device with no terminal is indistinguishable from a
# crash -- the app vanishes, Nickel comes back, and whatever the user was
# doing is gone. A stale RECENT row is an ORDINARY condition (the file was
# deleted, renamed, half-copied, or lives on a card that is not mounted), so
# the run must survive it and put the user back on the MAIN MENU.
#
# THE FAILURE IS REAL, NOT SIMULATED: the .sfc cartridge floor above is the
# one refusal this suite can trigger from outside the process without a core
# that lies. BAD.sfc is 8192 bytes when it is recorded into RECENT and 212
# bytes when it is selected -- exactly the shape of a file that was fine and
# is not any more (an interrupted re-copy), and the one case recent_prune_
# missing cannot catch, because the file still exists.
#
# The run asserts RECOVERY, not a printed message: it fails the ROM, comes
# back, and starts a DIFFERENT game, so what is proven is that the process
# was still alive and still driving its UI. A test that only grepped for
# "COULD NOT LOAD" would pass against a koboy that printed it and exited.
rd="$(mktemp -d)"; sd="$(mktemp -d)"
head -c 8192 /dev/zero > "$rd/BAD.sfc"
: > "$rd/GOOD.gb"
# Row geometry is this file's usual 1264x1680 derivation: row_h=64, row r's
# centre is 8 + 64 + r*64 + 32. $rd is FLAT so no folder row shifts an index.
#   MAIN MENU: row 0 RECENT, row 1 ALL GAMES
#   browser:   row 0 BAD.sfc, row 1 GOOD.gb   (alphabetical, src/romlist.c)
s1="$(mktemp)"; printf 'tap 200 168\ntap 200 104\n' > "$s1"   # ALL GAMES -> BAD.sfc
s2="$(mktemp)"; printf 'tap 200 168\ntap 200 168\n' > "$s2"   # ALL GAMES -> GOOD.gb
for seed in "$s1" "$s2"; do
    rc=0
    SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy --core build/stub_core.so \
        --rom-dir "$rd" --save-dir "$sd" --ui-script "$seed" \
        --panel 1264x1680 --frames 3 --quiet >/dev/null 2>&1 || rc=$?
    [ "$rc" -eq 0 ] || {
        echo "FAIL: seeding the RECENT list exited $rc"
        rm -rf "$rd" "$sd" "$s1" "$s2"; exit 1; }
done
# RECENT is most-recent-first, so: row 0 GOOD.gb, row 1 BAD.sfc, row 2 BACK.
# Both names must be in recent.dat, or the taps below aim at nothing.
grep -qa "BAD.sfc" "$sd/recent.dat" && grep -qa "GOOD.gb" "$sd/recent.dat" || {
    echo "FAIL: the RECENT list was not seeded with both roms"
    rm -rf "$rd" "$sd" "$s1" "$s2"; exit 1; }

head -c 212 /dev/zero > "$rd/BAD.sfc"      # ...and now it is broken
s3="$(mktemp)"
#   tap 200 104 -- MAIN MENU row 0, RECENT
#   tap 200 168 -- RECENT row 1, BAD.sfc            <- the load that fails
#   tap 200 104 -- MAIN MENU row 0, RECENT          <- proves we came BACK
#   tap 200 104 -- RECENT row 0, GOOD.gb            <- and can still play
# The third and fourth taps are the discriminator. A koboy that exited on the
# failed load never consumes them, and the run reports the exit code it died
# with instead of 0.
printf 'tap 200 104\ntap 200 168\ntap 200 104\ntap 200 104\n' > "$s3"
rc=0
out=$(SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy --core build/stub_core.so \
        --rom-dir "$rd" --save-dir "$sd" --ui-script "$s3" \
        --panel 1264x1680 --frames 3 2>&1) || rc=$?
[ "$rc" -eq 0 ] || {
    echo "FAIL: a RECENT row that could not load exited $rc instead of recovering"
    echo "      (1 = the old fatal-and-quit; 4 = it never reached the second game;"
    echo "       124 = it hung on the error message)"
    echo "$out" | tail -20
    rm -rf "$rd" "$sd" "$s1" "$s2" "$s3"; exit 1; }
echo "$out" | grep -q "chose $rd/BAD.sfc (recent)" || {
    echo "FAIL: the run never selected the broken rom, so it proved nothing"
    echo "$out"; rm -rf "$rd" "$sd" "$s1" "$s2" "$s3"; exit 1; }
echo "$out" | grep -q "too short to be a cartridge" || {
    echo "FAIL: the broken rom was not refused for being short"
    echo "$out"; rm -rf "$rd" "$sd" "$s1" "$s2" "$s3"; exit 1; }
echo "$out" | grep -q "COULD NOT LOAD" || {
    echo "FAIL: nothing was drawn on the panel to say why the game did not start"
    echo "$out"; rm -rf "$rd" "$sd" "$s1" "$s2" "$s3"; exit 1; }
echo "$out" | grep -q "chose $rd/GOOD.gb (recent)" || {
    echo "FAIL: the run did not come back to the MAIN MENU and start another game"
    echo "$out"; rm -rf "$rd" "$sd" "$s1" "$s2" "$s3"; exit 1; }
echo "$out" | grep -q '^presented=' || {
    echo "FAIL: the recovered run never reached the emulator loop"
    echo "$out"; rm -rf "$rd" "$sd" "$s1" "$s2" "$s3"; exit 1; }
# The FACEPLATE followed the second ROM. Everything derived from the
# extension -- layout, buttons, ceiling, and the core itself -- sits inside
# the retry loop, so a second trip round must redo it; a koboy that recovered
# but kept the first ROM's presentation would dress a Game Boy as an SNES.
echo "$out" | grep -q "faceplate DMG" || {
    echo "FAIL: the recovered run kept the failed rom's faceplate"
    echo "$out" | grep -i faceplate; rm -rf "$rd" "$sd" "$s1" "$s2" "$s3"; exit 1; }
rm -rf "$rd" "$sd" "$s1" "$s2" "$s3"
echo "ok: a RECENT row that cannot load returns to the MAIN MENU, and the next game starts"

# ...and the SAME failure from ALL GAMES, which is the other startup entry
# point, plus the half of the fix that is about the list rather than the
# process: a ROM is recorded as "played" only once it actually LOADED.
# Recording at pick time (where it used to happen) promoted a ROM that failed
# to the top of the RECENT list -- the wall the user just hit, moved to row 0
# of the screen they have to walk past to try something else.
rd="$(mktemp -d)"; sd="$(mktemp -d)"
head -c 212 /dev/zero > "$rd/BAD.sfc"
: > "$rd/GOOD.gb"
s4="$(mktemp)"
#   tap 200 168 -- MAIN MENU row 1, ALL GAMES
#   tap 200 104 -- browser row 0, BAD.sfc   <- fails
#   tap 200 168 -- MAIN MENU row 1, ALL GAMES
#   tap 200 168 -- browser row 1, GOOD.gb   <- plays
printf 'tap 200 168\ntap 200 104\ntap 200 168\ntap 200 168\n' > "$s4"
rc=0
out=$(SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy --core build/stub_core.so \
        --rom-dir "$rd" --save-dir "$sd" --ui-script "$s4" \
        --panel 1264x1680 --frames 3 2>&1) || rc=$?
[ "$rc" -eq 0 ] || {
    echo "FAIL: a browser pick that could not load exited $rc instead of recovering"
    echo "$out" | tail -20; rm -rf "$rd" "$sd" "$s4"; exit 1; }
echo "$out" | grep -q "chose $rd/GOOD.gb\$" || {
    echo "FAIL: the browser run did not come back and start another game"
    echo "$out"; rm -rf "$rd" "$sd" "$s4"; exit 1; }
grep -qa "GOOD.gb" "$sd/recent.dat" || {
    echo "FAIL: the game that DID load was not recorded as recent"
    rm -rf "$rd" "$sd" "$s4"; exit 1; }
grep -qa "BAD.sfc" "$sd/recent.dat" && {
    echo "FAIL: a rom that never loaded was recorded as recently played"
    rm -rf "$rd" "$sd" "$s4"; exit 1; }
rm -rf "$rd" "$sd" "$s4"
echo "ok: a browser pick that cannot load recovers, and is not recorded as played"

# THE OTHER HALF OF THE RULE: with no list to go back to there is nothing to
# recover TO, so --rom must still fail the run. The cartridge-floor block
# above already asserts the exit code for exactly this file; what it cannot
# see is the DISTINCTION, which is the whole basis of the fix -- the same
# 212-byte .sfc that returns to the menu when it was tapped in a list must
# still exit nonzero when it was named on the command line. Without this
# check, "never exit on a bad rom" would pass everything above and would
# silently make `koboy --rom nonsense` report success to its launcher.
rd="$(mktemp -d)"
head -c 212 /dev/zero > "$rd/BAD.sfc"
rc=0
SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy --core build/stub_core.so \
    --rom "$rd/BAD.sfc" --frames 3 --quiet >/dev/null 2>&1 || rc=$?
[ "$rc" -ne 0 ] || {
    echo "FAIL: --rom with an unloadable file exited 0; there was no menu to return to"
    rm -rf "$rd"; exit 1; }
[ "$rc" -ne 124 ] || {
    echo "FAIL: --rom with an unloadable file hung instead of exiting"
    rm -rf "$rd"; exit 1; }
rm -rf "$rd"
echo "ok: --rom with an unloadable file still ends the run (rc=$rc)"

# ------------------- ...and the same failure from MENU -> CHOOSE ROM
# The mid-session half. It had the same fatal()-then-quit shape as the startup
# path and a weaker excuse: the startup path could at least argue there was
# nowhere to go back to, while this one is written INSIDE the loop that draws
# the MAIN MENU. The previous game is gone by then (flushed and unloaded so
# CHOOSE ROM can have the core to itself), so there is no resuming it -- but
# choosing a different game is what the user opened this screen to do.
#
# This is the first automated coverage the mid-session load has ever had.
# MODE_MENU's sub-screens were passed NULL for every script argument with a
# comment saying they could never be driven; the `menu` verb made that stale,
# and they now take the same cursor as every other screen.
#
# ROW COORDINATES ARE HARDCODED, same derivation as every other tap in this
# file (row_h=64, row r's centre is 8 + 64 + r*64 + 32) and same fragility: a
# row inserted ABOVE CHOOSE ROM in the MENU strands this tap on RESUME or
# FRAMES, and the run then exits 0 having tested nothing. The "switched to"
# assertion below is what catches that -- it is printed only by a mid-session
# load that SUCCEEDED, so a tap that missed CHOOSE ROM fails here rather than
# passing quietly.
rd="$(mktemp -d)"; sd="$(mktemp -d)"
head -c 212 /dev/zero > "$rd/BAD.sfc"
: > "$rd/GOOD.gb"
s5="$(mktemp)"
#   menu        -- open the in-game MENU
#   tap 200 424 -- MENU row 5, CHOOSE ROM (SAVE LOAD RESET GRAY FRAMES ...)
#   tap 200 168 -- mid-session MAIN MENU row 1, ALL GAMES
#   tap 200 104 -- browser row 0, BAD.sfc   <- the load that fails
#   tap 200 168 -- MAIN MENU row 1 again    <- proves we came BACK
#   tap 200 168 -- browser row 1, GOOD.gb   <- and can still start a game
printf 'menu\ntap 200 424\ntap 200 168\ntap 200 104\ntap 200 168\ntap 200 168\n' > "$s5"
rc=0
out=$(SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy --core build/stub_core.so \
        --rom "$rd/GOOD.gb" --rom-dir "$rd" --save-dir "$sd" --ui-script "$s5" \
        --panel 1264x1680 --frames 60 2>&1) || rc=$?
[ "$rc" -eq 0 ] || {
    echo "FAIL: a mid-session pick that could not load exited $rc instead of recovering"
    echo "$out" | tail -20; rm -rf "$rd" "$sd" "$s5"; exit 1; }
echo "$out" | grep -q "COULD NOT LOAD" || {
    echo "FAIL: the mid-session run never reached the failing load"
    echo "      (a tap that missed CHOOSE ROM leaves the game running and exits 0)"
    echo "$out"; rm -rf "$rd" "$sd" "$s5"; exit 1; }
echo "$out" | grep -q "switched to $rd/GOOD.gb" || {
    echo "FAIL: the mid-session run did not come back and load another game"
    echo "$out"; rm -rf "$rd" "$sd" "$s5"; exit 1; }
echo "$out" | grep -q "switched to $rd/BAD.sfc" && {
    echo "FAIL: the rom that failed to load was reported as loaded"
    echo "$out"; rm -rf "$rd" "$sd" "$s5"; exit 1; }
grep -qa "BAD.sfc" "$sd/recent.dat" && {
    echo "FAIL: a mid-session rom that never loaded was recorded as recently played"
    rm -rf "$rd" "$sd" "$s5"; exit 1; }
rm -rf "$rd" "$sd" "$s5"
echo "ok: MENU -> CHOOSE ROM survives a rom that cannot load"

# ----------------------- MENU -> CHOOSE ROM ACROSS SYSTEMS, which is a crash
# REPORTED FROM THE DEVICE: playing a GBA game, MENU -> CHOOSE ROM, picked a
# Mega Drive game, koboy died to Nickel. The log had two "switched to" lines
# and only ONE "koboy: core" line -- the .md was handed to gpSP, which
# executed Mega Drive data as ARM code:
#
#     bad jump 8000000 (8000000)
#     Segmentation fault
#
# The mid-session path reloaded into the core opened at STARTUP for the FIRST
# ROM's extension. Everything else derived from the extension was equally
# stale: the faceplate, the button complement, the scale ceiling and the
# .srm binding. A switch now ends the session and re-enters the setup loop,
# so all of it is derived again.
#
# THE ASSERTION IS THAT THE CORE CHANGED, not that the process lived. A koboy
# that kept gpSP and happened not to crash on some other file would pass a
# liveness check, and that is exactly the run this bug hid behind for a day.
#
# Same stand-in-core harness as the "core chosen by extension" block far
# above -- a copy of the binary in its own directory with .so files named
# what the resolver will ask for -- because the choice is only observable
# end to end.
d_sw="$(mktemp -d)"
cp build/koboy        "$d_sw/koboy"
cp build/stub_core.so "$d_sw/gambatte_libretro.so"
cp build/stub_core.so "$d_sw/genesis_plus_gx_libretro.so"
mkdir -p "$d_sw/roms" "$d_sw/save"
printf '\0' > "$d_sw/roms/AAA.gb"
head -c 32768 /dev/zero > "$d_sw/roms/ZZZ.md"
sw_script="$(mktemp)"
#   menu        -- open the in-game MENU on the Game Boy game
#   tap 200 424 -- MENU row 5, CHOOSE ROM (row_h=64, centre 8+64+r*64+32)
#   tap 200 168 -- MAIN MENU row 1, ALL GAMES
#   tap 200 168 -- browser row 1, ZZZ.md (row 0 is AAA.gb; alphabetical)
printf 'menu\ntap 200 424\ntap 200 168\ntap 200 168\n' > "$sw_script"
rc=0
out=$(SDL_VIDEODRIVER=dummy timeout 30 "$d_sw/koboy" --rom "$d_sw/roms/AAA.gb" \
        --rom-dir "$d_sw/roms" --save-dir "$d_sw/save" --ui-script "$sw_script" \
        --panel 1264x1680 --frames 60 2>&1) || rc=$?
[ "$rc" -eq 0 ] || {
    echo "FAIL: switching systems mid-session exited $rc (139 = the reported crash)"
    echo "$out" | tail -20; rm -rf "$d_sw" "$sw_script"; exit 1; }
echo "$out" | grep -q "switched to $d_sw/roms/ZZZ.md" || {
    echo "FAIL: the run never switched to the .md"
    echo "      (a tap that missed CHOOSE ROM leaves the game running and exits 0)"
    echo "$out"; rm -rf "$d_sw" "$sw_script"; exit 1; }
# THE ONE THAT MATTERS. Two core lines, naming two different .so files. The
# device's log had one, and that single missing line is the whole bug.
ncore=$(echo "$out" | grep -c "^koboy: core /")
[ "$ncore" -eq 2 ] || {
    echo "FAIL: $ncore core lines, wanted 2 -- the second game did not get its own core"
    echo "$out" | grep "koboy: core"; rm -rf "$d_sw" "$sw_script"; exit 1; }
echo "$out" | grep -qx "koboy: core $d_sw/gambatte_libretro.so" || {
    echo "FAIL: the .gb did not open gambatte"
    echo "$out" | grep "koboy: core"; rm -rf "$d_sw" "$sw_script"; exit 1; }
echo "$out" | grep -qx "koboy: core $d_sw/genesis_plus_gx_libretro.so" || {
    echo "FAIL: the .md did not open the Mega Drive core -- THIS IS THE CRASH"
    echo "$out" | grep "koboy: core"; rm -rf "$d_sw" "$sw_script"; exit 1; }
# ...and the core was not the only stale thing. The faceplate follows the
# extension (DMG for the Game Boy, the LCD strip for the Mega Drive) and the
# game rect is re-resolved for the new system.
#
# What is NOT asserted here, and is not claimed to be: that the video
# pipeline and the input state were rebuilt. Both are now locals of the
# session loop -- created after the re-fit, destroyed at the loop's bottom,
# referenced nowhere else -- so there is no variable that could carry one
# across a switch, and a mutant that reuses the video anyway produces
# identical output on this harness (both stub cores report 160x144, so only
# the LAYOUT differs and nothing gets dropped). Making it observable would
# need two stub cores with different geometry in one process, which this
# harness cannot express; asserting it by construction is the honest option.
echo "$out" | grep -q "faceplate DMG" && echo "$out" | grep -q "faceplate LCD" || {
    echo "FAIL: the faceplate did not follow the system across the switch"
    echo "$out" | grep -i faceplate; rm -rf "$d_sw" "$sw_script"; exit 1; }
echo "$out" | grep -q "LCD layout, game 480x432" || {
    echo "FAIL: the game rect was not re-resolved for the second system"
    echo "$out" | grep -i geometry; rm -rf "$d_sw" "$sw_script"; exit 1; }
# ...and neither was the SAVE BINDING. Each game's SRAM must land in its own
# .srm: a stale binding writes the Mega Drive's memory into AAA.srm and
# ZZZ.srm never appears at all, which is a silently corrupted save rather
# than a crash -- the half of this bug nobody would have reported.
[ -f "$d_sw/save/AAA.srm" ] || {
    echo "FAIL: the first game's save was not written"
    ls -la "$d_sw/save"; rm -rf "$d_sw" "$sw_script"; exit 1; }
[ -f "$d_sw/save/ZZZ.srm" ] || {
    echo "FAIL: the second game's save went somewhere else -- the .srm binding is stale"
    ls -la "$d_sw/save"; rm -rf "$d_sw" "$sw_script"; exit 1; }
echo "$out" | grep -q '^presented=' || {
    echo "FAIL: the switched-to game never reached the emulator loop"
    echo "$out"; rm -rf "$d_sw" "$sw_script"; exit 1; }
echo "ok: MENU -> CHOOSE ROM across systems opens the second system's core"

# ...AND THE SAME QUESTION ASKED OF THE STARTUP RECENT LIST, because that is
# where the owner will meet this next: the run above just put the .md into
# their recent.dat, so their next launch offers it as row 0 of RECENT.
#
# The startup path derives the core from cfg.rom_path AFTER the pick, so it
# was never capable of the mid-session bug -- but "probably fine" is what the
# mid-session comment said too, so this asserts it instead. The list is the
# one the run above wrote; row 0 is the .md because it was played last.
sw_script2="$(mktemp)"
printf 'tap 200 104\ntap 200 104\n' > "$sw_script2"   # MAIN MENU RECENT, row 0
rc=0
out=$(SDL_VIDEODRIVER=dummy timeout 30 "$d_sw/koboy" \
        --rom-dir "$d_sw/roms" --save-dir "$d_sw/save" --ui-script "$sw_script2" \
        --panel 1264x1680 --frames 5 2>&1) || rc=$?
[ "$rc" -eq 0 ] || {
    echo "FAIL: starting the .md from RECENT exited $rc"
    echo "$out" | tail -20; rm -rf "$d_sw" "$sw_script" "$sw_script2"; exit 1; }
echo "$out" | grep -q "chose $d_sw/roms/ZZZ.md (recent)" || {
    echo "FAIL: RECENT row 0 was not the .md the switch just recorded"
    echo "$out"; rm -rf "$d_sw" "$sw_script" "$sw_script2"; exit 1; }
echo "$out" | grep -qx "koboy: core $d_sw/genesis_plus_gx_libretro.so" || {
    echo "FAIL: a .md started from RECENT did not open the Mega Drive core"
    echo "$out" | grep "koboy: core"; rm -rf "$d_sw" "$sw_script" "$sw_script2"; exit 1; }
rm -rf "$d_sw" "$sw_script" "$sw_script2"
echo "ok: a .md started from the startup RECENT list gets its own core too"

# ------------------------- AND THE SAME TRIP THREE TIMES, because the session
# loop's whole job is being re-entered. One switch proves the teardown runs;
# it does not prove the teardown can run AGAIN -- a core handle not cleared,
# an SRAM pointer left dangling, a video destroyed twice all survive a single
# pass and fail on the second or third.
#
# gb -> md -> gb -> md, four sessions in one process. Every session must open
# a core and wear the faceplate its own extension asks for, so the two log
# lines have to ALTERNATE; a run that quietly stopped switching after the
# first one would still exit 0.
#
# This script is also the reason the emulator loop steps over cleared states
# while looking for a `menu` marker (uiscript_state_is_idle): without that a
# script could open the in-game MENU exactly once, and this test could not be
# written at all.
d_sw3="$(mktemp -d)"
cp build/koboy        "$d_sw3/koboy"
cp build/stub_core.so "$d_sw3/gambatte_libretro.so"
cp build/stub_core.so "$d_sw3/genesis_plus_gx_libretro.so"
mkdir -p "$d_sw3/roms" "$d_sw3/save"
printf '\0' > "$d_sw3/roms/AAA.gb"
head -c 32768 /dev/zero > "$d_sw3/roms/ZZZ.md"
sw3="$(mktemp)"
# Three switches. Each is: menu, CHOOSE ROM (row 5), ALL GAMES (row 1), then
# the browser row -- 104 for AAA.gb, 168 for ZZZ.md.
{ printf 'menu\ntap 200 424\ntap 200 168\ntap 200 168\n'
  printf 'menu\ntap 200 424\ntap 200 168\ntap 200 104\n'
  printf 'menu\ntap 200 424\ntap 200 168\ntap 200 168\n'; } > "$sw3"
rc=0
out=$(SDL_VIDEODRIVER=dummy timeout 60 "$d_sw3/koboy" --rom "$d_sw3/roms/AAA.gb" \
        --rom-dir "$d_sw3/roms" --save-dir "$d_sw3/save" --ui-script "$sw3" \
        --panel 1264x1680 --frames 200 2>&1) || rc=$?
[ "$rc" -eq 0 ] || {
    echo "FAIL: switching three times exited $rc"
    echo "$out" | tail -20; rm -rf "$d_sw3" "$sw3"; exit 1; }
got=$(echo "$out" | grep -E "^koboy: core /" | sed "s|$d_sw3/||;s|_libretro.so||" | tr '\n' ' ')
[ "$got" = "koboy: core gambatte koboy: core genesis_plus_gx koboy: core gambatte koboy: core genesis_plus_gx " ] || {
    echo "FAIL: the four sessions did not alternate cores"
    echo "      got: $got"
    rm -rf "$d_sw3" "$sw3"; exit 1; }
faces=$(echo "$out" | grep -c "faceplate")
[ "$faces" -eq 4 ] || {
    echo "FAIL: $faces faceplate lines across four sessions, wanted 4"
    echo "$out" | grep -i faceplate; rm -rf "$d_sw3" "$sw3"; exit 1; }
echo "$out" | grep -q '^presented=' || {
    echo "FAIL: the fourth session never reached the emulator loop"
    echo "$out"; rm -rf "$d_sw3" "$sw3"; exit 1; }
# ONE summary for the run, not one per session: the counters live outside the
# session loop precisely so `presented=` keeps meaning what it always meant.
nsum=$(echo "$out" | grep -c '^presented=')
[ "$nsum" -eq 1 ] || {
    echo "FAIL: $nsum presented= lines, wanted 1 -- the run's counters went per-session"
    rm -rf "$d_sw3" "$sw3"; exit 1; }
rm -rf "$d_sw3" "$sw3"
echo "ok: four sessions in one process, each with its own core and faceplate"
