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

# GEOMETRY CHURN: base-only changes must not re-fit; a max change must.
#
# A Game & Watch title alternates between the whole unit and the LCD alone
# several times a second. The reserved rect, the chrome around it and video's
# buffers are all sized from MAX, so a base change leaves them all correct and
# re-fitting is pure waste -- a video_destroy/video_create, a full faceplate
# repaint and a forced full-rect refresh, several times a second, on e-ink.
# koboy therefore logs "geometry settled" only when max moves.
#
# Both directions are asserted. Checking only that base churn stays quiet
# would pass just as well against a frontend that ignored geometry entirely,
# which is the failure mode this pair exists to rule out.
rc=0
out=$(KOBOY_STUB_OSCILLATE=1 SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy \
        --core build/stub_core.so --rom "$ROM" --frames 120 2>&1) || rc=$?
[ "$rc" -eq 0 ] || { echo "FAIL: oscillating-base run exited $rc"; exit 1; }
n=$(echo "$out" | grep -c "geometry settled" || true)
[ "$n" -eq 0 ] || { echo "FAIL: base-only churn re-fit $n time(s); max never moved"; exit 1; }
echo "$out" | grep -q "presented=" || { echo "FAIL: oscillating run never presented"; exit 1; }
echo "ok: base-only geometry churn does not re-fit"

rc=0
out=$(KOBOY_STUB_MAXGROW=1 SDL_VIDEODRIVER=dummy timeout 30 ./build/koboy \
        --core build/stub_core.so --rom "$ROM" --frames 120 2>&1) || rc=$?
[ "$rc" -eq 0 ] || { echo "FAIL: max-grow run exited $rc"; exit 1; }
echo "$out" | grep -q "geometry settled" \
    || { echo "FAIL: a max change did NOT re-fit"; exit 1; }
echo "ok: a max change does re-fit"

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
