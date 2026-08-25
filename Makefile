CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter
INC     := -Isrc -Itests
TESTSRC := $(wildcard tests/test_*.c)
TESTBIN := $(patsubst tests/%.c,build/%,$(TESTSRC))
SRC     := $(filter-out src/main.c src/probe.c src/platform_%.c,$(wildcard src/*.c))

build:
	@mkdir -p build tests/golden

build/test_%: tests/test_%.c $(SRC) | build
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(SRC) -lm -ldl

build/stub_core.so: tests/stub_core.c | build
	$(CC) $(CFLAGS) $(INC) -shared -fPIC -o $@ $<

build/test_core: build/stub_core.so

test: build $(TESTBIN)
	@rc=0; for t in $(TESTBIN); do echo "== $$t"; ./$$t || rc=1; done; exit $$rc

clean:
	rm -rf build

.PHONY: build test clean host

SDL_CFLAGS := $(shell pkg-config --cflags sdl2)
SDL_LIBS   := $(shell pkg-config --libs sdl2)

build/koboy: src/main.c src/platform_sdl.c $(SRC) | build
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

build/koboy-arm: src/main.c src/platform_kobo.c $(SRC) $(FBINK_LIB) | build
	$(CROSS)gcc $(CFLAGS) $(ARM_FLAGS) $(INC) -I$(FBINK_DIR) -DKOBOY_PLATFORM_KOBO \
	    -o $@ src/main.c src/platform_kobo.c $(SRC) $(FBINK_LIB) -ldl -lm

# Task 18 adds build/koboy-probe-arm; until then `kobo` builds the emulator
# only, rather than naming a target with no rule.
kobo: build/koboy-arm

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
	# Shipped only if it has been built: the probe is a separate deliverable,
	# and a missing one must not break packaging the emulator.
	@if [ -f build/koboy-probe-arm ]; then \
	    cp build/koboy-probe-arm build/pkg/.adds/koboy/koboy-probe; \
	    chmod +x build/pkg/.adds/koboy/koboy-probe; \
	    echo "dist: including koboy-probe"; \
	else \
	    echo "dist: no build/koboy-probe-arm, skipping the probe"; \
	fi
	chmod +x build/pkg/.adds/koboy/koboy build/pkg/.adds/koboy/koboy.sh
	mkdir -p dist && rm -f dist/koboy-$(VERSION).zip
	# -D omits directory entries: unzip creates the directories it needs from
	# the file paths, and a bare ".adds/" entry in the listing is exactly the
	# kind of thing that makes "writes nothing outside .adds/koboy/" hard to
	# check mechanically.
	cd build/pkg && zip -qrD ../../dist/koboy-$(VERSION).zip .adds
	@echo "dist: dist/koboy-$(VERSION).zip"
.PHONY: dist
