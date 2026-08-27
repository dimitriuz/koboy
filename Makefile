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
VERSION := 0.5.0

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
	mkdir -p build/pkg/.adds/koboy/roms
	printf 'Put .gb, .gbc, .gba, .mgw, .nes, .min, .ws, .wsc, .ngp, .ngc, .a26,\n.col, .int, .sms, .gg, .md, .sfc, .smc, .pce and .zip files here.\nSubdirectories work; the browser walks them one level at a time.\nkoboy lists them at startup and picks the core from the extension:\n.gb/.gbc use gambatte, .mgw uses gw (Game & Watch),\n.nes uses fceumm (NES), .min uses PokeMini (Pokemon Mini),\n.ws/.wsc use wswan (WonderSwan, WonderSwan Color),\n.ngp/.ngc use race (Neo Geo Pocket, Neo Geo Pocket Color),\n.a26 uses stella2014 (Atari 2600),\n.col uses gearcoleco (ColecoVision),\n.int uses freeintv (Intellivision),\n.sms/.gg/.md use genesis_plus_gx (Master System, Game Gear, Mega Drive),\n.sfc/.smc use snes9x2005 (SNES),\n.pce uses mednafen_pce_fast (PC Engine / TurboGrafx-16),\n.gba uses gpSP (Game Boy Advance),\n.zip uses fbneo (arcade) -- SEE BELOW, and see README-fbneo.txt.\n\nEXTENSIONS THAT ARE DELIBERATELY NOT LISTED. If a file you expect\nis missing from the browser, it is almost certainly one of these,\nand each is a decision rather than a gap:\n  .bin  NOT read as Mega Drive. .bin is the extension of a dozen\n        other systems (TI-99, Odyssey 2, Atari 5200, Vectrex ...)\n        and of the Intellivision BIOS files below. koboy picks the\n        core from the extension alone, so claiming .bin would send\n        somebody else\047s file to a Mega Drive emulator. Rename a\n        Mega Drive .bin to .md and it works.\n  .gen  Also Mega Drive, also not listed: one system, one extension.\n        Rename to .md.\n  .sgx  SuperGrafx. The PC Engine core here cannot emulate that\n        hardware and would draw it wrongly rather than refuse.\n  .chd/.cue  PC Engine CD and Mega CD. These need a system-card or\n        BIOS image and CD emulation that koboy does not have.\n\nA NOTE ON SNES FILES. A .sfc or .smc smaller than 8192 bytes is\nrefused with a message about being too short. That is on purpose:\nthe SNES core crashes on such a file instead of rejecting it, and\nthe two things that produce one are a half-finished download and\nthe ._name.smc stubs macOS leaves on FAT32 cards. Neither is a game.\n\nTWO SYSTEMS NEED A BIOS THAT IS NOT OURS TO SHIP. Put these files in\nthe koboy directory itself (the one above this one, beside koboy):\n  ColecoVision   colecovision.rom   8192 bytes\n  Intellivision  exec.bin           8192 bytes\n  Intellivision  grom.bin           2048 bytes\nWithout them a .col shows a NO BIOS screen and a .int does not run.\nEvery other system here needs no BIOS file at all -- the Game Boy\nAdvance included: its core carries an open-source BIOS inside it.\n\nARCADE IS INCLUDED, AND IT IS THE BIG FILE. fbneo_libretro.so is\n41 MB of the 61 MB this package unpacks to -- every other core put\ntogether is 18 MB, and most of those are under 3. If you\nhave no arcade romset, DELETE .adds/koboy/fbneo_libretro.so and\nyou get that space back; nothing else needs it and no other\nsystem stops working. Keep it and .zip files in this directory\nrun. README-fbneo.txt, beside the koboy binary, has the rest --\nwhich romset version, why some zips are not games, and where\nhiscore.dat goes.\n' \
	    > build/pkg/.adds/koboy/roms/README.txt
	# The arcade notes, in the koboy directory rather than in roms/, because
	# they are about the CORE (which romset revision, which zips are devices
	# rather than games, where hiscore.dat goes) and not about where to put
	# files. This text used to be the whole content of a second archive; the
	# archive is gone and the text is not, because every question it answers
	# still gets asked and every one of them looks like a broken emulator
	# until it is answered. tests/test_dist.sh asserts three of its facts by
	# name.
	printf 'koboy: the arcade core, FinalBurn Neo\n\nfbneo_libretro.so is in .adds/koboy/ and .zip files in roms/ run\nwithout you doing anything. An arcade "ROM" is a zip of one board\ndump, named the way FinalBurn Neo names it -- galaga.zip, dkong.zip,\nmspacman.zip.\n\nIT IS 41 MB ON THE CARD, MOST OF THIS INSTALL. If you have no arcade\nromset, delete .adds/koboy/fbneo_libretro.so. Nothing else depends on\nit and no other system changes; .zip files simply stop listing.\n\nTHE SET HAS TO MATCH THE CORE. This build is FinalBurn Neo v1.0.0.03\n(revision of 2025-07-24). A set built for a different FBNeo release,\nand a MAME set of any vintage, will not load -- the failure looks\nexactly like a broken emulator and is not one.\n\nSOME ZIPS ARE NOT GAMES. A complete set carries device and BIOS zips\n(neogeo.zip, midssio.zip, namcoc69.zip, ...) that games load beside\nthemselves. They list in the browser and cannot be started. Leave\nthem there: tapper does not run without midssio.zip.\n\nCOIN, THEN START. A board will not begin until it has been paid:\nSELECT is Coin and START is Start. The faceplate B and A are the\nboard buttons 1 and 2, and the two extra discs are 3 and 4. Note that\na board spends its first ten or fifteen seconds on a self-test and\nignores the coin slot while it does -- Galaga takes about 800 frames.\nA coin inserted then is lost, which looks exactly like a dead button.\n\nWHAT THIS PANEL IS GOOD AT. The 1980s classics turned their monitor\non its side, so Galaga, Dig Dug, Donkey Kong, Frogger and Ms. Pac-Man\nare PORTRAIT games on a portrait e-reader. Single-screen boards with\nlittle motion (Dig Dug, Donkey Kong, Ms. Pac-Man) change 1.5 to 2.6%%\nof the picture per frame and look their best. Galaga and Galaxian are\nsingle-screen and still change 67%% and 86%%, because the STARFIELD\nscrolls -- turn MOTION on for those.\n\nNO BIOS FILE IS NEEDED for the 1980s boards -- an arcade PCB has its\nwhole program on the board. Later hardware (Neo Geo, CPS) wants a\nBIOS zip beside the games in roms/, not in the koboy directory.\n\nSAVING. An arcade board has no battery, so there is no .srm and the\nin-game MENU save states are the way to keep a game mid-play.\n\nHIGH SCORES are the other half, and they need one file you supply:\nput hiscore.dat in .adds/koboy/fbneo/ (make the directory). koboy\nturns the feature on; without that file it is simply inert. With it,\neach board writes .adds/koboy/fbneo/<board>.hi when you leave the\ngame and reads it back next time -- verified on Ms. Pac-Man, whose\nattract screen shows the saved score instead of a blank one.\n' \
	    > build/pkg/.adds/koboy/README-fbneo.txt
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
