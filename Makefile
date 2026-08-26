CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter
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

.PHONY: build test clean host kobo-syntax

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
.PHONY: kobo fbink

# ------------------------------------------------------------------ packaging
# The archive unzips onto the device's user partition and touches nothing else:
# no KoboRoot.tgz, nothing under /usr, nothing the firmware updater will notice.
# Everything it writes lives in .adds/koboy/, so uninstalling is `rm -rf` of one
# directory and a bad build cannot brick anything.
VERSION := 0.1.0

dist: kobo $(CORE_SO) | build
	rm -rf build/pkg && mkdir -p build/pkg/.adds/koboy
	cp build/koboy-arm           build/pkg/.adds/koboy/koboy
	cp scripts/koboy.sh          build/pkg/.adds/koboy/
	cp scripts/nm-koboy          build/pkg/.adds/koboy/
	cp scripts/kfmon-koboy.ini   build/pkg/.adds/koboy/
	cp config/koboy.ini          build/pkg/.adds/koboy/
	cp README.md TESTED.md       build/pkg/.adds/koboy/
	cp $(CORE_SO)                build/pkg/.adds/koboy/
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
	mkdir -p build/pkg/.adds/koboy/roms
	printf 'Put .gb and .gbc files in this directory.\nkoboy lists them at startup.\n' \
	    > build/pkg/.adds/koboy/roms/README.txt
	mkdir -p dist && rm -f dist/koboy-$(VERSION).zip
	# -D omits directory entries: unzip creates the directories it needs from
	# the file paths, and a bare ".adds/" entry in the listing is exactly the
	# kind of thing that makes "writes nothing outside .adds/koboy/" hard to
	# check mechanically.
	cd build/pkg && zip -qrD ../../dist/koboy-$(VERSION).zip .adds
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
	mkdir -p dist && rm -f dist/koboy-probe-$(VERSION).zip
	cd build/probe-pkg && zip -qrD ../../dist/koboy-probe-$(VERSION).zip .adds
	@echo "probe-dist: dist/koboy-probe-$(VERSION).zip"
.PHONY: probe-dist
