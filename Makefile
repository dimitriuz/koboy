CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter
# THE RELEASE NUMBER LIVES IN ./VERSION and not in this file. Three things read
# it -- this Makefile, tests/test_dist.sh and .github/workflows/release.yml's
# tag gate -- and it used to be a `VERSION :=` line buried three hundred lines
# down in the packaging section, so the other two each carried their own copy of
# an awk one-liner to dig it back out of make syntax. A bare file is the only
# format all three read without a parser.
#
# HARD ERROR rather than an empty default, and the reason is the failure it
# replaces: an unset VERSION packages `dist/koboy-.zip`, which builds, uploads
# and installs perfectly well and is named after nothing.
VERSION := $(strip $(shell cat VERSION 2>/dev/null))
ifeq ($(VERSION),)
$(error cannot read ./VERSION, the one place the release number lives)
endif
INC     := -Isrc -Itests
TESTSRC := $(wildcard tests/test_*.c)
TESTBIN := $(patsubst tests/%.c,build/%,$(TESTSRC))
SRC     := $(filter-out src/main.c src/probe.c src/platform_%.c,$(wildcard src/*.c))
# Header prerequisites, and they are load-bearing rather than tidiness. Without
# them `make` does not relink a test binary when only a header changed, so a
# MUTANT APPLIED TO A HEADER runs against a stale binary and appears not to
# fail -- which would silently invalidate the one discipline this project leans
# on hardest (break the guard, confirm the test fails). Verified: touching
# src/input.h produced a byte-identical build/test_input_buttons before this.
HDR     := $(wildcard src/*.h) $(wildcard tests/*.h)

build:
	@mkdir -p build tests/golden

build/test_%: tests/test_%.c $(SRC) $(HDR) | build
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(SRC) -lm -ldl

build/stub_core.so: tests/stub_core.c | build
	$(CC) $(CFLAGS) $(INC) -shared -fPIC -o $@ $<

build/test_core: build/stub_core.so

# Kobo-side syntax check, host-only and cheap: main.c has no FBInk dependency
# of its own -- everything it needs from the device backend comes through
# platform_kobo.h -- so its entire Kobo side (every platform_kobo_* prototype
# and every call site under #ifdef KOBOY_PLATFORM_KOBO) is checkable on a
# machine with neither fbink.h nor the Linaro cross toolchain, which is every
# dev host so far. This is the guard follow-up #14 was filed about: a
# prototype mismatch between main.c and platform_kobo.c links cleanly and
# misbehaves only on the device, and nothing before this ran on a host that
# could catch it.
kobo-syntax:
	@$(CC) -std=c99 -Wall -Wextra $(INC) -DKOBOY_PLATFORM_KOBO -fsyntax-only src/main.c \
	    || { echo "FAIL: src/main.c does not compile under -DKOBOY_PLATFORM_KOBO -- check platform_kobo.h against platform_kobo.c"; exit 1; }

test: build kobo-syntax $(TESTBIN)
	@rc=0; for t in $(TESTBIN); do echo "== $$t"; ./$$t || rc=1; done; exit $$rc

clean:
	rm -rf build

# ------------------------------------------------------------------- lint
# NEITHER `lint` NOR `coverage` PUTS ANYTHING INTO A SHIPPED BINARY, and the
# next reader will worry about exactly that, so it is said here rather than
# left to be inferred. `clang -fsyntax-only` emits no object at all, and gcov
# is a COMPILER FEATURE rather than a library -- `--coverage` instruments a
# host-only build under build/cov/ that is thrown away. `make kobo`, `make
# dist` and scripts/verify-core.sh never see either, so koboy-arm's
# libc/libm/libdl closure -- the dependency ceiling CLAUDE.md calls
# non-negotiable -- is untouched by both targets. No new dependency is added:
# clang is optional (the target skips itself when it is absent) and gcov ships
# with gcc.
#
# WHY A SECOND FRONT END. Every line of this project has only ever been
# compiled by one compiler. clang carries diagnostic classes gcc does not, and
# the one this project cares about most is a TEST THAT CANNOT FAIL: a
# self-comparison of literals is a compile-time-visible shape, and the last
# four of those were found by a human reading code.
#
# THREE GROUPS, because not every file compiles on every host:
#   portable  src/*.c minus the backends and probe.c, plus tests/ -- always
#   sdl       platform_sdl.c + the host main.c, only with pkg-config sdl2
#   kobo      platform_kobo.c, probe.c and main.c under
#             -DKOBOY_PLATFORM_KOBO, only once third_party/fbink/fbink.h has
#             been fetched (`make fbink`)
# A missing group is SKIPPED with a message rather than failing the target:
# this has to be runnable on a bare CI box, which is the whole point of it
# being cheap. `make kobo-syntax` above is the guard that does NOT skip.
#
# -Wshadow IS ON FOR src/ AND OFF FOR tests/, and that asymmetry is a
# measurement rather than a compromise: on the tree as of this commit clang
# reports EIGHTY shadowed locals and every single one of them is in tests/ --
# the `int i` reused in a nested block that test files are made of. src/ has
# none. Turning it on for the half that is clean makes it a real guard; turning
# it on for the half that is not would make `make lint` red on day one and
# therefore ignored. To see them anyway: make lint LINT_TEST_EXTRA=-Wshadow
#
# -Werror IS LOAD-BEARING HERE, and it is the difference between a target and
# a target that works. Verified by mutant: a shadowed local injected into
# src/pacing.c was REPORTED by clang and `make lint` still exited 0, because a
# warning is not an error and `make` only reads the exit code. A lint target
# nothing gates on is a lint target nobody runs. It is safe to be strict here
# precisely because this target is not on the path of `make test`, `make
# host`, `make kobo` or `make dist`: a future clang that adds a warning to
# -Wall can make `make lint` red without making the project unbuildable.
LINTCC        ?= clang
#
# THE FLAG NAMES WERE CHOSEN BY MUTANT, NOT FROM THE MANUAL, and two of them
# are not what a reader would guess. `-Wtautological-compare` does NOT catch
# `unsigned x >= 0` -- that is -Wtautological-unsigned-zero-compare, which no
# -W group turns on. `-Wunreachable-code` does NOT catch a statement after a
# `return` -- that is -Wunreachable-code-return, reachable only through
# -Wunreachable-code-aggressive. Both were verified by injecting exactly those
# two shapes into src/ and watching this target stay GREEN with the obvious
# flags, then go red with these.
LINTFLAGS     ?= -std=c11 -Werror -Wall -Wextra -Wno-unused-parameter \
                 -Wtautological-compare -Wtautological-unsigned-zero-compare \
                 -Wtautological-constant-in-range-compare \
                 -Wunreachable-code-aggressive
LINT_SRC_EXTRA  ?= -Wshadow
LINT_TEST_EXTRA ?=
LINTPORTABLE  := $(filter-out src/probe.c src/platform_%.c,$(wildcard src/*.c))

# ONE SHELL FOR THE WHOLE RECIPE, and it is not style. Make runs each recipe
# LINE in its own shell, so the `exit 0` that skips a missing clang exited only
# that line's shell and the next line ran anyway -- caught by running
# `make lint LINTCC=clang-does-not-exist` and watching it fail with 127 after
# printing "skipping". A skip has to skip.
lint:
	@set -e; \
	if ! command -v $(LINTCC) >/dev/null 2>&1; then \
	    echo "lint: no $(LINTCC) on PATH -- skipping (install clang, or set LINTCC)"; \
	    exit 0; \
	fi; \
	echo "lint: portable src/ + tests/"; \
	$(LINTCC) $(LINTFLAGS) $(LINT_SRC_EXTRA) $(INC) -fsyntax-only $(LINTPORTABLE); \
	$(LINTCC) $(LINTFLAGS) $(LINT_TEST_EXTRA) $(INC) -fsyntax-only $(wildcard tests/*.c); \
	if pkg-config --exists sdl2 2>/dev/null; then \
	    echo "lint: SDL backend"; \
	    $(LINTCC) $(LINTFLAGS) $(LINT_SRC_EXTRA) $(INC) `pkg-config --cflags sdl2` \
	        -fsyntax-only src/platform_sdl.c src/main.c; \
	else echo "lint: no sdl2 via pkg-config -- SDL backend skipped"; fi; \
	if [ -f $(FBINK_DIR)/fbink.h ]; then \
	    echo "lint: Kobo backend"; \
	    $(LINTCC) $(LINTFLAGS) $(LINT_SRC_EXTRA) $(INC) -I$(FBINK_DIR) -DKOBOY_PLATFORM_KOBO \
	        -fsyntax-only src/platform_kobo.c src/probe.c src/main.c; \
	else echo "lint: no $(FBINK_DIR)/fbink.h -- Kobo backend skipped (make fbink)"; fi; \
	echo "lint: clean"

# --------------------------------------------------------------- coverage
# Per-file line coverage of src/, from running the whole host suite. See the
# ceiling note on `lint` above -- this links nothing into koboy-arm either.
#
# The work is in scripts/coverage.sh rather than here because the interesting
# part is a MERGE, not a compile: the pattern rule above is whole-program, so
# every one of the 28 test binaries contains its own copy of every file in
# $(SRC), and gcov reports 28 separate instances of src/video.c. The suite's
# real coverage is the union of them. The script's header has the rest,
# including why src/main.c, src/probe.c and the two backends -- which no test
# binary links at all -- are compiled unrun so their zero has a denominator.
coverage:
	@sh scripts/coverage.sh

.PHONY: build test clean host kobo-syntax lint coverage

SDL_CFLAGS := $(shell pkg-config --cflags sdl2)
SDL_LIBS   := $(shell pkg-config --libs sdl2)

build/koboy: src/main.c src/platform_sdl.c $(SRC) $(HDR) | build
	$(CC) $(CFLAGS) $(INC) $(SDL_CFLAGS) -o $@ $^ $(SDL_LIBS) -ldl -lm

host: build/koboy build/stub_core.so

# ------------------------------------------------------------------ device
# The prefix of the cross toolchain that actually targets the device's glibc
# 2.19 -- Linaro's archived 4.9-2014.09 armhf release. Override CROSS if yours
# differs; see docs/cross-compiling.md for how that toolchain is obtained and
# why a distro cross-compiler will not do. (The previous default here,
# arm-kobo-linux-gnueabihf-, was koxtoolchain's tuple; that build never
# completed, so no such compiler exists on this host.)
CROSS ?= arm-linux-gnueabihf-

ARM_FLAGS := -march=armv7-a -mfpu=neon -mfloat-abi=hard
FBINK_DIR := third_party/fbink
FBINK_LIB := $(FBINK_DIR)/Release/libfbink.a

# Clones and cross-builds a static libfbink.a on first use. Not a phony target:
# rebuilding FBInk on every make would cost minutes for nothing.
$(FBINK_LIB):
	CROSS=$(CROSS) ARM_FLAGS="$(ARM_FLAGS)" sh scripts/build-fbink.sh

fbink: $(FBINK_LIB)

build/koboy-arm: src/main.c src/platform_kobo.c $(SRC) $(HDR) $(FBINK_LIB) | build
	$(CROSS)gcc $(CFLAGS) $(ARM_FLAGS) $(INC) -I$(FBINK_DIR) -DKOBOY_PLATFORM_KOBO \
	    -o $@ src/main.c src/platform_kobo.c $(SRC) $(FBINK_LIB) -ldl -lm

# koboy-probe is self-contained -- it never touches config.c/video.c/etc, only
# FBInk and raw evdev -- so it links against src/probe.c alone, not $(SRC).
# Same toolchain and libfbink.a as build/koboy-arm, because it is answering
# questions ("does this platform have hasEclipseWfm", "what stride does this
# panel report") about the exact same hardware seam koboy itself uses.
build/koboy-probe-arm: src/probe.c $(FBINK_LIB) | build
	$(CROSS)gcc $(CFLAGS) $(ARM_FLAGS) $(INC) -I$(FBINK_DIR) \
	    -o $@ src/probe.c $(FBINK_LIB) -ldl -lm

kobo: build/koboy-arm build/koboy-probe-arm

# Not phony, for the same reason as $(FBINK_LIB) above: a full gambatte
# cross-build is minutes of work, and `make dist` must not pay for it on every
# invocation. Delete the .so to force a rebuild.
CORE_SO := dist/gambatte_libretro.so
$(CORE_SO):
	CROSS=$(CROSS) sh scripts/build-core.sh

core: $(CORE_SO)

# The Game & Watch core, same non-phony reasoning as $(CORE_SO) above: its
# cross-build is minutes of work and `make dist` must not repeat it every
# time. OUT is passed so the script writes straight into dist/ under the
# shipped name (its own default is build/gw_libretro_arm.so, which is where
# an exploratory build belongs, not a packaged one). Delete the .so to force
# a rebuild.
CORE_GW_SO := dist/gw_libretro.so
$(CORE_GW_SO):
	CROSS=$(CROSS) OUT=$@ sh scripts/build-gw-core.sh kobo

core-gw: $(CORE_GW_SO)

# The NES and Pokemon Mini cores, same non-phony reasoning again: fceumm in
# particular is ~600 translation units and minutes of cross-build, and `make
# dist` must not repeat it. Delete the .so to force a rebuild.
CORE_NES_SO := dist/fceumm_libretro.so
$(CORE_NES_SO):
	CROSS=$(CROSS) OUT=$@ sh scripts/build-fceumm-core.sh kobo

core-nes: $(CORE_NES_SO)

CORE_PM_SO := dist/pokemini_libretro.so
$(CORE_PM_SO):
	CROSS=$(CROSS) OUT=$@ sh scripts/build-pokemini-core.sh kobo

core-pm: $(CORE_PM_SO)

# The WonderSwan and Neo Geo Pocket cores, same non-phony reasoning as every
# core rule above: a cross-build is minutes of work and `make dist` must not
# repeat it. Each is named for the .so its own upstream Makefile produces, so
# a file in dist/ can be traced back to the project that built it -- the same
# convention gw/fceumm/pokemini already follow. Delete the .so to force a
# rebuild.
CORE_WS_SO := dist/mednafen_wswan_libretro.so
$(CORE_WS_SO):
	CROSS=$(CROSS) OUT=$@ sh scripts/build-wswan-core.sh kobo

core-ws: $(CORE_WS_SO)

CORE_NGP_SO := dist/race_libretro.so
$(CORE_NGP_SO):
	CROSS=$(CROSS) OUT=$@ sh scripts/build-race-core.sh kobo

core-ngp: $(CORE_NGP_SO)

# The Atari 2600, ColecoVision, Intellivision and Master System / Game Gear
# cores. Same non-phony reasoning as every core rule above: a cross-build is
# minutes of work and `make dist` must not repeat it. Each is named for the
# .so its own upstream Makefile produces, so a file in dist/ can be traced
# back to the project that built it. Delete the .so to force a rebuild.
#
# TWO OF THESE FOUR NEED A BIOS THE PACKAGE DOES NOT AND MUST NOT CARRY --
# gearcoleco wants colecovision.rom, freeintv wants exec.bin and grom.bin.
# The build scripts say which files and where they go; tests/test_dist.sh
# asserts none of them ever reaches the zip.
CORE_A26_SO := dist/stella2014_libretro.so
$(CORE_A26_SO):
	CROSS=$(CROSS) OUT=$@ sh scripts/build-stella-core.sh kobo

core-a26: $(CORE_A26_SO)

CORE_COL_SO := dist/gearcoleco_libretro.so
$(CORE_COL_SO):
	CROSS=$(CROSS) OUT=$@ sh scripts/build-gearcoleco-core.sh kobo

core-col: $(CORE_COL_SO)

CORE_INT_SO := dist/freeintv_libretro.so
$(CORE_INT_SO):
	CROSS=$(CROSS) OUT=$@ sh scripts/build-freeintv-core.sh kobo

core-int: $(CORE_INT_SO)

CORE_SMS_SO := dist/genesis_plus_gx_libretro.so
$(CORE_SMS_SO):
	CROSS=$(CROSS) OUT=$@ sh scripts/build-gpgx-core.sh kobo

core-sms: $(CORE_SMS_SO)

# MEGA DRIVE HAS NO RULE OF ITS OWN, and its absence is the point: Genesis
# Plus GX is natively a Mega Drive core, so $(CORE_SMS_SO) above already IS
# the Mega Drive core and `.md` routes to the same shared object as `.sms`
# and `.gg` (config_core_for_rom). Adding the system cost a table row and a
# pair of faceplate discs, not a build. Do not add a second GPGX rule here
# expecting a second .so -- there is only one, and `dist` already carries it.

CORE_SNES_SO := dist/snes9x2005_libretro.so
$(CORE_SNES_SO):
	CROSS=$(CROSS) OUT=$@ sh scripts/build-snes-core.sh kobo

core-snes: $(CORE_SNES_SO)

CORE_PCE_SO := dist/mednafen_pce_fast_libretro.so
$(CORE_PCE_SO):
	CROSS=$(CROSS) OUT=$@ sh scripts/build-pce-core.sh kobo

core-pce: $(CORE_PCE_SO)

CORE_GBA_SO := dist/gpsp_libretro.so
$(CORE_GBA_SO):
	CROSS=$(CROSS) OUT=$@ sh scripts/build-gba-core.sh kobo

core-gba: $(CORE_GBA_SO)

# EVERY CORE SHIPS IN THE MAIN PACKAGE. That is one archive for fifteen
# systems, and it is a REVERSAL: arcade shipped separately for most of this
# project's life, behind a `fbneo-dist` target that no longer exists.
#
# Why it was split, and why that stopped being right. FinalBurn Neo is 39.2 MB
# stripped against 18.2 MB for the other fourteen cores put together, so a
# package carrying it looked like a tenfold blowup on a 4 MB download and most
# people running a Game Boy on an e-reader have no arcade romset. But the
# figure that decides a download is the COMPRESSED one, and it was never
# measured until the split was questioned: the package is 18.6 MB, of which
# FBNeo is 13.6 -- it deflates 67%, because a driver table is mostly similar
# to the next driver table. 18.6 MB is an unremarkable download, and it
# bought a second archive, a second README, a second set of install
# instructions and a `.zip` row in the browser that failed to load until the
# user found the other download.
#
# So the size is now the USER'S choice instead of the packager's: deleting
# `.adds/koboy/fbneo_libretro.so` reclaims 39 MB on the card and costs arcade
# support and nothing else. The README says so.
#
# `core-fbneo` remains, and it is the target for rebuilding just this core.
# `fbneo-dist` was removed rather than kept, because a target that produces an
# archive nothing ships is a target that rots -- and tests/test_dist.sh was
# the only thing still building it.
#
# tests/test_dist.sh asserts all fourteen cores ARE in the package and that its
# size stays under a cap. Read that cap's comment before adding a core: it is
# a prompt to make this decision again, not a limit to route around.
CORE_FBNEO_SO := dist/fbneo_libretro.so
$(CORE_FBNEO_SO):
	CROSS=$(CROSS) OUT=$@ sh scripts/build-fbneo-core.sh kobo

core-fbneo: $(CORE_FBNEO_SO)
.PHONY: kobo fbink core core-gw core-nes core-pm core-ws core-ngp \
        core-a26 core-col core-int core-sms core-snes core-pce core-gba \
        core-fbneo

# ------------------------------------------------------------------ packaging
# The archive unzips onto the device's user partition and touches nothing else:
# no KoboRoot.tgz, nothing under /usr, nothing the firmware updater will notice.
# Everything it writes lives in .adds/koboy/, so uninstalling is `rm -rf` of one
# directory and a bad build cannot brick anything.

dist: kobo $(CORE_SO) $(CORE_GW_SO) $(CORE_NES_SO) $(CORE_PM_SO) $(CORE_WS_SO) $(CORE_NGP_SO) \
      $(CORE_A26_SO) $(CORE_COL_SO) $(CORE_INT_SO) $(CORE_SMS_SO) \
      $(CORE_SNES_SO) $(CORE_PCE_SO) $(CORE_GBA_SO) $(CORE_FBNEO_SO) | build
	rm -rf build/pkg && mkdir -p build/pkg/.adds/koboy
	cp build/koboy-arm           build/pkg/.adds/koboy/koboy
	cp scripts/koboy.sh          build/pkg/.adds/koboy/
	cp scripts/nm-koboy          build/pkg/.adds/koboy/
	cp scripts/kfmon-koboy.ini   build/pkg/.adds/koboy/
	cp config/koboy.ini          build/pkg/.adds/koboy/
	cp README.md TESTED.md       build/pkg/.adds/koboy/
	cp $(CORE_SO)                build/pkg/.adds/koboy/
	cp $(CORE_GW_SO)             build/pkg/.adds/koboy/
	cp $(CORE_NES_SO)            build/pkg/.adds/koboy/
	cp $(CORE_PM_SO)             build/pkg/.adds/koboy/
	cp $(CORE_WS_SO)             build/pkg/.adds/koboy/
	cp $(CORE_NGP_SO)            build/pkg/.adds/koboy/
	cp $(CORE_A26_SO)            build/pkg/.adds/koboy/
	cp $(CORE_COL_SO)            build/pkg/.adds/koboy/
	cp $(CORE_INT_SO)            build/pkg/.adds/koboy/
	cp $(CORE_SMS_SO)            build/pkg/.adds/koboy/
	cp $(CORE_SNES_SO)           build/pkg/.adds/koboy/
	cp $(CORE_PCE_SO)            build/pkg/.adds/koboy/
	cp $(CORE_GBA_SO)            build/pkg/.adds/koboy/
	cp $(CORE_FBNEO_SO)          build/pkg/.adds/koboy/
	# `kobo` (a prerequisite above) now always produces build/koboy-probe-arm
	# too, so this branch is normally taken -- kept as a guard rather than an
	# unconditional cp so a partial/manual build still packages the emulator
	# instead of failing outright over a missing, separable deliverable.
	@if [ -f build/koboy-probe-arm ]; then \
	    cp build/koboy-probe-arm build/pkg/.adds/koboy/koboy-probe; \
	    chmod +x build/pkg/.adds/koboy/koboy-probe; \
	    echo "dist: including koboy-probe"; \
	else \
	    echo "dist: no build/koboy-probe-arm, skipping the probe"; \
	fi
	chmod +x build/pkg/.adds/koboy/koboy build/pkg/.adds/koboy/koboy.sh
	# The browser needs somewhere to look. zip -qrD (below) omits directory
	# entries entirely, so an empty roms/ would just not be in the archive and
	# the browser's first run would report a missing directory -- the
	# README.txt is what actually makes the directory exist in the zip.
	#
	# Both of these live in packaging/ as real files rather than as printf
	# here, and that is the point: a 40-line document assembled from an
	# escaped one-line shell string cannot be reviewed in a diff, cannot be
	# linked from the README, and is invisible to anyone reading the repo --
	# which is where someone decides whether to install this at all. They are
	# copied, not generated.
	mkdir -p build/pkg/.adds/koboy/roms
	cp packaging/roms-README.txt build/pkg/.adds/koboy/roms/README.txt
	# The arcade notes, in the koboy directory rather than in roms/, because
	# they are about the CORE (which romset revision, which zips are devices
	# rather than games, where hiscore.dat goes) and not about where to put
	# files. This text used to be the whole content of a second archive; the
	# archive is gone and the text is not, because every question it answers
	# still gets asked and every one of them looks like a broken emulator
	# until it is answered. tests/test_dist.sh asserts three of its facts by
	# name.
	cp packaging/README-fbneo.txt build/pkg/.adds/koboy/README-fbneo.txt
	# The ONE entry in this archive that is not hidden. Every payload path
	# starts with `.adds`, and a leading dot hides the whole package from
	# Linux file managers, Finder and Explorer alike -- so the archive opens
	# looking EMPTY, which reads as a broken download. `.adds` is not
	# negotiable (it is where Kobo add-ons live and where the launcher looks),
	# so the fix is a visible file that explains the hidden one. It is inert:
	# nothing reads it, and it says so.
	#
	# Deliberately .md and not .txt: Nickel indexes TXT as a book, so a
	# README.txt at the drive root would turn up in the owner's library.
	cp packaging/KOBOY-INSTALL.md build/pkg/KOBOY-INSTALL.md
	mkdir -p dist && rm -f dist/koboy-$(VERSION).zip
	# -D omits directory entries: unzip creates the directories it needs from
	# the file paths, and a bare ".adds/" entry in the listing is exactly the
	# kind of thing that makes "writes nothing outside .adds/koboy/" hard to
	# check mechanically.
	cd build/pkg && zip -qrD ../../dist/koboy-$(VERSION).zip .adds KOBOY-INSTALL.md
	@echo "dist: dist/koboy-$(VERSION).zip"
.PHONY: dist

# The probe's own package: just the probe binary and a README, deployable
# before the emulator, the core or even koboy.ini exist. This is the growth
# path for TESTED.md -- someone with a device the author does not own can
# characterise it without building or even wanting the rest of the project.
probe-dist: build/koboy-probe-arm | build
	rm -rf build/probe-pkg && mkdir -p build/probe-pkg/.adds/koboy
	cp build/koboy-probe-arm    build/probe-pkg/.adds/koboy/koboy-probe
	cp docs/probe-readme.md     build/probe-pkg/.adds/koboy/README.md
	chmod +x build/probe-pkg/.adds/koboy/koboy-probe
	# Same hidden-root problem as `dist`; see the note there.
	cp packaging/KOBOY-INSTALL.md build/probe-pkg/KOBOY-INSTALL.md
	mkdir -p dist && rm -f dist/koboy-probe-$(VERSION).zip
	cd build/probe-pkg && zip -qrD ../../dist/koboy-probe-$(VERSION).zip .adds KOBOY-INSTALL.md
	@echo "probe-dist: dist/koboy-probe-$(VERSION).zip"
.PHONY: probe-dist
