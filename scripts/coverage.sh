#!/bin/sh
# Per-file line coverage of src/, measured by running the whole host test
# suite. Invoked by `make coverage`; runnable on its own from the repo root.
#
# THIS PUTS NOTHING INTO THE SHIPPED BINARY, and that is the first thing a
# reader of this file will want to know. gcov is a COMPILER FEATURE, not a
# library: --coverage makes gcc emit counters into a host-only build under
# build/cov/ that is thrown away afterwards. `make kobo`, `make dist` and
# scripts/verify-core.sh never see it, so koboy-arm's libc/libm/libdl closure
# -- the dependency ceiling CLAUDE.md calls non-negotiable -- is untouched by
# this script existing or by anyone running it. No new dependency: gcc and
# gcov ship together, and awk is already assumed by every other script here.
#
# WHY IT MERGES ACROSS BINARIES INSTEAD OF READING ONE. The Makefile's test
# rule is whole-program:
#
#     build/test_%: tests/test_%.c $(SRC) $(HDR)
#             $(CC) ... -o $@ $< $(SRC) -lm -ldl
#
# so EVERY test binary compiles EVERY file in $(SRC). gcov then reports 28
# separate instances of src/video.c, one per binary, each showing only what
# that binary's own test happened to reach -- and the highest of them is not
# the answer either. The suite's real coverage is the UNION: a line is covered
# if ANY binary executed it. That merge is what the awk at the bottom does, and
# it is the whole reason this is a script rather than a one-line `gcov` in the
# Makefile.
#
# THE FILES WITH NO TEST BINARY AT ALL. src/main.c, src/probe.c and the two
# src/platform_*.c backends are `filter-out`ed from $(SRC), so nothing links
# them and gcov has no data for them. Reporting them as absent would hide the
# finding; reporting them as "0%" with no denominator would hide its size. So
# they are compiled to a coverage object that is never run, which gives a real
# executable-line count against a genuine zero. src/main.c is the point of the
# whole exercise -- see the report this baseline was taken for.
#
# The two backends and probe.c need headers a bare host may not have (SDL2,
# and third_party/fbink from scripts/build-fbink.sh). Each is SKIPPED with a
# note rather than failing the run: a coverage number that cannot be produced
# on CI is a number nobody looks at.
set -eu

CC=${CC:-gcc}
OUT=${OUT:-build/cov}
COVFLAGS="-std=c11 -O0 -g --coverage -Wall -Wextra -Wno-unused-parameter"
INC="-Isrc -Itests"

# $(SRC) as the Makefile computes it: everything in src/ except main.c,
# probe.c and the platform backends. Kept in step by hand, which is the same
# arrangement tests/test_dist.sh has with the packaging rules.
SRC=$(ls src/*.c | grep -v -E '^src/(main|probe|platform_[a-z]+)\.c$' | tr '\n' ' ')

rm -rf "$OUT"
mkdir -p "$OUT" build tests/golden

echo "coverage: building and running the suite with --coverage"
# stub_core.so first: build/test_core dlopens it by that literal path, so the
# suite has to be run from the repo root and the stub has to exist. Built
# WITHOUT --coverage -- it is a fake libretro core, not code under test.
$CC -std=c11 -O2 -g $INC -shared -fPIC -o build/stub_core.so tests/stub_core.c

fails=0
for t in tests/test_*.c; do
    name=$(basename "$t" .c)
    # shellcheck disable=SC2086
    $CC $COVFLAGS $INC -o "$OUT/$name" "$t" $SRC -lm -ldl
    # Run from the repo root: several tests open tests/golden/*.pgm and
    # build/stub_core.so by relative path. Output is discarded -- `make test`
    # is where a failure is reported; here a non-zero exit only means the
    # counters for that binary may be short, and the run continues so one
    # broken test cannot void the whole baseline.
    if ! "./$OUT/$name" >/dev/null 2>&1; then
        echo "coverage: WARNING $name exited non-zero; its counters may be partial"
        fails=$((fails + 1))
    fi
done
[ "$fails" -eq 0 ] || echo "coverage: $fails test binaries failed -- fix them before trusting this table"

# The never-linked files, compiled but never run, purely for a denominator.
echo "coverage: compiling the files no test binary links (for a real zero)"
# shellcheck disable=SC2086
$CC $COVFLAGS $INC -c -o "$OUT/unlinked-main.o" src/main.c
if pkg-config --exists sdl2 2>/dev/null; then
    # shellcheck disable=SC2086
    $CC $COVFLAGS $INC $(pkg-config --cflags sdl2) -c -o "$OUT/unlinked-platform_sdl.o" src/platform_sdl.c
else
    echo "coverage: no sdl2 via pkg-config -- src/platform_sdl.c omitted from the table"
fi
if [ -f third_party/fbink/fbink.h ]; then
    # shellcheck disable=SC2086
    $CC $COVFLAGS $INC -Ithird_party/fbink -DKOBOY_PLATFORM_KOBO -c -o "$OUT/unlinked-platform_kobo.o" src/platform_kobo.c
    # shellcheck disable=SC2086
    $CC $COVFLAGS $INC -Ithird_party/fbink -c -o "$OUT/unlinked-probe.o" src/probe.c
else
    echo "coverage: no third_party/fbink/fbink.h -- src/platform_kobo.c and src/probe.c omitted"
    echo "coverage: (run scripts/build-fbink.sh, or make fbink, to include them)"
fi

echo "coverage: running gcov"
# --stdout rather than gcov's default of writing .gcov files, and that is not
# a preference. gcov writes its report into the CURRENT directory, named after
# the SOURCE -- so 28 binaries' worth of src/video.c counters all land on one
# `video.c.gcov` and the last one silently wins. Streaming them into one file
# keeps all 28 and lets the awk below take the union. It also means gcov runs
# from the repo root, which it has to: a .gcov report interleaves the source
# text, so gcov must be able to OPEN `src/video.c` at the path recorded in the
# .gcno, and that path is relative to where the compile happened.
gcov -b -p --stdout "$OUT"/*.gcno > "$OUT/all.gcov" 2>"$OUT/gcov.log" || {
    echo "coverage: gcov failed -- see $OUT/gcov.log"; exit 1; }

# ------------------------------------------------------------------ merge
# A .gcov line is `count:lineno:source`. count is `-` for a line that carries
# no code, `#####` or `====` for an executable line that never ran, and a
# number otherwise. The union over all 28 binaries: a line is EXECUTABLE if
# any report calls it executable, and COVERED if any report gives it a
# non-zero count.
report="$OUT/coverage.txt"
awk '
    # Each file'"'"'s block in the stream opens with a Source: tag, so this is
    # also what separates one file from the next.
    /^ *-: *0:Source:/ { sub(/^ *-: *0:Source:/, "", $0); src = $0; next }
    src == "" { next }
    src !~ /(^|\/)src\/[^\/]*\.c$/ { next }
    {
        line = $0
        n = index(line, ":")
        if (n == 0) next
        cnt = substr(line, 1, n - 1)
        rest = substr(line, n + 1)
        m = index(rest, ":")
        if (m == 0) next
        no = rest + 0
        gsub(/^ +| +$/, "", cnt)
        if (cnt == "-") next               # not an executable line
        key = src SUBSEP no
        exe[key] = 1
        if (cnt != "#####" && cnt != "====" && cnt + 0 > 0) hit[key] = 1
    }
    END {
        for (k in exe) {
            split(k, p, SUBSEP)
            f = p[1]
            sub(/^.*\/src\//, "src/", f)
            tot[f]++
            if (k in hit) cov[f]++
        }
        printf "%-24s %8s %8s %8s\n", "file", "lines", "covered", "percent"
        printf "%-24s %8s %8s %8s\n", "------------------------", "--------", "--------", "--------"
        T = 0; C = 0
        n = 0
        for (f in tot) { files[n++] = f }
        # insertion sort by percentage ascending -- the uncovered files are the
        # point of this table, so they belong at the top where they are read.
        for (i = 1; i < n; i++) {
            v = files[i]; pv = (tot[v] ? cov[v] * 1000 / tot[v] : 0)
            j = i - 1
            while (j >= 0) {
                w = files[j]; pw = (tot[w] ? cov[w] * 1000 / tot[w] : 0)
                if (pw < pv || (pw == pv && w < v)) break
                files[j + 1] = w; j--
            }
            files[j + 1] = v
        }
        for (i = 0; i < n; i++) {
            f = files[i]
            printf "%-24s %8d %8d %7.1f%%\n", f, tot[f], cov[f], tot[f] ? cov[f] * 100.0 / tot[f] : 0
            T += tot[f]; C += cov[f]
        }
        printf "%-24s %8s %8s %8s\n", "------------------------", "--------", "--------", "--------"
        printf "%-24s %8d %8d %7.1f%%\n", "TOTAL", T, C, T ? C * 100.0 / T : 0
    }
' "$OUT/all.gcov" | tee "$report"

echo
echo "coverage: table also written to $report"
echo "coverage: the merged per-line gcov stream is $OUT/all.gcov"
